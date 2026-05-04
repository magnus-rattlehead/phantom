#include <stddef.h>
#include <stdint.h>
#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

#include "font.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* UTF-16 surrogate-pair encoding constants (codepoints above U+FFFF) */
#define UNICODE_BMP_MAX 0xFFFF
#define UNICODE_SMP_START 0x10000
#define SURROGATE_HIGH_START 0xD800
#define SURROGATE_LOW_START 0xDC00
#define SURROGATE_DATA_BITS 0x3FF

#define ASCII_PRINTABLE_FIRST 0x20
#define ASCII_PRINTABLE_LAST 0x7E

#define GLYPH_CACHE_BUCKETS                                                    \
    1024 /* number of buckets in hash table, must be power of 2 */
#define GLYPH_CACHE_MAX                                                        \
    8192 /* pool entries; far more than any session needs                      \
          */

typedef struct GlyphEntry {
    uint32_t cp;
    int ax, ay; /* top-left position in atlas (texels) */
    int aw, ah; /* glyph bitmap dimensions in pixels */
    int bx, by; /* left bearing and above-baseline distance */
    int valid;  /* 0 = codepoint not present in font face */
    struct GlyphEntry *next;
} GlyphEntry;

typedef struct {
#if defined(__APPLE__)
    CTFontRef ct_font;
    CGColorSpaceRef cs_rgb; /* cached; reused across rasterize() calls */
#else
    FT_Library ft_lib;
    FT_Face ft_face;
#endif
    float dpi_scale;
    GlyphEntry *buckets[GLYPH_CACHE_BUCKETS];
    GlyphEntry pool[GLYPH_CACHE_MAX];
    int pool_used;
    int pack_x;
    int pack_y;
    int row_h; /* tallest glyph in current shelf row */
} FontPriv;

/* Builtin glyph drawing adapted from Alacritty (builtin_font.rs). */

struct Canvas {
    uint8_t *buf;
    int width;
    int height;
};

static struct Canvas canvas_new(int w, int h) {
    struct Canvas c;
    c.width = w;
    c.height = h;
    c.buf = calloc((size_t)w * h, 1);
    return c;
}

static void canvas_free(struct Canvas *c) {
    free(c->buf);
    c->buf = NULL;
}

static void canvas_fill(struct Canvas *c, uint8_t v) {
    memset(c->buf, v, (size_t)(c->width * c->height));
}

static void canvas_rect(struct Canvas *c, int x, int y, int w, int h) {
    int ex = x + w < c->width ? x + w : c->width;
    int ey = y + h < c->height ? y + h : c->height;
    for (int i = y; i < ey; i++)
        memset(c->buf + i * c->width + x, 255, (size_t)(ex - x));
}

static void canvas_hline(struct Canvas *c, int x, int y, int len, int stroke) {
    int sy = (int)(y - stroke / 2.0f);
    int ey = (int)(y + stroke / 2.0f);
    sy = sy > 0 ? sy : 0;
    ey = ey < c->height ? ey : c->height;
    int ex = x + len < c->width ? x + len : c->width;
    for (int i = sy; i < ey; i++)
        memset(c->buf + i * c->width + x, 255, (size_t)(ex - x));
}

static void canvas_vline(struct Canvas *c, int x, int y, int len, int stroke) {
    int sx = (int)(x - stroke / 2.0f);
    int ex = (int)(x + stroke / 2.0f);
    sx = sx > 0 ? sx : 0;
    ex = ex < c->width ? ex : c->width;
    int ey = y + len < c->height ? y + len : c->height;
    for (int i = y; i < ey; i++)
        memset(c->buf + i * c->width + sx, 255, (size_t)(ex - sx));
}

static void canvas_put_pixel(struct Canvas *c, int x, int y, uint8_t v) {
    if (x < 0 || y < 0 || x >= c->width || y >= c->height)
        return;
    int idx = y * c->width + x;
    if (v > c->buf[idx])
        c->buf[idx] = v;
}

static void canvas_line_wu(struct Canvas *c, float x0, float y0, float x1,
                           float y1) {
    int steep = fabsf(y1 - y0) > fabsf(x1 - x0);
    if (steep) {
        float t = x0;
        x0 = y0;
        y0 = t;
        t = x1;
        x1 = y1;
        y1 = t;
    }
    if (x0 > x1) {
        float t = x0;
        x0 = x1;
        x1 = t;
        t = y0;
        y0 = y1;
        y1 = t;
    }
    float dx = x1 - x0, dy = y1 - y0;
    float grad = (dx < 1e-6f && dx > -1e-6f) ? 1.0f : dy / dx;

    float xe = roundf(x0);
    float ye = y0 + grad * (xe - x0);
    float xgap = 1.0f - fmodf(x0 + 0.5f, 1.0f);
    int xpxl1 = (int)xe;
    float frac1 = ye - floorf(ye);
    if (steep) {
        canvas_put_pixel(c, (int)ye, xpxl1,
                         (uint8_t)((1.0f - frac1) * xgap * 255.0f));
        canvas_put_pixel(c, (int)ye + 1, xpxl1,
                         (uint8_t)(frac1 * xgap * 255.0f));
    } else {
        canvas_put_pixel(c, xpxl1, (int)ye,
                         (uint8_t)((1.0f - frac1) * xgap * 255.0f));
        canvas_put_pixel(c, xpxl1, (int)ye + 1,
                         (uint8_t)(frac1 * xgap * 255.0f));
    }
    float intery = ye + grad;

    xe = roundf(x1);
    ye = y1 + grad * (xe - x1);
    xgap = fmodf(x1 + 0.5f, 1.0f);
    int xpxl2 = (int)xe;
    float frac2 = ye - floorf(ye);
    if (steep) {
        canvas_put_pixel(c, (int)ye, xpxl2,
                         (uint8_t)((1.0f - frac2) * xgap * 255.0f));
        canvas_put_pixel(c, (int)ye + 1, xpxl2,
                         (uint8_t)(frac2 * xgap * 255.0f));
    } else {
        canvas_put_pixel(c, xpxl2, (int)ye,
                         (uint8_t)((1.0f - frac2) * xgap * 255.0f));
        canvas_put_pixel(c, xpxl2, (int)ye + 1,
                         (uint8_t)(frac2 * xgap * 255.0f));
    }

    for (int xi = xpxl1 + 1; xi < xpxl2; xi++) {
        float frac = intery - floorf(intery);
        if (steep) {
            canvas_put_pixel(c, (int)intery, xi,
                             (uint8_t)((1.0f - frac) * 255.0f));
            canvas_put_pixel(c, (int)intery + 1, xi, (uint8_t)(frac * 255.0f));
        } else {
            canvas_put_pixel(c, xi, (int)intery,
                             (uint8_t)((1.0f - frac) * 255.0f));
            canvas_put_pixel(c, xi, (int)intery + 1, (uint8_t)(frac * 255.0f));
        }
        intery += grad;
    }
}

