#pragma once
#include "terminal.h"
#include <SDL3/SDL.h>

typedef struct SearchState SearchState;

SearchState *search_create(void);
void search_destroy(SearchState *s);
void search_set_event_type(uint32_t t);

void search_open(SearchState *s, Terminal *t);
void search_close(SearchState *s, Terminal *t);
int search_is_active(const SearchState *s);

/* Call when search is active; each returns 1 if the event was consumed. */
int search_handle_key(SearchState *s, Terminal *t, const SDL_KeyboardEvent *e);
int search_handle_text(SearchState *s, Terminal *t,
                       const SDL_TextInputEvent *e);

/* Renderer accessors.  query/query_len/current_idx: main-thread only.
 * result_count and result_contains acquire the internal lock. */
const char *search_query(const SearchState *s);
int search_query_len(const SearchState *s);
int search_result_count(SearchState *s);
int search_current_idx(const SearchState *s);
/* sb->total_rows captured when the last search was triggered; the renderer
 * uses this to map live-grid rows to their abs_row indices. */
int search_live_sb_base(const SearchState *s);
/* abs_row of the currently selected result, or -1 if there are no results. */
int search_current_abs_row(SearchState *s);
/* Returns a malloc'd sorted copy of the result set (caller frees).
 * *out_count is set to the number of entries; returns NULL if empty. */
int *search_snapshot(SearchState *s, int *out_count);
