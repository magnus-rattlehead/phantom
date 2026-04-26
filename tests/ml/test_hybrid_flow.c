#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../src/ml/autocomplete.h"
#include "../../src/ml/ccg.h"

typedef struct History      History;
typedef struct Terminal     Terminal;
typedef struct TerminalState TerminalState;

extern size_t build_fim_prefix(History *history, Terminal *term,
                               TerminalState *ts,
                               CCG *ccg, const char *last_cmd,
                               const char *query,
                               char *out, size_t out_cap);

static void train_above_threshold(CCG *g, uint64_t h, const char *cmd)
{
    for (int i = 0; i < CCG_INSTANT_THRESHOLD; i++)
        ccg_train(g, h, cmd);
}

static void test_instant_suggestion_no_llm(void)
{
    CCG *g = ccg_create();
    uint64_t h = ccg_hash_state("", "", -1);
    train_above_threshold(g, h, "make");

    Autocomplete *ac = autocomplete_create(NULL, NULL, NULL, NULL, NULL, g);
    assert(NULL != ac);

    assert(NULL == autocomplete_get_suggestion(ac));

    autocomplete_query(ac, "");
    autocomplete_drain(ac);

    const char *sug = autocomplete_get_suggestion(ac);
    assert(NULL != sug);
    assert(0 == strcmp("make", sug));

    autocomplete_destroy(ac);
    ccg_destroy(g);
}

static void test_no_instant_below_threshold(void)
{
    CCG *g = ccg_create();
    uint64_t h = ccg_hash_state("", "", -1);
    ccg_train(g, h, "make");

    Autocomplete *ac = autocomplete_create(NULL, NULL, NULL, NULL, NULL, g);
    assert(NULL != ac);

    autocomplete_query(ac, "");
    autocomplete_drain(ac);

    /* ccg_lookup returns NULL below threshold → no instant suggestion. */
    const char *sug = autocomplete_get_suggestion(ac);
    assert(NULL == sug);

    autocomplete_destroy(ac);
    ccg_destroy(g);
}

static void test_no_ccg_falls_through(void)
{
    Autocomplete *ac = autocomplete_create(NULL, NULL, NULL, NULL, NULL, NULL);
    assert(NULL != ac);

    autocomplete_query(ac, "zzz_unknown_cmd arg");
    autocomplete_drain(ac);

    const char *sug = autocomplete_get_suggestion(ac);
    assert(NULL == sug);

    autocomplete_destroy(ac);
}

static void test_ccg_enrichment_in_prefix(void)
{
    CCG *g = ccg_create();
    uint64_t h = ccg_hash_state("", "ls", -1);
    train_above_threshold(g, h, "git status");
    ccg_train(g, h, "git log");
    ccg_train(g, h, "git log");

    char out[2048] = {0};
    size_t len = build_fim_prefix(NULL, NULL, NULL, g, "ls", NULL, out, sizeof out);
    assert(len > 0);

    assert(NULL != strstr(out, "[LIKELY]:"));
    assert(NULL != strstr(out, "git status"));

    ccg_destroy(g);
}

static void test_prefix_no_hint_when_ccg_null(void)
{
    char out[512] = {0};
    size_t len = build_fim_prefix(NULL, NULL, NULL, NULL, NULL, NULL, out, sizeof out);
    assert(len > 0);
    assert(NULL == strstr(out, "[LIKELY]:"));
    assert(NULL != strstr(out, "<|fim_prefix|>"));
}

static void test_prefix_no_hint_below_threshold(void)
{
    CCG *g = ccg_create();
    uint64_t h = ccg_hash_state("", "", -1);
    ccg_train(g, h, "make");

    char out[512] = {0};
    build_fim_prefix(NULL, NULL, NULL, g, "", NULL, out, sizeof out);

    ccg_destroy(g);
}

int main(void)
{
    test_instant_suggestion_no_llm();
    test_no_instant_below_threshold();
    test_no_ccg_falls_through();
    test_ccg_enrichment_in_prefix();
    test_prefix_no_hint_when_ccg_null();
    test_prefix_no_hint_below_threshold();
    printf("test_hybrid_flow: all passed\n");
    return 0;
}