static void canvas_arc(struct Canvas *c, int stroke) {
    int short_side = c->width < c->height ? c->width : c->height;
    float radius = (short_side + stroke) / 2.0f;
    float sf = (float)stroke;
    float dist_bias = (short_side % 2 == stroke % 2) ? 0.0f : 0.5f;
    float x_off = 0.0f, y_off = 0.0f;
    if (c->height > c->width) {
        y_off = c->height / 2.0f - radius + sf / 2.0f;
        if ((c->width % 2 != c->height % 2) && (c->height % 2 == stroke % 2))
            y_off += 1.0f;
    } else {
        x_off = c->width / 2.0f - radius + sf / 2.0f;
        if ((c->width % 2 != c->height % 2) && (c->width % 2 == stroke % 2))
            x_off += 1.0f;
    }

    int r_i = (short_side + stroke + 1) / 2;
    for (int py = 0; py < r_i; py++) {
        for (int px = 0; px < r_i; px++) {
            float dist = sqrtf((float)(px * px + py * py)) + dist_bias;
            float v;
            if (dist < radius - sf - 1.0f)
                v = 0.0f;
            else if (dist < radius - sf)
                v = (dist - (radius - sf - 1.0f)) * 255.0f;
            else if (dist < radius - 1.0f)
                v = 255.0f;
            else if (dist < radius)
                v = (radius - dist) * 255.0f;
            else
                v = 0.0f;
            canvas_put_pixel(c, px + (int)x_off, py + (int)y_off, (uint8_t)v);
        }
    }

    if (c->height > c->width) {
        canvas_rect(c, (int)(c->width / 2.0f - sf / 2.0f), 0, stroke,
                    (int)y_off);
    } else {
        canvas_rect(c, 0, (int)(c->height / 2.0f - sf / 2.0f), (int)x_off,
                    stroke);
    }
}

static void canvas_flip_h(struct Canvas *c, int extra) {
    int center = c->width / 2;
    for (int row = 0; row < c->height; row++) {
        uint8_t *r = c->buf + row * c->width;
        if (extra)
            r[c->width - 1] = r[0];
        for (int col = 0; col < center; col++) {
            uint8_t t = r[col];
            r[col] = r[c->width - 1 - col - extra];
            r[c->width - 1 - col - extra] = t;
        }
    }
}

static void canvas_flip_v(struct Canvas *c, int extra) {
    int center = c->height / 2;
    if (extra)
        memcpy(c->buf + (c->height - 1) * c->width, c->buf, (size_t)c->width);
    for (int row = 1; row <= center; row++) {
        uint8_t *a = c->buf + (row - 1) * c->width;
        uint8_t *b = c->buf + (c->height - row - extra) * c->width;
        for (int col = 0; col < c->width; col++) {
            uint8_t t = a[col];
            a[col] = b[col];
            b[col] = t;
        }
    }
}

static int calculate_stroke_size(int cell_width) {
    int s = (int)roundf((float)cell_width / 8.0f);
    return s < 1 ? 1 : s;
}

