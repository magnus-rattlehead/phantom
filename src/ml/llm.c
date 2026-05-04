#include "llm.h"
#include "../config.h"

#include "llama.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ml_log.h"
#define LLM_LOG(...) ml_log("llm", __VA_ARGS__)

#define LLM_N_CTX 4096
#define LLM_TOK_BUF 2048
#define LLM_PIECE_BUF 16

struct LLM {
    struct llama_model *model;
    struct llama_context *ctx;
    struct llama_sampler *sampler;
    const struct llama_vocab *vocab;

    pthread_t load_thread;
    volatile int ready; /* set only after both model and ctx succeed */
    char *model_path_copy;

    int32_t history_n_tokens;
    int32_t suffix_n_tokens;
    char *history_text;

    /* Continuation state - valid after llm_complete_from_seq. */
    llama_pos gen_pos;
    llama_token gen_last_tok;
    int gen_done;
};

#if defined(__APPLE__) && PHANTOM_TESTING
#include <sys/qos.h>
volatile qos_class_t g_llm_load_qos = QOS_CLASS_UNSPECIFIED;
#endif

static void *load_worker(void *arg) {
    LLM *l = arg;

#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#if PHANTOM_TESTING
    g_llm_load_qos = qos_class_self();
#endif
#endif

    LLM_LOG("loading model: %s (gpu_layers=%d)", l->model_path_copy,
            LLM_GPU_LAYERS);

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = LLM_GPU_LAYERS;
    l->model = llama_model_load_from_file(l->model_path_copy, mp);
    if (!l->model) {
        fprintf(stderr, "llm: failed to load model: %s\n", l->model_path_copy);
        goto cleanup;
    }

    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = LLM_N_CTX;
    cp.n_seq_max = 1;
    cp.offload_kqv = true;
    l->ctx = llama_init_from_model(l->model, cp);
    if (!l->ctx) {
        fprintf(stderr, "llm: failed to create context\n");
        goto cleanup;
    }

    l->vocab = llama_model_get_vocab(l->model);

    l->sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
    /* repeat penalty (1.1) over last 64 tokens; greedy sampling */
    llama_sampler_chain_add(l->sampler,
                            llama_sampler_init_penalties(64, 1.1f, 0.0f, 0.0f));
    llama_sampler_chain_add(l->sampler, llama_sampler_init_greedy());

    LLM_LOG("warming up Metal pipelines...");
    {
        llama_memory_t mem = llama_get_memory(l->ctx);
        llama_token tok = 0;

        llama_token *hist_toks =
            malloc(LLM_WARMUP_HIST_TOKS * sizeof(llama_token));
        if (hist_toks) {
            for (int i = 0; i < LLM_WARMUP_HIST_TOKS; i++)
                hist_toks[i] = tok;
            struct llama_batch bhist =
                llama_batch_get_one(hist_toks, LLM_WARMUP_HIST_TOKS);
            (void)llama_decode(l->ctx, bhist);
            free(hist_toks);
        }

        llama_pos suf_pos = LLM_WARMUP_HIST_TOKS;
        for (int suf_n = LLM_WARMUP_SUF_MIN; suf_n <= LLM_WARMUP_SUF_MAX;
             suf_n++) {
            struct llama_batch bsuf = llama_batch_init(suf_n, 0, 1);
            bsuf.n_tokens = suf_n;
            for (int i = 0; i < suf_n; i++) {
                bsuf.token[i] = tok;
                bsuf.pos[i] = suf_pos + i;
                bsuf.n_seq_id[i] = 1;
                bsuf.seq_id[i][0] = 0;
                bsuf.logits[i] = (i == suf_n - 1) ? 1 : 0;
            }
            (void)llama_decode(l->ctx, bsuf);
            llama_batch_free(bsuf);
            suf_pos += suf_n;
        }

        struct llama_batch bgen = llama_batch_init(1, 0, 1);
        bgen.n_tokens = 1;
        bgen.token[0] = tok;
        bgen.pos[0] = suf_pos;
        bgen.n_seq_id[0] = 1;
        bgen.seq_id[0][0] = 0;
        bgen.logits[0] = 1;
        (void)llama_decode(l->ctx, bgen);
        llama_batch_free(bgen);

        llama_memory_clear(mem, false);
    }

    free(l->model_path_copy);
    l->model_path_copy = NULL;
    LLM_LOG("model ready (n_ctx=%d)", LLM_N_CTX);
    l->ready = 1;
    return NULL;

cleanup:
    if (l->model) {
        llama_model_free(l->model);
        l->model = NULL;
    }
    free(l->model_path_copy);
    l->model_path_copy = NULL;
    return NULL;
}

