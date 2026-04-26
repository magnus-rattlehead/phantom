#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#if defined(__APPLE__)
#  include <dispatch/dispatch.h>
#endif

#include "../src/terminal.h"
#include "../src/config.h"

extern int row_swar_scan(const Cell *row, int cols,
                         const uint32_t *qcps, int qlen);


static void get(Terminal *t, Cell *cells, int *cc, int *cr)
{
    terminal_get_state(t, cells, cc, cr);
}


static void test_create_destroy(void)
{
    Terminal *t = terminal_create(80, 24);
    assert(t != NULL);
    assert(80 == terminal_cols(t));
    assert(24 == terminal_rows(t));
    terminal_destroy(t);
}

static void test_printable_chars(void)
{
    Terminal *t = terminal_create(80, 24);
    terminal_feed(t, "AB", 2);

    Cell cells[80 * 24];
    int cc, cr;
    get(t, cells, &cc, &cr);

    assert('A' == cells[0].ch);
    assert('B' == cells[1].ch);
    assert(2 == cc);
    assert(0 == cr);
    terminal_destroy(t);
}

static void test_newline_and_cr(void)
{
    Terminal *t = terminal_create(80, 24);
    terminal_feed(t, "AB\r\n", 4);

    Cell cells[80 * 24];
    int cc, cr;
    get(t, cells, &cc, &cr);

    assert(0 == cc);
    assert(1 == cr);
    terminal_destroy(t);
}

static void test_erase_display(void)
{
    Terminal *t = terminal_create(80, 24);
    terminal_feed(t, "HELLO", 5);
    terminal_feed(t, "\x1b[2J", 4);

    Cell cells[80 * 24];
    int cc, cr;
    get(t, cells, &cc, &cr);

    assert(' ' == cells[0].ch);
    assert(' ' == cells[4].ch);
    terminal_destroy(t);
}

static void test_cursor_position(void)
{
    Terminal *t = terminal_create(80, 24);
    terminal_feed(t, "\x1b[5;10H", 7);

    Cell cells[80 * 24];
    int cc, cr;
    get(t, cells, &cc, &cr);

    assert(9 == cc);
    assert(4 == cr);
    terminal_destroy(t);
}

static void test_sgr_reset(void)
{
    Terminal *t = terminal_create(80, 24);
    terminal_feed(t, "\x1b[31m", 5);
    terminal_feed(t, "X", 1);
    terminal_feed(t, "\x1b[0m", 4);
    terminal_feed(t, "Y", 1);

    Cell cells[80 * 24];
    int cc, cr;
    get(t, cells, &cc, &cr);

    assert('X' == cells[0].ch);
    assert(cells[0].fg != cells[1].fg);
    assert('Y' == cells[1].ch);
    terminal_destroy(t);
}

static void test_alt_screen_isolation(void)
{
    Terminal *t = terminal_create(80, 24);
    terminal_feed(t, "PRIMARY", 7);

    /* Enter alternate screen  -  cursor resets, buffer is blank */
    terminal_feed(t, "\x1b[?1049h", 8);

    Cell cells[80 * 24];
    int cc, cr;
    get(t, cells, &cc, &cr);

    assert(' ' == cells[0].ch);
    assert(0 == cc && 0 == cr);

    terminal_feed(t, "ALT", 3);
    get(t, cells, &cc, &cr);
    assert('A' == cells[0].ch);

    /* Exit alternate screen  -  primary screen content is restored, cursor
     * returns to where it was when smcup fired (after writing "PRIMARY"). */
    terminal_feed(t, "\x1b[?1049l", 8);
    get(t, cells, &cc, &cr);

    assert('P' == cells[0].ch);
    assert('R' == cells[1].ch);
    assert(7 == cc && 0 == cr);
    terminal_destroy(t);
}

static void test_alt_screen_cursor_restore(void)
{
    Terminal *t = terminal_create(80, 24);
    terminal_feed(t, "\x1b[3;5H", 6);
    terminal_feed(t, "\x1b[?1049h", 8);
    terminal_feed(t, "\x1b[?1049l", 8);

    Cell cells[80 * 24];
    int cc, cr;
    get(t, cells, &cc, &cr);

    assert(4 == cc);
    assert(2 == cr);
    terminal_destroy(t);
}

