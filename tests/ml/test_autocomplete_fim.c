#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../src/terminal.h"
#include "../../src/ml/autocomplete.h"
#include "../../src/ml/context.h"
#include "../../src/ml/history.h"

/* build_fim_prefix is non-static in autocomplete.c for testability. */
typedef struct CCG CCG;
extern size_t build_fim_prefix(History *history, Terminal *term,
                                TerminalState *ts,
                                CCG *ccg, const char *last_cmd,
                                const char *query,
                                char *out, size_t out_cap);


static History *make_history(const char *path)
{
    unlink(path);
    char *saved = NULL;
    const char *home = getenv("HOME");
    if (home) saved = strdup(home);
    unsetenv("HOME");
    History *h = history_open(path);
    if (saved) { setenv("HOME", saved, 1); free(saved); }
    return h;
}


static void test_prefix_starts_with_fim_token(void)
{
    char out[256];
    size_t n = build_fim_prefix(NULL, NULL, NULL, NULL, NULL, NULL, out, sizeof out);
    assert(n > 0);
    assert(0 == strncmp(out, "<|fim_prefix|>", 14));
}

static void test_prefix_ends_with_input_marker(void)
{
    char out[256];
    size_t n = build_fim_prefix(NULL, NULL, NULL, NULL, NULL, NULL, out, sizeof out);
    assert(n >= 9);
    assert(0 == strcmp(out + n - 9, "[INPUT]: "));
}

static void test_no_error_context_when_exit_minus_one(void)
{
    /* Fresh terminal: exit_code == -1 → no error injection. */
    Terminal *term = terminal_create(80, 24);
    TerminalState *ts = terminal_state_create(NULL);

    char out[4096];
    build_fim_prefix(NULL, term, ts, NULL, NULL, NULL, out, sizeof out);

    assert(NULL == strstr(out, "[STDERR]:"));

    terminal_state_destroy(ts);
    terminal_destroy(term);
}

static void test_no_error_context_when_exit_zero(void)
{
    Terminal *term = terminal_create(80, 24);
    /* Feed a successful (exit 0) OSC 133;D sequence. */
    terminal_feed(term, "\x1b]133;D;0\x07", 10);
    TerminalState *ts = terminal_state_create(NULL);

    char out[4096];
    build_fim_prefix(NULL, term, ts, NULL, NULL, NULL, out, sizeof out);

    assert(NULL == strstr(out, "[STDERR]:"));

    terminal_state_destroy(ts);
    terminal_destroy(term);
}

static void test_error_context_injected_on_nonzero_exit(void)
{
    Terminal *term = terminal_create(80, 24);
    terminal_feed(term, "\x1b]133;D;1\x07", 10);

    TerminalState *ts = terminal_state_create(term);

    char out[4096];
    build_fim_prefix(NULL, term, ts, NULL, NULL, NULL, out, sizeof out);

    assert(NULL != strstr(out, "[LAST_CMD]:"));
    assert(NULL != strstr(out, "exit 1"));

    terminal_state_destroy(ts);
    terminal_destroy(term);
}

static void test_error_context_before_history(void)
{
    const char *path = "/tmp/phantom_test_fim.txt";
    History *h = make_history(path);
    history_append(h, "git status");

    Terminal *term = terminal_create(80, 24);
    terminal_feed(term, "\x1b]133;D;2\x07", 10);
    TerminalState *ts = terminal_state_create(term);

    char out[4096];
    build_fim_prefix(h, term, ts, NULL, NULL, NULL, out, sizeof out);

    /* Error context must appear before history lines. */
    char *err_pos  = strstr(out, "[LAST_CMD]:");
    char *hist_pos = strstr(out, "$ git status");
    assert(NULL != err_pos);
    assert(NULL != hist_pos);
    assert(err_pos < hist_pos);

    terminal_state_destroy(ts);
    terminal_destroy(term);
    history_close(h);
    unlink(path);
}

static void test_history_lines_included(void)
{
    const char *path = "/tmp/phantom_test_fim2.txt";
    History *h = make_history(path);
    history_append(h, "ls -la");
    history_append(h, "cd /tmp");

    char out[4096];
    build_fim_prefix(h, NULL, NULL, NULL, NULL, NULL, out, sizeof out);

    assert(NULL != strstr(out, "$ ls -la\n"));
    assert(NULL != strstr(out, "$ cd /tmp\n"));

    history_close(h);
    unlink(path);
}

static void test_ansi_stripped_in_error_context(void)
{
    Terminal *term = terminal_create(80, 24);
    /* Feed some text with ANSI color codes, then mark exit 1. */
    terminal_feed(term, "\x1b[31merror: file not found\x1b[0m\r\n", 31);
    terminal_feed(term, "\x1b]133;D;1\x07", 10);

    TerminalState *ts = terminal_state_create(term);

    char out[4096];
    build_fim_prefix(NULL, term, ts, NULL, NULL, NULL, out, sizeof out);

    /* ANSI escapes must not appear in the injected error context. */
    assert(NULL == strstr(out, "\x1b[31m"));
    assert(NULL == strstr(out, "\x1b[0m"));
    /* Visible text must survive stripping. */
    assert(NULL != strstr(out, "error: file not found"));

    terminal_state_destroy(ts);
    terminal_destroy(term);
}

static void test_null_inputs_no_crash(void)
{
    char out[64];
    size_t n = build_fim_prefix(NULL, NULL, NULL, NULL, NULL, NULL, out, sizeof out);
    assert(n > 0);
    assert('\0' == out[n]);
}

static void test_tiny_buffer_no_overflow(void)
{
    char out[4];  /* absurdly small */
    size_t n = build_fim_prefix(NULL, NULL, NULL, NULL, NULL, NULL, out, sizeof out);
    assert(n < sizeof out);
    assert('\0' == out[n]);
}

int main(void)
{
    test_prefix_starts_with_fim_token();
    test_prefix_ends_with_input_marker();
    test_no_error_context_when_exit_minus_one();
    test_no_error_context_when_exit_zero();
    test_error_context_injected_on_nonzero_exit();
    test_error_context_before_history();
    test_history_lines_included();
    test_ansi_stripped_in_error_context();
    test_null_inputs_no_crash();
    test_tiny_buffer_no_overflow();
    printf("test_autocomplete_fim: all passed\n");
    return 0;
}