static int rasterize_builtin(FontAtlas *atlas, FontPriv *priv, GlyphEntry *e) {
    uint32_t cp = e->cp;
    if (!((cp >= 0x2500 && cp <= 0x257F) || (cp >= 0x2580 && cp <= 0x259F)))
        return 0;

    int W = atlas->cell_w;
    int H = atlas->cell_h;
    int stroke = calculate_stroke_size(W);
    int heavy = stroke * 2;
    int xc = W / 2;
    int yc = H / 2;

    struct Canvas c = canvas_new(W, H);
    if (!c.buf)
        return 0;

    if (cp >= 0x2580 && cp <= 0x259F) {
        int xh = (int)roundf(W / 2.0f);
        int yh = (int)roundf(H / 2.0f);
        switch (cp) {
        case 0x2580:
            canvas_rect(&c, 0, 0, W, yh);
            break;
        case 0x2581: {
            int y = H - (int)roundf(H / 8.0f);
            canvas_rect(&c, 0, y, W, H - y);
            break;
        }
        case 0x2582: {
            int y = H - (int)roundf(H / 4.0f);
            canvas_rect(&c, 0, y, W, H - y);
            break;
        }
        case 0x2583: {
            int y = H - (int)roundf(3 * H / 8.0f);
            canvas_rect(&c, 0, y, W, H - y);
            break;
        }
        case 0x2584:
            canvas_rect(&c, 0, yh, W, H - yh);
            break;
        case 0x2585: {
            int y = H - (int)roundf(5 * H / 8.0f);
            canvas_rect(&c, 0, y, W, H - y);
            break;
        }
        case 0x2586: {
            int y = H - (int)roundf(3 * H / 4.0f);
            canvas_rect(&c, 0, y, W, H - y);
            break;
        }
        case 0x2587: {
            int y = H - (int)roundf(7 * H / 8.0f);
            canvas_rect(&c, 0, y, W, H - y);
            break;
        }
        case 0x2588:
            canvas_fill(&c, 255);
            break;
        case 0x2589:
            canvas_rect(&c, 0, 0, (int)roundf(7 * W / 8.0f), H);
            break;
        case 0x258A:
            canvas_rect(&c, 0, 0, (int)roundf(6 * W / 8.0f), H);
            break;
        case 0x258B:
            canvas_rect(&c, 0, 0, (int)roundf(5 * W / 8.0f), H);
            break;
        case 0x258C:
            canvas_rect(&c, 0, 0, xh, H);
            break;
        case 0x258D:
            canvas_rect(&c, 0, 0, (int)roundf(3 * W / 8.0f), H);
            break;
        case 0x258E:
            canvas_rect(&c, 0, 0, (int)roundf(2 * W / 8.0f), H);
            break;
        case 0x258F:
            canvas_rect(&c, 0, 0, (int)roundf(W / 8.0f), H);
            break;
        case 0x2590:
            canvas_rect(&c, xh, 0, W - xh, H);
            break;
        case 0x2591:
            canvas_fill(&c, 64);
            break;
        case 0x2592:
            canvas_fill(&c, 128);
            break;
        case 0x2593:
            canvas_fill(&c, 192);
            break;
        case 0x2594:
            canvas_rect(&c, 0, 0, W, (int)roundf(H / 8.0f));
            break;
        case 0x2595: {
            int x = W - (int)roundf(W / 8.0f);
            canvas_rect(&c, x, 0, W - x, H);
            break;
        }
        default: {
            /* Quadrant blocks: bits 0x1=UL 0x2=UR 0x4=LL 0x8=LR */
            static const uint8_t qbits[10] = {0x4, 0x8, 0x1, 0xD, 0x9,
                                              0x7, 0xB, 0x2, 0x6, 0xE};
            uint8_t bits = qbits[cp - 0x2596];
            if (bits & 0x1)
                canvas_rect(&c, 0, 0, xh, yh);
            if (bits & 0x2)
                canvas_rect(&c, xh, 0, W - xh, yh);
            if (bits & 0x4)
                canvas_rect(&c, 0, yh, xh, H - yh);
            if (bits & 0x8)
                canvas_rect(&c, xh, yh, W - xh, H - yh);
            break;
        }
        }
    } else {
        if (cp >= 0x256D && cp <= 0x2570) { /* rounded corners */
            int extra_h = (stroke % 2 != W % 2) ? 1 : 0;
            int extra_v = (stroke % 2 != H % 2) ? 1 : 0;
            canvas_arc(&c, stroke);
            if (cp == 0x256D) {
                canvas_flip_h(&c, extra_h);
                canvas_flip_v(&c, extra_v);
            } else if (cp == 0x256E) {
                canvas_flip_v(&c, extra_v);
            }
            /* 0x256F: native orientation */
            else if (cp == 0x2570) {
                canvas_flip_h(&c, extra_h);
            }
        } else if (cp >= 0x2571 && cp <= 0x2573) { /* diagonals */
            for (int i = 0; i < 2 * stroke; i++) {
                float off = i / 2.0f;
                if (cp == 0x2571 || cp == 0x2573)
                    canvas_line_wu(&c, (float)W, off, 0.0f, (float)H + off);
                if (cp == 0x2572 || cp == 0x2573)
                    canvas_line_wu(&c, 0.0f, off, (float)W, (float)H + off);
            }
        } else if ((cp >= 0x2504 && cp <= 0x250B) || /* dashed lines */
                   cp == 0x254C || cp == 0x254D || cp == 0x254E ||
                   cp == 0x254F) {
            int is_h, gaps, ds;
            switch (cp) {
            case 0x2504:
                is_h = 1;
                gaps = 2;
                ds = stroke;
                break;
            case 0x2505:
                is_h = 1;
                gaps = 2;
                ds = heavy;
                break;
            case 0x2506:
                is_h = 0;
                gaps = 2;
                ds = stroke;
                break;
            case 0x2507:
                is_h = 0;
                gaps = 2;
                ds = heavy;
                break;
            case 0x2508:
                is_h = 1;
                gaps = 3;
                ds = stroke;
                break;
            case 0x2509:
                is_h = 1;
                gaps = 3;
                ds = heavy;
                break;
            case 0x250A:
                is_h = 0;
                gaps = 3;
                ds = stroke;
                break;
            case 0x250B:
                is_h = 0;
                gaps = 3;
                ds = heavy;
                break;
            case 0x254C:
                is_h = 1;
                gaps = 1;
                ds = stroke;
                break;
            case 0x254D:
                is_h = 1;
                gaps = 1;
                ds = heavy;
                break;
            case 0x254E:
                is_h = 0;
                gaps = 1;
                ds = stroke;
                break;
            default:
                is_h = 0;
                gaps = 1;
                ds = heavy;
                break;
            }
            if (is_h) {
                int gap = W / 8 > 1 ? W / 8 : 1;
                int seg = (W - gap * gaps) / (gaps + 1);
                seg = seg > 1 ? seg : 1;
                for (int g = 0; g <= gaps; g++)
                    canvas_hline(&c, g * (seg + gap), yc, seg, ds);
            } else {
                int gap = H / 8 > 1 ? H / 8 : 1;
                int seg = (H - gap * gaps) / (gaps + 1);
                seg = seg > 1 ? seg : 1;
                for (int g = 0; g <= gaps; g++)
                    canvas_vline(&c, xc, g * (seg + gap), seg, ds);
            }
        }
        /* Directional box drawing: (l,r,u,d) weights packed as
         * (l<<6|r<<4|u<<2|d), 0=none 1=light 2=heavy. */
        else {
#define D(l, r, u, d) (uint8_t)(((l) << 6) | ((r) << 4) | ((u) << 2) | (d))
            static const uint8_t box[128] = {
                /* 2500 */ D(1, 1, 0, 0),
                D(2, 2, 0, 0),
                D(0, 0, 1, 1),
                D(0, 0, 2, 2),
                /* 2504 */ 0,
                0,
                0,
                0,
                0,
                0,
                0,
                0, /* dashes, handled above */
                /* 250C */ D(0, 1, 0, 1),
                D(0, 2, 0, 1),
                D(0, 1, 0, 2),
                D(0, 2, 0, 2),
                /* 2510 */ D(1, 0, 0, 1),
                D(2, 0, 0, 1),
                D(1, 0, 0, 2),
                D(2, 0, 0, 2),
                /* 2514 */ D(0, 1, 1, 0),
                D(0, 2, 1, 0),
                D(0, 1, 2, 0),
                D(0, 2, 2, 0),
                /* 2518 */ D(1, 0, 1, 0),
                D(2, 0, 1, 0),
                D(1, 0, 2, 0),
                D(2, 0, 2, 0),
                /* 251C */ D(0, 1, 1, 1),
                D(0, 2, 1, 1),
                D(0, 1, 2, 1),
                D(0, 1, 1, 2),
                /* 2520 */ D(0, 1, 2, 2),
                D(0, 2, 2, 1),
                D(0, 2, 1, 2),
                D(0, 2, 2, 2),
                /* 2524 */ D(1, 0, 1, 1),
                D(2, 0, 1, 1),
                D(1, 0, 2, 1),
                D(1, 0, 1, 2),
                /* 2528 */ D(1, 0, 2, 2),
                D(2, 0, 2, 1),
                D(2, 0, 1, 2),
                D(2, 0, 2, 2),
                /* 252C */ D(1, 1, 0, 1),
                D(2, 1, 0, 1),
                D(1, 2, 0, 1),
                D(2, 2, 0, 1),
                /* 2530 */ D(1, 1, 0, 2),
                D(2, 1, 0, 2),
                D(1, 2, 0, 2),
                D(2, 2, 0, 2),
                /* 2534 */ D(1, 1, 1, 0),
                D(2, 1, 1, 0),
                D(1, 2, 1, 0),
                D(2, 2, 1, 0),
                /* 2538 */ D(1, 1, 2, 0),
                D(2, 1, 2, 0),
                D(1, 2, 2, 0),
                D(2, 2, 2, 0),
                /* 253C */ D(1, 1, 1, 1),
                D(2, 1, 1, 1),
                D(1, 2, 1, 1),
                D(2, 2, 1, 1),
                /* 2540 */ D(1, 1, 2, 1),
                D(1, 1, 1, 2),
                D(1, 1, 2, 2),
                D(2, 1, 2, 1),
                /* 2544 */ D(1, 2, 2, 1),
                D(2, 1, 1, 2),
                D(1, 2, 1, 2),
                D(2, 2, 2, 1),
                /* 2548 */ D(2, 2, 1, 2),
                D(2, 1, 2, 2),
                D(1, 2, 2, 2),
                D(2, 2, 2, 2),
                /* 254C */ 0,
                0,
                0,
                0, /* dashes, handled above */
                /* 2550 */ 0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0, /* double lines */
                /* 2560 */ 0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0, /* double lines */
                /* 256D */ 0,
                0,
                0,
                0, /* rounded corners above */
                /* 2571 */ 0,
                0,
                0, /* diagonals above */
                /* 2574 */ D(1, 0, 0, 0),
                D(0, 0, 1, 0),
                D(0, 1, 0, 0),
                D(0, 0, 0, 1),
                /* 2578 */ D(2, 0, 0, 0),
                D(0, 0, 2, 0),
                D(0, 2, 0, 0),
                D(0, 0, 0, 2),
                /* 257C */ D(1, 2, 0, 0),
                D(0, 0, 1, 2),
                D(2, 1, 0, 0),
                D(0, 0, 2, 1),
            };
#undef D
            uint8_t dir = box[cp - 0x2500];
            int sl = ((dir >> 6) & 3) == 1   ? stroke
                     : ((dir >> 6) & 3) == 2 ? heavy
                                             : 0;
            int sr = ((dir >> 4) & 3) == 1   ? stroke
                     : ((dir >> 4) & 3) == 2 ? heavy
                                             : 0;
            int su = ((dir >> 2) & 3) == 1   ? stroke
                     : ((dir >> 2) & 3) == 2 ? heavy
                                             : 0;
            int sd = ((dir >> 0) & 3) == 1   ? stroke
                     : ((dir >> 0) & 3) == 2 ? heavy
                                             : 0;
            if (sl)
                canvas_hline(&c, 0, yc, xc, sl);
            if (sr)
                canvas_hline(&c, xc, yc, W - xc, sr);
            if (su)
                canvas_vline(&c, xc, 0, yc, su);
            if (sd)
                canvas_vline(&c, xc, yc, H - yc, sd);
        }
    }

    if (priv->pack_x + W > FONT_ATLAS_W) {
        priv->pack_x = 0;
        priv->pack_y += priv->row_h + 1;
        priv->row_h = 0;
    }
    if (priv->pack_y + H > FONT_ATLAS_H) {
        canvas_free(&c);
        e->valid = 0;
        return 1;
    }

    glBindTexture(GL_TEXTURE_2D, atlas->texture_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, priv->pack_x, priv->pack_y, W, H, GL_RED,
                    GL_UNSIGNED_BYTE, c.buf);
    glBindTexture(GL_TEXTURE_2D, 0);
    canvas_free(&c);

    e->ax = priv->pack_x;
    e->ay = priv->pack_y;
    e->aw = W;
    e->ah = H;
    e->bx = 0;
    e->by = atlas->ascent_px;
    e->valid = 1;

    if (H > priv->row_h)
        priv->row_h = H;
    priv->pack_x += W + 1;
    return 1;
}

