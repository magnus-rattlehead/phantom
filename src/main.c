#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pty.h"
#include "terminal.h"
#include "render.h"
#include "input.h"
#include "search.h"
#include "ml/autocomplete.h"
#include "ml/ccg.h"
#include "ml/context.h"
#include "ml/history.h"
#include "ml/llm.h"
#include "config.h"
#include "utf8.h"

#if defined(__APPLE__)
#include <libproc.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
/* Defined in platform_macos.m  -  disables the "Press and Hold" accent picker
 * so held keys repeat normally, matching every other terminal emulator. */
void platform_disable_press_and_hold(void);
#endif

/* SDL user event types (registered at startup) */
static Uint32 g_event_pty_data;
static Uint32 g_event_pty_exit;
static Uint32 g_event_tui_enter;
static Uint32 g_event_tui_exit;
static Uint32 g_event_ac_ready;

/* Shared singletons  -  one per application lifetime. */
static History     *g_history;
static LLM         *g_llm_base;
static LLM         *g_llm_instruct;
static CCG         *g_ccg;
static SearchState *g_search;
static SDL_Window  *g_window;
static int          g_cell_w, g_cell_h;

#define MAX_TABS     9
#define CMD_BUF_SIZE 4096

typedef struct {
    TerminalState *ts;
    Autocomplete  *ac;
} CwdCbCtx;

typedef struct {
    Terminal      *term;
    Pty           *pty;
    TerminalState *ts;
    Autocomplete  *ac;
    int            cols, rows;
    int            tui_active;
    char           cmd_buf[CMD_BUF_SIZE];
    int            cmd_len;
    int            sel_dragging;
    int            sel_anchor_col, sel_anchor_row;
    CwdCbCtx       cwd_ctx;
    char           title[TAB_TITLE_MAX * 4 + 4]; /* UTF-8, TAB_TITLE_MAX codepoints */
    pid_t          osc_title_pid; /* fg pgid that last set an OSC title; 0 = none */
} Tab;

static Tab  g_tabs[MAX_TABS];
static int  g_n_tabs = 0;
static int  g_active = 0;

/* Converts a pixel dimension to a grid-cell count, falling back to
 * `fallback` when cell_sz is zero (renderer not yet initialised). */
static int px_to_cells(int px, int cell_sz, int fallback)
{
    return (cell_sz > 0) ? px / cell_sz : fallback;
}

#define HISTORY_PATH_LEN 512
static const char *history_path(void)
{
    static char buf[HISTORY_PATH_LEN];
    const char *home = getenv("HOME");
    if (!home) return NULL;
    snprintf(buf, sizeof buf, "%s/.phantom_history", home);
    return buf;
}

static void osc7_cwd_handler(const char *path, void *arg)
{
    CwdCbCtx *ctx = arg;
    terminal_state_update_cwd(ctx->ts, path);
    autocomplete_request_env_probe(ctx->ac);
}

static void title_handler(const char *title, void *arg)
{
    int  idx = (int)(intptr_t)arg;
    Tab *t   = &g_tabs[idx];
    const unsigned char *s = (const unsigned char *)title;
    int chars = 0, bytes = 0;
    while (*s && chars < TAB_TITLE_MAX) {
        const unsigned char *prev = s;
        utf8_decode(&s);
        bytes += (int)(s - prev);
        chars++;
    }
    memcpy(t->title, title, (size_t)bytes);
    t->title[bytes] = '\0';
    /* Record fg pgid so poll_fg_title() won't clobber this OSC title. */
    t->osc_title_pid = (t->pty && t->pty->master_fd >= 0)
                       ? tcgetpgrp(t->pty->master_fd) : 0;
}

/* Called from main thread: refresh tab title from the OS foreground process.
 * Skips update when the same fg pgid already set an OSC title this cycle. */
static void poll_fg_title(Tab *t)
{
    if (!t->pty || t->pty->master_fd < 0) return;
    pid_t fg = tcgetpgrp(t->pty->master_fd);
    if (fg <= 0) return;
    if (fg == t->osc_title_pid) return; /* OSC title still current */
    t->osc_title_pid = 0;
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
                if (n > 1 && name[n - 1] == '\n') name[n - 1] = '\0';
            }
        }
    }
#endif
    if (name[0]) snprintf(t->title, sizeof t->title, "%s", name);
}

