#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct Autocomplete Autocomplete;
typedef struct History History;
typedef struct LLM LLM;
typedef struct TerminalState TerminalState;
typedef struct Terminal Terminal;
typedef struct CCG CCG;

/* Creates the autocomplete coordinator.
 * llm_base: base model for FIM completions; llm_instruct: instruct model for
 * # NL queries.  Either (or both) may be NULL to disable that feature.
 * ts, term, and ccg may also be NULL. */
Autocomplete *autocomplete_create(History *history, LLM *llm_base,
                                  LLM *llm_instruct, TerminalState *ts,
                                  Terminal *term, CCG *ccg);
void autocomplete_destroy(Autocomplete *ac);

/* Registers the SDL user event type that the worker pushes when a suggestion
 * is ready.  Must be called before the first autocomplete_query. */
void autocomplete_set_event_type(uint32_t event_type);

/* Triggers an async suggestion fetch for current_line. */
void autocomplete_query(Autocomplete *ac, const char *current_line);

#if PHANTOM_TESTING
/* Blocks until the worker has finished processing all submitted queries. */
void autocomplete_drain(Autocomplete *ac);
#endif

/* Returns the cached suggestion string, or NULL if none ready. Owned by ac. */
const char *autocomplete_get_suggestion(const Autocomplete *ac);

/* Discards the cached suggestion. */
void autocomplete_clear(Autocomplete *ac);

/* Records a completed command transition into the CCG.
 * Computes state hash from current CWD + previous command + exit_code.
 * No-op when ccg was NULL at creation time.  Call from main thread on Enter. */
void autocomplete_record_command(Autocomplete *ac, const char *cmd,
                                 int exit_code);

/* Signals the fsprobe thread to re-probe CWD, git branch, and fs map.
 * Call after an OSC 7 CWD update to refresh the filesystem listing. */
void autocomplete_request_env_probe(Autocomplete *ac);
