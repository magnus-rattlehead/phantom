#pragma once
#include <stdint.h>

/* Atlas is 2048x2048 single-channel (GL_RED); glyphs packed via
 * glTexSubImage2D. */
#define FONT_ATLAS_W 2048
#define FONT_ATLAS_H 2048

typedef struct {
    float x0, y0, x1, y1; /* pixel-space glyph box */
    float s0, t0, s1, t1; /* atlas UV coordinates   */
} GlyphQuad;

typedef struct {
    uint32_t texture_id;
    int cell_w;
    int cell_h;
    int ascent_px;              /* baseline-to-cell-top offset in physical px */
    int underline_pos_px;       /* pixels below baseline to underline top */
    int underline_thickness_px; /* underline height in pixels (>= 1) */
    void *priv;                 /* FontPriv*, managed by font.c */
} FontAtlas;

/* Initialise atlas from a TTF file. Requires an active GL context.
 * Pre-warms printable ASCII on first call to avoid first-frame stalls.
 * Returns 0 on success. */
int font_atlas_create(FontAtlas *atlas, const char *ttf_path, float size_pts,
                      float dpi_scale, float line_height);
int font_atlas_create_bold(FontAtlas *atlas, const char *ttf_path,
                           float size_pts, float dpi_scale, float line_height);
int font_atlas_create_italic(FontAtlas *atlas, const char *ttf_path,
                             float size_pts, float dpi_scale,
                             float line_height);
int font_atlas_create_bold_italic(FontAtlas *atlas, const char *ttf_path,
                                  float size_pts, float dpi_scale,
                                  float line_height);

void font_atlas_destroy(FontAtlas *atlas);

/* Returns 1 if cp can be rendered; rasterizes on first call. */
int font_has_glyph(FontAtlas *atlas, uint32_t cp);

/* Fill *out with quad geometry for cp at (baseline_x, baseline_y).
 * Rasterizes on first call; writes a zero-size quad for missing glyphs. */
void font_atlas_glyph(FontAtlas *atlas, uint32_t cp, float baseline_x,
                      float baseline_y, GlyphQuad *out);
