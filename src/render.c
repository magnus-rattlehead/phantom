#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

#include "config.h"
#include "font.h"
#include "render.h"
#include "search.h"
#include "terminal.h"
#include "utf8.h"
#include "util.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provide empty-string fallbacks so resolve_font_path can reference these
 * unconditionally regardless of whether cmake found a system font. */
#if !defined(PHANTOM_DEFAULT_FONT)
#define PHANTOM_DEFAULT_FONT ""
#endif
#if !defined(PHANTOM_DEFAULT_FONT_BOLD)
#define PHANTOM_DEFAULT_FONT_BOLD ""
#endif
#if !defined(PHANTOM_DEFAULT_FONT_ITALIC)
#define PHANTOM_DEFAULT_FONT_ITALIC ""
#endif
#if !defined(PHANTOM_DEFAULT_FONT_BOLD_ITALIC)
#define PHANTOM_DEFAULT_FONT_BOLD_ITALIC ""
#endif

/* Full-cell quads: the fragment shader composites the glyph atlas onto the
 * background within the glyph's UV bounds -- no separate background quad. */

#define CELL_FPV 16 /* floats/vertex: pos(2) uv(2) fg(4) bg(4) bounds(4) */
#define UL_FPV 6    /* x, y, r, g, b, a */
#define SB_FPV 8    /* pos(2) uv(2) col(4) */
#define VERTS_PQ 6  /* 2 triangles per quad */

/* Scrollbar pill geometry and animation timing */
#define SB_W_PX 6       /* thumb width in framebuffer pixels */
#define SB_INSET 3      /* gap between pill and right window edge */
#define SB_SHOW_MS 1200 /* full-opacity hold after last scroll event */
#define SB_FADE_MS 400  /* fade-out duration */

/* Vertex component widths (floats) */
#define VTX_POS 2
#define VTX_UV 2
#define VTX_COL 4
#define VTX_BOUNDS 4

#define SHADER_LOG_LEN 512
#define FONT_PATH_LEN 1024

#define BYTE_MASK 0xFF
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

static const char *UL_FRAG = "#version 330 core\n"
                             "in vec4 v_col;\n"
                             "out vec4 frag;\n"
                             "void main() { frag = v_col; }\n";

/* Scrollbar: SDF capsule (r = half-width -> fully rounded ends). */
static const char *SB_VERT =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "layout(location=2) in vec4 a_col;\n"
    "uniform vec2 u_screen;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_col;\n"
    "void main() {\n"
    "    float nx = (a_pos.x / u_screen.x) * 2.0 - 1.0;\n"
    "    float ny = 1.0 - (a_pos.y / u_screen.y) * 2.0;\n"
    "    gl_Position = vec4(nx, ny, 0.0, 1.0);\n"
    "    v_uv = a_uv; v_col = a_col;\n"
    "}\n";

static const char *SB_FRAG =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "in vec4 v_col;\n"
    "uniform vec2 u_size;\n" /* pill pixel dimensions (width, height) */
    "out vec4 frag;\n"
    "void main() {\n"
    /* re-center UV so origin is pill center; scale to pixel space */
    "    vec2 p = (v_uv - 0.5) * u_size;\n"
    /* corner radius = half width -> fully rounded ends (stadium/capsule) */
    "    float r = u_size.x * 0.5;\n"
    /* IQ rounded-rect SDF: shrink box by r, measure distance to shrunken box */
    "    vec2 q = abs(p) - u_size * 0.5 + r;\n"
    "    float d = length(max(q,0.0))+min(max(q.x,q.y),0.0)-r;\n"
    /* 1px antialiased edge: negative d = inside, positive = outside */
    "    float a = 1.0 - smoothstep(-0.5, 0.5, d);\n"
    /* multiply SDF alpha by vertex alpha (pre-scaled fade animation value) */
    "    frag = vec4(v_col.rgb, v_col.a * a);\n"
    "}\n";

/* Cell shader: one full-cell quad; fragment gates atlas sampling to the
 * glyph's UV sub-rect and blends fg/bg by coverage. */
static const char *CELL_VERT =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "layout(location=2) in vec4 a_fg;\n"
    "layout(location=3) in vec4 a_bg;\n"
    "layout(location=4) in vec4 a_uv_bounds;\n" /* [s0,t0,s1,t1] glyph rect in
                                                   atlas */
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
    "uniform sampler2D u_atlas;\n" /* GL_R8 single-channel glyph coverage atlas
                                    */
    "out vec4 frag;\n"
    "void main() {\n"
    /* gate atlas sampling to the glyph's sub-rect; pixels outside show bg */
    "    bool in_bounds = v_uv.x >= v_uv_bounds.x && v_uv.x <= v_uv_bounds.z\n"
    "                  && v_uv.y >= v_uv_bounds.y && v_uv.y <= v_uv_bounds.w;\n"
    /* atlas red channel = glyph coverage: 0=transparent, 1=fully inked */
    "    float a = in_bounds ? texture(u_atlas, v_uv).r : 0.0;\n"
    /* blend fg ink over bg by coverage; alpha always 1 (cells are opaque) */
    "    frag = vec4(mix(v_bg.rgb, v_fg.rgb, a), 1.0);\n"
    "}\n";

/* Font face indices (atlas/VAO/VBO arrays are parallel to these). */
#define FACE_REGULAR 0
#define FACE_BOLD 1
#define FACE_ITALIC 2
#define FACE_BOLD_ITALIC 3
#define FACE_COUNT 4

#define MAX_CELLS (512 * 256)

struct Renderer {
    SDL_Window *window;
    SDL_GLContext gl_ctx;

    FontAtlas atlases[FACE_COUNT];
    int face_is_real[FACE_COUNT]; /* 0 = falls back to FACE_REGULAR */

    GLuint cell_prog;
    GLint cell_u_screen;
    GLint cell_u_atlas;

    GLuint ul_prog;
    GLint ul_u_screen;

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
    int cell_buf_cap;

    /* Search highlight colors derived from palette at create time */
    uint32_t hl_rgba;        /* non-active hit bg */
    uint32_t hl_active_rgba; /* active hit bg */

    /* Scrollbar pill */
    GLuint sb_prog;
    GLint sb_u_screen;
    GLint sb_u_size;
    GLuint sb_vao, sb_vbo;
    Uint64 sb_last_scroll_ms;
    int sb_last_offset;

    int needs_ctx_update; /* set on window resize; triggers CGL backbuffer sync
                           */
};

