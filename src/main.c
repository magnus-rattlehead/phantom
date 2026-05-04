#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "input.h"
#include "ml/autocomplete.h"
#include "ml/ccg.h"
#include "ml/context.h"
#include "ml/history.h"
#include "ml/llm.h"
#include "pty.h"
#include "render.h"
#include "search.h"
#include "terminal.h"
#include "utf8.h"

#if defined(__APPLE__)
#include <libproc.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
void platform_disable_press_and_hold(void);
#include "macos.h"
#endif

static Uint32 g_event_pty_data;
static Uint32 g_event_pty_exit;
static Uint32 g_event_tui_enter;
static Uint32 g_event_tui_exit;
static Uint32 g_event_ac_ready;
#if defined(__APPLE__)
static Uint32 g_event_macos_menu;
#endif

static History *g_history;
static LLM *g_llm_base;
static LLM *g_llm_instruct;
static CCG *g_ccg;
static SearchState *g_search;
static SDL_Window *g_window;
static int g_cell_w, g_cell_h;

#define MAX_TABS 256
#define MAX_PANES 256 /* per collect_panes stack */
#define CMD_BUF_SIZE 4096
#define DIVIDER_PX 1
#define DIV_COLOR 0x928374FFu

typedef struct Pane Pane;
typedef struct PaneNode PaneNode;
typedef struct Tab Tab;

typedef struct {
    TerminalState *ts;
    Autocomplete *ac;
} CwdCbCtx;

struct PaneNode {
    int is_split;
    Pane *pane;   /* leaf: non-NULL */
    int vertical; /* 1 = left|right, 0 = top|bottom */
    float ratio;
    PaneNode *first;
    PaneNode *second;
    PaneNode *parent;
    int x_px, y_px, w_px, h_px;
};

struct Tab {
    PaneNode *root;
    Pane *active;
};

struct Pane {
    Terminal *term;
    Pty *pty;
    TerminalState *ts;
    Autocomplete *ac;
    int cols, rows;
    int tui_active;
    char cmd_buf[CMD_BUF_SIZE];
    int cmd_len;
    int sel_dragging;
    int sel_anchor_col, sel_anchor_vrow; /* virtual row */
    int sb_dragging;
    int sb_drag_anchor_y;   /* pixel y at scrollbar drag start */
    int sb_drag_anchor_off; /* scroll_offset at drag start */
    CwdCbCtx cwd_ctx;
    char title[TAB_TITLE_MAX * 4 + 4];
    pid_t osc_title_pid;
    int x_px, y_px, w_px, h_px;
    PaneNode *node;
    Tab *tab;
};

static Tab g_tabs[MAX_TABS];
static int g_n_tabs = 0;
static int g_active = 0;
static float g_dpi_scale = 1.0f;

static int px_to_cells(int px, int cell_sz, int fallback) {
    if (cell_sz <= 0)
        return fallback;
    int n = px / cell_sz;
    return (n < 1) ? 1 : n;
}

static float dpi_scale(void) {
    int pt_w, px_w;
    SDL_GetWindowSize(g_window, &pt_w, NULL);
    SDL_GetWindowSizeInPixels(g_window, &px_w, NULL);
    return (pt_w > 0) ? (float)px_w / (float)pt_w : 1.0f;
}

static int tab_bar_h(void) { return (g_n_tabs > 1) ? g_cell_h : 0; }

#define HISTORY_PATH_LEN 512
static const char *history_path(void) {
    static char buf[HISTORY_PATH_LEN];
    const char *home = getenv("HOME");
    if (!home)
        return NULL;
    snprintf(buf, sizeof buf, "%s/.phantom_history", home);
    return buf;
}

#define MODEL_PATH_LEN 1024
static const char *resolve_model_path(const char *cfg) {
    static char bufs[2][MODEL_PATH_LEN];
    static int slot = 0;
    char *buf = bufs[slot++ & 1];
    if (!cfg || '\0' == cfg[0])
        return cfg;
    const char *home = getenv("HOME");
    if (home) {
        const char *base = strrchr(cfg, '/');
        const char *name = base ? base + 1 : cfg;
        snprintf(buf, MODEL_PATH_LEN,
                 "%s/Library/Application Support/phantom/models/%s", home,
                 name);
        if (0 == access(buf, F_OK))
            return buf;
    }
    return cfg;
}

static void layout_node(PaneNode *n, int x, int y, int w, int h);

static Pane *pane_first_leaf(PaneNode *n) {
    while (n->is_split)
        n = n->first;
    return n->pane;
}

static int pane_count(PaneNode *n) {
    if (!n->is_split)
        return 1;
    return pane_count(n->first) + pane_count(n->second);
}

static void collect_panes(PaneNode *n, Pane **out, int *cnt) {
    if (!n->is_split) {
        if (*cnt < MAX_PANES)
            out[(*cnt)++] = n->pane;
        return;
    }
    collect_panes(n->first, out, cnt);
    collect_panes(n->second, out, cnt);
}

