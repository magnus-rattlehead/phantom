#include "llama.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Call tracking
typedef enum { STUB_CALL_SEQ_RM, STUB_CALL_DECODE } stub_call_type;
typedef struct { stub_call_type type; int seq_id; } stub_call;

int       g_stub_seq_rm_count = 0;
int       g_stub_decode_count = 0;
stub_call g_stub_calls[64];
int       g_stub_n_calls = 0;

void stub_reset(void)
{
    g_stub_seq_rm_count = 0;
    g_stub_decode_count = 0;
    g_stub_n_calls      = 0;
}

void llama_backend_init(void) {}
void llama_backend_free(void) {}

struct llama_model_params llama_model_default_params(void)
{
    struct llama_model_params p;
    memset(&p, 0, sizeof p);
    return p;
}

struct llama_model *llama_model_load_from_file(
    const char *path, struct llama_model_params params)
{
    (void)path; (void)params;
    return (struct llama_model *)1;
}

void llama_model_free(struct llama_model *m) { (void)m; }

struct llama_context_params llama_context_default_params(void)
{
    struct llama_context_params p;
    memset(&p, 0, sizeof p);
    return p;
}

struct llama_context *llama_init_from_model(struct llama_model *model,
    struct llama_context_params params)
{
    (void)model; (void)params;
    return (struct llama_context *)1;
}

void llama_free(struct llama_context *ctx) { (void)ctx; }

const struct llama_vocab *llama_model_get_vocab(
    const struct llama_model *model)
{
    (void)model;
    return (const struct llama_vocab *)1;
}

struct llama_sampler_chain_params llama_sampler_chain_default_params(void)
{
    struct llama_sampler_chain_params p;
    memset(&p, 0, sizeof p);
    return p;
}

struct llama_sampler *llama_sampler_chain_init(
    struct llama_sampler_chain_params params)
{
    (void)params;
    return (struct llama_sampler *)1;
}

void llama_sampler_chain_add(struct llama_sampler *chain,
    struct llama_sampler *smpl)
{
    (void)chain; (void)smpl;
}

struct llama_sampler *llama_sampler_init_greedy(void)
{
    return (struct llama_sampler *)1;
}

struct llama_sampler *llama_sampler_init_penalties(
    int32_t penalty_last_n, float penalty_repeat,
    float penalty_freq, float penalty_present)
{
    (void)penalty_last_n; (void)penalty_repeat;
    (void)penalty_freq;   (void)penalty_present;
    return (struct llama_sampler *)1;
}

void llama_sampler_free(struct llama_sampler *smpl) { (void)smpl; }

#define STUB_EOG_TOKEN 2

llama_token llama_sampler_sample(struct llama_sampler *smpl,
    struct llama_context *ctx, int32_t idx)
{
    (void)smpl; (void)ctx; (void)idx;
    return STUB_EOG_TOKEN;  /* immediately stop generation */
}

void llama_sampler_accept(struct llama_sampler *smpl, llama_token token)
{
    (void)smpl; (void)token;
}


llama_memory_t llama_get_memory(const struct llama_context *ctx)
{
    (void)ctx;
    return (llama_memory_t)1;
}

void llama_memory_clear(llama_memory_t mem, bool data)
{
    (void)mem; (void)data;
}

bool llama_memory_seq_rm(llama_memory_t mem, llama_seq_id seq_id,
    llama_pos p0, llama_pos p1)
{
    (void)mem; (void)p0; (void)p1;
    g_stub_seq_rm_count++;
    if (g_stub_n_calls < 64) {
        g_stub_calls[g_stub_n_calls].type   = STUB_CALL_SEQ_RM;
        g_stub_calls[g_stub_n_calls].seq_id = (int)seq_id;
        g_stub_n_calls++;
    }
    return true;
}

void llama_memory_seq_cp(llama_memory_t mem,
    llama_seq_id seq_id_src, llama_seq_id seq_id_dst,
    llama_pos p0, llama_pos p1)
{
    (void)mem; (void)seq_id_src; (void)seq_id_dst; (void)p0; (void)p1;
}

int32_t llama_tokenize(const struct llama_vocab *vocab,
    const char *text, int32_t text_len,
    llama_token *tokens, int32_t n_tokens_max,
    bool add_special, bool parse_special)
{
    (void)vocab; (void)add_special; (void)parse_special;
    if (!text || text_len <= 0 || !tokens || n_tokens_max < 1) return -1;
    tokens[0] = 42;
    return 1;
}

bool llama_vocab_is_eog(const struct llama_vocab *vocab, llama_token token)
{
    (void)vocab;
    return (STUB_EOG_TOKEN == token);
}

llama_token llama_vocab_nl(const struct llama_vocab *vocab)
{
    (void)vocab;
    return 1;
}

int32_t llama_token_to_piece(const struct llama_vocab *vocab,
    llama_token token, char *buf, int32_t length,
    int32_t lstrip, bool special)
{
    (void)vocab; (void)token; (void)lstrip; (void)special;
    if (!buf || length < 2) return -1;
    buf[0] = 'x';
    buf[1] = '\0';
    return 1;
}


struct llama_batch llama_batch_get_one(llama_token *tokens, int32_t n_tokens)
{
    struct llama_batch b;
    memset(&b, 0, sizeof b);
    b.n_tokens = n_tokens;
    b.token    = tokens;
    return b;
}

struct llama_batch llama_batch_init(int32_t n_tokens, int32_t embd,
    int32_t n_seq_max)
{
    struct llama_batch b;
    memset(&b, 0, sizeof b);
    (void)embd;
    if (n_tokens <= 0) return b;
    b.token    = malloc(sizeof(llama_token)    * (size_t)n_tokens);
    b.pos      = malloc(sizeof(llama_pos)      * (size_t)n_tokens);
    b.n_seq_id = malloc(sizeof(int32_t)        * (size_t)n_tokens);
    b.seq_id   = malloc(sizeof(llama_seq_id *) * (size_t)n_tokens);
    for (int i = 0; i < n_tokens; i++)
        b.seq_id[i] = malloc(sizeof(llama_seq_id) * (size_t)n_seq_max);
    b.logits   = malloc(sizeof(int8_t)         * (size_t)n_tokens);
    return b;
}

void llama_batch_free(struct llama_batch batch)
{
    if (NULL == batch.seq_id) return;
    for (int i = 0; i < batch.n_tokens; i++)
        free(batch.seq_id[i]);
    free(batch.seq_id);
    free(batch.logits);
    free(batch.n_seq_id);
    free(batch.pos);
    free(batch.token);
}

int llama_decode(struct llama_context *ctx, struct llama_batch batch)
{
    (void)ctx; (void)batch;
    g_stub_decode_count++;
    if (g_stub_n_calls < 64) {
        g_stub_calls[g_stub_n_calls].type   = STUB_CALL_DECODE;
        g_stub_calls[g_stub_n_calls].seq_id = -1;
        g_stub_n_calls++;
    }
    return 0;
}

void llama_set_abort_callback(struct llama_context *ctx,
                               ggml_abort_callback cb, void *data)
{
    (void)ctx; (void)cb; (void)data;
}