static void test_osc_no_spill(void)
{
    Terminal *t = terminal_create(80, 24);
    terminal_feed(t, "\x1b]0;window title\x07", 18);
    terminal_feed(t, "\x1b]2;another title\x1b\\", 20);

    Cell cells[80 * 24];
    int cc, cr;
    get(t, cells, &cc, &cr);

    assert(' ' == cells[0].ch);
    assert(0 == cc && 0 == cr);
    terminal_destroy(t);
}

static void test_app_cursor_keys(void)
{
    Terminal *t = terminal_create(80, 24);
    assert(0 == terminal_app_cursor_keys(t));

    terminal_feed(t, "\x1b[?1h", 5);
    assert(1 == terminal_app_cursor_keys(t));

    terminal_feed(t, "\x1b[?1l", 5);
    assert(0 == terminal_app_cursor_keys(t));

    terminal_destroy(t);
}

static void test_cursor_visibility(void)
{
    Terminal *t = terminal_create(80, 24);
    assert(1 == terminal_cursor_visible(t));

    terminal_feed(t, "\x1b[?25l", 6);
    assert(0 == terminal_cursor_visible(t));

    terminal_feed(t, "\x1b[?25h", 6);
    assert(1 == terminal_cursor_visible(t));

    terminal_destroy(t);
}

static void test_dec_special_graphics(void)
{
    Terminal *t = terminal_create(80, 24);

    terminal_feed(t, "\x1b(0", 3);
    terminal_feed(t, "jq", 2);
    terminal_feed(t, "\x1b(B", 3);
    terminal_feed(t, "X", 1);

    Cell cells[80 * 24];
    int cc, cr;
    get(t, cells, &cc, &cr);

    assert(0x2518u == cells[0].ch);
    assert(0x2500u == cells[1].ch);
    assert('X'     == cells[2].ch);
    terminal_destroy(t);
}

static void test_csi_gt_no_sgr(void)
{
    Terminal *t = terminal_create(80, 24);
    terminal_feed(t, "A", 1);
    terminal_feed(t, "\x1b[>4;2m", 7);
    terminal_feed(t, "B", 1);

    Cell cells[80 * 24];
    int cc, cr;
    get(t, cells, &cc, &cr);

    assert(!(cells[1].attrs & ATTR_UNDERLINE));
    terminal_destroy(t);
}

static void test_erase_in_line(void)
{
    Terminal *t = terminal_create(10, 3);
    terminal_feed(t, "ABCDEFGHIJ", 10);
    terminal_feed(t, "\x1b[1;5H", 6);

    terminal_feed(t, "\x1b[K",  3);
    Cell cells[10 * 3];
    int cc, cr;
    get(t, cells, &cc, &cr);
    assert('A' == cells[0].ch);
    assert('D' == cells[3].ch);
    assert(' ' == cells[4].ch);
    assert(' ' == cells[9].ch);

    terminal_feed(t, "\x1b[1;6H", 6);
    terminal_feed(t, "\x1b[1K", 4);
    get(t, cells, &cc, &cr);
    assert(' ' == cells[0].ch);
    assert(' ' == cells[5].ch);

    terminal_destroy(t);
}

static void test_insert_lines(void)
{
    Terminal *t = terminal_create(10, 4);
    terminal_feed(t, "AAAAAAAAAA\r\n", 12);
    terminal_feed(t, "BBBBBBBBBB\r\n", 12);
    terminal_feed(t, "CCCCCCCCCC\r\n", 12);
    terminal_feed(t, "DDDDDDDDDD",   10);

    terminal_feed(t, "\x1b[2;1H", 6);
    terminal_feed(t, "\x1b[1L",   4);

    Cell cells[10 * 4];
    int cc, cr;
    get(t, cells, &cc, &cr);

    assert('A' == cells[0 * 10].ch);
    assert(' ' == cells[1 * 10].ch);
    assert('B' == cells[2 * 10].ch);
    assert('C' == cells[3 * 10].ch);
    terminal_destroy(t);
}

static void test_delete_lines(void)
{
    Terminal *t = terminal_create(10, 4);
    terminal_feed(t, "AAAAAAAAAA\r\n", 12);
    terminal_feed(t, "BBBBBBBBBB\r\n", 12);
    terminal_feed(t, "CCCCCCCCCC\r\n", 12);
    terminal_feed(t, "DDDDDDDDDD",   10);

    terminal_feed(t, "\x1b[2;1H", 6);
    terminal_feed(t, "\x1b[1M",   4);

    Cell cells[10 * 4];
    int cc, cr;
    get(t, cells, &cc, &cr);

    assert('A' == cells[0 * 10].ch);
    assert('C' == cells[1 * 10].ch);
    assert('D' == cells[2 * 10].ch);
    assert(' ' == cells[3 * 10].ch);
    terminal_destroy(t);
}