LLM *llm_create(const char *model_path) {
    if (!model_path || '\0' == model_path[0])
        return NULL;

    LLM *l = calloc(1, sizeof *l);
    if (!l)
        return NULL;

    l->model_path_copy = strdup(model_path);
    if (!l->model_path_copy) {
        free(l);
        return NULL;
    }

    llama_backend_init();

#if defined(__APPLE__)
    {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
        int rc = pthread_create(&l->load_thread, &attr, load_worker, l);
        pthread_attr_destroy(&attr);
        if (0 != rc) {
            fprintf(stderr, "llm: pthread_create failed\n");
            free(l->model_path_copy);
            free(l);
            llama_backend_free();
            return NULL;
        }
    }
#else
    if (0 != pthread_create(&l->load_thread, NULL, load_worker, l)) {
        fprintf(stderr, "llm: pthread_create failed\n");
        free(l->model_path_copy);
        free(l);
        llama_backend_free();
        return NULL;
    }
#endif

    return l;
}

void llm_wait_ready(LLM *l) {
    if (!l || !l->load_thread)
        return;
    pthread_join(l->load_thread, NULL);
    l->load_thread = 0;
}

void llm_destroy(LLM *l) {
    if (!l)
        return;
    if (l->load_thread)
        pthread_join(l->load_thread, NULL);
    if (l->sampler)
        llama_sampler_free(l->sampler);
    if (l->ctx)
        llama_free(l->ctx);
    if (l->model)
        llama_model_free(l->model);
    free(l->history_text);
    free(l->model_path_copy);
    free(l);
}

void llm_backend_teardown(void) { llama_backend_free(); }

/* Tokenizes text into a freshly malloc'd buffer.
 * Caller must free *out_toks on success (n > 0).
 * Returns token count, or -1 on OOM / empty tokenization. */
static int tokenize_text(const struct llama_vocab *vocab, const char *text,
                         llama_token **out_toks) {
    llama_token *toks = malloc((size_t)LLM_TOK_BUF * sizeof(llama_token));
    if (!toks)
        return -1;
    int n = llama_tokenize(vocab, text, (int32_t)strlen(text), toks,
                           LLM_TOK_BUF, /*add_special=*/false,
                           /*parse_special=*/true);
    if (n <= 0) {
        free(toks);
        return -1;
    }
    *out_toks = toks;
    return n;
}

/* KV layout: history at 0..H-1, suffix at H..H+S-1.  Rewind = seq_rm(0,H,-1).
 */

int llm_pin_history(LLM *l, const char *history_prefix) {
    if (!l || !l->ready || !history_prefix)
        return -1;

    llama_memory_t mem = llama_get_memory(l->ctx);
    int n;

    if (l->history_text && 0 == strcmp(l->history_text, history_prefix)) {
        LLM_LOG("pin_history: cache hit (%d tokens)", l->history_n_tokens);
        n = l->history_n_tokens;
    } else {
        llama_memory_seq_rm(mem, 0, 0, -1);

        llama_token *toks = NULL;
        n = tokenize_text(l->vocab, history_prefix, &toks);
        if (n < 0)
            return -1;

        struct llama_batch batch = llama_batch_get_one(toks, n);
        int rc = llama_decode(l->ctx, batch);
        free(toks);
        if (0 != rc)
            return -1;

        l->history_n_tokens = n;
        l->suffix_n_tokens = 0;
        free(l->history_text);
        l->history_text = strdup(history_prefix);
        LLM_LOG("pin_history: pinned %d tokens", n);

        /* Probe the batch_init path so decode_suffix starts warm. */
        {
            llama_memory_t mem = llama_get_memory(l->ctx);
            struct llama_batch w =
                llama_batch_init(LLM_WARMUP_DUMMY_TOKS, 0, 1);
            w.n_tokens = LLM_WARMUP_DUMMY_TOKS;
            for (int i = 0; i < LLM_WARMUP_DUMMY_TOKS; i++) {
                w.token[i] = 0;
                w.pos[i] = (llama_pos)n + i;
                w.n_seq_id[i] = 1;
                w.seq_id[i][0] = 0;
                w.logits[i] = (i == LLM_WARMUP_DUMMY_TOKS - 1) ? 1 : 0;
            }
            (void)llama_decode(l->ctx, w);
            llama_batch_free(w);
            /* Remove dummy tokens so KV cache is clean after pin_history. */
            llama_memory_seq_rm(mem, 0, (llama_pos)n, -1);
        }
    }

    return n;
}

