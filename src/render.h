#pragma once
#include <SDL3/SDL.h>

#include "ml/autocomplete.h"
#include "search.h"
#include "terminal.h"

typedef struct Renderer Renderer;

/* Creates the OpenGL renderer and font atlas for window. Returns NULL on
 * failure. */
Renderer *renderer_create(SDL_Window *window);

void renderer_destroy(Renderer *r);

/* Call once per frame before any renderer_draw calls.
 * Acquires GL context, sets viewport, clears to background color. */
void renderer_frame_begin(Renderer *r);

/* is_active: 1 = draw cursor, 0 = hide it (inactive pane). */
void renderer_draw(Renderer *r, Terminal *term, SearchState *s, int x_px,
                   int y_px, int w_px, int h_px, int is_active);

/* No-op when search_is_active(s) returns 0. */
void renderer_draw_search_overlay(Renderer *r, SearchState *s, Terminal *term);

/* No-op when no suggestion is available. */
void renderer_draw_autocomplete_overlay(Renderer *r, const Autocomplete *ac,
                                        Terminal *term, int x_px, int y_px);

/* Returns the font atlas cell dimensions in device pixels. */
void renderer_get_cell_size(const Renderer *r, int *cell_w, int *cell_h);

/* Draws a pill-shaped scrollbar overlay within the given pane bounds.
 * Returns 1 while a fade animation is in progress. */
int renderer_draw_scrollbar(Renderer *r, Terminal *term, int x_px, int y_px,
                            int w_px, int h_px);

/* Draws the tab bar at the bottom. No-op when n <= 1. */
void renderer_draw_tab_bar(Renderer *r, int n, int active,
                           const char *const *titles);

/* Draws a solid-colored rectangle in physical pixels (for pane dividers). */
void renderer_fill_rect(Renderer *r, int x, int y, int w, int h, uint32_t rgba);

/* Signal that the window was resized; syncs CGL backbuffer next frame. */
void renderer_notify_resize(Renderer *r);

int renderer_scrollbar_hit(int x_px, int y_px, int w_px, int h_px, int mx,
                           int my);

/* Convert scrollbar drag position to a scroll offset.
 * anchor_y/anchor_off: pixel y and scroll_offset at drag start. */
int renderer_scrollbar_drag_offset(const Terminal *term, int h_px, int anchor_y,
                                   int anchor_off, int my);