static void test_implicit_rmcup(void)
{
    Terminal *t = terminal_create(80, 24);
    terminal_feed(t, "PRIMARY", 7);

    terminal_feed(t, "\x1b[?1049h", 8);
    terminal_feed(t, "ALT", 3);

    Cell cells[80 * 24];
    int cc, cr;
    get(t, cells, &cc, &cr);
    assert('A' == cells[0].ch);

    terminal_feed(t, "\x1b[?1l", 5);
    get(t, cells, &cc, &cr);

    assert('P' == cells[0].ch);
    assert(7 == cc && 0 == cr);
    terminal_destroy(t);
}

static void test_resize_row_growth_pulls_scrollback(void)
{
    Terminal *t = terminal_create(10, 3);

    terminal_feed(t, "AAAAAAAAAA\r\n", 12);
    terminal_feed(t, "BBBBBBBBBB\r\n", 12);
    terminal_feed(t, "CCCCCCCCCC\r\n", 12);
    terminal_feed(t, "DDDDDDDDDD\r\n", 12);
    terminal_feed(t, "EEEEEEEEEE",   10);

    terminal_resize(t, 10, 5);

    Cell cells[10 * 5];
    int cc, cr;
    terminal_get_state(t, cells, &cc, &cr);

    assert('A' == cells[0 * 10].ch);
    assert('B' == cells[1 * 10].ch);
    assert('C' == cells[2 * 10].ch);
    assert('D' == cells[3 * 10].ch);
    assert('E' == cells[4 * 10].ch);

    assert(4 == cr);

    terminal_destroy(t);
}

static void test_resize_row_shrink_pushes_scrollback(void)
{
    Terminal *t = terminal_create(10, 5);

    terminal_feed(t, "AAAAAAAAAA\r\n", 12);
    terminal_feed(t, "BBBBBBBBBB\r\n", 12);
    terminal_feed(t, "CCCCCCCCCC\r\n", 12);
    terminal_feed(t, "DDDDDDDDDD\r\n", 12);
    terminal_feed(t, "EEEEEEEEEE",   10);

    terminal_resize(t, 10, 3);

    Cell cells[10 * 3];
    int cc, cr;
    terminal_get_state(t, cells, &cc, &cr);

    assert('C' == cells[0 * 10].ch);
    assert('D' == cells[1 * 10].ch);
    assert('E' == cells[2 * 10].ch);
    assert(2 == cr);

    terminal_scroll(t, 9999);
    terminal_get_state(t, cells, &cc, &cr);
    assert('A' == cells[0 * 10].ch);

    terminal_destroy(t);
}

static void test_resize_col_change_preserves_content(void)
{
    Terminal *t = terminal_create(10, 3);

    terminal_feed(t, "HELLO     \r\n", 12);
    terminal_feed(t, "WORLD     \r\n", 12);
    terminal_feed(t, "ABCDE",          5);

    terminal_resize(t, 20, 3);

    Cell cells[20 * 3];
    int cc, cr;
    terminal_get_state(t, cells, &cc, &cr);

    assert('H' == cells[0 * 20 + 0].ch);
    assert('E' == cells[0 * 20 + 1].ch);
    assert('L' == cells[0 * 20 + 2].ch);
    assert('L' == cells[0 * 20 + 3].ch);
    assert('O' == cells[0 * 20 + 4].ch);
    assert(' ' == cells[0 * 20 + 10].ch);

    assert('W' == cells[1 * 20 + 0].ch);
    assert('A' == cells[2 * 20 + 0].ch);

    terminal_resize(t, 5, 3);

    Cell cells2[5 * 3];
    terminal_get_state(t, cells2, &cc, &cr);
    assert('H' == cells2[0 * 5 + 0].ch);
    assert('E' == cells2[0 * 5 + 1].ch);
    assert('W' == cells2[1 * 5 + 0].ch);

    terminal_destroy(t);
}