static GlyphEntry *cache_find(FontPriv *priv, uint32_t cp) {
    GlyphEntry *e = priv->buckets[cp & (GLYPH_CACHE_BUCKETS - 1)];
    for (; e; e = e->next) {
        if (e->cp == cp)
            return e;
    }
    return NULL;
}

/* Returns NULL if the glyph pool is exhausted. */
static GlyphEntry *cache_alloc(FontPriv *priv, uint32_t cp) {
    if (priv->pool_used >= GLYPH_CACHE_MAX)
        return NULL;
    GlyphEntry *e = &priv->pool[priv->pool_used++];
    memset(e, 0, sizeof *e);
    e->cp = cp;
    e->next = priv->buckets[cp & (GLYPH_CACHE_BUCKETS - 1)];
    priv->buckets[cp & (GLYPH_CACHE_BUCKETS - 1)] = e;
    return e;
}

#if defined(__APPLE__)
static CGGlyph cp_to_glyph(CTFontRef font, uint32_t cp) {
    UniChar chars[2];
    int nchars;
    if (cp > UNICODE_BMP_MAX) {
        /* Supplementary codepoints require a surrogate pair in UTF-16. */
        uint32_t u = cp - UNICODE_SMP_START;
        chars[0] = (UniChar)(SURROGATE_HIGH_START + (u >> 10));
        chars[1] = (UniChar)(SURROGATE_LOW_START + (u & SURROGATE_DATA_BITS));
        nchars = 2;
    } else {
        chars[0] = (UniChar)cp;
        nchars = 1;
    }
    CGGlyph g = 0;
    CTFontGetGlyphsForCharacters(font, chars, &g, nchars);
    return g;
}
#endif

