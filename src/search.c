#include "search.h"
#include "utf8.h"
#include "util.h"

#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Uint32 g_search_event_type = 0;

void search_set_event_type(uint32_t t) { g_search_event_type = t; }

static void search_done_cb(void *arg) {
    (void)arg;
    if (!g_search_event_type)
        return;
    SDL_Event e;
    SDL_zero(e);
    e.type = g_search_event_type;
    SDL_PushEvent(&e);
}

#if PHANTOM_DEBUG
static void sb_log(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "search: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
#define SB_LOG sb_log
#else
static void sb_log_noop(const char *fmt, ...) { (void)fmt; }
#define SB_LOG sb_log_noop
#endif

#define SEARCH_QUERY_INIT 64
#define SEARCH_RESULTS_INIT 64

struct SearchState {
    int active;
    char *query;
    int query_len;
    int query_cap;

    pthread_mutex_t lock; /* guards results, results_count, results_cap,
                             results_dirty */
    int *results;         /* absolute scrollback row indices of matched lines */
    int results_count;
    int results_cap;
    int results_dirty; /* set when background thread appends; cleared on sort */
    int current;
    int live_sb_base; /* sb->total_rows at trigger time; renderer uses this to
                         map live-grid rows to abs_row */
    int event_pushed; /* throttle: only one SDL wakeup event per search run */
};

static void search_result_cb(int abs_row, void *arg) {
    SearchState *s = arg;
    pthread_mutex_lock(&s->lock);
    if (s->results_count == s->results_cap) {
        int new_cap = s->results_cap * 2;
        int *newbuf = realloc(s->results, (size_t)new_cap * sizeof(int));
        /* on realloc fail, keep partial results rather than losing all */
        if (!newbuf) {
            pthread_mutex_unlock(&s->lock);
            return;
        }
        s->results = newbuf;
        s->results_cap = new_cap;
    }
    s->results[s->results_count++] = abs_row;
    s->results_dirty = 1;
    /* one SDL wakeup per run; avoid flooding the event queue */
    int push = !s->event_pushed;
    if (push)
        s->event_pushed = 1;
    SB_LOG("hit abs_row=%d (count=%d)", abs_row, s->results_count);
    pthread_mutex_unlock(&s->lock);
    /* push outside the lock: SDL_PushEvent can block, and the main thread
     * also acquires s->lock in the render path */
    if (push && g_search_event_type) {
        SDL_Event e;
        SDL_zero(e);
        e.type = g_search_event_type;
        SDL_PushEvent(&e);
    }
}

SearchState *search_create(void) {
    SearchState *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->query = malloc(SEARCH_QUERY_INIT);
    if (!s->query) {
        free(s);
        return NULL;
    }
    s->query[0] = '\0';
    s->query_cap = SEARCH_QUERY_INIT;
    s->results = malloc(SEARCH_RESULTS_INIT * sizeof(int));
    if (!s->results) {
        free(s->query);
        free(s);
        return NULL;
    }
    s->results_cap = SEARCH_RESULTS_INIT;
    pthread_mutex_init(&s->lock, NULL);
    return s;
}

void search_destroy(SearchState *s) {
    if (!s)
        return;
    pthread_mutex_destroy(&s->lock);
    free(s->query);
    free(s->results);
    free(s);
}

/* Reset all result-state fields; must be called with s->lock held. */
static void search_reset_results(SearchState *s) {
    s->results_count = 0;
    s->results_dirty = 0;
    s->current = 0;
}

void search_open(SearchState *s, Terminal *t) {
    terminal_search_cancel(t);
    s->active = 1;
    s->query_len = 0;
    s->query[0] = '\0';
    pthread_mutex_lock(&s->lock);
    search_reset_results(s);
    pthread_mutex_unlock(&s->lock);
    terminal_scroll_bottom(t);
}

void search_close(SearchState *s, Terminal *t) {
    terminal_search_cancel(t);
    s->active = 0;
}

int search_is_active(const SearchState *s) { return s->active; }

static void trigger_search(SearchState *s, Terminal *t) {
    terminal_search_cancel(t);
    pthread_mutex_lock(&s->lock);
    search_reset_results(s);
    s->event_pushed = 0;
    pthread_mutex_unlock(&s->lock);
    s->live_sb_base = terminal_sb_total_rows(t);
    if (s->query_len > 0) {
        SB_LOG("trigger query=\"%s\" live_sb_base=%d", s->query,
               s->live_sb_base);
        terminal_search(t, s->query, search_result_cb, search_done_cb, s);
    }
}

static void navigate(SearchState *s, Terminal *t, int dir) {
    int abs_row = -1;
    pthread_mutex_lock(&s->lock);
    /* background thread may have appended since last navigate */
    if (s->results_dirty) {
        qsort(s->results, (size_t)s->results_count, sizeof(int), int_cmp);
        s->results_dirty = 0;
    }
    int count = s->results_count;
    if (count > 0) {
        /* +count before % avoids negative modulo when dir == -1 */
        s->current = (s->current + dir + count) % count;
        abs_row = s->results[s->current];
    }
    pthread_mutex_unlock(&s->lock);
    if (0 == count)
        return;
    SB_LOG("navigate %s -> idx=%d abs_row=%d", dir > 0 ? "next" : "prev",
           s->current, abs_row);
    terminal_scroll_to_row(t, abs_row);
}

int search_handle_key(SearchState *s, Terminal *t, const SDL_KeyboardEvent *e) {
    SDL_Keycode sym = e->key;
    SDL_Keymod mod = e->mod;

    if (SDLK_ESCAPE == sym) {
        search_close(s, t);
        return 1;
    }

    if (SDLK_RETURN == sym || SDLK_RETURN2 == sym || SDLK_KP_ENTER == sym) {
        if (mod & SDL_KMOD_SHIFT) {
            navigate(s, t, -1);
        } else {
            navigate(s, t, +1);
        }
        return 1;
    }

    if (SDLK_BACKSPACE == sym) {
        if (s->query_len > 0) {
            /* step back one full UTF-8 codepoint to avoid a partial sequence */
            int i = utf8_prev_start(s->query, s->query_len);
            s->query_len = i;
            s->query[i] = '\0';
            trigger_search(s, t);
        }
        return 1;
    }
    return 0;
}

/* SDL fires TEXT_INPUT separately from KEY_DOWN for IME/compose support;
 * all text input while search is open feeds the query. */
int search_handle_text(SearchState *s, Terminal *t,
                       const SDL_TextInputEvent *e) {
    const char *text = e->text;
    int len = (int)strlen(text);
    if (0 == len)
        return 1;

    if (s->query_len + len + 1 > s->query_cap) {
        int new_cap = s->query_cap * 2;
        while (new_cap < s->query_len + len + 1)
            new_cap *= 2;
        char *newbuf = realloc(s->query, (size_t)new_cap);
        if (!newbuf)
            return 1;
        s->query = newbuf;
        s->query_cap = new_cap;
    }
    memcpy(s->query + s->query_len, text, (size_t)len);
    s->query_len += len;
    s->query[s->query_len] = '\0';
    trigger_search(s, t);
    return 1;
}

const char *search_query(const SearchState *s) { return s->query; }
int search_query_len(const SearchState *s) { return s->query_len; }
int search_current_idx(const SearchState *s) { return s->current; }
int search_live_sb_base(const SearchState *s) { return s->live_sb_base; }

int search_current_abs_row(SearchState *s) {
    pthread_mutex_lock(&s->lock);
    int row = (s->results_count > 0) ? s->results[s->current] : -1;
    pthread_mutex_unlock(&s->lock);
    return row;
}

int search_result_count(SearchState *s) {
    pthread_mutex_lock(&s->lock);
    int c = s->results_count;
    pthread_mutex_unlock(&s->lock);
    return c;
}

int *search_snapshot(SearchState *s, int *out_count) {
    pthread_mutex_lock(&s->lock);
    /* sort in place; renderer draws highlights top-to-bottom */
    if (s->results_dirty) {
        qsort(s->results, (size_t)s->results_count, sizeof(int), int_cmp);
        s->results_dirty = 0;
    }
    *out_count = s->results_count;
    int *snap = NULL;
    if (s->results_count > 0) {
        snap = malloc((size_t)s->results_count * sizeof(int));
        if (snap)
            /* deep copy: background thread may realloc s->results at any time
             */
            memcpy(snap, s->results, (size_t)s->results_count * sizeof(int));
        else
            *out_count = 0;
    }
    pthread_mutex_unlock(&s->lock);
    return snap;
}
