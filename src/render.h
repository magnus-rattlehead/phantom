#pragma once
#include <SDL3/SDL.h>

#include "terminal.h"
#include "search.h"
#include "ml/autocomplete.h"

typedef struct Renderer Renderer;

/* Creates the OpenGL renderer and font atlas for window. Returns NULL on failure. */
Renderer *renderer_create(SDL_Window *window);

void      renderer_destroy(Renderer *r);

/* Renders one frame of term into the window's GL context.
 * Pass a non-NULL SearchState to blend search highlights into cell backgrounds. */
void      renderer_draw(Renderer *r, Terminal *term, SearchState *s);

/* Draws the search bar overlay on top of the last rendered frame.
 * No-op when search_is_active(s) returns 0. */
void      renderer_draw_search_overlay(Renderer *r, SearchState *s,
                                       Terminal *term);

/* Draws the autocomplete ghost-text suggestion at the cursor position.
 * No-op when no suggestion is available. */
void      renderer_draw_autocomplete_overlay(Renderer *r,
                                             const Autocomplete *ac,
                                             Terminal *term);

/* Returns the font atlas cell dimensions in device pixels. */
void      renderer_get_cell_size(const Renderer *r, int *cell_w, int *cell_h);

/* Draws an 8px scrollbar overlay on the right edge. No-op when no scrollback. */
void      renderer_draw_scrollbar(Renderer *r, Terminal *term);

/* Draws the tab bar at the bottom. No-op when n <= 1.
 * titles[i] is the UTF-8 label for tab i; long titles are truncated with "…". */
void      renderer_draw_tab_bar(Renderer *r, int n, int active,
                               const char *const *titles);