static GLuint compile_shader(GLenum type, const char *src) {
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

static GLuint compile_prog(const char *vsrc, const char *fsrc) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vsrc);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fsrc);
    if (!vs || !fs) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    /* shader objects not needed once linked */
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

static void rgba_to_floats(uint32_t c, float *r, float *g, float *b, float *a) {
    *r = ((c >> 24) & BYTE_MASK) / BYTE_MAX_F;
    *g = ((c >> 16) & BYTE_MASK) / BYTE_MAX_F;
    *b = ((c >> 8) & BYTE_MASK) / BYTE_MAX_F;
    *a = ((c >> 0) & BYTE_MASK) / BYTE_MAX_F;
}

static void setup_cell_vao(GLuint vao, GLuint vbo, GLsizeiptr sz) {
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sz, NULL, GL_STREAM_DRAW);
    int stride = CELL_FPV * (int)sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, VTX_POS, GL_FLOAT, GL_FALSE, stride, (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, VTX_UV, GL_FLOAT, GL_FALSE, stride,
                          (void *)(VTX_POS * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, VTX_COL, GL_FLOAT, GL_FALSE, stride,
                          (void *)((VTX_POS + VTX_UV) * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(
        3, VTX_COL, GL_FLOAT, GL_FALSE, stride,
        (void *)((VTX_POS + VTX_UV + VTX_COL) * sizeof(float)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(
        4, VTX_BOUNDS, GL_FLOAT, GL_FALSE, stride,
        (void *)((VTX_POS + VTX_UV + VTX_COL + VTX_COL) * sizeof(float)));
    glBindVertexArray(0);
}

static void setup_ul_vao(GLuint vao, GLuint vbo, GLsizeiptr sz) {
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sz, NULL, GL_STREAM_DRAW);
    int stride = UL_FPV * (int)sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, VTX_POS, GL_FLOAT, GL_FALSE, stride, (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, VTX_COL, GL_FLOAT, GL_FALSE, stride,
                          (void *)(VTX_POS * sizeof(float)));
    glBindVertexArray(0);
}

/* Fixed 192-byte VBO: one pill quad (6 verts * SB_FPV * 4 bytes). */
static void setup_sb_vao(GLuint vao, GLuint vbo) {
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, 192, NULL, GL_STREAM_DRAW);
    int stride = SB_FPV * (int)sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride,
                          (void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride,
                          (void *)(4 * sizeof(float)));
    glBindVertexArray(0);
}

/* alpha_scale baked into vertex alpha; avoids an extra per-frame uniform. */
static void push_sb_quad(float v[6 * SB_FPV], float x0, float y0, float x1,
                         float y1, uint32_t rgba, float alpha_scale) {
    float r, g, b, a;
    rgba_to_floats(rgba, &r, &g, &b, &a);
    a *= alpha_scale;
    float pts[6][4] = {
        {x0, y0, 0.f, 0.f}, {x1, y0, 1.f, 0.f}, {x1, y1, 1.f, 1.f},
        {x0, y0, 0.f, 0.f}, {x1, y1, 1.f, 1.f}, {x0, y1, 0.f, 1.f},
    };

    for (int i = 0; i < 6; i++) {
        v[i * SB_FPV + 0] = pts[i][0];
        v[i * SB_FPV + 1] = pts[i][1];
        v[i * SB_FPV + 2] = pts[i][2];
        v[i * SB_FPV + 3] = pts[i][3];
        v[i * SB_FPV + 4] = r;
        v[i * SB_FPV + 5] = g;
        v[i * SB_FPV + 6] = b;
        v[i * SB_FPV + 7] = a;
    }
}

/* Append 6 vertices for one cell quad to v[*n].  UVs are extrapolated from
 * the glyph quad to the full cell so the fragment shader can gate atlas
 * sampling to the glyph sub-rect.  NULL/zero-size q uses the sentinel
 * uv_bounds (1,1,0,0) which makes in_bounds always false -> pure bg fill. */
static void push_cell_quad(float *v, int *n, float cx0, float cy0, float cx1,
                           float cy1, const GlyphQuad *q, uint32_t fg,
                           uint32_t bg) {
    float fr, fg_, fb, fa, br, bg_, bb, ba;
    rgba_to_floats(fg, &fr, &fg_, &fb, &fa);
    rgba_to_floats(bg, &br, &bg_, &bb, &ba);

    float s00, s10, t00, t10;
    float bs, bt, es, et;

    if (q && q->x1 > q->x0 && q->y1 > q->y0) {
        /* Extrapolate atlas UVs from the glyph quad to the full cell corners.
         * dsdx/dtdy are the UV rates-of-change per pixel; applying them to
         * the cell corners gives correct atlas coordinates even when the cell
         * is wider/taller than the glyph bitmap. */
        float inv_gw = 1.0f / (q->x1 - q->x0);
        float inv_gh = 1.0f / (q->y1 - q->y0);
        float dsdx = (q->s1 - q->s0) * inv_gw;
        float dtdy = (q->t1 - q->t0) * inv_gh;
        s00 = q->s0 + (cx0 - q->x0) * dsdx;
        s10 = q->s0 + (cx1 - q->x0) * dsdx;
        t00 = q->t0 + (cy0 - q->y0) * dtdy;
        t10 = q->t0 + (cy1 - q->y0) * dtdy;
        bs = q->s0;
        bt = q->t0;
        es = q->s1;
        et = q->t1;
    } else {
        /* No glyph: sentinel uv_bounds (x_min > x_max) forces bg everywhere. */
        s00 = s10 = t00 = t10 = 0.0f;
        bs = 1.0f;
        bt = 1.0f;
        es = 0.0f;
        et = 0.0f;
    }

    float row[CELL_FPV * VERTS_PQ] = {
        cx0, cy0, s00, t00, fr, fg_, fb, fa, br, bg_, bb, ba, bs, bt, es, et,
        cx1, cy0, s10, t00, fr, fg_, fb, fa, br, bg_, bb, ba, bs, bt, es, et,
        cx0, cy1, s00, t10, fr, fg_, fb, fa, br, bg_, bb, ba, bs, bt, es, et,
        cx1, cy0, s10, t00, fr, fg_, fb, fa, br, bg_, bb, ba, bs, bt, es, et,
        cx1, cy1, s10, t10, fr, fg_, fb, fa, br, bg_, bb, ba, bs, bt, es, et,
        cx0, cy1, s00, t10, fr, fg_, fb, fa, br, bg_, bb, ba, bs, bt, es, et,
    };
    memcpy(v + *n, row, sizeof row);
    *n += CELL_FPV * VERTS_PQ;
}

/* Used for underlines, cursor, and search bar background. */
static void push_ul_quad(float *v, int *n, float x0, float y0, float x1,
                         float y1, uint32_t color) {
    float r, g, b, a;
    rgba_to_floats(color, &r, &g, &b, &a);
    float q[UL_FPV * VERTS_PQ] = {
        x0, y0, r, g, b, a, x1, y0, r, g, b, a, x0, y1, r, g, b, a,
        x1, y0, r, g, b, a, x1, y1, r, g, b, a, x0, y1, r, g, b, a,
    };
    memcpy(v + *n, q, sizeof q);
    *n += UL_FPV * VERTS_PQ;
}

/* Returns hue in [0, 360), or -1 for achromatic colors (saturation < 0.15). */
static float hue_of(uint32_t rgba) {
    float r = (float)((rgba >> 24) & 0xFFu) / 255.0f;
    float g = (float)((rgba >> 16) & 0xFFu) / 255.0f;
    float b = (float)((rgba >> 8) & 0xFFu) / 255.0f;
    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float d = mx - mn;
    if (d < 0.01f || mx < 0.01f || d / mx < 0.15f)
        return -1.0f;
    float h;
    if (mx == r)
        h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (mx == g)
        h = (b - r) / d + 2.0f;
    else
        h = (r - g) / d + 4.0f;
    return h / 6.0f * 360.0f;
}

static uint32_t hsl_to_rgba(float h, float s, float l, float a) {
    float c = (1.0f - fabsf(2.0f * l - 1.0f)) * s;
    float hp = h / 60.0f;
    float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    float m = l - c / 2.0f;
    float r = m, g = m, b = m;
    int seg = (int)hp % 6;
    if (seg == 0) {
        r += c;
        g += x;
    } else if (seg == 1) {
        r += x;
        g += c;
    } else if (seg == 2) {
        g += c;
        b += x;
    } else if (seg == 3) {
        g += x;
        b += c;
    } else if (seg == 4) {
        r += x;
        b += c;
    } else {
        r += c;
        b += x;
    }
    return ((uint32_t)(r * 255.0f) << 24) | ((uint32_t)(g * 255.0f) << 16) |
           ((uint32_t)(b * 255.0f) << 8) | (uint32_t)(a * 255.0f);
}

/* Derive search highlight colors from the palette's dominant hue.
 * Uses circular mean hue of chromatic entries, then picks complementary
 * (+180 deg) for active and split-complementary (+150 deg) for non-active
 * so both sit opposite the palette's dominant color region. */
static void derive_highlight_colors(uint32_t *hl, uint32_t *hl_active) {
    static const uint32_t palette[] = {
        COLOR_0,  COLOR_1,  COLOR_2,  COLOR_3,  COLOR_4,  COLOR_5,
        COLOR_6,  COLOR_7,  COLOR_8,  COLOR_9,  COLOR_10, COLOR_11,
        COLOR_12, COLOR_13, COLOR_14, COLOR_15,
    };
    float sin_sum = 0.0f, cos_sum = 0.0f;
    int n = 0;
    for (int i = 0; i < 16; i++) {
        float h = hue_of(palette[i]);
        if (h < 0.0f)
            continue;
        float rad = h * (float)M_PI / 180.0f;
        sin_sum += sinf(rad);
        cos_sum += cosf(rad);
        n++;
    }
    /* circular mean: atan2 of the average unit-vector components */
    float avg = (n > 0) ? atan2f(sin_sum / (float)n, cos_sum / (float)n) *
                              180.0f / (float)M_PI
                        : 0.0f;
    if (avg < 0.0f)
        avg += 360.0f;

    float h_active = fmodf(avg + 180.0f, 360.0f);   /* complementary hue */
    float h_inactive = fmodf(avg + 150.0f, 360.0f); /* split-complementary */

    *hl = hsl_to_rgba(h_inactive, 0.65f, 0.32f, 0.60f);
    *hl_active = hsl_to_rgba(h_active, 0.85f, 0.45f, 1.00f);
}

/* Resolve font path: user config takes priority, then cmake system default.
 * Returns 1 if a path was written, 0 if neither source provided one. */
static int resolve_font_path(char *out, size_t cap, const char *config_path,
                             const char *system_path) {
    if (config_path && '\0' != config_path[0]) {
        snprintf(out, cap, "%s", config_path);
    } else if (system_path && '\0' != system_path[0]) {
        snprintf(out, cap, "%s", system_path);
    } else {
        return 0;
    }
    return 1;
}

Renderer *renderer_create(SDL_Window *window) {
    Renderer *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->window = window;

    r->gl_ctx = SDL_GL_CreateContext(window);
    if (!r->gl_ctx) {
        fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
        free(r);
        return NULL;
    }
    SDL_GL_MakeCurrent(window, r->gl_ctx);
    /* Prefer adaptive vsync (-1); fall back to regular vsync (1) so the
     * render loop is always capped at the display refresh rate. */
    if (!SDL_GL_SetSwapInterval(-1))
        SDL_GL_SetSwapInterval(1);

    r->cell_prog = compile_prog(CELL_VERT, CELL_FRAG);
    r->ul_prog = compile_prog(UL_VERT, UL_FRAG);
    r->sb_prog = compile_prog(SB_VERT, SB_FRAG);
    if (!r->cell_prog || !r->ul_prog || !r->sb_prog) {
        renderer_destroy(r);
        return NULL;
    }

    /* Cache uniform locations; glGetUniformLocation is slow per-call. */
    r->cell_u_screen = glGetUniformLocation(r->cell_prog, "u_screen");
    r->cell_u_atlas = glGetUniformLocation(r->cell_prog, "u_atlas");
    r->ul_u_screen = glGetUniformLocation(r->ul_prog, "u_screen");
    r->sb_u_screen = glGetUniformLocation(r->sb_prog, "u_screen");
    r->sb_u_size = glGetUniformLocation(r->sb_prog, "u_size");

    size_t cell_sz = (size_t)(MAX_CELLS * CELL_FPV * VERTS_PQ) * sizeof(float);
    size_t ul_sz = (size_t)(MAX_CELLS * UL_FPV * VERTS_PQ) * sizeof(float);
    r->cell_buf_sz = (GLsizeiptr)cell_sz;
    r->ul_buf_sz = (GLsizeiptr)ul_sz;

    /* One VAO+VBO pair per font face; lets each face bind its own atlas
     * texture without rebinding mid-draw. */
    for (int f = 0; f < FACE_COUNT; f++) {
        glGenVertexArrays(1, &r->face_vao[f]);
        glGenBuffers(1, &r->face_vbo[f]);
        setup_cell_vao(r->face_vao[f], r->face_vbo[f], (GLsizeiptr)cell_sz);
        r->face_verts[f] = malloc(cell_sz);
        if (!r->face_verts[f]) {
            renderer_destroy(r);
            return NULL;
        }
    }

    glGenVertexArrays(1, &r->ul_vao);
    glGenBuffers(1, &r->ul_vbo);
    setup_ul_vao(r->ul_vao, r->ul_vbo, (GLsizeiptr)ul_sz);

    glGenVertexArrays(1, &r->sb_vao);
    glGenBuffers(1, &r->sb_vbo);
    setup_sb_vao(r->sb_vao, r->sb_vbo);
    r->sb_last_scroll_ms = SDL_GetTicks();
    r->sb_last_offset = -1;

    r->ul_verts = malloc(ul_sz);
    if (!r->ul_verts) {
        renderer_destroy(r);
        return NULL;
    }

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
        renderer_destroy(r);
        return NULL;
    }
    if (0 != font_atlas_create(&r->atlases[FACE_REGULAR], p, FONT_SIZE,
                               dpi_scale, LINE_HEIGHT)) {
        fprintf(stderr, "font_atlas_create failed: %s\n", p);
        renderer_destroy(r);
        return NULL;
    }
    r->face_is_real[FACE_REGULAR] = 1;

    /* Bold/italic/bold-italic faces are optional; missing ones fall back to
     * FACE_REGULAR at render time via face_is_real[]. */
    if (resolve_font_path(p, sizeof p, FONT_BOLD, PHANTOM_DEFAULT_FONT_BOLD) &&
        0 == font_atlas_create_bold(&r->atlases[FACE_BOLD], p, FONT_SIZE,
                                    dpi_scale, LINE_HEIGHT))
        r->face_is_real[FACE_BOLD] = 1;

    if (resolve_font_path(p, sizeof p, FONT_ITALIC,
                          PHANTOM_DEFAULT_FONT_ITALIC) &&
        0 == font_atlas_create_italic(&r->atlases[FACE_ITALIC], p, FONT_SIZE,
                                      dpi_scale, LINE_HEIGHT))
        r->face_is_real[FACE_ITALIC] = 1;

    if (resolve_font_path(p, sizeof p, FONT_BOLD_ITALIC,
                          PHANTOM_DEFAULT_FONT_BOLD_ITALIC) &&
        0 == font_atlas_create_bold_italic(&r->atlases[FACE_BOLD_ITALIC], p,
                                           FONT_SIZE, dpi_scale, LINE_HEIGHT))
        r->face_is_real[FACE_BOLD_ITALIC] = 1;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    derive_highlight_colors(&r->hl_rgba, &r->hl_active_rgba);
    return r;
}

void renderer_destroy(Renderer *r) {
    if (!r)
        return;
    for (int f = 0; f < FACE_COUNT; f++) {
        font_atlas_destroy(&r->atlases[f]);
        if (r->face_vao[f]) {
            glDeleteVertexArrays(1, &r->face_vao[f]);
            glDeleteBuffers(1, &r->face_vbo[f]);
        }
        free(r->face_verts[f]);
    }
    if (r->cell_prog)
        glDeleteProgram(r->cell_prog);
    if (r->ul_prog)
        glDeleteProgram(r->ul_prog);
    if (r->sb_prog)
        glDeleteProgram(r->sb_prog);
    if (r->ul_vao) {
        glDeleteVertexArrays(1, &r->ul_vao);
        glDeleteBuffers(1, &r->ul_vbo);
    }
    if (r->sb_vao) {
        glDeleteVertexArrays(1, &r->sb_vao);
        glDeleteBuffers(1, &r->sb_vbo);
    }
    free(r->ul_verts);
    free(r->cell_buf);
    if (r->gl_ctx)
        SDL_GL_DestroyContext(r->gl_ctx);
    free(r);
}

void renderer_notify_resize(Renderer *r) { r->needs_ctx_update = 1; }

void renderer_get_cell_size(const Renderer *r, int *cell_w, int *cell_h) {
    *cell_w = r->atlases[FACE_REGULAR].cell_w;
    *cell_h = r->atlases[FACE_REGULAR].cell_h;
}

/* Rec. 601 relative luminance (0..1), no pow() needed for our precision. */
static float luminance(uint32_t rgba) {
    float r = (float)((rgba >> 24) & 0xFFu);
    float g = (float)((rgba >> 16) & 0xFFu);
    float b = (float)((rgba >> 8) & 0xFFu);
    return (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
}

static uint32_t blend_rgba(uint32_t src, uint32_t dst) {
    uint32_t sa = src & 0xFFu;
    uint32_t sr = (src >> 24) & 0xFFu;
    uint32_t sg = (src >> 16) & 0xFFu;
    uint32_t sb = (src >> 8) & 0xFFu;
    uint32_t dr = (dst >> 24) & 0xFFu;
    uint32_t dg = (dst >> 16) & 0xFFu;
    uint32_t db = (dst >> 8) & 0xFFu;
    uint32_t r = (sr * sa + dr * (255u - sa)) / 255u;
    uint32_t g = (sg * sa + dg * (255u - sa)) / 255u;
    uint32_t b = (sb * sa + db * (255u - sa)) / 255u;
    return (r << 24) | (g << 16) | (b << 8) | 0xFFu;
}

typedef struct {
    GLuint prog;
    GLint u_screen_loc;
    float win_w, win_h;
    GLuint vao, vbo;
    int buf_sz;
    float *verts;
    int n_floats;
    int fpv;
    GLint u_atlas_loc; /* -1 if no atlas */
    GLuint texture_id; /* 0 if no texture */
} BatchDesc;

/* Upload and draw verts as GL_TRIANGLES.  VBO is orphaned before upload
 * so the driver can retire the old buffer without a GPU/CPU sync stall. */
static void flush_vertex_batch(const BatchDesc *b) {
    glUseProgram(b->prog);
    glUniform2f(b->u_screen_loc, b->win_w, b->win_h);
    if (b->texture_id) {
        glUniform1i(b->u_atlas_loc, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, b->texture_id);
    }
    glBindVertexArray(b->vao);
    glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)b->buf_sz, NULL, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    (GLsizeiptr)(b->n_floats * (int)sizeof(float)), b->verts);
    glDrawArrays(GL_TRIANGLES, 0, b->n_floats / b->fpv);
}

