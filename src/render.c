#if defined(__APPLE__)
#  define GL_SILENCE_DEPRECATION
#  include <OpenGL/gl3.h>
#else
#  include <GL/gl.h>
#endif

#include "render.h"
#include "font.h"
#include "terminal.h"
#include "search.h"
#include "utf8.h"
#include "config.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provide empty-string fallbacks so resolve_font_path can reference these
 * unconditionally regardless of whether cmake found a system font. */
#if !defined(PHANTOM_DEFAULT_FONT)
#  define PHANTOM_DEFAULT_FONT ""
#endif
#if !defined(PHANTOM_DEFAULT_FONT_BOLD)
#  define PHANTOM_DEFAULT_FONT_BOLD ""
#endif
#if !defined(PHANTOM_DEFAULT_FONT_ITALIC)
#  define PHANTOM_DEFAULT_FONT_ITALIC ""
#endif
#if !defined(PHANTOM_DEFAULT_FONT_BOLD_ITALIC)
#  define PHANTOM_DEFAULT_FONT_BOLD_ITALIC ""
#endif

/*
 * Full-cell quads: the fragment shader composites the glyph atlas onto the
 * background within the glyph's UV bounds  -  no separate background quad needed.
 *
 * Vertex formats:
 *   CELL: x,y, s,t, fg(rgba), bg(rgba), uv_bounds(s0,t0,s1,t1) = 16 floats
 *   UL:   x,y, r,g,b,a                                          =  6 floats
 */

#define CELL_FPV  16  /* floats/vertex: pos(2) uv(2) fg(4) bg(4) bounds(4) */
#define UL_FPV     6  /* x, y, r, g, b, a */
#define VERTS_PQ   6  /* 2 triangles per quad */

/* Vertex component widths (floats) */
#define VTX_POS    2
#define VTX_UV     2
#define VTX_COL    4
#define VTX_BOUNDS 4

#define SHADER_LOG_LEN 512
#define FONT_PATH_LEN  1024

#define BYTE_MASK  0xFF
#define BYTE_MAX_F 255.0f

static const char *UL_VERT =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec4 a_col;\n"
    "uniform vec2 u_screen;\n"
    "out vec4 v_col;\n"
    "void main() {\n"
    "    float nx = (a_pos.x / u_screen.x) * 2.0 - 1.0;\n"
    "    float ny = 1.0 - (a_pos.y / u_screen.y) * 2.0;\n"
    "    gl_Position = vec4(nx, ny, 0.0, 1.0);\n"
    "    v_col = a_col;\n"
    "}\n";

static const char *UL_FRAG =
    "#version 330 core\n"
    "in vec4 v_col;\n"
    "out vec4 frag;\n"
    "void main() { frag = v_col; }\n";

static const char *CELL_VERT =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "layout(location=2) in vec4 a_fg;\n"
    "layout(location=3) in vec4 a_bg;\n"
    "layout(location=4) in vec4 a_uv_bounds;\n"
    "uniform vec2 u_screen;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_fg;\n"
    "out vec4 v_bg;\n"
    "out vec4 v_uv_bounds;\n"
    "void main() {\n"
    "    float nx = (a_pos.x / u_screen.x) * 2.0 - 1.0;\n"
    "    float ny = 1.0 - (a_pos.y / u_screen.y) * 2.0;\n"
    "    gl_Position = vec4(nx, ny, 0.0, 1.0);\n"
    "    v_uv        = a_uv;\n"
    "    v_fg        = a_fg;\n"
    "    v_bg        = a_bg;\n"
    "    v_uv_bounds = a_uv_bounds;\n"
    "}\n";

/*
 * Fragment shader: sample the atlas only within the glyph's UV region
 * (v_uv_bounds); output background everywhere else.  The no-glyph sentinel
 * is uv_bounds = (1,1,0,0) whose x-min > x-max makes in_bounds always false.
 */
static const char *CELL_FRAG =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "in vec4 v_fg;\n"
    "in vec4 v_bg;\n"
    "in vec4 v_uv_bounds;\n"
    "uniform sampler2D u_atlas;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "    bool in_bounds = v_uv.x >= v_uv_bounds.x && v_uv.x <= v_uv_bounds.z\n"
    "                  && v_uv.y >= v_uv_bounds.y && v_uv.y <= v_uv_bounds.w;\n"
    "    float a = in_bounds ? texture(u_atlas, v_uv).r : 0.0;\n"
    "    frag = vec4(mix(v_bg.rgb, v_fg.rgb, a), 1.0);\n"
    "}\n";

/* Font face indices used for atlas/VAO/VBO/vertex arrays. */
#define FACE_REGULAR     0
#define FACE_BOLD        1
#define FACE_ITALIC      2
#define FACE_BOLD_ITALIC 3
#define FACE_COUNT       4

#define MAX_CELLS (512 * 256)

struct Renderer {
    SDL_Window   *window;
    SDL_GLContext gl_ctx;

    FontAtlas atlases[FACE_COUNT];
    int       face_is_real[FACE_COUNT]; /* 0 = falls back to FACE_REGULAR */

    GLuint cell_prog;
    GLint  cell_u_screen;
    GLint  cell_u_atlas;

    GLuint ul_prog;
    GLint  ul_u_screen;

    GLuint face_vao[FACE_COUNT];
    GLuint face_vbo[FACE_COUNT];
    GLuint ul_vao, ul_vbo;

    /* CPU-side vertex staging buffers */
    float *face_verts[FACE_COUNT];
    float *ul_verts;

    /* VBO capacities for stream-orphaning uploads */
    GLsizeiptr cell_buf_sz;
    GLsizeiptr ul_buf_sz;

    /* Reusable cell snapshot, grown on demand, never shrunk */
    Cell *cell_buf;
    int   cell_buf_cap;