static void pty_on_data(void *arg)
{
    int tab_idx = (int)(intptr_t)arg;
    if (tab_idx != g_active) return;
    SDL_Event e;
    SDL_zero(e);
    e.type = g_event_pty_data;
    SDL_PushEvent(&e);
}

static void pty_on_exit(void *arg)
{
    SDL_Event e;
    SDL_zero(e);
    e.type       = g_event_pty_exit;
    e.user.data1 = arg;
    SDL_PushEvent(&e);
}

static void alt_screen_handler(void *arg, int entering)
{
    SDL_Event e;
    SDL_zero(e);
    e.type       = entering ? g_event_tui_enter : g_event_tui_exit;
    e.user.data1 = arg;
    SDL_PushEvent(&e);
}

static int tab_new(void)
{
    if (g_n_tabs >= MAX_TABS) return -1;
    int   idx = g_n_tabs;
    Tab  *t   = &g_tabs[idx];
    memset(t, 0, sizeof *t);

    int win_px_w, win_px_h;
    SDL_GetWindowSizeInPixels(g_window, &win_px_w, &win_px_h);
    t->cols = px_to_cells(win_px_w, g_cell_w, TERM_COLS_FALLBACK);
    t->rows = px_to_cells(win_px_h - g_cell_h, g_cell_h, TERM_ROWS_FALLBACK);

    t->term = terminal_create(t->cols, t->rows);
    if (!t->term) return -1;

    t->ts = terminal_state_create(t->term);
    if (!t->ts) { terminal_destroy(t->term); return -1; }

    t->ac = autocomplete_create(g_history, g_llm_base, g_llm_instruct,
                                t->ts, t->term, g_ccg);
    if (!t->ac) {
        terminal_state_destroy(t->ts);
        terminal_destroy(t->term);
        return -1;
    }

    t->cwd_ctx.ts = t->ts;
    t->cwd_ctx.ac = t->ac;
    terminal_set_alt_screen_callback(t->term, alt_screen_handler,
                                     (void *)(intptr_t)idx);
    terminal_set_cwd_callback(t->term, osc7_cwd_handler, &t->cwd_ctx);
    /* Default title: shell basename, updated later via OSC 0/2. */
    const char *shell = getenv("SHELL");
    if (!shell || '\0' == shell[0]) shell = "/bin/sh";
    const char *base  = strrchr(shell, '/');
    snprintf(t->title, sizeof t->title, "%s", base ? base + 1 : shell);
    terminal_set_title_callback(t->term, title_handler,
                                (void *)(intptr_t)idx);

    t->pty = calloc(1, sizeof(Pty));
    if (!t->pty || 0 != pty_open(t->pty, t->cols, t->rows)) {
        free(t->pty);
        autocomplete_destroy(t->ac);
        terminal_state_destroy(t->ts);
        terminal_destroy(t->term);
        return -1;
    }
    t->pty->on_data      = pty_on_data;
    t->pty->on_exit      = pty_on_exit;
    t->pty->callback_arg = (void *)(intptr_t)idx;
    terminal_state_set_shell_pid(t->ts, t->pty->child_pid);
    pty_start_reader(t->pty, t->term);

    g_n_tabs++;
    return idx;
}

/* Refuses when g_n_tabs == 1; compacts g_tabs[] and fixes callback args. */
static void tab_close(int idx)
{
    if (g_n_tabs <= 1 || idx < 0 || idx >= g_n_tabs) return;
    Tab *t = &g_tabs[idx];
    if (search_is_active(g_search))
        search_close(g_search, t->term);
    pty_close(t->pty);
    free(t->pty);
    autocomplete_destroy(t->ac);
    terminal_state_destroy(t->ts);
    terminal_destroy(t->term);

    int n_shift = g_n_tabs - idx - 1;
    if (n_shift > 0)
        memmove(&g_tabs[idx], &g_tabs[idx + 1],
                (size_t)n_shift * sizeof(Tab));
    g_n_tabs--;

    if (g_active > idx)
        g_active--;
    else if (g_active >= g_n_tabs)
        g_active = g_n_tabs - 1;

    /* Pointers to embedded CwdCbCtx may have shifted  -  re-register all. */
    for (int i = idx; i < g_n_tabs; i++) {
        Tab *s = &g_tabs[i];
        s->pty->callback_arg = (void *)(intptr_t)i;
        terminal_set_alt_screen_callback(s->term, alt_screen_handler,
                                         (void *)(intptr_t)i);
        terminal_set_cwd_callback(s->term, osc7_cwd_handler, &s->cwd_ctx);
        terminal_set_title_callback(s->term, title_handler,
                                    (void *)(intptr_t)i);
    }
}

