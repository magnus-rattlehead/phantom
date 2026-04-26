#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../src/ml/autocomplete.h"
#include "../../src/ml/ccg.h"
#include "../../src/ml/history.h"
#include "../../src/config.h"

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

static void test_record_increments_edge(void)
{
    CCG *g = ccg_create();
    Autocomplete *ac = autocomplete_create(NULL, NULL, NULL, NULL, NULL, g);
    assert(NULL != ac);

    autocomplete_record_command(ac, "git status", 0);
    autocomplete_record_command(ac, "git status", 0);

    /* State hash for (cwd="", last_cmd="", exit_code=0) → first call.
     * After first call last_cmd becomes "git status".
     * So second call hashes (cwd="", last_cmd="git status", exit=0). */
    uint64_t h0 = ccg_hash_state("", "", 0);
    uint32_t freq = 0;
    ccg_lookup(g, h0, &freq);
    assert(1 == freq);

    uint64_t h1 = ccg_hash_state("", "git status", 0);
    ccg_lookup(g, h1, &freq);
    assert(1 == freq);

    autocomplete_destroy(ac);
    ccg_destroy(g);
}

static void test_last_cmd_rotates(void)
{
    CCG *g = ccg_create();
    Autocomplete *ac = autocomplete_create(NULL, NULL, NULL, NULL, NULL, g);

    autocomplete_record_command(ac, "ls", 0);
    autocomplete_record_command(ac, "git log", 0);

    /* Second transition: state = (cwd="", last_cmd="ls", exit=0) → "git log" */
    uint64_t h = ccg_hash_state("", "ls", 0);
    uint32_t freq = 0;
    ccg_lookup(g, h, &freq);
    assert(1 == freq);

    autocomplete_destroy(ac);
    ccg_destroy(g);
}

static void test_nonzero_exit_still_trains(void)
{
    CCG *g = ccg_create();
    Autocomplete *ac = autocomplete_create(NULL, NULL, NULL, NULL, NULL, g);

    /* Failed command still adds an edge. */
    autocomplete_record_command(ac, "git push", 128);

    uint64_t h = ccg_hash_state("", "", 128);
    uint32_t freq = 0;
    ccg_lookup(g, h, &freq);
    assert(1 == freq);

    autocomplete_destroy(ac);
    ccg_destroy(g);
}

static void test_prune_triggered_at_interval(void)
{
    CCG *g = ccg_create();
    Autocomplete *ac = autocomplete_create(NULL, NULL, NULL, NULL, NULL, g);

    /* Fill up CCG_PRUNE_INTERVAL edges, each trained once (freq=1). */
    char cmd[32];
    for (int i = 0; i < CCG_PRUNE_INTERVAL; i++) {
        snprintf(cmd, sizeof cmd, "cmd_%d", i);
        autocomplete_record_command(ac, cmd, 0);
    }

    /* After CCG_PRUNE_INTERVAL records the prune fires.
     * All freq=1 edges should be gone. */
    uint64_t h = ccg_hash_state("", "", 0);
    uint32_t freq = 0;
    ccg_lookup(g, h, &freq);
    /* freq=0 means node was pruned (only had freq=1 edge). */
    assert(0 == freq);

    autocomplete_destroy(ac);
    ccg_destroy(g);
}

static void test_high_freq_edge_survives_prune(void)
{
    CCG *g = ccg_create();
    Autocomplete *ac = autocomplete_create(NULL, NULL, NULL, NULL, NULL, g);

    /* Train "make" enough times to survive pruning. */
    uint64_t target = ccg_hash_state("", "", 0);
    for (int i = 0; i < 3; i++)
        ccg_train(g, target, "make");

    char cmd[32];
    for (int i = 0; i < CCG_PRUNE_INTERVAL; i++) {
        snprintf(cmd, sizeof cmd, "noise_%d", i);
        autocomplete_record_command(ac, cmd, 0);
    }

    uint32_t freq = 0;
    ccg_lookup(g, target, &freq);
    assert(3 == freq);

    autocomplete_destroy(ac);
    ccg_destroy(g);
}

static void test_null_ccg_no_crash(void)
{
    /* autocomplete_record_command must be a no-op when ccg is NULL. */
    Autocomplete *ac = autocomplete_create(NULL, NULL, NULL, NULL, NULL, NULL);
    assert(NULL != ac);
    autocomplete_record_command(ac, "ls", 0);
    autocomplete_destroy(ac);
}

int main(void)
{
    (void)make_history;   /* suppress unused-function warning */
    test_record_increments_edge();
    test_last_cmd_rotates();
    test_nonzero_exit_still_trains();
    test_prune_triggered_at_interval();
    test_high_freq_edge_survives_prune();
    test_null_ccg_no_crash();
    printf("test_ccg_training: all passed\n");
    return 0;
}