static void test_resize_scrollback_survives_col_change(void)
{
    Terminal *t = terminal_create(10, 3);

    terminal_feed(t, "AAAAAAAAAA\r\n", 12);
    terminal_feed(t, "BBBBBBBBBB\r\n", 12);
    terminal_feed(t, "CCCCCCCCCC\r\n", 12);
    terminal_feed(t, "DDDDDDDDDD",   10);

    terminal_resize(t, 20, 3);

    terminal_scroll(t, 9999);
    Cell cells[20 * 3];
    int cc, cr;
    terminal_get_state(t, cells, &cc, &cr);

    assert('A' == cells[0 * 20 + 0].ch);
    assert(' ' == cells[0 * 20 + 10].ch);

    terminal_destroy(t);
}

static void test_scrollback_push_and_read(void)
{
    Terminal *t = terminal_create(10, 5);
    int total = SB_CHUNK_ROWS + 3;
    char line[13];
    for (int i = 0; i < total; i++) {
        char ch = (char)('A' + (i % 26));
        line[0] = ch;
        line[1] = '\r';
        line[2] = '\n';
        terminal_feed(t, line, 3);
    }
    terminal_scroll(t, 9999);

    Cell cells[10 * 5];
    int cc, cr;
    terminal_get_state(t, cells, &cc, &cr);

    assert(cells[0].ch != ' ');
    terminal_destroy(t);
}

static void test_scrollback_preserved_on_resize(void)
{
    Terminal *t = terminal_create(10, 5);
    for (int i = 0; i < SB_CHUNK_ROWS * 2; i++)
        terminal_feed(t, "line\r\n", 6);
    terminal_resize(t, 80, 24);

    Cell cells[80 * 24];
    int cc, cr;
    terminal_get_state(t, cells, &cc, &cr);
    assert(-1 != cc && -1 != cr);

    terminal_scroll(t, 9999);
    terminal_get_state(t, cells, &cc, &cr);
    assert(-1 == cc && -1 == cr);

    terminal_destroy(t);
}

static void test_scrollback_scroll_clamping(void)
{
    Terminal *t = terminal_create(10, 4);
    terminal_feed(t, "A\r\n", 3);
    terminal_scroll(t, 999999);
    terminal_scroll(t, 999999);
    terminal_scroll_bottom(t);
    terminal_scroll(t, -999999);

    Cell cells[10 * 4];
    int cc, cr;
    terminal_get_state(t, cells, &cc, &cr);
    assert(0 == cc && 1 == cr);
    terminal_destroy(t);
}

static void test_scrollback_infinite(void)
{
    Terminal *t = terminal_create(5, 3);
    int total = SB_CHUNK_ROWS * 4 + 7;
    for (int i = 0; i < total; i++)
        terminal_feed(t, "X\r\n", 3);
    terminal_scroll(t, 999999);

    Cell cells[5 * 3];
    int cc, cr;
    terminal_get_state(t, cells, &cc, &cr);
    assert('X' == cells[0].ch);
    terminal_destroy(t);
}

static Cell make_cell(uint32_t ch)
{
    Cell c;
    c.ch    = ch;
    c.fg    = 0xFFFFFFFFu;
    c.bg    = 0x000000FFu;
    c.attrs = 0;
    return c;
}

