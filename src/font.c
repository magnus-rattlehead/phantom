#if defined(__APPLE__)
#  define GL_SILENCE_DEPRECATION
#  include <OpenGL/gl3.h>
#  include <CoreText/CoreText.h>
#  include <CoreGraphics/CoreGraphics.h>
#else
#  include <GL/gl.h>
#  include <ft2build.h>
#  include FT_FREETYPE_H
#endif

#include "font.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Unicode codepoint range constants for UTF-16 surrogate pair encoding */
#define UNICODE_BMP_MAX      0xFFFF
#define UNICODE_SMP_START    0x10000
#define SURROGATE_HIGH_START 0xD800
#define SURROGATE_LOW_START  0xDC00
#define SURROGATE_DATA_BITS  0x3FF

/* ASCII printable range (for atlas pre-warming) */
#define ASCII_PRINTABLE_FIRST 0x20
#define ASCII_PRINTABLE_LAST  0x7E

#define GLYPH_CACHE_BUCKETS 1024
#define GLYPH_CACHE_MAX     8192  /* pool entries; far more than any session needs */

typedef struct GlyphEntry {
    uint32_t          cp;
    int               ax, ay;   /* top-left position in atlas (texels) */
    int               aw, ah;   /* glyph bitmap dimensions in pixels */
    int               bx, by;   /* left bearing and above-baseline distance */
    int               valid;    /* 0 = codepoint not present in font face */
    struct GlyphEntry *next;
} GlyphEntry;

typedef struct {
#if defined(__APPLE__)
    CTFontRef        ct_font;
    CGColorSpaceRef  cs_rgb;    /* cached DeviceRGB colorspace for bitmap contexts */
#else
    FT_Library       ft_lib;
    FT_Face          ft_face;
#endif
    float            dpi_scale; /* physical pixels per logical pixel */
    GlyphEntry      *buckets[GLYPH_CACHE_BUCKETS];
    GlyphEntry       pool[GLYPH_CACHE_MAX];
    int              pool_used;
    int              pack_x;
    int              pack_y;
    int              row_h;
} FontPriv;

static GlyphEntry *cache_find(FontPriv *priv, uint32_t cp)
{
    GlyphEntry *e = priv->buckets[cp % GLYPH_CACHE_BUCKETS];
    for (; e; e = e->next) {
        if (e->cp == cp) return e;
    }
    return NULL;
}

/* Returns NULL if the glyph pool is exhausted. */
static GlyphEntry *cache_alloc(FontPriv *priv, uint32_t cp)
{
    if (priv->pool_used >= GLYPH_CACHE_MAX) return NULL;
    GlyphEntry *e = &priv->pool[priv->pool_used++];
    memset(e, 0, sizeof *e);
    e->cp   = cp;
    e->next = priv->buckets[cp % GLYPH_CACHE_BUCKETS];
    priv->buckets[cp % GLYPH_CACHE_BUCKETS] = e;
    return e;
}

#if defined(__APPLE__)
static CGGlyph cp_to_glyph(CTFontRef font, uint32_t cp)
{
    UniChar chars[2];
    int nchars;
    if (cp > UNICODE_BMP_MAX) {
        /* Encode as a UTF-16 surrogate pair for supplementary codepoints. */
        uint32_t u = cp - UNICODE_SMP_START;
        chars[0]   = (UniChar)(SURROGATE_HIGH_START + (u >> 10));
        chars[1]   = (UniChar)(SURROGATE_LOW_START  + (u & SURROGATE_DATA_BITS));
        nchars = 2;
    } else {
        chars[0] = (UniChar)cp;
        nchars   = 1;
    }
    CGGlyph g = 0;
    CTFontGetGlyphsForCharacters(font, chars, &g, nchars);
    return g;
}
#endif

