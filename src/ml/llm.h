#pragma once
#include <stddef.h>

typedef struct LLM LLM;

/* Loads a GGUF model and initialises the llama.cpp context with Metal
 * acceleration.  Returns NULL on failure (e.g. model file absent). */
LLM *llm_create(const char *model_path);

void llm_destroy(LLM *l);

/* Blocks until the background load thread finishes. Safe to call multiple
 * times or on NULL. Call before using pin/decode/complete_from_seq. */
void llm_wait_ready(LLM *l);

/* Decodes history_prefix into KV seq 0 and caches result. No-op when text
 * is unchanged. Returns token count on success, -1 on error. */
int llm_pin_history(LLM *l, const char *history_prefix);

/* Rewinds seq 0 past pinned history, then decodes suffix_text into KV.
 * Set *cancel = 1 from another thread to abort mid-decode. */
int llm_decode_suffix(LLM *l, const char *suffix_text, volatile int *cancel);

/* Generates tokens from the current seq-0+seq-1 KV state.
 * allow_nl=1: newlines pass through (multiline output); 0: stop at newline.
 * Returns heap-allocated string (caller frees), or NULL.
 * Set *cancel = 1 from another thread to abort. */
char *llm_complete_from_seq(LLM *l, size_t max_tokens, int allow_nl,
                            volatile int *cancel);

/* Continue generating from where the last llm_complete_from_seq (or
 * llm_continue) left off.  Returns NULL on EOS, cancel, or error. */
char *llm_continue(LLM *l, size_t max_tokens, int allow_nl,
                   volatile int *cancel);

/* Call once after all llm_destroy() calls to release the llama.cpp backend. */
void llm_backend_teardown(void);