static Pane *pane_find_at(PaneNode *n, int x, int y) {
    if (x < n->x_px || x >= n->x_px + n->w_px)
        return NULL;
    if (y < n->y_px || y >= n->y_px + n->h_px)
        return NULL;
    if (!n->is_split)
        return n->pane;
    Pane *p = pane_find_at(n->first, x, y);
    if (p)
        return p;
    return pane_find_at(n->second, x, y);
}

static void layout_node(PaneNode *n, int x, int y, int w, int h) {
    n->x_px = x;
    n->y_px = y;
    n->w_px = w;
    n->h_px = h;
    if (!n->is_split) {
        Pane *p = n->pane;
        p->x_px = x;
        p->y_px = y;
        p->w_px = w;
        p->h_px = h;
        int nc = px_to_cells(w, g_cell_w, TERM_COLS_FALLBACK);
        int nr = px_to_cells(h, g_cell_h, TERM_ROWS_FALLBACK);
        if (nc != p->cols || nr != p->rows) {
            p->cols = nc;
            p->rows = nr;
            terminal_resize(p->term, nc, nr);
            pty_resize(p->pty, nc, nr);
        }
        return;
    }
    if (n->vertical) {
        int w1 = (int)(w * n->ratio);
        int w2 = w - w1 - DIVIDER_PX;
        if (w1 < 1)
            w1 = 1;
        if (w2 < 1)
            w2 = 1;
        layout_node(n->first, x, y, w1, h);
        layout_node(n->second, x + w1 + DIVIDER_PX, y, w2, h);
    } else {
        int h1 = (int)(h * n->ratio);
        int h2 = h - h1 - DIVIDER_PX;
        if (h1 < 1)
            h1 = 1;
        if (h2 < 1)
            h2 = 1;
        layout_node(n->first, x, y, w, h1);
        layout_node(n->second, x, y + h1 + DIVIDER_PX, w, h2);
    }
}

static void layout_all_tabs(int px_w, int px_h) {
    int tbh = tab_bar_h();
    for (int i = 0; i < g_n_tabs; i++)
        layout_node(g_tabs[i].root, 0, 0, px_w, px_h - tbh);
}

/* Returns 1 if any scrollbar fade is in progress. */
static int draw_panes(Renderer *r, PaneNode *n, Pane *active, SearchState *s) {
    if (!n->is_split) {
        Pane *p = n->pane;
        renderer_draw(r, p->term, p == active ? s : NULL, p->x_px, p->y_px,
                      p->w_px, p->h_px, p == active ? 1 : 0);
        int fading = renderer_draw_scrollbar(r, p->term, p->x_px, p->y_px,
                                             p->w_px, p->h_px);
        if (p == active)
            renderer_draw_autocomplete_overlay(r, p->ac, p->term, p->x_px,
                                               p->y_px);
        return fading;
    }
    int f1 = draw_panes(r, n->first, active, s);
    int f2 = draw_panes(r, n->second, active, s);
    return f1 | f2;
}

static void draw_dividers(Renderer *r, PaneNode *n) {
    if (!n->is_split)
        return;
    draw_dividers(r, n->first);
    draw_dividers(r, n->second);
    if (n->vertical) {
        int dx = n->first->x_px + n->first->w_px;
        renderer_fill_rect(r, dx, n->y_px, DIVIDER_PX, n->h_px, DIV_COLOR);
    } else {
        int dy = n->first->y_px + n->first->h_px;
        renderer_fill_rect(r, n->x_px, dy, n->w_px, DIVIDER_PX, DIV_COLOR);
    }
}

static void pane_focus_dir(Tab *t, int delta) {
    Pane *arr[MAX_PANES];
    int n = 0;
    collect_panes(t->root, arr, &n);
    for (int i = 0; i < n; i++) {
        if (arr[i] == t->active) {
            t->active = arr[(i + n + delta) % n];
            return;
        }
    }
}

static void osc7_cwd_handler(const char *path, void *arg) {
    CwdCbCtx *ctx = arg;
    terminal_state_update_cwd(ctx->ts, path);
    autocomplete_request_env_probe(ctx->ac);
}

static void title_handler(const char *title, void *arg) {
    Pane *p = arg;
    const unsigned char *s = (const unsigned char *)title;
    int chars = 0, bytes = 0;
    while (*s && chars < TAB_TITLE_MAX) {
        const unsigned char *prev = s;
        utf8_decode(&s);
        bytes += (int)(s - prev);
        chars++;
    }
    memcpy(p->title, title, (size_t)bytes);
    p->title[bytes] = '\0';
    p->osc_title_pid =
        (p->pty && p->pty->master_fd >= 0) ? tcgetpgrp(p->pty->master_fd) : 0;
}