    /* Search highlight colors derived from palette at create time */
    uint32_t hl_rgba;        /* non-active hit bg */
    uint32_t hl_active_rgba; /* active hit bg */
};

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[SHADER_LOG_LEN];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "shader compile error: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint compile_prog(const char *vsrc, const char *fsrc)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   vsrc);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fsrc);
    if (!vs || !fs) { glDeleteShader(vs); glDeleteShader(fs); return 0; }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[SHADER_LOG_LEN];
        glGetProgramInfoLog(prog, sizeof log, NULL, log);
        fprintf(stderr, "program link error: %s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static void rgba_to_floats(uint32_t c, float *r, float *g, float *b, float *a)
{
    *r = ((c >> 24) & BYTE_MASK) / BYTE_MAX_F;
    *g = ((c >> 16) & BYTE_MASK) / BYTE_MAX_F;
    *b = ((c >>  8) & BYTE_MASK) / BYTE_MAX_F;
    *a = ((c >>  0) & BYTE_MASK) / BYTE_MAX_F;
}

static void setup_cell_vao(GLuint vao, GLuint vbo, GLsizeiptr sz)
{
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sz, NULL, GL_STREAM_DRAW);
    int stride = CELL_FPV * (int)sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, VTX_POS, GL_FLOAT, GL_FALSE, stride,
                          (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, VTX_UV, GL_FLOAT, GL_FALSE, stride,
                          (void *)(VTX_POS * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, VTX_COL, GL_FLOAT, GL_FALSE, stride,
                          (void *)((VTX_POS + VTX_UV) * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, VTX_COL, GL_FLOAT, GL_FALSE, stride,
                          (void *)((VTX_POS + VTX_UV + VTX_COL)
                                   * sizeof(float)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, VTX_BOUNDS, GL_FLOAT, GL_FALSE, stride,
                          (void *)((VTX_POS + VTX_UV + VTX_COL + VTX_COL)
                                   * sizeof(float)));
    glBindVertexArray(0);
}

static void setup_ul_vao(GLuint vao, GLuint vbo, GLsizeiptr sz)
{
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sz, NULL, GL_STREAM_DRAW);
    int stride = UL_FPV * (int)sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, VTX_POS, GL_FLOAT, GL_FALSE, stride,
                          (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, VTX_COL, GL_FLOAT, GL_FALSE, stride,
                          (void *)(VTX_POS * sizeof(float)));
    glBindVertexArray(0);
}

/* UV values are extrapolated from the glyph quad to the full cell.
 * Fragments outside the glyph's UV range collapse to bg via mix(bg,fg,0). */
static void push_cell_quad(float *v, int *n,
                            float cx0, float cy0, float cx1, float cy1,
                            const GlyphQuad *q,
                            uint32_t fg, uint32_t bg)
{
    float fr, fg_, fb, fa, br, bg_, bb, ba;
    rgba_to_floats(fg, &fr, &fg_, &fb, &fa);
    rgba_to_floats(bg, &br, &bg_, &bb, &ba);

    float s00, s10, t00, t10;
    float bs, bt, es, et;

    if (q && q->x1 > q->x0 && q->y1 > q->y0) {
        float inv_gw = 1.0f / (q->x1 - q->x0);
        float inv_gh = 1.0f / (q->y1 - q->y0);
        float dsdx   = (q->s1 - q->s0) * inv_gw;
        float dtdy   = (q->t1 - q->t0) * inv_gh;
        s00 = q->s0 + (cx0 - q->x0) * dsdx;
        s10 = q->s0 + (cx1 - q->x0) * dsdx;
        t00 = q->t0 + (cy0 - q->y0) * dtdy;
        t10 = q->t0 + (cy1 - q->y0) * dtdy;
        bs = q->s0; bt = q->t0; es = q->s1; et = q->t1;
    } else {
        /* No glyph: sentinel uv_bounds (x_min > x_max) forces bg everywhere. */
        s00 = s10 = t00 = t10 = 0.0f;
        bs = 1.0f; bt = 1.0f; es = 0.0f; et = 0.0f;
    }

    float row[CELL_FPV * VERTS_PQ] = {
        cx0,cy0, s00,t00, fr,fg_,fb,fa, br,bg_,bb,ba, bs,bt,es,et,
        cx1,cy0, s10,t00, fr,fg_,fb,fa, br,bg_,bb,ba, bs,bt,es,et,
        cx0,cy1, s00,t10, fr,fg_,fb,fa, br,bg_,bb,ba, bs,bt,es,et,
        cx1,cy0, s10,t00, fr,fg_,fb,fa, br,bg_,bb,ba, bs,bt,es,et,
        cx1,cy1, s10,t10, fr,fg_,fb,fa, br,bg_,bb,ba, bs,bt,es,et,
        cx0,cy1, s00,t10, fr,fg_,fb,fa, br,bg_,bb,ba, bs,bt,es,et,
    };
    memcpy(v + *n, row, sizeof row);
    *n += CELL_FPV * VERTS_PQ;
}

static void push_ul_quad(float *v, int *n,
                          float x0, float y0, float x1, float y1,
                          uint32_t color)
{
    float r, g, b, a;
    rgba_to_floats(color, &r, &g, &b, &a);
    float q[UL_FPV * VERTS_PQ] = {
        x0,y0,r,g,b,a,  x1,y0,r,g,b,a,  x0,y1,r,g,b,a,
        x1,y0,r,g,b,a,  x1,y1,r,g,b,a,  x0,y1,r,g,b,a,
    };
    memcpy(v + *n, q, sizeof q);
    *n += UL_FPV * VERTS_PQ;
}

/* Returns hue in [0, 360), or -1 for achromatic colors (saturation < 0.15). */
static float hue_of(uint32_t rgba)
{
    float r = (float)((rgba >> 24) & 0xFFu) / 255.0f;
    float g = (float)((rgba >> 16) & 0xFFu) / 255.0f;
    float b = (float)((rgba >>  8) & 0xFFu) / 255.0f;
    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float d  = mx - mn;
    if (d < 0.01f || mx < 0.01f || d / mx < 0.15f) return -1.0f;
    float h;
    if (mx == r)      h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (mx == g) h = (b - r) / d + 2.0f;
    else              h = (r - g) / d + 4.0f;
    return h / 6.0f * 360.0f;
}

/* Build an RRGGBBAA color from HSL + alpha. */
static uint32_t hsl_to_rgba(float h, float s, float l, float a)
{
    float c  = (1.0f - fabsf(2.0f * l - 1.0f)) * s;
    float hp = h / 60.0f;
    float x  = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    float m  = l - c / 2.0f;
    float r = m, g = m, b = m;
    int   seg = (int)hp % 6;
    if      (seg == 0) { r += c; g += x; }
    else if (seg == 1) { r += x; g += c; }
    else if (seg == 2) { g += c; b += x; }
    else if (seg == 3) { g += x; b += c; }
    else if (seg == 4) { r += x; b += c; }
    else               { r += c; b += x; }
    return ((uint32_t)(r * 255.0f) << 24)
         | ((uint32_t)(g * 255.0f) << 16)
         | ((uint32_t)(b * 255.0f) <<  8)
         | (uint32_t)(a * 255.0f);
}

/* Derive two search highlight colors from the 16-color terminal palette.
 * Uses circular mean hue of chromatic palette entries, then takes the
 * complementary hue (±180°) for active and a split offset (±150°) for
 * non-active so both sit opposite the palette's dominant color region. */
static void derive_highlight_colors(uint32_t *hl, uint32_t *hl_active)
{
    static const uint32_t palette[] = {
        COLOR_0,  COLOR_1,  COLOR_2,  COLOR_3,
        COLOR_4,  COLOR_5,  COLOR_6,  COLOR_7,
        COLOR_8,  COLOR_9,  COLOR_10, COLOR_11,
        COLOR_12, COLOR_13, COLOR_14, COLOR_15,
    };
    float sin_sum = 0.0f, cos_sum = 0.0f;
    int   n = 0;
    for (int i = 0; i < 16; i++) {
        float h = hue_of(palette[i]);
        if (h < 0.0f) continue;
        float rad = h * (float)M_PI / 180.0f;
        sin_sum += sinf(rad);
        cos_sum += cosf(rad);
        n++;
    }
    float avg = (n > 0)
        ? atan2f(sin_sum / (float)n, cos_sum / (float)n) * 180.0f / (float)M_PI
        : 0.0f;
    if (avg < 0.0f) avg += 360.0f;

    float h_active   = fmodf(avg + 180.0f, 360.0f);
    float h_inactive = fmodf(avg + 150.0f, 360.0f);

    *hl        = hsl_to_rgba(h_inactive, 0.65f, 0.32f, 0.60f);
    *hl_active = hsl_to_rgba(h_active,   0.85f, 0.45f, 1.00f);
}

/* Resolve font path: user config takes priority, then cmake system default.
 * Returns 1 if a path was written, 0 if neither source provided one. */
static int resolve_font_path(char *out, size_t cap,
                              const char *config_path,
                              const char *system_path)
{
    if (config_path && '\0' != config_path[0]) {
        snprintf(out, cap, "%s", config_path);
    } else if (system_path && '\0' != system_path[0]) {
        snprintf(out, cap, "%s", system_path);
    } else {
        return 0;
    }
    return 1;
}

Renderer *renderer_create(SDL_Window *window)
{
    Renderer *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->window = window;

    r->gl_ctx = SDL_GL_CreateContext(window);
    if (!r->gl_ctx) {
        fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
        free(r); return NULL;
    }
    SDL_GL_MakeCurrent(window, r->gl_ctx);
    if (!SDL_GL_SetSwapInterval(-1))
        SDL_GL_SetSwapInterval(0);

    r->cell_prog = compile_prog(CELL_VERT, CELL_FRAG);
    r->ul_prog   = compile_prog(UL_VERT,   UL_FRAG);
    if (!r->cell_prog || !r->ul_prog) { renderer_destroy(r); return NULL; }

    r->cell_u_screen = glGetUniformLocation(r->cell_prog, "u_screen");
    r->cell_u_atlas  = glGetUniformLocation(r->cell_prog, "u_atlas");
    r->ul_u_screen   = glGetUniformLocation(r->ul_prog,   "u_screen");

    size_t cell_sz = (size_t)(MAX_CELLS * CELL_FPV * VERTS_PQ) * sizeof(float);
    size_t ul_sz   = (size_t)(MAX_CELLS * UL_FPV * VERTS_PQ) * sizeof(float);
    r->cell_buf_sz = (GLsizeiptr)cell_sz;
    r->ul_buf_sz   = (GLsizeiptr)ul_sz;

    for (int f = 0; f < FACE_COUNT; f++) {
        glGenVertexArrays(1, &r->face_vao[f]);
        glGenBuffers(1, &r->face_vbo[f]);
        setup_cell_vao(r->face_vao[f], r->face_vbo[f], (GLsizeiptr)cell_sz);
        r->face_verts[f] = malloc(cell_sz);
        if (!r->face_verts[f]) { renderer_destroy(r); return NULL; }
    }

    glGenVertexArrays(1, &r->ul_vao);
    glGenBuffers(1, &r->ul_vbo);
    setup_ul_vao(r->ul_vao, r->ul_vbo, (GLsizeiptr)ul_sz);

    r->ul_verts = malloc(ul_sz);
    if (!r->ul_verts) { renderer_destroy(r); return NULL; }

    /* Detect HiDPI scale: framebuffer pixels / window points. */
    int px_w, px_h, pt_w, pt_h;
    SDL_GetWindowSizeInPixels(window, &px_w, &px_h);
    SDL_GetWindowSize(window, &pt_w, &pt_h);
    float dpi_scale = (pt_w > 0) ? (float)px_w / (float)pt_w : 1.0f;

    /* Font resolution priority per face:
     *   1. FONT / FONT_BOLD / FONT_ITALIC / FONT_BOLD_ITALIC in config.h
     *   2. PHANTOM_DEFAULT_FONT_* injected by cmake (system font search) */
    char p[FONT_PATH_LEN];
    if (!resolve_font_path(p, sizeof p, FONT, PHANTOM_DEFAULT_FONT)) {
        fprintf(stderr, "no font path found  -  set FONT in config.h\n");
        renderer_destroy(r); return NULL;
    }
    if (0 != font_atlas_create(&r->atlases[FACE_REGULAR], p,
                                FONT_SIZE, dpi_scale, LINE_HEIGHT)) {
        fprintf(stderr, "font_atlas_create failed: %s\n", p);
        renderer_destroy(r); return NULL;
    }
    r->face_is_real[FACE_REGULAR] = 1;

    if (resolve_font_path(p, sizeof p, FONT_BOLD, PHANTOM_DEFAULT_FONT_BOLD)
        && 0 == font_atlas_create_bold(&r->atlases[FACE_BOLD], p,
                                        FONT_SIZE, dpi_scale, LINE_HEIGHT))
        r->face_is_real[FACE_BOLD] = 1;

    if (resolve_font_path(p, sizeof p, FONT_ITALIC, PHANTOM_DEFAULT_FONT_ITALIC)
        && 0 == font_atlas_create_italic(&r->atlases[FACE_ITALIC], p,
                                          FONT_SIZE, dpi_scale,
                                          LINE_HEIGHT))
        r->face_is_real[FACE_ITALIC] = 1;

    if (resolve_font_path(p, sizeof p, FONT_BOLD_ITALIC,
                          PHANTOM_DEFAULT_FONT_BOLD_ITALIC)
        && 0 == font_atlas_create_bold_italic(
                    &r->atlases[FACE_BOLD_ITALIC], p, FONT_SIZE,
                    dpi_scale, LINE_HEIGHT))
        r->face_is_real[FACE_BOLD_ITALIC] = 1;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    derive_highlight_colors(&r->hl_rgba, &r->hl_active_rgba);
    return r;
}

void renderer_destroy(Renderer *r)
{
    if (!r) return;
    for (int f = 0; f < FACE_COUNT; f++) {
        font_atlas_destroy(&r->atlases[f]);
        if (r->face_vao[f]) {
            glDeleteVertexArrays(1, &r->face_vao[f]);
            glDeleteBuffers(1, &r->face_vbo[f]);
        }
        free(r->face_verts[f]);
    }
    if (r->cell_prog) glDeleteProgram(r->cell_prog);
    if (r->ul_prog)   glDeleteProgram(r->ul_prog);
    if (r->ul_vao) {
        glDeleteVertexArrays(1, &r->ul_vao);
        glDeleteBuffers(1, &r->ul_vbo);
    }
    free(r->ul_verts);
    free(r->cell_buf);
    if (r->gl_ctx) SDL_GL_DestroyContext(r->gl_ctx);
    free(r);
}


void renderer_get_cell_size(const Renderer *r, int *cell_w, int *cell_h)
{
    *cell_w = r->atlases[FACE_REGULAR].cell_w;
    *cell_h = r->atlases[FACE_REGULAR].cell_h;
}


static int int_cmp(const void *a, const void *b)
{
    int ia = *(const int *)a, ib = *(const int *)b;
    return (ia > ib) - (ia < ib);
}

/* Rec. 601 relative luminance (0..1), no pow() needed for our precision. */
static float luminance(uint32_t rgba)
{
    float r = (float)((rgba >> 24) & 0xFFu);
    float g = (float)((rgba >> 16) & 0xFFu);
    float b = (float)((rgba >>  8) & 0xFFu);
    return (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
}

/* Alpha-composite src (RRGGBBAA) over dst, returning opaque RRGGBBAA. */
static uint32_t blend_rgba(uint32_t src, uint32_t dst)
{
    uint32_t sa = src & 0xFFu;
    uint32_t sr = (src >> 24) & 0xFFu;
    uint32_t sg = (src >> 16) & 0xFFu;
    uint32_t sb = (src >>  8) & 0xFFu;
    uint32_t dr = (dst >> 24) & 0xFFu;
    uint32_t dg = (dst >> 16) & 0xFFu;
    uint32_t db = (dst >>  8) & 0xFFu;
    uint32_t r  = (sr * sa + dr * (255u - sa)) / 255u;
    uint32_t g  = (sg * sa + dg * (255u - sa)) / 255u;
    uint32_t b  = (sb * sa + db * (255u - sa)) / 255u;
    return (r << 24) | (g << 16) | (b << 8) | 0xFFu;
}

static void flush_vertex_batch(GLuint prog, GLint u_screen_loc,
                                float win_w, float win_h,
                                GLuint vao, GLuint vbo, GLsizeiptr buf_sz,
                                const float *verts, int n_floats, int fpv,
                                GLint u_atlas_loc, GLuint texture_id)
{
    glUseProgram(prog);
    glUniform2f(u_screen_loc, win_w, win_h);
    if (texture_id) {
        glUniform1i(u_atlas_loc, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture_id);
    }
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, buf_sz, NULL, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    (GLsizeiptr)(n_floats * (int)sizeof(float)), verts);
    glDrawArrays(GL_TRIANGLES, 0, n_floats / fpv);
}

void renderer_draw(Renderer *r, Terminal *term, SearchState *s)
{
    /* SDL3 short-circuits SDL_GL_MakeCurrent when the context is already current.
     * Releasing first forces the full platform path on re-acquire, which
     * resizes the CGL backbuffer to the current window bounds. */
    SDL_GL_MakeCurrent(r->window, NULL);
    SDL_GL_MakeCurrent(r->window, r->gl_ctx);

    int win_w, win_h;
    SDL_GetWindowSizeInPixels(r->window, &win_w, &win_h);
    glViewport(0, 0, win_w, win_h);
    {
        float cr, cg, cb, ca;
        rgba_to_floats(TERM_DEFAULT_BG, &cr, &cg, &cb, &ca);
        glClearColor(cr, cg, cb, ca);
    }
    glClear(GL_COLOR_BUFFER_BIT);

    if (!term) return;

    int cols   = terminal_cols(term);
    int rows   = terminal_rows(term);
    int cell_w = r->atlases[FACE_REGULAR].cell_w;
    int cell_h = r->atlases[FACE_REGULAR].cell_h;

    int needed = cols * rows;
    if (needed > r->cell_buf_cap) {
        free(r->cell_buf);
        r->cell_buf = malloc((size_t)needed * sizeof(Cell));
        r->cell_buf_cap = r->cell_buf ? needed : 0;
    }
    if (!r->cell_buf) return;

    int cursor_col, cursor_row;
    terminal_get_state(term, r->cell_buf, &cursor_col, &cursor_row);
    const Cell *cells = r->cell_buf;

    int face_n[FACE_COUNT] = {0};
    int ul_n = 0;

    int search_active  = s && search_is_active(s);
    int sb_total       = search_active ? terminal_sb_total_rows(term) : 0;
    int scroll         = search_active ? terminal_scroll_offset(term) : 0;
    int live_base      = search_active ? search_live_sb_base(s)       : 0;
    int active_abs     = search_active ? search_current_abs_row(s)    : -1;

    /* Snapshot search results once per frame; binary search is lock-free. */
    int  snap_n   = 0;
    int *snap     = search_active ? search_snapshot(s, &snap_n) : NULL;

    for (int row = 0; row < rows; row++) {
        float row_y0 = (float)(row * cell_h);
        float row_y1 = row_y0 + (float)cell_h;

        uint32_t row_hl = 0;
        if (search_active) {
            int abs_row;
            if (scroll == 0 || row >= scroll) {
                /* Live grid row  -  use live_base captured at search time. */
                abs_row = live_base + (row - (scroll > 0 ? scroll : 0));
            } else {
                /* Scrollback row. */
                abs_row = sb_total - scroll + row;
            }
            if (abs_row >= 0 &&
                bsearch(&abs_row, snap, (size_t)snap_n,
                        sizeof(int), int_cmp)) {
                row_hl = (abs_row == active_abs)
                         ? r->hl_active_rgba
                         : r->hl_rgba;
            }
        }

        for (int col = 0; col < cols; col++) {
            const Cell *c = &cells[row * cols + col];
            float x0 = (float)(col * cell_w);
            float y0 = row_y0;
            float x1 = x0 + (float)cell_w;
            float y1 = row_y1;

            uint32_t bg = c->bg, fg = c->fg;
            if (c->attrs & ATTR_REVERSE) {
                uint32_t tmp = fg; fg = bg; bg = tmp;
            }
            if (terminal_cell_selected(term, col, row)) {
                uint32_t tmp = fg; fg = bg; bg = tmp;
            }
            if (row_hl) {
                bg = blend_rgba(row_hl, bg);
                float lbg = luminance(bg), lfg = luminance(fg);
                float bright = lbg > lfg ? lbg : lfg;
                float dim    = lbg > lfg ? lfg : lbg;
                if ((bright + 0.05f) / (dim + 0.05f) < 4.5f)
                    fg = lbg > 0.5f ? 0x282828FFu : 0xFBF1C7FFu;
            }

            const FontAtlas *ra = &r->atlases[FACE_REGULAR];
            if (c->attrs & ATTR_UNDERLINE) {
                float bly   = y0 + (float)ra->ascent_px;
                float ul_y0 = bly + (float)ra->underline_pos_px;
                float ul_y1 = ul_y0 + (float)ra->underline_thickness_px;
                if (ul_y0 < y0) ul_y0 = y0;
                if (ul_y1 > y1) ul_y1 = y1;
                if (ul_y0 < ul_y1)
                    push_ul_quad(r->ul_verts, &ul_n,
                                 x0, ul_y0, x1, ul_y1, fg);
            }

            /* Select the most specific face that is available. */
            int is_bold   = (c->attrs & ATTR_BOLD)   != 0;
            int is_italic = (c->attrs & ATTR_ITALIC)  != 0;
            int face;
            if (is_bold && is_italic
                && r->face_is_real[FACE_BOLD_ITALIC]) {
                face = FACE_BOLD_ITALIC;
            } else if (is_bold && r->face_is_real[FACE_BOLD]) {
                face = FACE_BOLD;
            } else if (is_italic && r->face_is_real[FACE_ITALIC]) {
                face = FACE_ITALIC;
            } else {
                face = FACE_REGULAR;
            }
            FontAtlas *ga = &r->atlases[face];

            GlyphQuad q;
            const GlyphQuad *qptr = NULL;
            if (c->ch != ' ' && font_has_glyph(ga, c->ch)) {
                font_atlas_glyph(ga, c->ch, x0,
                                 y0 + (float)ga->ascent_px, &q);
                if (q.x1 > q.x0 && q.y1 > q.y0) qptr = &q;
            }

            push_cell_quad(r->face_verts[face], &face_n[face],
                           x0, y0, x1, y1, qptr, fg, bg);
        }
    }

    free(snap);

    glUseProgram(r->cell_prog);
    glUniform2f(r->cell_u_screen, (float)win_w, (float)win_h);
    glUniform1i(r->cell_u_atlas, 0);
    glActiveTexture(GL_TEXTURE0);
    for (int f = 0; f < FACE_COUNT; f++) {
        if (0 == face_n[f]) continue;
        glBindTexture(GL_TEXTURE_2D, r->atlases[f].texture_id);
        glBindVertexArray(r->face_vao[f]);
        glBindBuffer(GL_ARRAY_BUFFER, r->face_vbo[f]);
        glBufferData(GL_ARRAY_BUFFER, r->cell_buf_sz, NULL, GL_STREAM_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        (GLsizeiptr)(face_n[f] * (int)sizeof(float)),
                        r->face_verts[f]);
        glDrawArrays(GL_TRIANGLES, 0, face_n[f] / CELL_FPV);
    }

    if (terminal_cursor_visible(term)
        && cursor_col >= 0 && cursor_row >= 0) {
        const Cell *cc = &cells[cursor_row * cols + cursor_col];
        uint32_t cur_color = (cc->attrs & ATTR_REVERSE) ? cc->bg : cc->fg;
        float cx  = (float)(cursor_col * cell_w);
        float cy0 = (float)(cursor_row * cell_h);
        float cy1 = cy0 + (float)cell_h;
        int shape = terminal_cursor_shape(term);
        if (0 == shape || 1 == shape || 2 == shape) {
            push_ul_quad(r->ul_verts, &ul_n,
                         cx, cy0,
                         cx + (float)cell_w, cy1, cur_color);
        } else if (3 == shape || 4 == shape) {
            push_ul_quad(r->ul_verts, &ul_n,
                         cx, cy1 - 2.0f,
                         cx + (float)cell_w, cy1, cur_color);
        } else {
            /* 5 = blink beam, 6 = steady beam */
            push_ul_quad(r->ul_verts, &ul_n,
                         cx, cy0, cx + 2.0f, cy1, cur_color);
        }
    }

    if (ul_n > 0) {
        flush_vertex_batch(r->ul_prog, r->ul_u_screen,
                           (float)win_w, (float)win_h,
                           r->ul_vao, r->ul_vbo, r->ul_buf_sz,
                           r->ul_verts, ul_n, UL_FPV, -1, 0);
    }

    glBindVertexArray(0);
    glUseProgram(0);
}

#define SEARCH_BAR_PAD       4    /* px inside bar, top/bottom/left */
#define SEARCH_BAR_MARGIN    8    /* px gap from window edge */
#define SEARCH_BAR_CELLS    36    /* bar width in cell units */
#define SEARCH_COUNTER_CELLS 7    /* cells reserved for " NN/MM" on the right */
/* Render text as a row of cell quads into an existing vertex buffer.
 * Each codepoint occupies one cell of width r->atlases[FACE_REGULAR].cell_w.
 * Returns the number of codepoints drawn. */
static int draw_str(Renderer *r, float *cv, int *cn,
                    float x, float cy0, float cy1,
                    const char *text, int max_cells,
                    uint32_t fg, uint32_t bg)
{
    const unsigned char *s  = (const unsigned char *)text;
    float                cw = (float)r->atlases[FACE_REGULAR].cell_w;
    int                  drawn = 0;

    while (*s && drawn < max_cells) {
        uint32_t cp  = utf8_decode(&s);
        float    cx0 = x + (float)drawn * cw;
        float cx1 = cx0 + cw;
        GlyphQuad q;
        const GlyphQuad *qptr = NULL;

        if (' ' != cp && font_has_glyph(&r->atlases[FACE_REGULAR], cp)) {
            font_atlas_glyph(&r->atlases[FACE_REGULAR], cp, cx0,
                             cy0 + (float)r->atlases[FACE_REGULAR].ascent_px, &q);
            if (q.x1 > q.x0 && q.y1 > q.y0) qptr = &q;
        }

        push_cell_quad(cv, cn, cx0, cy0, cx1, cy1, qptr, fg, bg);
        drawn++;
    }
    return drawn;
}
/* Upload verts[0..n_floats) to vbo and draw as GL_TRIANGLES.
 * When texture_id != 0, binds it to texture unit 0 and sets u_atlas=0. */
void renderer_draw_search_overlay(Renderer *r, SearchState *s,
                                   Terminal *term)
{
    if (!search_is_active(s)) return;
    (void)term;

    int win_w, win_h;
    SDL_GetWindowSizeInPixels(r->window, &win_w, &win_h);

    int cell_w = r->atlases[FACE_REGULAR].cell_w;
    int cell_h = r->atlases[FACE_REGULAR].cell_h;

    int ul_n = 0;

    float bar_w  = (float)(SEARCH_BAR_CELLS * cell_w);
    float bar_h  = (float)(cell_h + SEARCH_BAR_PAD * 2);
    float bar_x0 = (float)win_w - bar_w - (float)SEARCH_BAR_MARGIN;
    float bar_y0 = (float)SEARCH_BAR_MARGIN;

    push_ul_quad(r->ul_verts, &ul_n,
                 bar_x0, bar_y0,
                 bar_x0 + bar_w, bar_y0 + bar_h,
                 COLOR_8);

    if (ul_n > 0) {
        flush_vertex_batch(r->ul_prog, r->ul_u_screen,
                           (float)win_w, (float)win_h,
                           r->ul_vao, r->ul_vbo, r->ul_buf_sz,
                           r->ul_verts, ul_n, UL_FPV, -1, 0);
    }

    float text_y0  = bar_y0 + (float)SEARCH_BAR_PAD;
    float text_y1  = text_y0 + (float)cell_h;
    float text_x   = bar_x0 + (float)SEARCH_BAR_PAD;
    int   bar_cap  = SEARCH_BAR_CELLS - 1;  /* 1-cell right margin */
    int   cell_n   = 0;

    int prefix_cells = draw_str(r, r->face_verts[FACE_REGULAR], &cell_n,
                                text_x, text_y0, text_y1,
                                "Search: ", bar_cap,
                                COLOR_7, COLOR_8);

    /* Reserve SEARCH_COUNTER_CELLS + 1 (cursor) on the right. */
    int query_max = bar_cap - prefix_cells - SEARCH_COUNTER_CELLS - 1;
    if (query_max < 0) query_max = 0;
    float qx = text_x + (float)(prefix_cells * cell_w);

    int q_cells = 0;
    if (query_max > 0) {
        q_cells = draw_str(r, r->face_verts[FACE_REGULAR], &cell_n,
                           qx, text_y0, text_y1,
                           search_query(s), query_max,
                           COLOR_15, COLOR_8);
    }

    float cursor_x = qx + (float)(q_cells * cell_w);
    draw_str(r, r->face_verts[FACE_REGULAR], &cell_n, cursor_x, text_y0, text_y1,
             "|", 1, COLOR_11, COLOR_8);

    /* Match counter, right-aligned within the bar. */
    char counter[16];
    int  total   = search_result_count(s);
    int  current = search_current_idx(s);
    if (total > 0) {
        snprintf(counter, sizeof counter, " %d/%d", current + 1, total);
    } else {
        snprintf(counter, sizeof counter, " 0/0");
    }
    int   ctr_len = (int)strlen(counter);
    float ctr_x   = bar_x0 + bar_w
                    - (float)(ctr_len * cell_w) - (float)SEARCH_BAR_PAD;
    draw_str(r, r->face_verts[FACE_REGULAR], &cell_n, ctr_x, text_y0, text_y1,
             counter, ctr_len, COLOR_11, COLOR_8);

    if (cell_n > 0) {
        flush_vertex_batch(r->cell_prog, r->cell_u_screen,
                           (float)win_w, (float)win_h,
                           r->face_vao[FACE_REGULAR], r->face_vbo[FACE_REGULAR], r->cell_buf_sz,
                           r->face_verts[FACE_REGULAR], cell_n, CELL_FPV,
                           r->cell_u_atlas, r->atlases[FACE_REGULAR].texture_id);
    }

    glBindVertexArray(0);
    glUseProgram(0);
}

void renderer_draw_autocomplete_overlay(Renderer *r,
                                        const Autocomplete *ac,
                                        Terminal *term)
{
    const char *sug = autocomplete_get_suggestion(ac);
    if (!sug || '\0' == sug[0]) return;

    int cell_w = r->atlases[FACE_REGULAR].cell_w;
    int cell_h = r->atlases[FACE_REGULAR].cell_h;
    int cols   = terminal_cols(term);
    int rows   = terminal_rows(term);

    Cell *snap = malloc((size_t)(cols * rows) * sizeof(Cell));
    if (!snap) return;

    int cur_col = -1, cur_row = -1;
    terminal_get_state(term, snap, &cur_col, &cur_row);
    free(snap);

    if (cur_col < 0 || cur_row < 0) return;  /* scrolled back */

    int        cell_n = 0;
    const char *p     = sug;
    int         row   = cur_row;

    while ('\0' != *p && row < rows) {
        const char *nl      = strchr(p, '\n');
        size_t      seg_len = nl ? (size_t)(nl - p) : strlen(p);

        float x0;
        int   avail;
        if (row == cur_row) {
            /* First line: leave one cell gap so cursor block stays visible. */
            x0    = (float)((cur_col + 1) * cell_w);
            avail = cols - cur_col - 2;
        } else {
            x0    = 0.0f;
            avail = cols;
        }

        if (avail > 0 && seg_len > 0) {
            char seg[4096];
            size_t copy = seg_len < sizeof seg - 1 ? seg_len : sizeof seg - 1;
            memcpy(seg, p, copy);
            seg[copy] = '\0';
            float y0 = (float)(row * cell_h);
            float y1 = y0 + (float)cell_h;
            draw_str(r, r->face_verts[FACE_REGULAR], &cell_n,
                     x0, y0, y1, seg, avail, COLOR_8, TERM_DEFAULT_BG);
        }

        if (!nl) break;
        p = nl + 1;
        row++;
    }

    if (cell_n > 0) {
        int win_w, win_h;
        SDL_GetWindowSizeInPixels(r->window, &win_w, &win_h);
        flush_vertex_batch(r->cell_prog, r->cell_u_screen,
                           (float)win_w, (float)win_h,
                           r->face_vao[FACE_REGULAR], r->face_vbo[FACE_REGULAR],
                           r->cell_buf_sz,
                           r->face_verts[FACE_REGULAR], cell_n, CELL_FPV,
                           r->cell_u_atlas,
                           r->atlases[FACE_REGULAR].texture_id);
        glBindVertexArray(0);
        glUseProgram(0);
    }
}


void renderer_draw_scrollbar(Renderer *r, Terminal *term)
{
    int sb_total = terminal_sb_total_rows(term);
    if (sb_total <= 0) return;

    int win_w, win_h;
    SDL_GetWindowSizeInPixels(r->window, &win_w, &win_h);

    int rows   = terminal_rows(term);
    int scroll = terminal_scroll_offset(term);
    int total  = sb_total + rows;

    float fh       = (float)win_h;
    float thumb_h  = (float)rows / (float)total * fh;
    if (thumb_h < SCROLLBAR_MIN_H_PX) thumb_h = SCROLLBAR_MIN_H_PX;

    float track_travel = fh - thumb_h;
    float top_frac     = (sb_total > 0)
                         ? (float)(sb_total - scroll) / (float)sb_total
                         : 0.0f;
    float thumb_y = top_frac * track_travel;

    float x0 = (float)(win_w - SCROLLBAR_W_PX);
    float x1 = (float)win_w;
    int   ul_n = 0;

    push_ul_quad(r->ul_verts, &ul_n, x0, 0.0f, x1, fh, 0x1D202140u);
    push_ul_quad(r->ul_verts, &ul_n, x0, thumb_y, x1, thumb_y + thumb_h,
                 0x928374CCu);

    flush_vertex_batch(r->ul_prog, r->ul_u_screen,
                       (float)win_w, (float)win_h,
                       r->ul_vao, r->ul_vbo, r->ul_buf_sz,
                       r->ul_verts, ul_n, UL_FPV, -1, 0);
    glBindVertexArray(0);
    glUseProgram(0);
}

#define TAB_BAR_BG     COLOR_0
#define TAB_ACTIVE_BG  COLOR_8
#define TAB_FG         COLOR_7
#define TAB_ACTIVE_FG  COLOR_15


/* Build a padded, possibly-truncated label into out (UTF-8).
 * Total display width = slot_cells.  Inner = slot_cells - 2 (1-cell padding
 * each side).  Long titles get the last inner cell replaced with "…". */
static void make_tab_label(const char *title, int slot_cells,
                           char *out, int out_sz)
{
    int inner = slot_cells - 2;
    if (inner < 1) inner = 1;

    /* Count codepoints in title. */
    const unsigned char *s = (const unsigned char *)title;
    int ncp = 0;
    while (*s) { utf8_decode(&s); ncp++; }

    out[0] = ' '; /* leading padding */
    int pos = 1;

    if (ncp <= inner) {
        /* Fits  -  copy verbatim then space-pad. */
        int tbytes = (int)strlen(title);
        if (pos + tbytes < out_sz - 1) {
            memcpy(out + pos, title, (size_t)tbytes);
            pos += tbytes;
        }
        int pad = inner - ncp;
        while (pad-- > 0 && pos < out_sz - 1)
            out[pos++] = ' ';
    } else {
        /* Truncate: copy (inner-1) codepoints then append "…". */
        s = (const unsigned char *)title;
        for (int c = 0; c < inner - 1; c++) {
            const unsigned char *prev = s;
            utf8_decode(&s);
            int nb = (int)(s - prev);
            if (pos + nb >= out_sz - 4) break;
            memcpy(out + pos, prev, (size_t)nb);
            pos += nb;
        }
        /* U+2026 HORIZONTAL ELLIPSIS = 0xE2 0x80 0xA6 */
        if (pos + 3 < out_sz - 1) {
            out[pos++] = (char)0xE2;
            out[pos++] = (char)0x80;
            out[pos++] = (char)0xA6;
        }
    }
    out[pos++] = ' '; /* trailing padding */
    if (pos < out_sz) out[pos] = '\0';
    else out[out_sz - 1] = '\0';
}

void renderer_draw_tab_bar(Renderer *r, int n, int active,
                           const char *const *titles)
{
    if (n <= 1) return;

    int win_w, win_h;
    SDL_GetWindowSizeInPixels(r->window, &win_w, &win_h);

    int   cw    = r->atlases[FACE_REGULAR].cell_w;
    int   ch    = r->atlases[FACE_REGULAR].cell_h;
    float y0    = (float)(win_h - ch);
    float y1    = (float)win_h;
    float fw    = (float)win_w;

    int slot_cells = (cw > 0) ? win_w / n / cw : 3;
    if (slot_cells > TAB_TITLE_MAX + 2) slot_cells = TAB_TITLE_MAX + 2;
    if (slot_cells < 3)                 slot_cells = 3;
    int slot_px = slot_cells * cw;

    /* Background strip */
    int ul_n = 0;
    push_ul_quad(r->ul_verts, &ul_n, 0.0f, y0, fw, y1, TAB_BAR_BG);

    /* Active tab highlight */
    float ax0 = (float)(active * slot_px);
    float ax1 = ax0 + (float)slot_px;
    push_ul_quad(r->ul_verts, &ul_n, ax0, y0, ax1, y1, TAB_ACTIVE_BG);

    flush_vertex_batch(r->ul_prog, r->ul_u_screen,
                       fw, (float)win_h,
                       r->ul_vao, r->ul_vbo, r->ul_buf_sz,
                       r->ul_verts, ul_n, UL_FPV, -1, 0);

    /* Tab labels */
    char label[TAB_TITLE_MAX * 4 + 8];
    int  cell_n = 0;
    for (int i = 0; i < n; i++) {
        const char *title = (titles && titles[i]) ? titles[i] : "";
        make_tab_label(title, slot_cells, label, (int)sizeof label);
        float    lx = (float)(i * slot_px);
        uint32_t fg = (i == active) ? TAB_ACTIVE_FG : TAB_FG;
        uint32_t bg = (i == active) ? TAB_ACTIVE_BG : TAB_BAR_BG;
        draw_str(r, r->face_verts[FACE_REGULAR], &cell_n,
                 lx, y0, y1, label, slot_cells, fg, bg);
    }

    if (cell_n > 0) {
        flush_vertex_batch(r->cell_prog, r->cell_u_screen,
                           fw, (float)win_h,
                           r->face_vao[FACE_REGULAR], r->face_vbo[FACE_REGULAR],
                           r->cell_buf_sz,
                           r->face_verts[FACE_REGULAR], cell_n, CELL_FPV,
                           r->cell_u_atlas,
                           r->atlases[FACE_REGULAR].texture_id);
    }
    glBindVertexArray(0);
    glUseProgram(0);
}
