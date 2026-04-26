#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/ml/ansi.h"

static void test_no_escapes(void)
{
    char dst[64];
    size_t n = strip_ansi("hello world", 11, dst, sizeof dst);
    assert(11 == n);
    assert(0 == strcmp(dst, "hello world"));
}

static void test_sgr_strip(void)
{
    const char *src = "\x1b[32mgreen\x1b[0m";
    char dst[64];
    size_t n = strip_ansi(src, strlen(src), dst, sizeof dst);
    assert(5 == n);
    assert(0 == strcmp(dst, "green"));
}

static void test_osc_strip(void)
{
    /* OSC sequence terminated by BEL */
    const char *src = "\x1b]0;title\x07text";
    char dst[64];
    size_t n = strip_ansi(src, strlen(src), dst, sizeof dst);
    assert(4 == n);
    assert(0 == strcmp(dst, "text"));
}

static void test_osc_st_terminator(void)
{
    /* OSC sequence terminated by ST (ESC \) */
    const char *src = "\x1b]133;A\x1b\\cmd";
    char dst[64];
    size_t n = strip_ansi(src, strlen(src), dst, sizeof dst);
    assert(3 == n);
    assert(0 == strcmp(dst, "cmd"));
}

static void test_partial_esc_at_end(void)
{
    const char *src = "abc\x1b";
    char dst[64];
    size_t n = strip_ansi(src, strlen(src), dst, sizeof dst);
    assert(3 == n);
    assert(0 == strcmp(dst, "abc"));
}

static void test_dst_truncation(void)
{
    /* dst_cap=4 → 3 chars + NUL */
    char dst[4];
    size_t n = strip_ansi("abcde", 5, dst, sizeof dst);
    assert(3 == n);
    assert(0 == strcmp(dst, "abc"));
}

static void test_utf8_passthrough(void)
{
    const char *src = "caf\xc3\xa9";
    char dst[64];
    size_t n = strip_ansi(src, strlen(src), dst, sizeof dst);
    assert(5 == n);
    assert(0 == memcmp(dst, "caf\xc3\xa9", 5));
}

static void test_csi_cursor_move(void)
{
    /* ESC[2A = cursor up 2 */
    const char *src = "a\x1b[2Ab";
    char dst[64];
    size_t n = strip_ansi(src, strlen(src), dst, sizeof dst);
    assert(2 == n);
    assert(0 == strcmp(dst, "ab"));
}

static void test_c0_strip(void)
{
    /* BEL (\x07) and SI (\x0e) stripped; LF kept.
     * Use string concatenation to prevent hex escape greediness:
     * "\x0eb" would be parsed as 0xEB, not 0x0E followed by 'b'. */
    const char *src = "a\x07" "b\x0e" "b";
    char dst[64];
    size_t n = strip_ansi(src, strlen(src), dst, sizeof dst);
    assert(3 == n);
    assert(0 == strcmp(dst, "abb"));
}

static void test_lf_preserved(void)
{
    const char *src = "a\nb";
    char dst[64];
    size_t n = strip_ansi(src, strlen(src), dst, sizeof dst);
    assert(3 == n);
    assert(0 == strcmp(dst, "a\nb"));
}

static void test_cr_preserved(void)
{
    const char *src = "a\rb";
    char dst[64];
    size_t n = strip_ansi(src, strlen(src), dst, sizeof dst);
    assert(3 == n);
    assert(0 == strcmp(dst, "a\rb"));
}

static void test_empty_src(void)
{
    char dst[8];
    size_t n = strip_ansi("", 0, dst, sizeof dst);
    assert(0 == n);
    assert('\0' == dst[0]);
}

static void test_dst_cap_one(void)
{
    /* dst_cap=1 → only NUL */
    char dst[1];
    size_t n = strip_ansi("abc", 3, dst, sizeof dst);
    assert(0 == n);
    assert('\0' == dst[0]);
}

int main(void)
{
    test_no_escapes();
    test_sgr_strip();
    test_osc_strip();
    test_osc_st_terminator();
    test_partial_esc_at_end();
    test_dst_truncation();
    test_utf8_passthrough();
    test_csi_cursor_move();
    test_c0_strip();
    test_lf_preserved();
    test_cr_preserved();
    test_empty_src();
    test_dst_cap_one();
    printf("test_ansi: all passed\n");
    return 0;
}
