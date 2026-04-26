#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../src/ml/history.h"

/* Opens history backed by a controlled write_fp (no zsh_history read). */
static History *open_isolated(const char *path)
{
    char *saved = NULL;
    const char *home = getenv("HOME");
    if (home) saved = strdup(home);
    unsetenv("HOME");
    History *h = history_open(path);
    if (saved) { setenv("HOME", saved, 1); free(saved); }
    return h;
}

static void test_null_path(void)
{
    History *h = history_open(NULL);
    assert(h != NULL);
    history_append(h, "cmd");
    char *out[4];
    size_t n = history_recent(h, out, 4);
    assert(0 == n);
    history_close(h);
}

static void test_append_and_recent(void)
{
    const char *path = "/tmp/phantom_test_history.txt";
    unlink(path);
    History *h = open_isolated(path);
    assert(h != NULL);

    history_append(h, "ls -la");
    history_append(h, "cd /tmp");
    history_append(h, "echo hello");

    char *out[10];
    size_t n = history_recent(h, out, 10);
    assert(3 == n);
    assert(0 == strcmp(out[0], "echo hello"));
    assert(0 == strcmp(out[1], "cd /tmp"));
    assert(0 == strcmp(out[2], "ls -la"));

    for (size_t i = 0; i < n; i++) free(out[i]);
    history_close(h);
    unlink(path);
}

static void test_recent_limit(void)
{
    const char *path = "/tmp/phantom_test_history2.txt";
    unlink(path);

    History *h = open_isolated(path);
    history_append(h, "a");
    history_append(h, "b");
    history_append(h, "c");
    history_append(h, "d");
    history_append(h, "e");

    char *out[3];
    size_t n = history_recent(h, out, 3);
    assert(3 == n);
    assert(0 == strcmp(out[0], "e"));
    assert(0 == strcmp(out[1], "d"));
    assert(0 == strcmp(out[2], "c"));

    for (size_t i = 0; i < n; i++) free(out[i]);
    history_close(h);
    unlink(path);
}

int main(void)
{
    test_null_path();
    test_append_and_recent();
    test_recent_limit();
    printf("test_history: all passed\n");
    return 0;
}