static void rasterize(FontAtlas *atlas, FontPriv *priv, GlyphEntry *e) {
    uint32_t cp = e->cp;
    /* Double-line box chars (0x2550..0x256C) fall back to font glyph. */
    if (((cp >= 0x2500 && cp <= 0x257F) || (cp >= 0x2580 && cp <= 0x259F)) &&
        !(cp >= 0x2550 && cp <= 0x256C)) {
        if (rasterize_builtin(atlas, priv, e))
            return;
    }
#if defined(__APPLE__)
    CGGlyph glyph = cp_to_glyph(priv->ct_font, e->cp);
#else
    FT_UInt glyph = FT_Get_Char_Index(priv->ft_face, (FT_ULong)e->cp);
#endif
    if (0 == glyph) {
        e->valid = 0;
        return;
    }
#if defined(__APPLE__)
    CGRect bbox = CTFontGetBoundingRectsForGlyphs(
        priv->ct_font, kCTFontOrientationDefault, &glyph, NULL, 1);

    /* whitespace/zero-width: valid entry but no atlas upload needed */
    if (bbox.size.width < 0.5 || bbox.size.height < 0.5) {
        e->bx = e->by = e->aw = e->ah = 0;
        e->valid = 1;
        return;
    }

    /* CoreText bounding box is in logical (point) units, y-up. Floor/ceil
     * expands to integer boundaries before scaling to avoid sub-pixel clipping.
     */
    CGFloat left_f = floor(bbox.origin.x);
    CGFloat bottom_f = floor(bbox.origin.y);
    CGFloat right_f = ceil(bbox.origin.x + bbox.size.width);
    CGFloat top_f = ceil(bbox.origin.y + bbox.size.height);

    float ds = priv->dpi_scale;
    int aw = (int)ceil((right_f - left_f) * ds);
    int ah = (int)ceil((top_f - bottom_f) * ds);

    /* bx/by in physical pixels; render.c uses them for vertex placement. */
    e->bx = (int)round(left_f * ds);
    e->by = (int)round(top_f * ds);
#else
    if (0 != FT_Load_Glyph(priv->ft_face, glyph, FT_LOAD_DEFAULT)) {
        e->valid = 0;
        return;
    }
    if (0 != FT_Render_Glyph(priv->ft_face->glyph, FT_RENDER_MODE_NORMAL)) {
        e->valid = 0;
        return;
    }

    FT_GlyphSlot slot = priv->ft_face->glyph;
    FT_Bitmap *bm = &slot->bitmap;

    int aw = (int)bm->width;
    int ah = (int)bm->rows;

    if (0 == aw || 0 == ah) {
        e->bx = e->by = e->aw = e->ah = 0;
        e->valid = 1;
        return;
    }

    e->bx = slot->bitmap_left;
    e->by = slot->bitmap_top;
#endif
    e->valid = 1;

    if (priv->pack_x + aw > FONT_ATLAS_W) {
        priv->pack_x = 0;
        priv->pack_y += priv->row_h + 1;
        priv->row_h = 0;
    }
    if (priv->pack_y + ah > FONT_ATLAS_H) {
        e->valid = 0;
        return;
    }
#if defined(__APPLE__)
    /* CTFontDrawGlyphs requires a color context; grayscale is not supported. */
    size_t stride = (size_t)aw * 4;
    uint8_t *buf = malloc((size_t)ah * stride);
    if (!buf) {
        e->valid = 0;
        return;
    }

    /* Opaque black background: CoreGraphics AA blends against a known value. */
    for (int i = 0; i < aw * ah; i++) {
        buf[i * 4 + 0] = 0;
        buf[i * 4 + 1] = 0;
        buf[i * 4 + 2] = 0;
        buf[i * 4 + 3] = 255;
    }

    CGContextRef ctx = CGBitmapContextCreate(
        buf, (size_t)aw, (size_t)ah, 8, stride, priv->cs_rgb,
        kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
    if (!ctx) {
        free(buf);
        e->valid = 0;
        return;
    }

    CGContextSetRGBFillColor(ctx, 1.0, 1.0, 1.0, 1.0);
    CGContextSetShouldAntialias(ctx, true);
    CGContextSetAllowsAntialiasing(ctx, true);
    /* Subpixel rendering disabled: atlas is single-channel GL_RED. */
    CGContextSetShouldSmoothFonts(ctx, false);
    CGContextSetShouldSubpixelPositionFonts(ctx, false);
    CGContextSetShouldSubpixelQuantizeFonts(ctx, false);

    /* Scale so CoreText rasterizes at physical resolution; CTFont coordinate
     * space remains in logical units. */
    CGContextScaleCTM(ctx, (CGFloat)priv->dpi_scale, (CGFloat)priv->dpi_scale);

    /* Quartz y-up: offset (-left_f, -bottom_f) places the glyph's lower-left
     * logical pixel at the context origin. */
    CGPoint pos = {-left_f, -bottom_f};
    CTFontDrawGlyphs(priv->ct_font, &glyph, &pos, 1, ctx);
    CGContextRelease(ctx);

    /* kCGBitmapByteOrder32Little + AlphaPremultipliedFirst lays out [B,G,R,A];
     * R is at byte offset 2 and carries the luminance for our grayscale mask.
     */
    uint8_t *gray = malloc((size_t)(aw * ah));
    if (!gray) {
        free(buf);
        e->valid = 0;
        return;
    }
    for (int i = 0; i < aw * ah; i++) {
        gray[i] = buf[i * 4 + 2];
    }
    free(buf);

    glBindTexture(GL_TEXTURE_2D, atlas->texture_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, priv->pack_x, priv->pack_y, aw, ah,
                    GL_RED, GL_UNSIGNED_BYTE, gray);
    glBindTexture(GL_TEXTURE_2D, 0);
    free(gray);
#else
    /* FreeType may pad rows to an alignment boundary (pitch > width); upload
     * row-by-row when pitch != width to avoid corrupting adjacent glyphs. */
    glBindTexture(GL_TEXTURE_2D, atlas->texture_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (bm->pitch == aw) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, priv->pack_x, priv->pack_y, aw, ah,
                        GL_RED, GL_UNSIGNED_BYTE, bm->buffer);
    } else {
        for (int row = 0; row < ah; row++) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, priv->pack_x, priv->pack_y + row,
                            aw, 1, GL_RED, GL_UNSIGNED_BYTE,
                            bm->buffer + row * bm->pitch);
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
#endif
    e->ax = priv->pack_x;
    e->ay = priv->pack_y;
    e->aw = aw;
    e->ah = ah;

    if (ah > priv->row_h)
        priv->row_h = ah;
    priv->pack_x += aw + 1; /* +1 gap prevents bilinear bleed between glyphs */
}