static void poll_fg_title(Pane *p) {
    if (!p || !p->pty || p->pty->master_fd < 0)
        return;
    pid_t fg = tcgetpgrp(p->pty->master_fd);
    if (fg <= 0)
        return;
    if (fg == p->osc_title_pid)
        return;
    p->osc_title_pid = 0;
    char name[64] = {'\0'};
#if defined(__APPLE__)
    proc_name((int)fg, name, sizeof name);
#else
    {
        char path[64];
        snprintf(path, sizeof path, "/proc/%d/comm", (int)fg);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, name, sizeof name - 1);
            close(fd);
            if (n > 0) {
                name[n] = '\0';
                if (n > 1 && '\n' == name[n - 1])
                    name[n - 1] = '\0';
            }
        }
    }
#endif
    if (name[0])
        snprintf(p->title, sizeof p->title, "%s", name);
}

static void pty_on_data(void *arg) {
    Pane *p = arg;
    if (p->tab != &g_tabs[g_active])
        return;
    SDL_Event e;
    SDL_zero(e);
    e.type = g_event_pty_data;
    e.user.data1 = p;
    SDL_PushEvent(&e);
}

static void pty_on_exit(void *arg) {
    SDL_Event e;
    SDL_zero(e);
    e.type = g_event_pty_exit;
    e.user.data1 = arg;
    SDL_PushEvent(&e);
}

static void alt_screen_handler(void *arg, int entering) {
    SDL_Event e;
    SDL_zero(e);
    e.type = entering ? g_event_tui_enter : g_event_tui_exit;
    e.user.data1 = arg;
    SDL_PushEvent(&e);
}

static Pane *pane_create(Tab *t, int cols, int rows) {
    Pane *p = calloc(1, sizeof(Pane));
    if (!p)
        return NULL;
    p->tab = t;
    p->cols = cols;
    p->rows = rows;

    p->term = terminal_create(cols, rows);
    if (!p->term)
        goto fail;

    p->ts = terminal_state_create(p->term);
    if (!p->ts)
        goto fail;

    p->ac = autocomplete_create(g_history, g_llm_base, g_llm_instruct, p->ts,
                                p->term, g_ccg);
    if (!p->ac)
        goto fail;

    p->cwd_ctx.ts = p->ts;
    p->cwd_ctx.ac = p->ac;
    terminal_set_alt_screen_callback(p->term, alt_screen_handler, p);
    terminal_set_cwd_callback(p->term, osc7_cwd_handler, &p->cwd_ctx);
    terminal_set_title_callback(p->term, title_handler, p);

    {
        const char *shell = getenv("SHELL");
        if (!shell || '\0' == shell[0])
            shell = "/bin/sh";
        const char *base = strrchr(shell, '/');
        snprintf(p->title, sizeof p->title, "%s", base ? base + 1 : shell);
    }

    p->pty = calloc(1, sizeof(Pty));
    if (!p->pty || 0 != pty_open(p->pty, cols, rows))
        goto fail_pty;
    p->pty->on_data = pty_on_data;
    p->pty->on_exit = pty_on_exit;
    p->pty->callback_arg = p;
    terminal_state_set_shell_pid(p->ts, p->pty->child_pid);
    pty_start_reader(p->pty, p->term);
    return p;

fail_pty:
    free(p->pty);
    p->pty = NULL;
fail:
    if (p->ac)
        autocomplete_destroy(p->ac);
    if (p->ts)
        terminal_state_destroy(p->ts);
    if (p->term)
        terminal_destroy(p->term);
    free(p);
    return NULL;
}

static void pane_destroy(Pane *p) {
    if (!p)
        return;
    if (search_is_active(g_search) && p->tab && p == p->tab->active)
        search_close(g_search, p->term);
    pty_close(p->pty);
    free(p->pty);
    autocomplete_destroy(p->ac);
    terminal_state_destroy(p->ts);
    terminal_destroy(p->term);
    free(p);
}

static PaneNode *panenode_new_leaf(Pane *p) {
    PaneNode *n = calloc(1, sizeof(PaneNode));
    if (!n)
        return NULL;
    n->pane = p;
    p->node = n;
    return n;
}

static void panenode_destroy_tree(PaneNode *n) {
    if (!n)
        return;
    if (n->is_split) {
        panenode_destroy_tree(n->first);
        panenode_destroy_tree(n->second);
    } else {
        pane_destroy(n->pane);
    }
    free(n);
}

/* Fix p->tab pointers after memmove of g_tabs[]. Call for all shifted
 * entries (idx .. g_n_tabs-1) after compacting the array. */
static void fix_pane_tab_ptrs(PaneNode *n, Tab *t) {
    if (!n->is_split) {
        n->pane->tab = t;
        return;
    }
    fix_pane_tab_ptrs(n->first, t);
    fix_pane_tab_ptrs(n->second, t);
}

