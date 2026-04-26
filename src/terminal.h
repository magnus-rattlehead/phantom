#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct Terminal Terminal;

typedef struct {
    uint32_t ch;    /* Unicode codepoint; ' ' = empty cell */
    uint32_t fg;    /* packed RGBA, 0xRRGGBBAA */
    uint32_t bg;
    uint8_t  attrs; /* ATTR_* bitmask */
} Cell;

#define ATTR_BOLD      0x01
#define ATTR_UNDERLINE 0x02
#define ATTR_REVERSE   0x04
#define ATTR_ITALIC    0x08

/* Allocates a terminal of cols×rows cells with default colors. */
Terminal   *terminal_create(int cols, int rows);
void        terminal_destroy(Terminal *t);

/* Processes raw bytes (VT100/ANSI sequences) and updates the cell grid. */
void        terminal_feed(Terminal *t, const char *buf, size_t len);

/* Resizes the grid, preserving as much existing content as fits. */
void        terminal_resize(Terminal *t, int cols, int rows);

int         terminal_cols(const Terminal *t);
int         terminal_rows(const Terminal *t);

/* Returns 1 if application cursor key mode (DECCKM) is active. */
int         terminal_app_cursor_keys(const Terminal *t);
int         terminal_cursor_visible(const Terminal *t);
int         terminal_on_alt_screen(const Terminal *t);
/* DECSCUSR cursor shape: 0/1=block blink, 2=block, 3=underline blink,
 * 4=underline, 5=beam blink, 6=beam. */
int         terminal_cursor_shape(const Terminal *t);
/* Returns 1 if bracketed paste mode (?2004) is active. */
int         terminal_bracketed_paste(const Terminal *t);

/* Selection (main-thread only  -  no mutex, call only from event/render thread). */
void        terminal_set_selection(Terminal *t, int c0, int r0, int c1, int r1);
void        terminal_clear_selection(Terminal *t);
/* Returns 1 if the cell at (col, row) in the current visible grid is selected. */
int         terminal_cell_selected(const Terminal *t, int col, int row);
/* Returns malloc'd UTF-8 text for the current selection; NULL if none. Caller frees. */
char       *terminal_get_selected_text(Terminal *t);

/* Copies the full cell grid and cursor position; thread-safe (locks internally). */
void        terminal_get_state(Terminal *t, Cell *cells_out,
                                int *cursor_col, int *cursor_row);

/* Scroll the view by delta lines (positive = up/older, negative = down/newer).
 * Clamped to [0, available scrollback]. Thread-safe. */
void        terminal_scroll(Terminal *t, int delta);

/* Snap immediately to the live (bottom) view. Thread-safe. */
void        terminal_scroll_bottom(Terminal *t);

int  terminal_scroll_offset(const Terminal *t);
int  terminal_sb_total_rows(const Terminal *t);
/* Scrolls so that abs_row is visible at the top of the terminal. */
void terminal_scroll_to_row(Terminal *t, int abs_row);

/* Callback invoked for each scrollback row that matches a search query.
 * abs_row is 0-based from the oldest stored row. */
typedef void (*terminal_search_result_fn)(int abs_row, void *arg);
typedef void (*terminal_search_done_fn)(void *arg);

/* Starts an async search for query (UTF-8).  Cancels any prior search first.
 * Results delivered via result_cb; done_cb called once when scan completes
 * (or is cancelled). Both invoked from background threads. Thread-safe. */
void terminal_search(Terminal *t, const char *query,
                     terminal_search_result_fn result_cb,
                     terminal_search_done_fn   done_cb,
                     void *arg);

/* Signals the in-flight search to stop.  Returns immediately. */
void terminal_search_cancel(Terminal *t);

/* Called (with term->lock held) when the alternate screen is entered/exited.
 * entering: 1 = entering alt screen, 0 = exiting. */
typedef void (*terminal_alt_screen_fn)(void *arg, int entering);

void        terminal_set_alt_screen_callback(Terminal *t,
                                             terminal_alt_screen_fn fn,
                                             void *arg);

/* Returns the exit code from the last OSC 133;D sequence received,
 * or -1 if none has arrived yet.  Thread-safe. */
int         terminal_exit_code(const Terminal *t);

/* Registers a callback invoked (with term->lock held) whenever an
 * OSC 133;D sequence is received.  Pass NULL to clear. */
typedef void (*terminal_exit_code_fn)(int exit_code, void *arg);
void        terminal_set_exit_code_callback(Terminal *t,
                                            terminal_exit_code_fn fn,
                                            void *arg);

/* Registers a callback invoked whenever an OSC 7 (shell CWD) sequence is
 * received.  path is a NUL-terminated absolute filesystem path.
 * Called with term->lock held; pass NULL fn to clear. */
typedef void (*terminal_cwd_fn)(const char *path, void *arg);
void        terminal_set_cwd_callback(Terminal *t,
                                      terminal_cwd_fn fn,
                                      void *arg);

/* Registers a callback invoked whenever an OSC 0/2 (window title) sequence
 * is received.  title is NUL-terminated.  Called with term->lock held. */
typedef void (*terminal_title_fn)(const char *title, void *arg);
void        terminal_set_title_callback(Terminal *t,
                                        terminal_title_fn fn,
                                        void *arg);