static GlyphEntry *get_or_add(FontAtlas *atlas, uint32_t cp) {
    FontPriv *priv = (FontPriv *)atlas->priv;
    GlyphEntry *e = cache_find(priv, cp);
    if (!e) {
        e = cache_alloc(priv, cp);
        if (e)
            rasterize(atlas, priv, e);
    }
    return e;
}

static int atlas_create_impl(FontAtlas *atlas, const char *ttf_path,
                             float size_pts, float dpi_scale,
                             float line_height) {
    FontPriv *priv = calloc(1, sizeof *priv);
    if (!priv)
        return -1;
    priv->dpi_scale = dpi_scale;

#if defined(__APPLE__)
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        NULL, (const UInt8 *)ttf_path, (CFIndex)strlen(ttf_path), false);
    if (!url) {
        free(priv);
        return -1;
    }

    CGDataProviderRef provider = CGDataProviderCreateWithURL(url);
    CFRelease(url);
    if (!provider) {
        free(priv);
        return -1;
    }

    CGFontRef cg_font = CGFontCreateWithDataProvider(provider);
    CGDataProviderRelease(provider);
    if (!cg_font) {
        free(priv);
        return -1;
    }

    /* CTFont size stays in logical points; dpi_scale is applied via
     * CGContextScaleCTM in rasterize() and to all derived pixel metrics. */
    CTFontRef ct_font =
        CTFontCreateWithGraphicsFont(cg_font, (CGFloat)size_pts, NULL, NULL);
    CGFontRelease(cg_font);
    if (!ct_font) {
        free(priv);
        return -1;
    }

    priv->ct_font = ct_font;
    priv->cs_rgb = CGColorSpaceCreateDeviceRGB();

    {
        /* One CTFrame with two lines measures both line pitch and ascent.
         * ascent+descent alone underestimates pitch because it excludes leading
         * that some fonts encode separately. */
        CFStringRef sample = CFSTR("AQWMH_gyl \nAQWMH_gyl ");
        CFMutableAttributedStringRef astr =
            CFAttributedStringCreateMutable(NULL, 0);
        CFAttributedStringReplaceString(astr, CFRangeMake(0, 0), sample);
        CFAttributedStringSetAttribute(
            astr, CFRangeMake(0, CFStringGetLength(sample)),
            kCTFontAttributeName, ct_font);
        CTFramesetterRef fset = CTFramesetterCreateWithAttributedString(astr);
        CFRelease(astr);
        /* 4000x4000 rect avoids line-wrap; CFRangeMake(0,0) lays out all text
         */
        CGMutablePathRef fpath = CGPathCreateMutable();
        CGPathAddRect(fpath, NULL, CGRectMake(0, 0, 4000, 4000));
        CTFrameRef frame =
            CTFramesetterCreateFrame(fset, CFRangeMake(0, 0), fpath, NULL);
        CGPathRelease(fpath);
        CFRelease(fset);
        CFArrayRef lines = CTFrameGetLines(frame);
        CGFloat base_cell_h;
        if (CFArrayGetCount(lines) >= 2) {
            CGPoint orig[2];
            CTFrameGetLineOrigins(frame, CFRangeMake(0, 2), orig);
            base_cell_h = orig[0].y - orig[1].y;
        } else {
            /* Degenerate fallback; underestimates pitch. */
            base_cell_h = CTFontGetAscent(ct_font) + CTFontGetDescent(ct_font);
        }

        /* kCTLineBoundsExcludeTypographicLeading strips above-cap-height
         * padding, giving tight bounds for precise baseline placement.
         * Reuse the first line from the frame already in hand. */
        CGFloat base_ascent;
        if (CFArrayGetCount(lines) >= 1) {
            CTLineRef ln = (CTLineRef)CFArrayGetValueAtIndex(lines, 0);
            CGRect bounds = CTLineGetBoundsWithOptions(
                ln, kCTLineBoundsExcludeTypographicLeading);
            base_ascent = bounds.size.height + bounds.origin.y;
        } else {
            base_ascent = CTFontGetAscent(ct_font);
        }
        CFRelease(frame);

        /* Extra space from line_height multiplier is split evenly above/below
         * to keep text vertically centered within the cell. */
        CGFloat scaled_h = base_cell_h * (CGFloat)line_height;
        CGFloat extra = scaled_h - base_cell_h;
        atlas->cell_h = (int)ceilf((float)(scaled_h * dpi_scale));
        atlas->ascent_px = (int)floorf(
            (float)((base_ascent + extra / 2.0) * dpi_scale) + 0.5f);
        if (atlas->ascent_px < 1)
            atlas->ascent_px = 1;
    }

    {
        /* 'M' advance is the canonical monospace cell width (mirrors FreeType
         * path); every advance is identical in a monospace font. */
        UniChar uc = (UniChar)'M';
        CGGlyph g = 0;
        CGFloat adv_w = 0.0;
        if (CTFontGetGlyphsForCharacters(ct_font, &uc, &g, 1) && g) {
            CGSize adv = {0.0, 0.0};
            CTFontGetAdvancesForGlyphs(ct_font, kCTFontOrientationDefault, &g,
                                       &adv, 1);
            adv_w = adv.width;
        }
        if (adv_w > 0.0)
            atlas->cell_w = (int)ceilf((float)(adv_w * dpi_scale));
        else
            atlas->cell_w = atlas->cell_h / 2;
    }

    /* CoreText underline_position is y-up (negative = below baseline);
     * negate to convert to screen y-down convention. */
    {
        CGFloat ul_pos = CTFontGetUnderlinePosition(ct_font);
        CGFloat ul_thick = CTFontGetUnderlineThickness(ct_font);
        atlas->underline_pos_px = (int)round(-ul_pos * dpi_scale);
        if (atlas->underline_pos_px < 1)
            atlas->underline_pos_px = 1;
        atlas->underline_thickness_px = (int)ceil(ul_thick * dpi_scale);
        if (atlas->underline_thickness_px < 1)
            atlas->underline_thickness_px = 1;
    }