/* Remove pane p from tab t's tree; re-layout; set new active if needed.
 * Precondition: pane_count(t->root) >= 2. */
static void pane_remove(Tab *t, Pane *p) {
    PaneNode *node = p->node;
    PaneNode *parent = node->parent;
    if (!parent)
        return; /* only pane; caller must close tab */

    PaneNode *sibling =
        (parent->first == node) ? parent->second : parent->first;
    PaneNode *grandparent = parent->parent;
    sibling->parent = grandparent;
    if (!grandparent) {
        t->root = sibling;
    } else {
        if (grandparent->first == parent)
            grandparent->first = sibling;
        else
            grandparent->second = sibling;
    }

    if (t->active == p)
        t->active = pane_first_leaf(sibling);

    pane_destroy(p);
    free(node);
    free(parent);

    int px_w, px_h;
    SDL_GetWindowSizeInPixels(g_window, &px_w, &px_h);
    layout_node(t->root, 0, 0, px_w, px_h - tab_bar_h());
}

static void pane_close_active(Tab *t) {
    if (pane_count(t->root) <= 1)
        return;
    pane_remove(t, t->active);
}

/* Split t->active (vertical=1: left|right, 0: top|bottom).
 * Returns new pane or NULL on alloc failure. */
static Pane *pane_split(Tab *t, int vertical) {
    Pane *p = t->active;
    PaneNode *old_leaf = p->node;
    PaneNode *parent = old_leaf->parent;

    Pane *np = pane_create(t, p->cols, p->rows);
    if (!np)
        return NULL;

    PaneNode *new_leaf = panenode_new_leaf(np);
    if (!new_leaf) {
        pane_destroy(np);
        return NULL;
    }

    PaneNode *split = calloc(1, sizeof(PaneNode));
    if (!split) {
        pane_destroy(np);
        free(new_leaf);
        return NULL;
    }

    split->is_split = 1;
    split->vertical = vertical;
    split->ratio = 0.5f;
    split->first = old_leaf;
    split->second = new_leaf;
    split->parent = parent;
    old_leaf->parent = split;
    new_leaf->parent = split;

    if (!parent) {
        t->root = split;
    } else {
        if (parent->first == old_leaf)
            parent->first = split;
        else
            parent->second = split;
    }
    t->active = np;

    int px_w, px_h;
    SDL_GetWindowSizeInPixels(g_window, &px_w, &px_h);
    layout_node(t->root, 0, 0, px_w, px_h - tab_bar_h());
    return np;
}

static int tab_new(void) {
    if (g_n_tabs >= MAX_TABS)
        return -1;
    Tab *t = &g_tabs[g_n_tabs];
    memset(t, 0, sizeof *t);

    int px_w, px_h;
    SDL_GetWindowSizeInPixels(g_window, &px_w, &px_h);
    /* Tab bar appears once n_tabs reaches 2, so compute with final count. */
    int tbh = (g_n_tabs >= 1) ? g_cell_h : 0;
    int cols = px_to_cells(px_w, g_cell_w, TERM_COLS_FALLBACK);
    int rows = px_to_cells(px_h - tbh, g_cell_h, TERM_ROWS_FALLBACK);

    Pane *p = pane_create(t, cols, rows);
    if (!p)
        return -1;
    PaneNode *leaf = panenode_new_leaf(p);
    if (!leaf) {
        pane_destroy(p);
        return -1;
    }

    t->root = leaf;
    t->active = p;
    g_n_tabs++;

    /* If tab bar just appeared or disappeared, re-layout all tabs. */
    layout_all_tabs(px_w, px_h);
    return g_n_tabs - 1;
}

static void tab_close(int idx) {
    if (g_n_tabs <= 1 || idx < 0 || idx >= g_n_tabs)
        return;
    panenode_destroy_tree(g_tabs[idx].root);

    int n_shift = g_n_tabs - idx - 1;
    if (n_shift > 0)
        memmove(&g_tabs[idx], &g_tabs[idx + 1], (size_t)n_shift * sizeof(Tab));
    g_n_tabs--;

    if (g_active > idx)
        g_active--;
    else if (g_active >= g_n_tabs)
        g_active = g_n_tabs - 1;

    /* Pane->tab pointers shift with the array; re-point them. */
    for (int i = idx; i < g_n_tabs; i++)
        fix_pane_tab_ptrs(g_tabs[i].root, &g_tabs[i]);

    int px_w, px_h;
    SDL_GetWindowSizeInPixels(g_window, &px_w, &px_h);
    layout_all_tabs(px_w, px_h);
}

static void tab_switch(int idx) {
    if (idx < 0 || idx >= g_n_tabs || idx == g_active)
        return;
    Tab *old = &g_tabs[g_active];
    if (old->active) {
        autocomplete_clear(old->active->ac);
        if (search_is_active(g_search))
            search_close(g_search, old->active->term);
    }
    g_active = idx;
    poll_fg_title(g_tabs[idx].active);
}

