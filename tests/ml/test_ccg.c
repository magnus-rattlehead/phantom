#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/ml/ccg.h"

static void test_hash_deterministic(void)
{
    uint64_t h1 = ccg_hash_state("/home/user", "ls", 0);
    uint64_t h2 = ccg_hash_state("/home/user", "ls", 0);
    assert(h1 == h2);
}

static void test_hash_different_cwd(void)
{
    uint64_t h1 = ccg_hash_state("/home/user", "ls", 0);
    uint64_t h2 = ccg_hash_state("/tmp",       "ls", 0);
    assert(h1 != h2);
}

static void test_hash_different_cmd(void)
{
    uint64_t h1 = ccg_hash_state("/home/user", "ls",    0);
    uint64_t h2 = ccg_hash_state("/home/user", "ls -l", 0);
    assert(h1 != h2);
}

static void test_hash_different_exit(void)
{
    uint64_t h1 = ccg_hash_state("/home/user", "git push", 0);
    uint64_t h2 = ccg_hash_state("/home/user", "git push", 128);
    assert(h1 != h2);
}

static void test_lookup_empty(void)
{
    CCG *g = ccg_create();
    assert(NULL != g);
    uint32_t freq = 0;
    const char *s = ccg_lookup(g, 0xdeadbeefULL, &freq);
    assert(NULL == s);
    ccg_destroy(g);
}

static void test_train_and_lookup_below_threshold(void)
{
    CCG *g = ccg_create();
    uint64_t h = ccg_hash_state("/home/user", "ls", 0);

    /* train once -> freq=1, below CCG_INSTANT_THRESHOLD */
    assert(0 == ccg_train(g, h, "git status"));

    uint32_t freq = 0;
    const char *s = ccg_lookup(g, h, &freq);
    assert(NULL == s);
    assert(1 == freq);

    ccg_destroy(g);
}

static void test_lookup_above_threshold(void)
{
    CCG *g = ccg_create();
    uint64_t h = ccg_hash_state("/home/user", "ls", 0);

    for (int i = 0; i < CCG_INSTANT_THRESHOLD; i++)
        assert(0 == ccg_train(g, h, "git status"));

    uint32_t freq = 0;
    const char *s = ccg_lookup(g, h, &freq);
    assert(NULL != s);
    assert(0 == strcmp(s, "git status"));
    assert((uint32_t)CCG_INSTANT_THRESHOLD == freq);

    ccg_destroy(g);
}

static void test_lookup_returns_highest_freq(void)
{
    CCG *g = ccg_create();
    uint64_t h = ccg_hash_state("/tmp", "make", 0);

    for (int i = 0; i < 5; i++) ccg_train(g, h, "make test");
    for (int i = 0; i < 3; i++) ccg_train(g, h, "make clean");

    uint32_t freq = 0;
    const char *s = ccg_lookup(g, h, &freq);
    assert(NULL != s);
    assert(0 == strcmp(s, "make test"));
    assert(5 == freq);

    ccg_destroy(g);
}

static void test_top_k_ordering(void)
{
    CCG *g = ccg_create();
    uint64_t h = ccg_hash_state("/repo", "cd src", 0);

    for (int i = 0; i < 5; i++) ccg_train(g, h, "make");
    for (int i = 0; i < 3; i++) ccg_train(g, h, "git status");
    for (int i = 0; i < 1; i++) ccg_train(g, h, "ls");

    const char *out[3];
    int n = ccg_top_k(g, h, out, 3);
    assert(3 == n);
    assert(0 == strcmp(out[0], "make"));
    assert(0 == strcmp(out[1], "git status"));
    assert(0 == strcmp(out[2], "ls"));

    ccg_destroy(g);
}

static void test_top_k_fewer_than_k(void)
{
    CCG *g = ccg_create();
    uint64_t h = ccg_hash_state("/repo", "cd lib", 0);
    ccg_train(g, h, "ls");
    ccg_train(g, h, "ls");

    const char *out[5];
    int n = ccg_top_k(g, h, out, 5);
    assert(1 == n);
    assert(0 == strcmp(out[0], "ls"));

    ccg_destroy(g);
}

static void test_top_k_no_match(void)
{
    CCG *g = ccg_create();
    const char *out[3];
    int n = ccg_top_k(g, 0x1234567890abcdefULL, out, 3);
    assert(0 == n);
    ccg_destroy(g);
}

static void test_prune_removes_freq1(void)
{
    CCG *g = ccg_create();
    uint64_t h = ccg_hash_state("/tmp", "echo", 0);
    ccg_train(g, h, "rm -rf");

    ccg_prune(g);

    uint32_t freq = 0;
    const char *s = ccg_lookup(g, h, &freq);
    assert(NULL == s);
    assert(0 == freq);

    ccg_destroy(g);
}

static void test_prune_keeps_freq2(void)
{
    CCG *g = ccg_create();
    uint64_t h = ccg_hash_state("/tmp", "echo", 0);
    ccg_train(g, h, "rm -rf");
    ccg_train(g, h, "rm -rf");

    ccg_prune(g);

    uint32_t freq = 0;
    ccg_lookup(g, h, &freq);
    assert(2 == freq);

    ccg_destroy(g);
}

static void test_prune_partial(void)
{
    CCG *g = ccg_create();
    uint64_t h = ccg_hash_state("/home", "cd", 0);

    ccg_train(g, h, "ls");
    for (int i = 0; i < 3; i++)
        ccg_train(g, h, "git log");

    ccg_prune(g);

    /* "git log" still reachable */
    uint32_t freq = 0;
    const char *s = ccg_lookup(g, h, &freq);
    assert(NULL != s);
    assert(0 == strcmp(s, "git log"));

    /* top_k should only return "git log", not "ls" */
    const char *out[5];
    int n = ccg_top_k(g, h, out, 5);
    assert(1 == n);
    assert(0 == strcmp(out[0], "git log"));

    ccg_destroy(g);
}

static void test_overflow_edges(void)
{
    CCG *g = ccg_create();
    uint64_t h = ccg_hash_state("/x", "y", 0);

    char cmd[32];
    for (int i = 0; i <= CCG_MAX_EDGES; i++) {
        snprintf(cmd, sizeof cmd, "cmd_%d", i);
        ccg_train(g, h, cmd);
    }

    /* top_k must return at most CCG_MAX_EDGES results */
    const char *out[CCG_MAX_EDGES + 1];
    int n = ccg_top_k(g, h, out, CCG_MAX_EDGES + 1);
    assert(n <= CCG_MAX_EDGES);

    ccg_destroy(g);
}

int main(void)
{
    test_hash_deterministic();
    test_hash_different_cwd();
    test_hash_different_cmd();
    test_hash_different_exit();
    test_lookup_empty();
    test_train_and_lookup_below_threshold();
    test_lookup_above_threshold();
    test_lookup_returns_highest_freq();
    test_top_k_ordering();
    test_top_k_fewer_than_k();
    test_top_k_no_match();
    test_prune_removes_freq1();
    test_prune_keeps_freq2();
    test_prune_partial();
    test_overflow_edges();
    printf("test_ccg: all passed\n");
    return 0;
}