#else
    FT_Library lib;
    if (0 != FT_Init_FreeType(&lib)) {
        free(priv);
        return -1;
    }

    FT_Face face;
    if (0 != FT_New_Face(lib, ttf_path, 0, &face)) {
        FT_Done_FreeType(lib);
        free(priv);
        return -1;
    }

    /* FreeType bakes DPI upfront; pass physical pixel size directly. */
    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)(size_pts * dpi_scale));
    priv->ft_lib = lib;
    priv->ft_face = face;
    {
        /* FreeType metrics are 26.6 fixed-point (1/64 px); >> 6 converts. */
        FT_Size_Metrics *sm = &face->size->metrics;
        int ascent = (int)(sm->ascender >> 6);
        int descent = -(int)(sm->descender >> 6);

        atlas->ascent_px = ascent;
        atlas->cell_h = ascent + descent;
    }

    {
        /* 'M' advance is the canonical monospace cell width. */
        if (0 == FT_Load_Char(face, 'M', FT_LOAD_DEFAULT))
            atlas->cell_w = (int)(face->glyph->advance.x >> 6);
        else
            atlas->cell_w = atlas->cell_h / 2;
    }

    {
        /* Design units -> 26.6 pixel units via FT_MulFix, then >> 6 to pixels.
         */
        FT_Fixed scale = face->size->metrics.y_scale;
        int ul_pos = (int)(FT_MulFix(face->underline_position, scale) >> 6);
        int ul_thick = (int)(FT_MulFix(face->underline_thickness, scale) >> 6);
        atlas->underline_pos_px = -ul_pos;
        if (atlas->underline_pos_px < 1)
            atlas->underline_pos_px = 1;
        atlas->underline_thickness_px = ul_thick;
        if (atlas->underline_thickness_px < 1)
            atlas->underline_thickness_px = 1;
    }