static void tab_switch(int idx)
{
    if (idx < 0 || idx >= g_n_tabs || idx == g_active) return;
    Tab *old = &g_tabs[g_active];
    autocomplete_clear(old->ac);
    if (search_is_active(g_search))
        search_close(g_search, old->term);
    g_active = idx;
    poll_fg_title(&g_tabs[idx]);
}

int main(void)
{
#if defined(__APPLE__)
    platform_disable_press_and_hold();
#endif
    g_event_pty_data  = SDL_RegisterEvents(5);
    g_event_pty_exit  = g_event_pty_data + 1;
    g_event_tui_enter = g_event_pty_data + 2;
    g_event_tui_exit  = g_event_pty_data + 3;
    g_event_ac_ready  = g_event_pty_data + 4;

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

    g_history      = history_open(history_path());
    g_llm_base     = llm_create(LLM_BASE_MODEL_PATH);
    g_llm_instruct = llm_create(LLM_INSTRUCT_MODEL_PATH);
    g_ccg          = ccg_create();
    g_search       = search_create();
    autocomplete_set_event_type(g_event_ac_ready);
    search_set_event_type(g_event_ac_ready);

    if (-1 == tab_new()) {
        fprintf(stderr, "tab_new failed\n");
        return 1;
    }
    g_active = 0;
    terminal_feed(g_tabs[0].term,
                  "Loading shell...", sizeof("Loading shell...") - 1);

    SDL_Event event;
    int running      = 1;
    int needs_render = 1;

    while (running) {
        if (SDL_WaitEvent(&event)) {
        do {
            if (SDL_EVENT_QUIT == event.type ||
                SDL_EVENT_WINDOW_CLOSE_REQUESTED == event.type) {
                running = 0;
                break;
            }
            if (event.type == g_event_pty_exit) {
                int tab_idx = (int)(intptr_t)event.user.data1;
                if (g_n_tabs <= 1) {
                    running = 0;
                } else {
                    tab_close(tab_idx);
                    needs_render = 1;
                }
                break;
            }

            Tab *at = &g_tabs[g_active];

            if (event.type == g_event_pty_data) {
                needs_render = 1;
            } else if (event.type == g_event_ac_ready) {
                needs_render = 1;
            } else if (event.type == g_event_tui_enter
                       || event.type == g_event_tui_exit) {
                int tab_idx = (int)(intptr_t)event.user.data1;
                Tab *t      = &g_tabs[tab_idx];
                t->tui_active =
                    (event.type == g_event_tui_enter) ? 1 : 0;
                if (tab_idx == g_active) {
                    if (t->tui_active) autocomplete_clear(t->ac);
                    int px_w, px_h;
                    SDL_GetWindowSizeInPixels(g_window, &px_w, &px_h);
                    int nc = px_to_cells(px_w, g_cell_w, TERM_COLS_FALLBACK);
                    int nr = px_to_cells(px_h - g_cell_h, g_cell_h,
                                         TERM_ROWS_FALLBACK);
                    if (nc != t->cols || nr != t->rows) {
                        t->cols = nc;
                        t->rows = nr;
                        terminal_resize(t->term, t->cols, t->rows);
                        pty_resize(t->pty, t->cols, t->rows);
                    }
                    needs_render = 1;
                }
            } else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                int nc = px_to_cells(event.window.data1,
                                     g_cell_w, TERM_COLS_FALLBACK);
                int nr = px_to_cells(event.window.data2 - g_cell_h,
                                     g_cell_h, TERM_ROWS_FALLBACK);
                for (int i = 0; i < g_n_tabs; i++) {
                    Tab *t = &g_tabs[i];
                    if (nc != t->cols || nr != t->rows) {
                        t->cols = nc;
                        t->rows = nr;
                        terminal_resize(t->term, t->cols, t->rows);
                        pty_resize(t->pty, t->cols, t->rows);
                    }
                }
                needs_render = 1;
            } else if (event.type == SDL_EVENT_WINDOW_RESIZED) {
                needs_render = 1;
            } else if (SDL_EVENT_MOUSE_BUTTON_DOWN == event.type &&
                       SDL_BUTTON_LEFT == event.button.button) {
                int win_w, win_h;
                SDL_GetWindowSize(g_window, &win_w, &win_h);
                int cw = (at->cols > 0) ? win_w / at->cols : 1;
                int ch = (at->rows > 0) ? win_h / (at->rows + 1) : 1;
                if (g_n_tabs > 1 &&
                    (int)event.button.y >= win_h - ch) {
                    int slot_cells = win_w / g_n_tabs / cw;
                    if (slot_cells > TAB_TITLE_MAX + 2)
                        slot_cells = TAB_TITLE_MAX + 2;
                    if (slot_cells < 3) slot_cells = 3;
                    int slot_px = slot_cells * cw;
                    int ti = (slot_px > 0)
                             ? (int)(event.button.x / slot_px) : 0;
                    if (ti >= 0 && ti < g_n_tabs) {
                        tab_switch(ti);
                        needs_render = 1;
                    }
                    continue;
                }
                at->sel_anchor_col = (int)(event.button.x / cw);
                at->sel_anchor_row = (int)(event.button.y / ch);
                if (at->sel_anchor_col >= at->cols)
                    at->sel_anchor_col = at->cols - 1;
                if (at->sel_anchor_row >= at->rows)
                    at->sel_anchor_row = at->rows - 1;
                at->sel_dragging = 1;
                terminal_clear_selection(at->term);
                needs_render = 1;
            } else if (SDL_EVENT_MOUSE_MOTION == event.type &&
                       at->sel_dragging) {
                int win_w, win_h;
                SDL_GetWindowSize(g_window, &win_w, &win_h);
                int cw = (at->cols > 0) ? win_w / at->cols : 1;
                int ch = (at->rows > 0) ? win_h / (at->rows + 1) : 1;
                int cur_col = (int)(event.motion.x / cw);
                int cur_row = (int)(event.motion.y / ch);
                if (cur_col < 0) cur_col = 0;
                if (cur_col >= at->cols) cur_col = at->cols - 1;
                if (cur_row < 0) cur_row = 0;
                if (cur_row >= at->rows) cur_row = at->rows - 1;
                terminal_set_selection(at->term, at->sel_anchor_col,
                                       at->sel_anchor_row, cur_col, cur_row);
                needs_render = 1;
            } else if (SDL_EVENT_MOUSE_BUTTON_UP == event.type &&
                       SDL_BUTTON_LEFT == event.button.button) {
                if (at->sel_dragging) {
                    at->sel_dragging = 0;
                    char *text = terminal_get_selected_text(at->term);
                    if (text && *text) SDL_SetClipboardText(text);
                    free(text);
                    needs_render = 1;
                }
            } else {
                if (SDL_EVENT_KEY_DOWN == event.type) {
                    SDL_Keymod  mod = event.key.mod;
                    SDL_Keycode sym = event.key.key;
                    /* Cmd+F: open search */
                    if ((mod & SDL_KMOD_GUI) && 'f' == sym
                        && !search_is_active(g_search)) {
                        search_open(g_search, at->term);
                        needs_render = 1;
                        continue;
                    }
                    /* Cmd+T: new tab */
                    if ((mod & SDL_KMOD_GUI) && 't' == sym) {
                        if (-1 != tab_new()) {
                            tab_switch(g_n_tabs - 1);
                            needs_render = 1;
                        }
                        continue;
                    }
                    /* Cmd+W: close active tab (no-op when last tab) */
                    if ((mod & SDL_KMOD_GUI) && 'w' == sym) {
                        tab_close(g_active);
                        needs_render = 1;
                        continue;
                    }
                    /* Cmd+1..9: switch to tab N */
                    if ((mod & SDL_KMOD_GUI) &&
                        sym >= '1' && sym <= '9') {
                        tab_switch((int)(sym - '1'));
                        needs_render = 1;
                        continue;
                    }
                }
                /* Route to search module when active. */
                if (search_is_active(g_search)) {
                    int consumed = 0;
                    if (SDL_EVENT_KEY_DOWN == event.type)
                        consumed = search_handle_key(g_search, at->term,
                                                     &event.key);
                    else if (SDL_EVENT_TEXT_INPUT == event.type)
                        consumed = search_handle_text(g_search, at->term,
                                                      &event.text);
                    if (consumed) { needs_render = 1; continue; }
                }
                /* Shadow cmd_buf + autocomplete interception. */
                if (SDL_EVENT_KEY_DOWN == event.type
                    && !search_is_active(g_search)) {
                    SDL_Keycode sym = event.key.key;
                    if (SDLK_BACKSPACE == sym) {
                        if (at->cmd_len > 0) {
                            int i = utf8_prev_start(at->cmd_buf,
                                                    at->cmd_len);
                            at->cmd_len    = i;
                            at->cmd_buf[i] = '\0';
                        }
                        if (!at->tui_active)
                            autocomplete_query(at->ac, at->cmd_buf);
                    } else if (SDLK_RETURN == sym || SDLK_RETURN2 == sym
                               || SDLK_KP_ENTER == sym) {
                        if (at->cmd_len > 0) {
                            history_append(g_history, at->cmd_buf);
                            autocomplete_record_command(
                                at->ac, at->cmd_buf,
                                terminal_exit_code(at->term));
                        }
                        at->cmd_len    = 0;
                        at->cmd_buf[0] = '\0';
                        autocomplete_clear(at->ac);
                    } else if (SDLK_ESCAPE == sym) {
                        autocomplete_clear(at->ac);
                    } else if (SDLK_TAB == sym) {
                        const char *sug =
                            autocomplete_get_suggestion(at->ac);
                        if (sug && *sug) {
                            size_t slen = strlen(sug);
                            const char *cp = at->cmd_buf;
                            while (' ' == *cp || '\t' == *cp) cp++;
                            int is_nl        = ('#' == *cp);
                            int is_multiline =
                                (NULL != strchr(sug, '\n'));
                            if (is_nl) {
                                const char ctrl_u = '\x15';
                                pty_write(at->pty, &ctrl_u, 1);
                            }
                            if (is_multiline) {
                                pty_write(at->pty, PASTE_START,
                                          sizeof(PASTE_START) - 1);
                                pty_write(at->pty, sug, slen);
                                pty_write(at->pty, PASTE_END,
                                          sizeof(PASTE_END) - 1);
                                at->cmd_len    = 0;
                                at->cmd_buf[0] = '\0';
                            } else {
                                pty_write(at->pty, sug, slen);
                                if (is_nl) {
                                    if ((int)slen + 1 < CMD_BUF_SIZE) {
                                        memcpy(at->cmd_buf, sug, slen);
                                        at->cmd_len = (int)slen;
                                        at->cmd_buf[at->cmd_len] = '\0';
                                    } else {
                                        at->cmd_len    = 0;
                                        at->cmd_buf[0] = '\0';
                                    }
                                } else if (at->cmd_len + (int)slen + 1
                                           < CMD_BUF_SIZE) {
                                    memcpy(at->cmd_buf + at->cmd_len,
                                           sug, slen);
                                    at->cmd_len += (int)slen;
                                    at->cmd_buf[at->cmd_len] = '\0';
                                }
                            }
                            autocomplete_clear(at->ac);
                            needs_render = 1;
                            continue;
                        }
                    }
                } else if (SDL_EVENT_TEXT_INPUT == event.type
                           && !search_is_active(g_search)) {
                    const char *text = event.text.text;
                    int tlen = (int)strlen(text);
                    if (at->cmd_len + tlen + 1 < CMD_BUF_SIZE) {
                        memcpy(at->cmd_buf + at->cmd_len, text,
                               (size_t)tlen);
                        at->cmd_len           += tlen;
                        at->cmd_buf[at->cmd_len] = '\0';
                    }
                    if (!at->tui_active)
                        autocomplete_query(at->ac, at->cmd_buf);
                }
                input_handle_event(&event, at->term, at->pty);
                needs_render = 1;
            }
        } while (SDL_PollEvent(&event));
        }

        if (!running) break;
        if (needs_render) {
            Tab *at = &g_tabs[g_active];
            poll_fg_title(at);
            renderer_draw(renderer, at->term, g_search);
            renderer_draw_scrollbar(renderer, at->term);
            renderer_draw_search_overlay(renderer, g_search, at->term);
            renderer_draw_autocomplete_overlay(renderer, at->ac, at->term);
            const char *titles[MAX_TABS];
            for (int i = 0; i < g_n_tabs; i++)
                titles[i] = g_tabs[i].title;
            renderer_draw_tab_bar(renderer, g_n_tabs, g_active, titles);
            SDL_GL_SwapWindow(g_window);
            needs_render = 0;
        }
    }

    SDL_StopTextInput(g_window);
    for (int i = 0; i < g_n_tabs; i++) {
        Tab *t = &g_tabs[i];
        pty_close(t->pty);
        free(t->pty);
        autocomplete_destroy(t->ac);
        terminal_state_destroy(t->ts);
        terminal_destroy(t->term);
    }
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
