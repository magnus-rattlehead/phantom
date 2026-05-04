#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct Autocomplete Autocomplete;
typedef struct History History;
typedef struct LLM LLM;
typedef struct TerminalState TerminalState;
typedef struct Terminal Terminal;
typedef struct CCG CCG;

/* llm_base drives FIM completions; llm_instruct drives # NL queries.
 * Either may be NULL to disable that path. ts, term, ccg may be NULL. */
Autocomplete *autocomplete_create(History *history, LLM *llm_base,
                                  LLM *llm_instruct, TerminalState *ts,
                                  Terminal *term, CCG *ccg);
void autocomplete_destroy(Autocomplete *ac);

/* Must be called before the first autocomplete_query. */
void autocomplete_set_event_type(uint32_t event_type);

void autocomplete_query(Autocomplete *ac, const char *current_line);

#if PHANTOM_TESTING
/* Blocks until the worker has finished processing all submitted queries. */
void autocomplete_drain(Autocomplete *ac);
#endif

/* Returns cached suggestion (owned by ac), or NULL. */
const char *autocomplete_get_suggestion(const Autocomplete *ac);

void autocomplete_clear(Autocomplete *ac);

/* Records a command into CCG. State hash = CWD + prev_cmd + exit_code.
 * No-op when ccg was NULL at creation. Call from main thread on Enter. */
void autocomplete_record_command(Autocomplete *ac, const char *cmd,
                                 int exit_code);

/* Re-probes CWD/git/fs map; call after an OSC 7 CWD update. */
void autocomplete_request_env_probe(Autocomplete *ac);