static void rasterize(FontAtlas *atlas, FontPriv *priv, GlyphEntry *e)
{
#if defined(__APPLE__)
    CGGlyph glyph = cp_to_glyph(priv->ct_font, e->cp);
#else
    FT_UInt glyph = FT_Get_Char_Index(priv->ft_face, (FT_ULong)e->cp);
#endif
    if (0 == glyph) { e->valid = 0; return; }
#if defined(__APPLE__)
    CGRect bbox = CTFontGetBoundingRectsForGlyphs(
                      priv->ct_font, kCTFontOrientationDefault, &glyph, NULL, 1);

    if (bbox.size.width < 0.5 || bbox.size.height < 0.5) {
        e->bx = e->by = e->aw = e->ah = 0;
        e->valid = 1;
        return;
    }

    /* Bounding box in logical pixels (CoreText user-space units, y-up). */
    CGFloat left_f   = floor(bbox.origin.x);
    CGFloat bottom_f = floor(bbox.origin.y);
    CGFloat right_f  = ceil(bbox.origin.x + bbox.size.width);
    CGFloat top_f    = ceil(bbox.origin.y + bbox.size.height);

    /* Scale to physical pixels; bearings stay logical -> render.c scales. */
    float   ds  = priv->dpi_scale;
    int     aw  = (int)ceil((right_f  - left_f)   * ds);
    int     ah  = (int)ceil((top_f    - bottom_f)  * ds);

    /* bx/by stored in physical pixels for vertex coordinate arithmetic. */
    e->bx = (int)round(left_f * ds);
    e->by = (int)round(top_f  * ds);
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
    FT_Bitmap   *bm   = &slot->bitmap;

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

    /* Advance to the next shelf row if the current one is full. */
    if (priv->pack_x + aw > FONT_ATLAS_W) {
        priv->pack_x  = 0;
        priv->pack_y += priv->row_h + 1;
        priv->row_h   = 0;
    }
    if (priv->pack_y + ah > FONT_ATLAS_H) {
        e->valid = 0;
        return;
    }
#if defined(__APPLE__)
    /* Render into a 32-bit BGRA context on an opaque black background.
     * CTFontDrawGlyphs requires a color (non-grayscale) context on macOS. */
    size_t stride = (size_t)aw * 4;
    uint8_t *buf  = malloc((size_t)ah * stride);
    if (!buf) { e->valid = 0; return; }

    /* Opaque black background so CoreGraphics AA blends against a known value. */
    for (int i = 0; i < aw * ah; i++) {
        buf[i*4+0] = 0;
        buf[i*4+1] = 0;
        buf[i*4+2] = 0;
        buf[i*4+3] = 255;
    }

    CGContextRef ctx = CGBitmapContextCreate(
        buf, (size_t)aw, (size_t)ah, 8, stride, priv->cs_rgb,
        kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
    if (!ctx) { free(buf); e->valid = 0; return; }

    CGContextSetRGBFillColor(ctx, 1.0, 1.0, 1.0, 1.0);
    CGContextSetShouldAntialias(ctx, true);
    CGContextSetAllowsAntialiasing(ctx, true);
    /* Disable LCD subpixel rendering, the atlas is single-channel GL_RED. */
    CGContextSetShouldSmoothFonts(ctx, false);
    CGContextSetShouldSubpixelPositionFonts(ctx, false);
    CGContextSetShouldSubpixelQuantizeFonts(ctx, false);

    /* Scale context by dpi_scale so CoreText rasterises at physical resolution.
     * The glyph position is still in logical units (CTFont coordinate space). */
    CGContextScaleCTM(ctx, (CGFloat)priv->dpi_scale, (CGFloat)priv->dpi_scale);

    /* Draw at the offset that places the pixel box at (0,0) in the context.
     * Quartz y is up from the context bottom; (-left_f, -bottom_f) aligns the
     * glyph's lower-left logical pixel with the context origin. */
    CGPoint pos = { -left_f, -bottom_f };
    CTFontDrawGlyphs(priv->ct_font, &glyph, &pos, 1, ctx);
    CGContextRelease(ctx);

    /* kCGBitmapByteOrder32Little + AlphaPremultipliedFirst = [B,G,R,A]; R at offset 2. */
    uint8_t *gray = malloc((size_t)(aw * ah));
    if (!gray) { free(buf); e->valid = 0; return; }
    for (int i = 0; i < aw * ah; i++) {
        gray[i] = buf[i * 4 + 2];
    }
    free(buf);

    glBindTexture(GL_TEXTURE_2D, atlas->texture_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0,
                    priv->pack_x, priv->pack_y, aw, ah,
                    GL_RED, GL_UNSIGNED_BYTE, gray);
    glBindTexture(GL_TEXTURE_2D, 0);
    free(gray);
#else
    /* FreeType renders directly into bm->buffer, no extraction step needed.
     * FreeType may pad each row to alignment boundary, so bm->pitch can be
     * larger than bm->width */
    glBindTexture(GL_TEXTURE_2D, atlas->texture_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (bm->pitch == aw) {
        glTexSubImage2D(GL_TEXTURE_2D, 0,
                        priv->pack_x, priv->pack_y, aw, ah,
                        GL_RED, GL_UNSIGNED_BYTE, bm->buffer);
    } else {
        for (int row = 0; row < ah; row++) {
            glTexSubImage2D(GL_TEXTURE_2D, 0,
                            priv->pack_x, priv->pack_y + row, aw, 1,
                            GL_RED, GL_UNSIGNED_BYTE,
                            bm->buffer + row * bm->pitch);
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
#endif
    e->ax = priv->pack_x;
    e->ay = priv->pack_y;
    e->aw = aw;
    e->ah = ah;

    if (ah > priv->row_h) priv->row_h = ah;
    priv->pack_x += aw + 1;
}

static GlyphEntry *get_or_add(FontAtlas *atlas, uint32_t cp)
{
    FontPriv   *priv = (FontPriv *)atlas->priv;
    GlyphEntry *e    = cache_find(priv, cp);
    if (!e) {
        e = cache_alloc(priv, cp);
        if (e) rasterize(atlas, priv, e);
    }
    return e;
}

static int atlas_create_impl(FontAtlas *atlas, const char *ttf_path,
                              float size_pts, float dpi_scale,
                              float line_height)
{
    FontPriv *priv = calloc(1, sizeof *priv);
    if (!priv) return -1;
    priv->dpi_scale = dpi_scale;

#if defined(__APPLE__)
    /* Load font from file via CGFont, then wrap in CTFont for metrics. */
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
                       NULL, (const UInt8 *)ttf_path,
                       (CFIndex)strlen(ttf_path), false);
    if (!url) { free(priv); return -1; }

    CGDataProviderRef provider = CGDataProviderCreateWithURL(url);
    CFRelease(url);
    if (!provider) { free(priv); return -1; }

    CGFontRef cg_font = CGFontCreateWithDataProvider(provider);
    CGDataProviderRelease(provider);
    if (!cg_font) { free(priv); return -1; }

    /* CTFont size is in logical (point) units; Retina scaling is handled by
     * multiplying all derived metrics by dpi_scale and by CGContextScaleCTM
     * in rasterize(), so the CTFont itself stays at logical size. */
    CTFontRef ct_font = CTFontCreateWithGraphicsFont(
                            cg_font, (CGFloat)size_pts, NULL, NULL);
    CGFontRelease(cg_font);
    if (!ct_font) { free(priv); return -1; }

    priv->ct_font = ct_font;
    priv->cs_rgb  = CGColorSpaceCreateDeviceRGB();

    {
        /* cell_h: Kitty-style  -  actual inter-line spacing via CTFrame. */
        CFStringRef sample2 = CFSTR("AQWMH_gyl \nAQWMH_gyl ");
        CFMutableAttributedStringRef astr2 =
            CFAttributedStringCreateMutable(NULL, 0);
        CFAttributedStringReplaceString(
            astr2, CFRangeMake(0, 0), sample2);
        CFAttributedStringSetAttribute(
            astr2, CFRangeMake(0, CFStringGetLength(sample2)),
            kCTFontAttributeName, ct_font);
        CTFramesetterRef fset =
            CTFramesetterCreateWithAttributedString(astr2);
        CFRelease(astr2);
        CGMutablePathRef fpath = CGPathCreateMutable();
        CGPathAddRect(fpath, NULL, CGRectMake(0, 0, 4000, 4000));
        CTFrameRef frame = CTFramesetterCreateFrame(
            fset, CFRangeMake(0, 0), fpath, NULL);
        CGPathRelease(fpath);
        CFRelease(fset);
        CFArrayRef lines = CTFrameGetLines(frame);
        CGFloat base_cell_h;
        if (CFArrayGetCount(lines) >= 2) {
            CGPoint orig[2];
            CTFrameGetLineOrigins(frame, CFRangeMake(0, 2), orig);
            base_cell_h = orig[0].y - orig[1].y;
        } else {
            base_cell_h = CTFontGetAscent(ct_font)
                          + CTFontGetDescent(ct_font);
        }
        CFRelease(frame);

        /* ascent_px: Kitty-style  -  ExcludeTypographicLeading bounds. */
        CFStringRef sample = CFSTR("AQWMH_gyl ");
        CFMutableAttributedStringRef astr =
            CFAttributedStringCreateMutable(NULL, 0);
        CFAttributedStringReplaceString(
            astr, CFRangeMake(0, 0), sample);
        CFAttributedStringSetAttribute(
            astr, CFRangeMake(0, CFStringGetLength(sample)),
            kCTFontAttributeName, ct_font);
        CTLineRef ln = CTLineCreateWithAttributedString(astr);
        CFRelease(astr);
        CGRect bounds = CTLineGetBoundsWithOptions(
            ln, kCTLineBoundsExcludeTypographicLeading);
        CFRelease(ln);
        CGFloat base_ascent = bounds.size.height + bounds.origin.y;

        /* Apply line_height multiplier; extra space split above/below. */
        CGFloat scaled_h = base_cell_h * (CGFloat)line_height;
        CGFloat extra    = scaled_h - base_cell_h;
        atlas->cell_h    = (int)ceilf((float)(scaled_h   * dpi_scale));
        atlas->ascent_px = (int)floorf(
            (float)((base_ascent + extra / 2.0) * dpi_scale) + 0.5f);
        if (atlas->ascent_px < 1) { atlas->ascent_px = 1; }
    }

    {
        CGFloat max_adv = 0.0;
        for (int ch = 32; ch <= 126; ch++) {
            UniChar uc = (UniChar)ch;
            CGGlyph g = 0;
            if (CTFontGetGlyphsForCharacters(ct_font, &uc, &g, 1)
                    && g) {
                CGSize adv = { 0.0, 0.0 };
                CTFontGetAdvancesForGlyphs(
                    ct_font, kCTFontOrientationDefault, &g, &adv, 1);
                if (adv.width > max_adv) { max_adv = adv.width; }
            }
        }
        if (max_adv > 0.0) {
            atlas->cell_w =
                (int)ceilf((float)(max_adv * dpi_scale));
        } else {
            atlas->cell_w = atlas->cell_h / 2;
        }
    }

    /* Underline metrics: CoreText y-up (negative = below baseline) ->
     * screen y-down (positive = below baseline). */
    {
        CGFloat ul_pos   = CTFontGetUnderlinePosition(ct_font);
        CGFloat ul_thick = CTFontGetUnderlineThickness(ct_font);
        atlas->underline_pos_px = (int)round(-ul_pos * dpi_scale);
        if (atlas->underline_pos_px < 1) atlas->underline_pos_px = 1;
        atlas->underline_thickness_px = (int)ceil(ul_thick * dpi_scale);
        if (atlas->underline_thickness_px < 1) atlas->underline_thickness_px = 1;
    }
#else
    FT_Library lib;
    if (0 != FT_Init_FreeType(&lib)) { free(priv); return -1; }

    FT_Face face;
    if (0 != FT_New_Face(lib, ttf_path, 0, &face)) {
        FT_Done_FreeType(lib); free(priv); return -1;
    }

    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)(size_pts * dpi_scale));
    priv->ft_lib = lib;
    priv->ft_face = face;
    {
        FT_Size_Metrics *sm = &face->size->metrics;
        /* >> 6 is to convert to px */
        int ascent = (int)(sm->ascender >> 6);
        int descent = -(int)(sm->descender >> 6);

        atlas->ascent_px = ascent;
        atlas->cell_h = ascent + descent;
    }

    {
        if (0 == FT_Load_Char(face, 'M', FT_LOAD_DEFAULT))
            atlas->cell_w = (int)(face->glyph->advance.x >> 6);
        else
            atlas->cell_w = atlas->cell_h / 2;
    }

    {
        FT_Fixed scale = face->size->metrics.y_scale;
        int ul_pos = (int)(FT_MulFix(face->underline_position, scale) >> 6);
        int ul_thick = (int)(FT_MulFix(face->underline_thickness, scale) >> 6);
        atlas->underline_pos_px = -ul_pos;
        if (atlas->underline_pos_px < 1) atlas->underline_pos_px = 1;
        atlas->underline_thickness_px = ul_thick;
        if (atlas->underline_thickness_px < 1) atlas->underline_thickness_px = 1;
    }
#endif
    /* Create a zero-filled single-channel (GL_RED) atlas texture. */
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
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8,
                 FONT_ATLAS_W, FONT_ATLAS_H, 0,
                 GL_RED, GL_UNSIGNED_BYTE, zeros);
    free(zeros);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    atlas->priv = priv;

    /* Pre-warm printable ASCII to avoid first-frame latency spikes. */
    for (uint32_t cp = ASCII_PRINTABLE_FIRST; cp <= ASCII_PRINTABLE_LAST; cp++) {
        GlyphEntry *e = cache_alloc(priv, cp);
        if (e) rasterize(atlas, priv, e);
    }

    return 0;
}