int llm_decode_suffix(LLM *l, const char *suffix_text, volatile int *cancel) {
    if (!l || !l->ready || !suffix_text)
        return -1;
    if (cancel && *cancel)
        return -1;

    llama_memory_t mem = llama_get_memory(l->ctx);
    llama_memory_seq_rm(mem, 0, (llama_pos)l->history_n_tokens, -1);

    if ('\0' == *suffix_text) {
        l->suffix_n_tokens = 0;
        return 0;
    }

    llama_token *toks = NULL;
    int n = tokenize_text(l->vocab, suffix_text, &toks);
    if (n < 0)
        return -1;

    struct llama_batch batch = llama_batch_init(n, 0, 1);
    batch.n_tokens = n;
    llama_pos start = (llama_pos)l->history_n_tokens;
    for (int i = 0; i < n; i++) {
        batch.token[i] = toks[i];
        batch.pos[i] = start + i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i] = (i == n - 1) ? 1 : 0;
    }
    int rc = llama_decode(l->ctx, batch);
    llama_batch_free(batch);
    free(toks);
    if (0 != rc) {
        llama_memory_seq_rm(mem, 0, (llama_pos)l->history_n_tokens, -1);
        l->suffix_n_tokens = 0;
        return -1;
    }

    l->suffix_n_tokens = n;
    LLM_LOG("decode_suffix: %d tokens at pos %d", n, l->history_n_tokens);
    return n;
}

/* Updates l->gen_pos, l->gen_last_tok, l->gen_done on return. */
static char *gen_loop(LLM *l, llama_pos start_pos, size_t max_tokens,
                      int allow_nl, volatile int *cancel) {
    llama_pos next_pos = start_pos;
    llama_token nl_tok = llama_vocab_nl(l->vocab);
    char piece[LLM_PIECE_BUF * 2];

    size_t result_cap = max_tokens * LLM_PIECE_BUF + 1;
    char *result = malloc(result_cap);
    if (!result)
        return NULL;
    size_t result_len = 0;
    result[0] = '\0';

    struct llama_batch next = llama_batch_init(1, 0, 1);
    next.n_tokens = 1;
    next.n_seq_id[0] = 1;
    next.seq_id[0][0] = 0;
    next.logits[0] = 1;

    l->gen_done = 0;

    for (size_t i = 0; i < max_tokens; i++) {
        if (cancel && *cancel) {
            l->gen_done = 1;
            break;
        }

        llama_token tok = llama_sampler_sample(l->sampler, l->ctx, -1);
        llama_sampler_accept(l->sampler, tok);

        if (llama_vocab_is_eog(l->vocab, tok)) {
            l->gen_done = 1;
            break;
        }
        if (!allow_nl && tok == nl_tok) {
            l->gen_done = 1;
            break;
        }

        int n_piece =
            llama_token_to_piece(l->vocab, tok, piece, (int32_t)sizeof piece,
                                 /*lstrip=*/0, /*special=*/false);
        if (n_piece <= 0) {
            l->gen_done = 1;
            break;
        }

        if (result_len + (size_t)n_piece + 1 > result_cap) {
            result_cap *= 2;
            char *nb = realloc(result, result_cap);
            if (!nb) {
                l->gen_done = 1;
                break;
            }
            result = nb;
        }
        memcpy(result + result_len, piece, (size_t)n_piece);
        result_len += (size_t)n_piece;
        result[result_len] = '\0';

        l->gen_last_tok = tok;
        l->gen_pos = next_pos + 1;

        next.token[0] = tok;
        next.pos[0] = next_pos++;
        if (0 != llama_decode(l->ctx, next)) {
            l->gen_done = 1;
            break;
        }
    }
    llama_batch_free(next);

    if (0 == result_len) {
        free(result);
        return NULL;
    }
    return result;
}

char *llm_complete_from_seq(LLM *l, size_t max_tokens, int allow_nl,
                            volatile int *cancel) {
    if (!l || !l->ready)
        return NULL;

    llama_pos start = (llama_pos)(l->history_n_tokens + l->suffix_n_tokens);

    char *result = gen_loop(l, start, max_tokens, allow_nl, cancel);
    LLM_LOG("complete_from_seq: %s -> \"%s\"", result ? "ok" : "NULL",
            result ? result : "");
    return result;
}

char *llm_continue(LLM *l, size_t max_tokens, int allow_nl,
                   volatile int *cancel) {
    if (!l || !l->ready || l->gen_done)
        return NULL;

    char *result = gen_loop(l, l->gen_pos, max_tokens, allow_nl, cancel);
    LLM_LOG("llm_continue: %s -> \"%s\"", result ? "ok" : "NULL",
            result ? result : "");
    return result;
}
