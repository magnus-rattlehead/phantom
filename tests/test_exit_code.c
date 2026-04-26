#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/terminal.h"


static void feed(Terminal *t, const char *s)
{
    terminal_feed(t, s, strlen(s));
}

static void test_initial_exit_code(void)
{
    Terminal *t = terminal_create(80, 24);
    assert(-1 == terminal_exit_code(t));
    terminal_destroy(t);
}

static void test_osc_133_d_zero(void)
{
    Terminal *t = terminal_create(80, 24);
    feed(t, "\x1b]133;D;0\x07");
    assert(0 == terminal_exit_code(t));
    terminal_destroy(t);
}

static void test_osc_133_d_nonzero(void)
{
    Terminal *t = terminal_create(80, 24);
    feed(t, "\x1b]133;D;127\x07");
    assert(127 == terminal_exit_code(t));
    terminal_destroy(t);
}

static void test_osc_133_d_bare(void)
{
    Terminal *t = terminal_create(80, 24);
    feed(t, "\x1b]133;D\x07");
    assert(0 == terminal_exit_code(t));
    terminal_destroy(t);
}

static void test_osc_133_st_terminator(void)
{
    Terminal *t = terminal_create(80, 24);
    feed(t, "\x1b]133;D;42\x1b\\");
    assert(42 == terminal_exit_code(t));
    terminal_destroy(t);
}

static void test_osc_unrelated(void)
{
    Terminal *t = terminal_create(80, 24);
    feed(t, "\x1b]0;My Title\x07");
    assert(-1 == terminal_exit_code(t));
    terminal_destroy(t);
}

static void test_osc_133_a_ignored(void)
{
    Terminal *t = terminal_create(80, 24);
    feed(t, "\x1b]133;A\x07");
    assert(-1 == terminal_exit_code(t));
    terminal_destroy(t);
}

static void test_exit_code_overwritten(void)
{
    Terminal *t = terminal_create(80, 24);
    feed(t, "\x1b]133;D;1\x07");
    assert(1 == terminal_exit_code(t));
    feed(t, "\x1b]133;D;0\x07");
    assert(0 == terminal_exit_code(t));
    terminal_destroy(t);
}

static int g_cb_exit_code = -99;

static void exit_code_callback(int code, void *arg)
{
    (void)arg;
    g_cb_exit_code = code;
}

static void test_exit_code_callback(void)
{
    Terminal *t = terminal_create(80, 24);
    terminal_set_exit_code_callback(t, exit_code_callback, NULL);
    g_cb_exit_code = -99;
    feed(t, "\x1b]133;D;5\x07");
    assert(5  == terminal_exit_code(t));
    assert(5  == g_cb_exit_code);
    terminal_destroy(t);
}

static void test_callback_not_fired_for_other_osc(void)
{
    Terminal *t = terminal_create(80, 24);
    terminal_set_exit_code_callback(t, exit_code_callback, NULL);
    g_cb_exit_code = -99;
    feed(t, "\x1b]0;title\x07");
    assert(-99 == g_cb_exit_code);
    terminal_destroy(t);
}

int main(void)
{
    test_initial_exit_code();
    test_osc_133_d_zero();
    test_osc_133_d_nonzero();
    test_osc_133_d_bare();
    test_osc_133_st_terminator();
    test_osc_unrelated();
    test_osc_133_a_ignored();
    test_exit_code_overwritten();
    test_exit_code_callback();
    test_callback_not_fired_for_other_osc();
    printf("test_exit_code: all passed\n");
    return 0;
}