static void test_swar_scan(void)
{
    /* Build a row of 20 distinct ASCII cells. */
    Cell row[20];
    for (int i = 0; i < 20; i++)
        row[i] = make_cell((uint32_t)('a' + i));

    uint32_t q1[1];

    /* Match at start (position 0). */
    q1[0] = 'a';
    assert(1 == row_swar_scan(row, 20, q1, 1));

    /* Match in middle (position 5  -  inside the 8-cell SWAR window). */
    q1[0] = 'f';
    assert(1 == row_swar_scan(row, 20, q1, 1));

    /* Match at end (position 19, tail loop). */
    q1[0] = 't';
    assert(1 == row_swar_scan(row, 20, q1, 1));

    /* Match at position 7 (straddles 8-cell main/tail boundary). */
    q1[0] = 'h';
    assert(1 == row_swar_scan(row, 20, q1, 1));

    /* No match. */
    q1[0] = (uint32_t)('z' + 1);
    assert(0 == row_swar_scan(row, 20, q1, 1));

    /* Multi-codepoint query: match "fg" starting at position 5. */
    uint32_t q2[2] = { 'f', 'g' };
    assert(1 == row_swar_scan(row, 20, q2, 2));

    /* Multi-codepoint query: no match because second cell doesn't follow. */
    uint32_t q2b[2] = { 'f', 'z' };
    assert(0 == row_swar_scan(row, 20, q2b, 2));

    /* False-positive: cell whose low byte matches but full ch differs. */
    Cell fp[10];
    for (int i = 0; i < 10; i++)
        fp[i] = make_cell(0xFFFFu);     /* low byte = 0xFF, high bytes set */
    fp[5] = make_cell(0x01FFu);         /* low byte still 0xFF */
    uint32_t qfp[1] = { 0xFFFFu };
    assert(1 == row_swar_scan(fp, 10, qfp, 1));

    /* False-positive rejected: low byte matches but full codepoint does not. */
    Cell fp2[10];
    for (int i = 0; i < 10; i++)
        fp2[i] = make_cell(0x0101u);    /* low byte = 0x01 */
    uint32_t qfp2[1] = { 0x0201u };    /* same low byte, different full ch */
    assert(0 == row_swar_scan(fp2, 10, qfp2, 1));

    /* Non-ASCII first codepoint (ch > 0xFF): SWAR low-byte misses are OK
     * because the tail loop still scans by full ch. */
    Cell uni[10];
    for (int i = 0; i < 10; i++)
        uni[i] = make_cell(0x1F600u + (uint32_t)i);
    uint32_t qu[1] = { 0x1F605u };
    assert(1 == row_swar_scan(uni, 10, qu, 1));
}

#if defined(__APPLE__)
#include <time.h>

typedef struct {
    dispatch_semaphore_t sem;
    int                  hits;
    int                  target;
} SearchCtx;

static void search_hit_cb(int abs_row, void *arg)
{
    (void)abs_row;
    SearchCtx *ctx = (SearchCtx *)arg;
    ctx->hits++;
    if (ctx->hits >= ctx->target)
        dispatch_semaphore_signal(ctx->sem);
}

static void search_count_cb(int abs_row, void *arg)
{
    (void)abs_row;
    int *count = (int *)arg;
    (*count)++;
}

static void test_terminal_search_basic(void)
{
    Terminal *t = terminal_create(20, 3);
    for (int i = 0; i < 10; i++)
        terminal_feed(t, "hello world\r\n", 13);

    SearchCtx ctx = {
        .sem    = dispatch_semaphore_create(0),
        .hits   = 0,
        .target = 7,
    };
    terminal_search(t, "hello", search_hit_cb, NULL, &ctx);

    dispatch_semaphore_wait(ctx.sem,
        dispatch_time(DISPATCH_TIME_NOW, 2LL * 1000000000LL));
    assert(ctx.hits >= 7);
    dispatch_release(ctx.sem);
    terminal_search_cancel(t);
    terminal_destroy(t);
}

static void test_terminal_search_cancel(void)
{
    Terminal *t = terminal_create(20, 3);
    for (int i = 0; i < SB_CHUNK_ROWS * 3; i++)
        terminal_feed(t, "aaaa\r\n", 6);
    int dummy = 0;
    terminal_search(t, "aaaa", search_count_cb, NULL, &dummy);
    terminal_search_cancel(t);
    /* Give the background thread a moment to exit cleanly. */
    struct timespec ts = { 0, 20 * 1000000 };
    nanosleep(&ts, NULL);
    terminal_destroy(t);
}
#endif /* __APPLE__ */

int main(void)
{
    test_create_destroy();
    test_printable_chars();
    test_newline_and_cr();
    test_erase_display();
    test_cursor_position();
    test_sgr_reset();
    test_alt_screen_isolation();
    test_alt_screen_cursor_restore();
    test_implicit_rmcup();
    test_osc_no_spill();
    test_app_cursor_keys();
    test_cursor_visibility();
    test_dec_special_graphics();
    test_csi_gt_no_sgr();
    test_erase_in_line();
    test_insert_lines();
    test_delete_lines();
    test_resize_row_growth_pulls_scrollback();
    test_resize_row_shrink_pushes_scrollback();
    test_resize_col_change_preserves_content();
    test_resize_scrollback_survives_col_change();
    test_scrollback_push_and_read();
    test_scrollback_preserved_on_resize();
    test_scrollback_scroll_clamping();
    test_scrollback_infinite();
    test_swar_scan();
#if defined(__APPLE__)
    test_terminal_search_basic();
    test_terminal_search_cancel();
#endif
    printf("test_terminal: all passed\n");
    return 0;
}