int font_atlas_create(FontAtlas *atlas, const char *ttf_path,
                      float size_pts, float dpi_scale, float line_height)
{
    return atlas_create_impl(atlas, ttf_path, size_pts, dpi_scale,
                             line_height);
}

int font_atlas_create_bold(FontAtlas *atlas, const char *ttf_path,
                            float size_pts, float dpi_scale,
                            float line_height)
{
    return atlas_create_impl(atlas, ttf_path, size_pts, dpi_scale,
                             line_height);
}

int font_atlas_create_italic(FontAtlas *atlas, const char *ttf_path,
                              float size_pts, float dpi_scale,
                              float line_height)
{
    return atlas_create_impl(atlas, ttf_path, size_pts, dpi_scale,
                             line_height);
}

int font_atlas_create_bold_italic(FontAtlas *atlas, const char *ttf_path,
                                   float size_pts, float dpi_scale,
                                   float line_height)
{
    return atlas_create_impl(atlas, ttf_path, size_pts, dpi_scale,
                             line_height);
}

void font_atlas_destroy(FontAtlas *atlas)
{
    if (!atlas || !atlas->priv) return;
    FontPriv *priv = (FontPriv *)atlas->priv;
    if (atlas->texture_id) {
        glDeleteTextures(1, &atlas->texture_id);
        atlas->texture_id = 0;
    }
#if defined(__APPLE__)
    if (priv->ct_font) CFRelease(priv->ct_font);
    if (priv->cs_rgb)  CGColorSpaceRelease(priv->cs_rgb);
#else
    if (priv->ft_face) FT_Done_Face(priv->ft_face);
    if (priv->ft_lib)  FT_Done_FreeType(priv->ft_lib);
#endif
    free(priv);
    atlas->priv = NULL;
}

int font_has_glyph(FontAtlas *atlas, uint32_t cp)
{
    GlyphEntry *e = get_or_add(atlas, cp);
    return e && e->valid;
}

void font_atlas_glyph(FontAtlas *atlas, uint32_t cp,
                      float baseline_x, float baseline_y,
                      GlyphQuad *out)
{
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