void renderer_frame_begin(Renderer *r) {
    /* Release then reacquire forces CGL to flush its backbuffer on resize;
     * redundant MakeCurrent on non-resize frames is a no-op on macOS. */
    if (r->needs_ctx_update) {
        SDL_GL_MakeCurrent(r->window, NULL);
        r->needs_ctx_update = 0;
    }
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
}

void renderer_draw(Renderer *r, Terminal *term, SearchState *s, int x_px,
                   int y_px, int w_px, int h_px, int is_active) {
    if (!term)
        return;

    int win_w, win_h;
    SDL_GetWindowSizeInPixels(r->window, &win_w, &win_h);

    /* Clip rendering to pane bounds so adjacent panes don't bleed. */
    glEnable(GL_SCISSOR_TEST);
    glScissor(x_px, win_h - y_px - h_px, w_px, h_px);

    int cols = terminal_cols(term);
    int rows = terminal_rows(term);
    int cell_w = r->atlases[FACE_REGULAR].cell_w;
    int cell_h = r->atlases[FACE_REGULAR].cell_h;

    /* Grow cell snapshot buffer on terminal resize; never shrink. */
    int needed = cols * rows;
    if (needed > r->cell_buf_cap) {
        free(r->cell_buf);
        r->cell_buf = malloc((size_t)needed * sizeof(Cell));
        r->cell_buf_cap = r->cell_buf ? needed : 0;
    }
    if (!r->cell_buf)
        return;

    int cursor_col, cursor_row;
    terminal_get_state(term, r->cell_buf, &cursor_col, &cursor_row);
    const Cell *cells = r->cell_buf;

    int face_n[FACE_COUNT] = {0};
    int ul_n = 0;

    int search_active = s && search_is_active(s);
    int sb_total = search_active ? terminal_sb_total_rows(term) : 0;
    int scroll = search_active ? terminal_scroll_offset(term) : 0;
    int active_abs = search_active ? search_current_abs_row(s) : -1;

    /* Snapshot once per frame; bsearch below is lock-free. */
    int snap_n = 0;
    int *snap = search_active ? search_snapshot(s, &snap_n) : NULL;

    for (int row = 0; row < rows; row++) {
        float row_y0 = (float)(y_px + row * cell_h);
        float row_y1 = row_y0 + (float)cell_h;

        /* Resolve absolute row index for search highlight lookup.
         * Unified formula works for both scrollback (row < scroll) and live
         * rows (row >= scroll): live rows get abs_row >= sb_total, which
         * correctly tracks content shifts when new output arrives. */
        uint32_t row_hl = 0;
        if (search_active) {
            int abs_row = sb_total - scroll + row;
            if (bsearch(&abs_row, snap, (size_t)snap_n, sizeof(int), int_cmp)) {
                row_hl =
                    (abs_row == active_abs) ? r->hl_active_rgba : r->hl_rgba;
            }
        }

        for (int col = 0; col < cols; col++) {
            const Cell *c = &cells[row * cols + col];
            float x0 = (float)(x_px + col * cell_w);
            float y0 = row_y0;
            float x1 = x0 + (float)cell_w;
            float y1 = row_y1;

            uint32_t bg = c->bg, fg = c->fg;
            /* ATTR_REVERSE: swap fg/bg (VT100 reverse video). */
            if (c->attrs & ATTR_REVERSE) {
                uint32_t tmp = fg;
                fg = bg;
                bg = tmp;
            }
            if (terminal_cell_selected(term, col, row)) {
                uint32_t tmp = fg;
                fg = bg;
                bg = tmp;
            }
            if (row_hl) {
                /* Blend highlight over cell bg, then enforce WCAG 4.5:1
                 * contrast ratio by overriding fg with near-black/white. */
                bg = blend_rgba(row_hl, bg);
                float lbg = luminance(bg), lfg = luminance(fg);
                float bright = lbg > lfg ? lbg : lfg;
                float dim = lbg > lfg ? lfg : lbg;
                if ((bright + 0.05f) / (dim + 0.05f) < 4.5f)
                    fg = lbg > 0.5f ? 0x282828FFu : 0xFBF1C7FFu;
            }

            const FontAtlas *ra = &r->atlases[FACE_REGULAR];
            if (c->attrs & ATTR_UNDERLINE) {
                float bly = y0 + (float)ra->ascent_px;
                float ul_y0 = bly + (float)ra->underline_pos_px;
                float ul_y1 = ul_y0 + (float)ra->underline_thickness_px;
                /* Clamp within cell so underline never bleeds into adjacent
                 * rows. */
                if (ul_y0 < y0)
                    ul_y0 = y0;
                if (ul_y1 > y1)
                    ul_y1 = y1;
                if (ul_y0 < ul_y1)
                    push_ul_quad(r->ul_verts, &ul_n, x0, ul_y0, x1, ul_y1, fg);
            }

            /* Select most specific available face; fall back to regular when
             * bold/italic/bold-italic weren't found on disk. */
            int is_bold = (c->attrs & ATTR_BOLD) != 0;
            int is_italic = (c->attrs & ATTR_ITALIC) != 0;
            int face;
            if (is_bold && is_italic && r->face_is_real[FACE_BOLD_ITALIC]) {
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
            /* Skip space and missing glyphs; push_cell_quad draws bg
             * regardless. */
            if (c->ch != ' ' && font_has_glyph(ga, c->ch)) {
                font_atlas_glyph(ga, c->ch, x0, y0 + (float)ga->ascent_px, &q);
                if (q.x1 > q.x0 && q.y1 > q.y0)
                    qptr = &q;
            }

            push_cell_quad(r->face_verts[face], &face_n[face], x0, y0, x1, y1,
                           qptr, fg, bg);
        }
    }

    free(snap);

    glUseProgram(r->cell_prog);
    glUniform2f(r->cell_u_screen, (float)win_w, (float)win_h);
    glUniform1i(r->cell_u_atlas, 0);
    glActiveTexture(GL_TEXTURE0);
    for (int f = 0; f < FACE_COUNT; f++) {
        if (0 == face_n[f])
            continue;
        glBindTexture(GL_TEXTURE_2D, r->atlases[f].texture_id);
        glBindVertexArray(r->face_vao[f]);
        glBindBuffer(GL_ARRAY_BUFFER, r->face_vbo[f]);
        glBufferData(GL_ARRAY_BUFFER, r->cell_buf_sz, NULL, GL_STREAM_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        (GLsizeiptr)(face_n[f] * (int)sizeof(float)),
                        r->face_verts[f]);
        glDrawArrays(GL_TRIANGLES, 0, face_n[f] / CELL_FPV);
    }

    if (terminal_cursor_visible(term) && cursor_col >= 0 && cursor_row >= 0) {
        float cx = (float)(x_px + cursor_col * cell_w);
        float cy0 = (float)(y_px + cursor_row * cell_h);
        float cy1 = cy0 + (float)cell_h;
        float cw = (float)cell_w;
        float thick = (float)CURSOR_THICKNESS;
        uint32_t color = TERM_DEFAULT_FG;

        if (!is_active) {
            /* Hollow block on inactive pane: 4 border quads. */
            push_ul_quad(r->ul_verts, &ul_n, cx, cy0, cx + cw, cy0 + thick,
                         color);
            push_ul_quad(r->ul_verts, &ul_n, cx, cy1 - thick, cx + cw, cy1,
                         color);
            push_ul_quad(r->ul_verts, &ul_n, cx, cy0 + thick, cx + thick,
                         cy1 - thick, color);
            push_ul_quad(r->ul_verts, &ul_n, cx + cw - thick, cy0 + thick,
                         cx + cw, cy1 - thick, color);
        } else {
            int shape = terminal_cursor_shape(term);
            if (1 == shape || 2 == shape) {
                /* Block (app-requested via DECSCUSR). */
                push_ul_quad(r->ul_verts, &ul_n, cx, cy0, cx + cw, cy1, color);
            } else if (3 == shape || 4 == shape) {
                /* Underline. */
                push_ul_quad(r->ul_verts, &ul_n, cx, cy1 - thick, cx + cw, cy1,
                             color);
            } else {
                /* 0 (default), 5, 6: beam bar at left edge. */
                push_ul_quad(r->ul_verts, &ul_n, cx, cy0, cx + thick, cy1,
                             color);
            }
        }
    }

    if (ul_n > 0) {
        BatchDesc bd = {r->ul_prog,
                        r->ul_u_screen,
                        (float)win_w,
                        (float)win_h,
                        r->ul_vao,
                        r->ul_vbo,
                        (int)r->ul_buf_sz,
                        r->ul_verts,
                        ul_n,
                        UL_FPV,
                        -1,
                        0};
        flush_vertex_batch(&bd);
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
}

#define SEARCH_BAR_PAD 4       /* px inside bar, top/bottom/left */
#define SEARCH_BAR_MARGIN 8    /* px gap from window edge */
#define SEARCH_BAR_CELLS 36    /* bar width in cell units */
#define SEARCH_COUNTER_CELLS 7 /* cells reserved for " NN/MM" on the right */

/* Returns number of codepoints drawn; each occupies one cell width. */
static int draw_str(Renderer *r, float *cv, int *cn, float x, float cy0,
                    float cy1, const char *text, int max_cells, uint32_t fg,
                    uint32_t bg) {
    const unsigned char *s = (const unsigned char *)text;
    float cw = (float)r->atlases[FACE_REGULAR].cell_w;
    int drawn = 0;

    while (*s && drawn < max_cells) {
        uint32_t cp = utf8_decode(&s);
        float cx0 = x + (float)drawn * cw;
        float cx1 = cx0 + cw;
        GlyphQuad q;
        const GlyphQuad *qptr = NULL;

        if (' ' != cp && font_has_glyph(&r->atlases[FACE_REGULAR], cp)) {
            font_atlas_glyph(&r->atlases[FACE_REGULAR], cp, cx0,
                             cy0 + (float)r->atlases[FACE_REGULAR].ascent_px,
                             &q);
            if (q.x1 > q.x0 && q.y1 > q.y0)
                qptr = &q;
        }

        push_cell_quad(cv, cn, cx0, cy0, cx1, cy1, qptr, fg, bg);
        drawn++;
    }
    return drawn;
}

void renderer_draw_search_overlay(Renderer *r, SearchState *s, Terminal *term) {
    if (!search_is_active(s))
        return;
    (void)term;

    int win_w, win_h;
    SDL_GetWindowSizeInPixels(r->window, &win_w, &win_h);

    int cell_w = r->atlases[FACE_REGULAR].cell_w;
    int cell_h = r->atlases[FACE_REGULAR].cell_h;

    int ul_n = 0;

    float bar_w = (float)(SEARCH_BAR_CELLS * cell_w);
    float bar_h = (float)(cell_h + SEARCH_BAR_PAD * 2);
    float bar_x0 = (float)win_w - bar_w - (float)SEARCH_BAR_MARGIN;
    float bar_y0 = (float)SEARCH_BAR_MARGIN;

    push_ul_quad(r->ul_verts, &ul_n, bar_x0, bar_y0, bar_x0 + bar_w,
                 bar_y0 + bar_h, COLOR_8);

    if (ul_n > 0) {
        BatchDesc bd = {r->ul_prog,
                        r->ul_u_screen,
                        (float)win_w,
                        (float)win_h,
                        r->ul_vao,
                        r->ul_vbo,
                        (int)r->ul_buf_sz,
                        r->ul_verts,
                        ul_n,
                        UL_FPV,
                        -1,
                        0};
        flush_vertex_batch(&bd);
    }

    float text_y0 = bar_y0 + (float)SEARCH_BAR_PAD;
    float text_y1 = text_y0 + (float)cell_h;
    float text_x = bar_x0 + (float)SEARCH_BAR_PAD;
    int bar_cap = SEARCH_BAR_CELLS - 1; /* 1-cell right margin */
    int cell_n = 0;

    int prefix_cells =
        draw_str(r, r->face_verts[FACE_REGULAR], &cell_n, text_x, text_y0,
                 text_y1, "Search: ", bar_cap, COLOR_7, COLOR_8);

    /* +1 reserves a cell for the blinking "|" cursor glyph */
    int query_max = bar_cap - prefix_cells - SEARCH_COUNTER_CELLS - 1;
    if (query_max < 0)
        query_max = 0;
    float qx = text_x + (float)(prefix_cells * cell_w);

    int q_cells = 0;
    if (query_max > 0) {
        q_cells =
            draw_str(r, r->face_verts[FACE_REGULAR], &cell_n, qx, text_y0,
                     text_y1, search_query(s), query_max, COLOR_15, COLOR_8);
    }

    float cursor_x = qx + (float)(q_cells * cell_w);
    draw_str(r, r->face_verts[FACE_REGULAR], &cell_n, cursor_x, text_y0,
             text_y1, "|", 1, COLOR_11, COLOR_8);

    char counter[16];
    int total = search_result_count(s);
    int current = search_current_idx(s);
    if (total > 0) {
        snprintf(counter, sizeof counter, " %d/%d", current + 1, total);
    } else {
        snprintf(counter, sizeof counter, " 0/0");
    }
    int ctr_len = (int)strlen(counter);
    float ctr_x =
        bar_x0 + bar_w - (float)(ctr_len * cell_w) - (float)SEARCH_BAR_PAD;
    draw_str(r, r->face_verts[FACE_REGULAR], &cell_n, ctr_x, text_y0, text_y1,
             counter, ctr_len, COLOR_11, COLOR_8);

    if (cell_n > 0) {
        BatchDesc bd = {r->cell_prog,
                        r->cell_u_screen,
                        (float)win_w,
                        (float)win_h,
                        r->face_vao[FACE_REGULAR],
                        r->face_vbo[FACE_REGULAR],
                        (int)r->cell_buf_sz,
                        r->face_verts[FACE_REGULAR],
                        cell_n,
                        CELL_FPV,
                        r->cell_u_atlas,
                        r->atlases[FACE_REGULAR].texture_id};
        flush_vertex_batch(&bd);
    }

    glBindVertexArray(0);
    glUseProgram(0);
}

void renderer_draw_autocomplete_overlay(Renderer *r, const Autocomplete *ac,
                                        Terminal *term, int x_px, int y_px) {
    const char *sug = autocomplete_get_suggestion(ac);
    if (!sug || '\0' == sug[0])
        return;

    int cell_w = r->atlases[FACE_REGULAR].cell_w;
    int cell_h = r->atlases[FACE_REGULAR].cell_h;
    int cols = terminal_cols(term);
    int rows = terminal_rows(term);

    int cur_col = 0, cur_row = 0;
    terminal_cursor_pos(term, &cur_col, &cur_row);

    if (cur_col < 0 || cur_row < 0)
        return; /* scrolled back */

    int cell_n = 0;
    const char *p = sug;
    int row = cur_row;

    while ('\0' != *p && row < rows) {
        const char *nl = strchr(p, '\n');
        size_t seg_len = nl ? (size_t)(nl - p) : strlen(p);

        float x0;
        int avail;
        if (row == cur_row) {
            /* First line: leave one cell gap so cursor block stays visible. */
            x0 = (float)(x_px + (cur_col + 1) * cell_w);
            avail = cols - cur_col - 2;
        } else {
            x0 = (float)x_px;
            avail = cols;
        }

        if (avail > 0 && seg_len > 0) {
            char seg[4096];
            size_t copy = seg_len < sizeof seg - 1 ? seg_len : sizeof seg - 1;
            memcpy(seg, p, copy);
            seg[copy] = '\0';
            float y0 = (float)(y_px + row * cell_h);
            float y1 = y0 + (float)cell_h;
            draw_str(r, r->face_verts[FACE_REGULAR], &cell_n, x0, y0, y1, seg,
                     avail, COLOR_8, TERM_DEFAULT_BG);
        }

        if (!nl)
            break;
        p = nl + 1;
        row++;
    }

    if (cell_n > 0) {
        int win_w, win_h;
        SDL_GetWindowSizeInPixels(r->window, &win_w, &win_h);
        BatchDesc bd = {r->cell_prog,
                        r->cell_u_screen,
                        (float)win_w,
                        (float)win_h,
                        r->face_vao[FACE_REGULAR],
                        r->face_vbo[FACE_REGULAR],
                        (int)r->cell_buf_sz,
                        r->face_verts[FACE_REGULAR],
                        cell_n,
                        CELL_FPV,
                        r->cell_u_atlas,
                        r->atlases[FACE_REGULAR].texture_id};
        flush_vertex_batch(&bd);
        glBindVertexArray(0);
        glUseProgram(0);
    }
}

/* Compute scrollbar thumb height and available track travel in pixels.
 * Both outputs are in framebuffer pixels; thumb_h is clamped to the
 * SCROLLBAR_MIN_H_PX floor so the pill is always grabbable. */
static void scrollbar_thumb_metrics(Terminal *term, float h_px, float *thumb_h,
                                    float *track_travel) {
    int rows = terminal_rows(term);
    int sb_total = terminal_sb_total_rows(term);
    int total = sb_total + rows;
    *thumb_h = (float)rows / (float)total * h_px;
    if (*thumb_h < (float)SCROLLBAR_MIN_H_PX)
        *thumb_h = (float)SCROLLBAR_MIN_H_PX;
    *track_travel = h_px - *thumb_h;
}

int renderer_draw_scrollbar(Renderer *r, Terminal *term, int x_px, int y_px,
                            int w_px, int h_px) {
    int sb_total = terminal_sb_total_rows(term);
    if (sb_total <= 0)
        return 0;

    int win_w, win_h;
    SDL_GetWindowSizeInPixels(r->window, &win_w, &win_h);

    int scroll = terminal_scroll_offset(term);
    if (scroll != r->sb_last_offset) {
        r->sb_last_offset = scroll;
        r->sb_last_scroll_ms = SDL_GetTicks();
    }

    Uint64 elapsed = SDL_GetTicks() - r->sb_last_scroll_ms;
    float alpha;
    if (elapsed < SB_SHOW_MS) {
        alpha = 1.0f;
    } else if (elapsed < (Uint64)(SB_SHOW_MS + SB_FADE_MS)) {
        alpha = 1.0f - (float)(elapsed - SB_SHOW_MS) / (float)SB_FADE_MS;
    } else {
        return 0; /* fully faded; skip draw */
    }

    float thumb_h, track_travel;
    scrollbar_thumb_metrics(term, (float)h_px, &thumb_h, &track_travel);
    float top_frac = (float)(sb_total - scroll) / (float)sb_total;
    float thumb_y = (float)y_px + top_frac * track_travel;

    float x0 = (float)(x_px + w_px - SB_W_PX - SB_INSET);
    float x1 = (float)(x_px + w_px - SB_INSET);
    float w = x1 - x0;

    float verts[6 * SB_FPV];
    push_sb_quad(verts, x0, thumb_y, x1, thumb_y + thumb_h, 0x928374CCu, alpha);

    glUseProgram(r->sb_prog);
    glUniform2f(r->sb_u_screen, (float)win_w, (float)win_h);
    glUniform2f(r->sb_u_size, w, thumb_h);
    glBindVertexArray(r->sb_vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->sb_vbo);
    glBufferData(GL_ARRAY_BUFFER, 192, NULL, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, 192, verts);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);

    return (elapsed >= SB_SHOW_MS) ? 1 : 0;
}

#define TAB_BAR_BG COLOR_0
#define TAB_ACTIVE_BG COLOR_8
#define TAB_FG COLOR_7
#define TAB_ACTIVE_FG COLOR_15

/* Total display width = slot_cells; inner = slot_cells-2 (1-cell padding
 * each side).  Long titles get the last inner cell replaced with U+2026.
 * Single-pass: copy up to inner-1 codepoints, then check for more. */
static void make_tab_label(const char *title, int slot_cells, char *out,
                           int out_sz) {
    int inner = slot_cells - 2;
    if (inner < 1)
        inner = 1;

    out[0] = ' '; /* leading padding */
    int pos = 1;

    const unsigned char *s = (const unsigned char *)title;
    int c = 0;
    while (*s && c < inner - 1) {
        const unsigned char *prev = s;
        utf8_decode(&s);
        int nb = (int)(s - prev);
        if (pos + nb >= out_sz - 4)
            break;
        memcpy(out + pos, prev, (size_t)nb);
        pos += nb;
        c++;
    }

    if (*s) {
        /* More codepoints remain: title is truncated; append ellipsis. */
        /* U+2026 HORIZONTAL ELLIPSIS = 0xE2 0x80 0xA6 */
        if (pos + 3 < out_sz - 1) {
            out[pos++] = (char)0xE2;
            out[pos++] = (char)0x80;
            out[pos++] = (char)0xA6;
        }
    } else {
        /* Title fits: pad to inner width. */
        int pad = inner - c;
        while (pad-- > 0 && pos < out_sz - 1)
            out[pos++] = ' ';
    }

    out[pos++] = ' '; /* trailing padding */
    if (pos < out_sz)
        out[pos] = '\0';
    else
        out[out_sz - 1] = '\0';
}

void renderer_draw_tab_bar(Renderer *r, int n, int active,
                           const char *const *titles) {
    if (n <= 1)
        return;

    int win_w, win_h;
    SDL_GetWindowSizeInPixels(r->window, &win_w, &win_h);

    int cw = r->atlases[FACE_REGULAR].cell_w;
    int ch = r->atlases[FACE_REGULAR].cell_h;
    float y0 = (float)(win_h - ch);
    float y1 = (float)win_h;
    float fw = (float)win_w;

    int slot_cells = (cw > 0) ? win_w / n / cw : 3;
    if (slot_cells > TAB_TITLE_MAX + 2)
        slot_cells = TAB_TITLE_MAX + 2;
    if (slot_cells < 3)
        slot_cells = 3;
    int slot_px = slot_cells * cw;

    int ul_n = 0;
    push_ul_quad(r->ul_verts, &ul_n, 0.0f, y0, fw, y1, TAB_BAR_BG);

    float ax0 = (float)(active * slot_px);
    float ax1 = ax0 + (float)slot_px;
    push_ul_quad(r->ul_verts, &ul_n, ax0, y0, ax1, y1, TAB_ACTIVE_BG);

    {
        BatchDesc bd = {r->ul_prog,
                        r->ul_u_screen,
                        fw,
                        (float)win_h,
                        r->ul_vao,
                        r->ul_vbo,
                        (int)r->ul_buf_sz,
                        r->ul_verts,
                        ul_n,
                        UL_FPV,
                        -1,
                        0};
        flush_vertex_batch(&bd);
    }

    char label[TAB_TITLE_MAX * 4 + 8];
    int cell_n = 0;
    for (int i = 0; i < n; i++) {
        const char *title = (titles && titles[i]) ? titles[i] : "";
        make_tab_label(title, slot_cells, label, (int)sizeof label);
        float lx = (float)(i * slot_px);
        uint32_t fg = (i == active) ? TAB_ACTIVE_FG : TAB_FG;
        uint32_t bg = (i == active) ? TAB_ACTIVE_BG : TAB_BAR_BG;
        draw_str(r, r->face_verts[FACE_REGULAR], &cell_n, lx, y0, y1, label,
                 slot_cells, fg, bg);
    }

    if (cell_n > 0) {
        BatchDesc bd = {r->cell_prog,
                        r->cell_u_screen,
                        fw,
                        (float)win_h,
                        r->face_vao[FACE_REGULAR],
                        r->face_vbo[FACE_REGULAR],
                        (int)r->cell_buf_sz,
                        r->face_verts[FACE_REGULAR],
                        cell_n,
                        CELL_FPV,
                        r->cell_u_atlas,
                        r->atlases[FACE_REGULAR].texture_id};
        flush_vertex_batch(&bd);
    }
    glBindVertexArray(0);
    glUseProgram(0);
}

int renderer_scrollbar_hit(int x_px, int y_px, int w_px, int h_px, int mx,
                           int my) {
    int sb_x0 = x_px + w_px - SB_W_PX - SB_INSET;
    int sb_x1 = x_px + w_px - SB_INSET;
    return mx >= sb_x0 && mx < sb_x1 && my >= y_px && my < y_px + h_px;
}

int renderer_scrollbar_drag_offset(const Terminal *term, int h_px, int anchor_y,
                                   int anchor_off, int my) {
    int sb_total = terminal_sb_total_rows(term);
    if (sb_total <= 0)
        return 0;
    float thumb_h, track_travel;
    /* scrollbar_thumb_metrics takes non-const Terminal; the cast is safe
     * because the function only reads terminal state. */
    scrollbar_thumb_metrics((Terminal *)term, (float)h_px, &thumb_h,
                            &track_travel);
    if (track_travel <= 0.0f)
        return anchor_off;
    int dy = my - anchor_y;
    int delta = (int)((float)dy * (float)sb_total / track_travel);
    int off = anchor_off - delta;
    if (off < 0)
        off = 0;
    if (off > sb_total)
        off = sb_total;
    return off;
}

/* Reuses ul_prog since it already handles arbitrary colored quads. */
void renderer_fill_rect(Renderer *r, int x, int y, int w, int h,
                        uint32_t rgba) {
    if (w <= 0 || h <= 0)
        return;

    int win_w, win_h;
    SDL_GetWindowSizeInPixels(r->window, &win_w, &win_h);

    int n = 0;
    push_ul_quad(r->ul_verts, &n, (float)x, (float)y, (float)(x + w),
                 (float)(y + h), rgba);

    BatchDesc bd = {r->ul_prog,
                    r->ul_u_screen,
                    (float)win_w,
                    (float)win_h,
                    r->ul_vao,
                    r->ul_vbo,
                    (int)r->ul_buf_sz,
                    r->ul_verts,
                    n,
                    UL_FPV,
                    -1,
                    0};
    flush_vertex_batch(&bd);
    glBindVertexArray(0);
    glUseProgram(0);
}
