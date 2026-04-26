#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../src/ml/llm.h"

/* Globals exported from llama_stub.c. */
typedef enum { STUB_CALL_SEQ_RM, STUB_CALL_DECODE } stub_call_type;
typedef struct { stub_call_type type; int seq_id; } stub_call;

extern int       g_stub_seq_rm_count;
extern int       g_stub_decode_count;
extern stub_call g_stub_calls[64];
extern int       g_stub_n_calls;
extern void      stub_reset(void);


static LLM *make_llm(void)
{
    LLM *l = llm_create("fake_model");
    assert(NULL != l);
    llm_wait_ready(l);
    stub_reset();
    return l;
}


static void test_pin_noop_same_text(void)
{
    LLM *l = make_llm();

    llm_pin_history(l, "$ ls\n$ ");
    int rm_after_first = g_stub_seq_rm_count;
    assert(rm_after_first > 0);

    llm_pin_history(l, "$ ls\n$ ");
    assert(g_stub_seq_rm_count == rm_after_first);

    llm_destroy(l);
}

static void test_pin_clears_on_history_change(void)
{
    LLM *l = make_llm();

    llm_pin_history(l, "prefix A");
    assert(1 == g_stub_seq_rm_count);

    stub_reset();
    llm_pin_history(l, "prefix B");
    assert(1 == g_stub_seq_rm_count);

    llm_destroy(l);
}

static void test_suffix_cleared_before_decode(void)
{
    LLM *l = make_llm();

    llm_pin_history(l, "history");

    stub_reset();
    llm_decode_suffix(l, "git ", NULL);

    /* seq_rm(seq_id=1) must come before the first decode. */
    assert(g_stub_n_calls >= 2);
    assert(STUB_CALL_SEQ_RM == g_stub_calls[0].type);
    assert(0                 == g_stub_calls[0].seq_id);
    assert(STUB_CALL_DECODE  == g_stub_calls[1].type);

    llm_destroy(l);
}

int main(void)
{
    test_pin_noop_same_text();
    test_pin_clears_on_history_change();
    test_suffix_cleared_before_decode();
    printf("test_kv_cache: all passed\n");
    return 0;
}