#endif
    unsigned char *zeros = calloc((size_t)(FONT_ATLAS_W * FONT_ATLAS_H), 1);
    if (!zeros) {
#if defined(__APPLE__)
        CFRelease(priv->ct_font);
        CGColorSpaceRelease(priv->cs_rgb);
#else
        FT_Done_Face(priv->ft_face);
        FT_Done_FreeType(priv->ft_lib);
#endif
        free(priv);
        return -1;
    }
    glGenTextures(1, &atlas->texture_id);
    glBindTexture(GL_TEXTURE_2D, atlas->texture_id);
    /* GL_UNPACK_ALIGNMENT 1: single-channel rows are not 4-byte-aligned. */
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, FONT_ATLAS_W, FONT_ATLAS_H, 0, GL_RED,
                 GL_UNSIGNED_BYTE, zeros);
    free(zeros);
    /* CLAMP_TO_EDGE prevents border pixels of one glyph bleeding into adjacent
     * glyphs when UVs land exactly on the atlas edge. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    atlas->priv = priv;

    /* Pre-warm ASCII to avoid rasterization stalls on the first rendered frame.
     */
    for (uint32_t cp = ASCII_PRINTABLE_FIRST; cp <= ASCII_PRINTABLE_LAST;
         cp++) {
        GlyphEntry *e = cache_alloc(priv, cp);
        if (e)
            rasterize(atlas, priv, e);
    }

    return 0;
}

int font_atlas_create(FontAtlas *atlas, const char *ttf_path, float size_pts,
                      float dpi_scale, float line_height) {
    return atlas_create_impl(atlas, ttf_path, size_pts, dpi_scale, line_height);
}

int font_atlas_create_bold(FontAtlas *atlas, const char *ttf_path,
                           float size_pts, float dpi_scale, float line_height) {
    return atlas_create_impl(atlas, ttf_path, size_pts, dpi_scale, line_height);
}

int font_atlas_create_italic(FontAtlas *atlas, const char *ttf_path,
                             float size_pts, float dpi_scale,
                             float line_height) {
    return atlas_create_impl(atlas, ttf_path, size_pts, dpi_scale, line_height);
}

int font_atlas_create_bold_italic(FontAtlas *atlas, const char *ttf_path,
                                  float size_pts, float dpi_scale,
                                  float line_height) {
    return atlas_create_impl(atlas, ttf_path, size_pts, dpi_scale, line_height);
}

void font_atlas_destroy(FontAtlas *atlas) {
    if (!atlas || !atlas->priv)
        return;
    FontPriv *priv = (FontPriv *)atlas->priv;
    if (atlas->texture_id) {
        glDeleteTextures(1, &atlas->texture_id);
        atlas->texture_id = 0;
    }
#if defined(__APPLE__)
    if (priv->ct_font)
        CFRelease(priv->ct_font);
    if (priv->cs_rgb)
        CGColorSpaceRelease(priv->cs_rgb);
#else
    if (priv->ft_face)
        FT_Done_Face(priv->ft_face);
    if (priv->ft_lib)
        FT_Done_FreeType(priv->ft_lib);
#endif
    free(priv);
    atlas->priv = NULL;
}

int font_has_glyph(FontAtlas *atlas, uint32_t cp) {
    GlyphEntry *e = get_or_add(atlas, cp);
    return e && e->valid;
}

void font_atlas_glyph(FontAtlas *atlas, uint32_t cp, float baseline_x,
                      float baseline_y, GlyphQuad *out) {
    GlyphEntry *e = get_or_add(atlas, cp);
    if (!e || !e->valid || e->aw <= 0) {
        memset(out, 0, sizeof *out);
        return;
    }
    out->x0 = roundf(baseline_x + (float)e->bx);
    out->y0 = roundf(baseline_y - (float)e->by);
    out->x1 = out->x0 + (float)e->aw;
    out->y1 = out->y0 + (float)e->ah;
    out->s0 = (float)e->ax / (float)FONT_ATLAS_W;
    out->t0 = (float)e->ay / (float)FONT_ATLAS_H;
    out->s1 = (float)(e->ax + e->aw) / (float)FONT_ATLAS_W;
    out->t1 = (float)(e->ay + e->ah) / (float)FONT_ATLAS_H;
}
