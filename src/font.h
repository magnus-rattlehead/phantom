#pragma once
#include <stdint.h>

/* Atlas is 2048×2048 single-channel (GL_RED); glyphs packed via glTexSubImage2D. */
#define FONT_ATLAS_W 2048
#define FONT_ATLAS_H 2048

/* Screen-space glyph bounding box + atlas UVs for one character. */
typedef struct {
    float x0, y0, x1, y1;  /* pixel-space glyph box */
    float s0, t0, s1, t1;  /* atlas UV coordinates   */
} GlyphQuad;

typedef struct {
    uint32_t texture_id;
    int      cell_w;
    int      cell_h;
    int      ascent_px;
    int      underline_pos_px;        /* pixels below baseline to underline top */
    int      underline_thickness_px;  /* underline height in pixels (>= 1) */
    void    *priv;          /* FontPriv*, managed by font.c */
} FontAtlas;

/* Initialises atlas from a TTF at size_pts logical points and dpi_scale
 * (physical pixels per logical pixel). Returns 0 on success.
 * Requires an active GL context; pre-warms ASCII glyphs on first call. */
int  font_atlas_create(FontAtlas *atlas, const char *ttf_path,
                       float size_pts, float dpi_scale,
                       float line_height);
int  font_atlas_create_bold(FontAtlas *atlas, const char *ttf_path,
                             float size_pts, float dpi_scale,
                             float line_height);
int  font_atlas_create_italic(FontAtlas *atlas, const char *ttf_path,
                               float size_pts, float dpi_scale,
                               float line_height);
int  font_atlas_create_bold_italic(FontAtlas *atlas, const char *ttf_path,
                                    float size_pts, float dpi_scale,
                                    float line_height);

void font_atlas_destroy(FontAtlas *atlas);

/* Returns 1 if cp can be rendered; rasterizes and caches the glyph on first call. */
int  font_has_glyph(FontAtlas *atlas, uint32_t cp);

/* Fills *out with the pixel-space quad and atlas UVs for cp at (baseline_x, baseline_y).
 * Rasterizes on first call; writes a zero-size quad if the glyph is missing. */
void font_atlas_glyph(FontAtlas *atlas, uint32_t cp,
                      float baseline_x, float baseline_y,
                      GlyphQuad *out);