#define PASTE_START "\x1b[200~"
#define PASTE_END "\x1b[201~"

int main(void) {
#if defined(__APPLE__)
    platform_disable_press_and_hold();
#endif
#if defined(__APPLE__)
    g_event_pty_data = SDL_RegisterEvents(6);
#else
    g_event_pty_data = SDL_RegisterEvents(5);
#endif
    g_event_pty_exit = g_event_pty_data + 1;
    g_event_tui_enter = g_event_pty_data + 2;
    g_event_tui_exit = g_event_pty_data + 3;
    g_event_ac_ready = g_event_pty_data + 4;
#if defined(__APPLE__)
    g_event_macos_menu = g_event_pty_data + 5;
#endif

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);

    g_window = SDL_CreateWindow("phantom", WIN_INITIAL_W, WIN_INITIAL_H,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                    SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_StartTextInput(g_window);

    Renderer *renderer = renderer_create(g_window);
    if (!renderer) {
        fprintf(stderr, "renderer_create failed\n");
        SDL_Quit();
        return 1;
    }
    renderer_get_cell_size(renderer, &g_cell_w, &g_cell_h);

#if defined(__APPLE__)
    macos_set_menu_event_type(g_event_macos_menu);
    macos_window_init(g_window, TERM_DEFAULT_BG);
    macos_add_menu_bar();
#endif

    g_history = history_open(history_path());
    g_llm_base = llm_create(resolve_model_path(LLM_BASE_MODEL_PATH));
    g_llm_instruct = llm_create(resolve_model_path(LLM_INSTRUCT_MODEL_PATH));
    g_ccg = ccg_create();
    g_search = search_create();
    autocomplete_set_event_type(g_event_ac_ready);
    search_set_event_type(g_event_ac_ready);

    if (-1 == tab_new()) {
        fprintf(stderr, "tab_new failed\n");
        return 1;
    }
    terminal_feed(g_tabs[0].active->term, "Loading shell...",
                  sizeof("Loading shell...") - 1);

    SDL_Event event;
    int running = 1;
    int needs_render = 1;
    int scrollbar_fading = 0;

    while (running) {
        int got_event = scrollbar_fading ? SDL_WaitEventTimeout(&event, 16)
                                         : SDL_WaitEvent(&event);
        if (scrollbar_fading)
            needs_render = 1;
        if (got_event) {
            do {
                if (SDL_EVENT_QUIT == event.type ||
                    SDL_EVENT_WINDOW_CLOSE_REQUESTED == event.type) {
                    running = 0;
                    break;
                }

                if (event.type == g_event_pty_exit) {
                    Pane *p = event.user.data1;
                    Tab *t = p->tab;
                    int ti = (int)(t - g_tabs);
                    if (pane_count(t->root) <= 1) {
                        if (g_n_tabs <= 1) {
                            running = 0;
                        } else {
                            tab_close(ti);
                            needs_render = 1;
                        }
                    } else {
                        if (t->active == p)
                            pane_focus_dir(t, 1);
                        pane_remove(t, p);
                        needs_render = 1;
                    }
                    break;
                }

                Tab *at = &g_tabs[g_active];
                Pane *ap = at->active;

                if (event.type == g_event_pty_data) {
                    needs_render = 1;
                } else if (event.type == g_event_ac_ready) {
                    needs_render = 1;
#if defined(__APPLE__)
                } else if (event.type == g_event_macos_menu) {
                    switch (event.user.code) {
                    case MACOS_MENU_COPY: {
                        char *sel = terminal_get_selected_text(ap->term);
                        if (sel && *sel)
                            SDL_SetClipboardText(sel);
                        free(sel);
                        break;
                    }
                    case MACOS_MENU_PASTE: {
                        char *clip = SDL_GetClipboardText();
                        if (clip && *clip) {
                            terminal_scroll_bottom(ap->term);
                            if (terminal_bracketed_paste(ap->term))
                                pty_write(ap->pty, PASTE_START,
                                          sizeof(PASTE_START) - 1);
                            pty_write(ap->pty, clip, strlen(clip));
                            if (terminal_bracketed_paste(ap->term))
                                pty_write(ap->pty, PASTE_END,
                                          sizeof(PASTE_END) - 1);
                        }
                        SDL_free(clip);
                        break;
                    }
                    case MACOS_MENU_SELECT_ALL: {
                        int so = terminal_scroll_offset(ap->term);
                        terminal_set_selection(ap->term, 0, -so, ap->cols - 1,
                                               ap->rows - 1 - so);
                        break;
                    }
                    case MACOS_MENU_NEW_TAB:
                        if (-1 != tab_new())
                            tab_switch(g_n_tabs - 1);
                        break;
                    case MACOS_MENU_CLOSE_TAB:
                        tab_close(g_active);
                        break;
                    case MACOS_MENU_CLEAR_SCROLLBACK:
                        terminal_clear_scrollback(ap->term);
                        break;
                    case MACOS_MENU_FIND:
                        if (!search_is_active(g_search))
                            search_open(g_search, ap->term);
                        break;
                    case MACOS_MENU_SPLIT_RIGHT:
                        pane_split(at, 1);
                        break;
                    case MACOS_MENU_SPLIT_DOWN:
                        pane_split(at, 0);
                        break;
                    case MACOS_MENU_CLOSE_PANE:
                        pane_close_active(at);
                        break;
                    case MACOS_MENU_FOCUS_PREV_PANE:
                        pane_focus_dir(at, -1);
                        break;
                    case MACOS_MENU_FOCUS_NEXT_PANE:
                        pane_focus_dir(at, 1);
                        break;
                    }
                    needs_render = 1;
#endif
                } else if (event.type == g_event_tui_enter ||
                           event.type == g_event_tui_exit) {
                    Pane *p = event.user.data1;
                    p->tui_active = (event.type == g_event_tui_enter) ? 1 : 0;
                    if (p->tab == &g_tabs[g_active]) {
                        if (p->tui_active)
                            autocomplete_clear(p->ac);
                        int nc =
                            px_to_cells(p->w_px, g_cell_w, TERM_COLS_FALLBACK);
                        int nr =
                            px_to_cells(p->h_px, g_cell_h, TERM_ROWS_FALLBACK);
                        if (nc != p->cols || nr != p->rows) {
                            p->cols = nc;
                            p->rows = nr;
                            terminal_resize(p->term, nc, nr);
                            pty_resize(p->pty, nc, nr);
                        }
                        needs_render = 1;
                    }
                } else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                    g_dpi_scale = dpi_scale();
                    renderer_notify_resize(renderer);
                    layout_all_tabs(event.window.data1, event.window.data2);
                    needs_render = 1;
                } else if (event.type == SDL_EVENT_WINDOW_RESIZED) {
                    needs_render = 1;
                } else if (SDL_EVENT_MOUSE_BUTTON_DOWN == event.type &&
                           SDL_BUTTON_RIGHT == event.button.button) {
#if defined(__APPLE__)
                    char *sel = terminal_get_selected_text(ap->term);
                    int has_sel = (sel && *sel) ? 1 : 0;
                    free(sel);
                    macos_show_context_menu(g_window, (int)event.button.x,
                                            (int)event.button.y, has_sel,
                                            g_n_tabs, pane_count(at->root));
                    needs_render = 1;
#endif
                } else if (SDL_EVENT_MOUSE_BUTTON_DOWN == event.type &&
                           SDL_BUTTON_LEFT == event.button.button) {
                    int mx = (int)(event.button.x * g_dpi_scale);
                    int my = (int)(event.button.y * g_dpi_scale);

                    int px_w, px_h;
                    SDL_GetWindowSizeInPixels(g_window, &px_w, &px_h);
                    if (g_n_tabs > 1 && my >= px_h - g_cell_h) {
                        int slot_px =
                            (g_cell_w > 0)
                                ? (px_w / g_n_tabs / g_cell_w) * g_cell_w
                                : 1;
                        if (slot_px < 1)
                            slot_px = 1;
                        int ti = mx / slot_px;
                        if (ti >= 0 && ti < g_n_tabs) {
                            tab_switch(ti);
                            needs_render = 1;
                        }
                        continue;
                    }

                    Pane *clicked = pane_find_at(at->root, mx, my);
                    if (clicked && clicked != at->active) {
                        at->active = clicked;
                        ap = clicked;
                    }

                    /* Start scrollbar drag if click lands on the pill strip. */
                    if (clicked && terminal_sb_total_rows(clicked->term) > 0 &&
                        renderer_scrollbar_hit(clicked->x_px, clicked->y_px,
                                               clicked->w_px, clicked->h_px, mx,
                                               my)) {
                        clicked->sb_dragging = 1;
                        clicked->sb_drag_anchor_y = my;
                        clicked->sb_drag_anchor_off =
                            terminal_scroll_offset(clicked->term);
                    } else if (clicked) {
                        int col = (g_cell_w > 0)
                                      ? (mx - clicked->x_px) / g_cell_w
                                      : 0;
                        int row = (g_cell_h > 0)
                                      ? (my - clicked->y_px) / g_cell_h
                                      : 0;
                        if (col < 0)
                            col = 0;
                        if (col >= clicked->cols)
                            col = clicked->cols - 1;
                        if (row < 0)
                            row = 0;
                        if (row >= clicked->rows)
                            row = clicked->rows - 1;
                        clicked->sel_anchor_col = col;
                        /* Store anchor as virtual row (grid-relative; negative
                         * = scrollback) so selection stays stable on scroll. */
                        clicked->sel_anchor_vrow =
                            row - terminal_scroll_offset(clicked->term);
                        clicked->sel_dragging = 1;
                        terminal_clear_selection(clicked->term);
                    }
                    needs_render = 1;
                } else if (SDL_EVENT_MOUSE_MOTION == event.type) {
                    int mx = (int)(event.motion.x * g_dpi_scale);
                    int my = (int)(event.motion.y * g_dpi_scale);
                    if (ap->sb_dragging) {
                        int new_off = renderer_scrollbar_drag_offset(
                            ap->term, ap->h_px, ap->sb_drag_anchor_y,
                            ap->sb_drag_anchor_off, my);
                        int cur_off = terminal_scroll_offset(ap->term);
                        terminal_scroll(ap->term, new_off - cur_off);
                        needs_render = 1;
                    } else if (ap->sel_dragging) {
                        int cur_col =
                            (g_cell_w > 0) ? (mx - ap->x_px) / g_cell_w : 0;
                        int cur_row =
                            (g_cell_h > 0) ? (my - ap->y_px) / g_cell_h : 0;
                        if (cur_col < 0)
                            cur_col = 0;
                        if (cur_col >= ap->cols)
                            cur_col = ap->cols - 1;
                        if (cur_row < 0)
                            cur_row = 0;
                        if (cur_row >= ap->rows)
                            cur_row = ap->rows - 1;
                        int cur_vrow =
                            cur_row - terminal_scroll_offset(ap->term);
                        terminal_set_selection(ap->term, ap->sel_anchor_col,
                                               ap->sel_anchor_vrow, cur_col,
                                               cur_vrow);
                        needs_render = 1;
                    }
                } else if (SDL_EVENT_MOUSE_BUTTON_UP == event.type &&
                           SDL_BUTTON_LEFT == event.button.button) {
                    if (ap->sb_dragging) {
                        ap->sb_dragging = 0;
                        needs_render = 1;
                    }
                    if (ap->sel_dragging) {
                        ap->sel_dragging = 0;
                        char *text = terminal_get_selected_text(ap->term);
                        if (text && *text)
                            SDL_SetClipboardText(text);
                        free(text);
                        needs_render = 1;
                    }
                } else {
                    if (SDL_EVENT_KEY_DOWN == event.type) {
                        SDL_Keymod mod = event.key.mod;
                        SDL_Keycode sym = event.key.key;

                        if ((mod & SDL_KMOD_GUI) && 'f' == sym &&
                            !search_is_active(g_search)) {
                            search_open(g_search, ap->term);
                            needs_render = 1;
                            continue;
                        }
                        if ((mod & SDL_KMOD_GUI) && 't' == sym) {
                            if (-1 != tab_new()) {
                                tab_switch(g_n_tabs - 1);
                                needs_render = 1;
                            }
                            continue;
                        }
                        /* Cmd+W closes pane if split, tab if last pane. */
                        if ((mod & SDL_KMOD_GUI) && 'w' == sym) {
                            if (pane_count(at->root) > 1) {
                                pane_close_active(at);
                            } else {
                                tab_close(g_active);
                            }
                            needs_render = 1;
                            continue;
                        }
                        if ((mod & SDL_KMOD_GUI) && !(mod & SDL_KMOD_SHIFT) &&
                            'd' == sym) {
                            pane_split(at, 1);
                            needs_render = 1;
                            continue;
                        }
                        if ((mod & SDL_KMOD_GUI) && (mod & SDL_KMOD_SHIFT) &&
                            'd' == sym) {
                            pane_split(at, 0);
                            needs_render = 1;
                            continue;
                        }
                        if ((mod & SDL_KMOD_GUI) && '[' == sym) {
                            pane_focus_dir(at, -1);
                            needs_render = 1;
                            continue;
                        }
                        if ((mod & SDL_KMOD_GUI) && ']' == sym) {
                            pane_focus_dir(at, 1);
                            needs_render = 1;
                            continue;
                        }
                        if ((mod & SDL_KMOD_GUI) && sym >= '1' && sym <= '9') {
                            tab_switch((int)(sym - '1'));
                            needs_render = 1;
                            continue;
                        }
                    }

                    if (search_is_active(g_search)) {
                        int consumed = 0;
                        if (SDL_EVENT_KEY_DOWN == event.type)
                            consumed = search_handle_key(g_search, ap->term,
                                                         &event.key);
                        else if (SDL_EVENT_TEXT_INPUT == event.type)
                            consumed = search_handle_text(g_search, ap->term,
                                                          &event.text);
                        if (consumed) {
                            needs_render = 1;
                            continue;
                        }
                    }

                    if (SDL_EVENT_KEY_DOWN == event.type &&
                        !search_is_active(g_search)) {
                        SDL_Keycode sym = event.key.key;
                        if (SDLK_BACKSPACE == sym) {
                            if (ap->cmd_len > 0) {
                                int i =
                                    utf8_prev_start(ap->cmd_buf, ap->cmd_len);
                                ap->cmd_len = i;
                                ap->cmd_buf[i] = '\0';
                            }
                            if (!ap->tui_active)
                                autocomplete_query(ap->ac, ap->cmd_buf);
                        } else if (SDLK_RETURN == sym || SDLK_RETURN2 == sym ||
                                   SDLK_KP_ENTER == sym) {
                            if (ap->cmd_len > 0) {
                                history_append(g_history, ap->cmd_buf);
                                autocomplete_record_command(
                                    ap->ac, ap->cmd_buf,
                                    terminal_exit_code(ap->term));
                            }
                            ap->cmd_len = 0;
                            ap->cmd_buf[0] = '\0';
                            autocomplete_clear(ap->ac);
                        } else if (SDLK_ESCAPE == sym) {
                            autocomplete_clear(ap->ac);
                        } else if (SDLK_TAB == sym) {
                            const char *sug =
                                autocomplete_get_suggestion(ap->ac);
                            if (sug && *sug) {
                                size_t slen = strlen(sug);
                                const char *cp = ap->cmd_buf;
                                while (' ' == *cp || '\t' == *cp)
                                    cp++;
                                int is_nl = ('#' == *cp);
                                int is_multiline = (NULL != strchr(sug, '\n'));
                                if (is_nl) {
                                    const char ctrl_u = '\x15';
                                    pty_write(ap->pty, &ctrl_u, 1);
                                }
                                if (is_multiline) {
                                    pty_write(ap->pty, PASTE_START,
                                              sizeof(PASTE_START) - 1);
                                    pty_write(ap->pty, sug, slen);
                                    pty_write(ap->pty, PASTE_END,
                                              sizeof(PASTE_END) - 1);
                                    ap->cmd_len = 0;
                                    ap->cmd_buf[0] = '\0';
                                } else {
                                    pty_write(ap->pty, sug, slen);
                                    if (is_nl) {
                                        if ((int)slen + 1 < CMD_BUF_SIZE) {
                                            memcpy(ap->cmd_buf, sug, slen);
                                            ap->cmd_len = (int)slen;
                                            ap->cmd_buf[ap->cmd_len] = '\0';
                                        } else {
                                            ap->cmd_len = 0;
                                            ap->cmd_buf[0] = '\0';
                                        }
                                    } else if (ap->cmd_len + (int)slen + 1 <
                                               CMD_BUF_SIZE) {
                                        memcpy(ap->cmd_buf + ap->cmd_len, sug,
                                               slen);
                                        ap->cmd_len += (int)slen;
                                        ap->cmd_buf[ap->cmd_len] = '\0';
                                    }
                                }
                                autocomplete_clear(ap->ac);
                                needs_render = 1;
                                continue;
                            }
                        }
                    } else if (SDL_EVENT_TEXT_INPUT == event.type &&
                               !search_is_active(g_search)) {
                        const char *text = event.text.text;
                        int tlen = (int)strlen(text);
                        if (ap->cmd_len + tlen + 1 < CMD_BUF_SIZE) {
                            memcpy(ap->cmd_buf + ap->cmd_len, text,
                                   (size_t)tlen);
                            ap->cmd_len += tlen;
                            ap->cmd_buf[ap->cmd_len] = '\0';
                        }
                        if (!ap->tui_active)
                            autocomplete_query(ap->ac, ap->cmd_buf);
                    }
                    input_handle_event(&event, ap->term, ap->pty);
                    needs_render = 1;
                }
            } while (SDL_PollEvent(&event));
        }

        if (!running)
            break;
        if (needs_render) {
            Tab *at = &g_tabs[g_active];
            Pane *ap = at->active;
            poll_fg_title(ap);
            renderer_frame_begin(renderer);
            scrollbar_fading = draw_panes(renderer, at->root, ap, g_search);
            draw_dividers(renderer, at->root);
            renderer_draw_search_overlay(renderer, g_search, ap->term);
            const char *titles[MAX_TABS];
            for (int i = 0; i < g_n_tabs; i++)
                titles[i] = g_tabs[i].active ? g_tabs[i].active->title : "";
            renderer_draw_tab_bar(renderer, g_n_tabs, g_active, titles);
            SDL_GL_SwapWindow(g_window);
            needs_render = 0;
        }
    }

    /* Hide the window first so the UI disappears immediately.
     * The slow teardown below (LLM context free, thread joins) is then
     * invisible to the user. */
    SDL_HideWindow(g_window);
    SDL_StopTextInput(g_window);

    for (int i = 0; i < g_n_tabs; i++)
        panenode_destroy_tree(g_tabs[i].root);
    ccg_destroy(g_ccg);
    llm_destroy(g_llm_base);
    llm_destroy(g_llm_instruct);
    llm_backend_teardown();
    history_close(g_history);
    search_destroy(g_search);
    renderer_destroy(renderer);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
}
