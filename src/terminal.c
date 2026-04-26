#include "terminal.h"
#include "config.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#  include <compression.h>
#  include <dispatch/dispatch.h>
#else
#  include <unistd.h>
#  include <zlib.h>
#  if defined(__x86_64__) && PHANTOM_HAVE_AVX2
#    include <immintrin.h>
#  endif
#endif
#if defined(__aarch64__)
#  include <arm_neon.h>
#endif

#if PHANTOM_DEBUG
#  include <stdarg.h>
static void vt_log(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "phantom: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
#  define VT_LOG vt_log
#else
static void vt_log_noop(const char *fmt, ...) { (void)fmt; }
#  define VT_LOG vt_log_noop
#endif

#define COL_DEFAULT_FG TERM_DEFAULT_FG
#define COL_DEFAULT_BG TERM_DEFAULT_BG

#define CSI_PARAMS_MAX   16  /* max parsed params in a single CSI sequence */
#define TAB_STOP_WIDTH    8  /* columns between tab stops */

/* xterm 256-color palette layout: 0-15 = ANSI, 16-231 = 6×6×6 RGB cube,
 * 232-255 = grayscale ramp. */
#define ANSI_COLOR_COUNT    16   /* first N entries are the named ANSI colors */
#define GRAYSCALE_START    232   /* index where the grayscale ramp begins */
#define COLOR_CUBE_DIM       6   /* one axis of the 6×6×6 RGB cube */
#define COLOR_CUBE_BASE     55   /* minimum nonzero component value in the cube */
#define COLOR_CUBE_STEP     40   /* component value increment per cube level */
#define GRAYSCALE_BASE       8   /* darkest gray component value */
#define GRAYSCALE_STEP      10   /* component increment between grayscale levels */


/* RGBA alpha channel: fully opaque */
#define RGBA_ALPHA_OPAQUE 0xFF

/* UTF-8 byte classification thresholds and payload masks */
#define UTF8_CONT_BYTE_MIN   0x80  /* lower bound for continuation bytes */
#define UTF8_2BYTE_LEAD_MIN  0xC0  /* lower bound for 2-byte sequence lead */
#define UTF8_3BYTE_LEAD_MIN  0xE0  /* lower bound for 3-byte sequence lead */
#define UTF8_4BYTE_LEAD_MIN  0xF0  /* lower bound for 4-byte sequence lead */
#define UTF8_4BYTE_DATA_MASK 0x07  /* payload bits in a 4-byte lead byte */
#define UTF8_3BYTE_DATA_MASK 0x0F  /* payload bits in a 3-byte lead byte */
#define UTF8_2BYTE_DATA_MASK 0x1F  /* payload bits in a 2-byte lead byte */
#define UTF8_CONT_DATA_MASK  0x3F  /* payload bits in a continuation byte */

/* Codepoint thresholds for UTF-8 sequence length (used in encoding) */
#define UTF8_2BYTE_LIMIT  0x800u    /* codepoints >= this need 2+ bytes */
#define UTF8_3BYTE_LIMIT  0x10000u  /* codepoints >= this need 3+ bytes */
#define UTF8_MAX_BYTES    4         /* maximum bytes in any UTF-8 sequence */

/* ASCII / CSI byte range constants */
#define ASCII_PRINTABLE_FIRST 0x20  /* space  -  first printable ASCII char */
#define ASCII_PRINTABLE_LAST  0x7E  /* ~  -  last printable ASCII char */
#define DEC_SPECIAL_FIRST     0x60  /* `  -  first DEC special-graphics char */
#define CSI_FINAL_FIRST       0x40  /* @  -  first valid CSI final byte */
#define CSI_FINAL_LAST        0x7E  /* ~  -  last valid CSI final byte */
#define CSI_BUF_SIZE          64    /* max bytes buffered for one CSI sequence */
#define OSC_BUF_SIZE        4096    /* max bytes buffered for one OSC sequence */

/* ASCII control character codes */
#define ASCII_BEL  '\x07'
#define ASCII_BS   '\b'
#define ASCII_HT   '\t'
#define ASCII_LF   '\n'
#define ASCII_VT   '\v'
#define ASCII_FF   '\f'
#define ASCII_CR   '\r'
#define ASCII_ESC  '\x1b'

/* SGR (Select Graphic Rendition) parameter codes */
#define SGR_RESET             0
#define SGR_BOLD              1
#define SGR_ITALIC            3
#define SGR_UNDERLINE         4
#define SGR_REVERSE           7
#define SGR_BOLD_OFF         22
#define SGR_ITALIC_OFF       23
#define SGR_UNDERLINE_OFF    24
#define SGR_REVERSE_OFF      27
#define SGR_FG_FIRST         30   /* first normal foreground color code */
#define SGR_FG_LAST          37
#define SGR_FG_EXT           38   /* extended FG color (256/RGB subcommand) */
#define SGR_FG_DEFAULT       39
#define SGR_BG_FIRST         40   /* first normal background color code */
#define SGR_BG_LAST          47
#define SGR_BG_EXT           48   /* extended BG color (256/RGB subcommand) */
#define SGR_BG_DEFAULT       49
#define SGR_FG_BRIGHT_FIRST  90   /* first bright foreground color code */
#define SGR_FG_BRIGHT_LAST   97
#define SGR_BG_BRIGHT_FIRST 100   /* first bright background color code */
#define SGR_BG_BRIGHT_LAST  107
#define SGR_COLOR256          5   /* subcommand: 256-color palette index follows */
#define SGR_COLORRGB          2   /* subcommand: R;G;B components follow */
#define ANSI_BRIGHT_OFFSET    8   /* bright colors start at palette index 8 */

/* DECPM (DEC private mode) numbers used in ?h / ?l sequences */
#define DECPM_DECCKM            1
#define DECPM_CURSOR_VISIBLE   25
#define DECPM_ALT_SCREEN       47
#define DECPM_ALT_SCREEN_2   1047
#define DECPM_ALT_SCREEN_3   1049
#define DECPM_BRACKETED_PASTE 2004

/* DEC special-graphics charset designator byte */
#define CHARSET_DEC_SPECIAL '0'

/* Standard 16 ANSI colors (RGBA) */
static const uint32_t ANSI_COLORS[16] = {
    COLOR_0,  COLOR_1,  COLOR_2,  COLOR_3,
    COLOR_4,  COLOR_5,  COLOR_6,  COLOR_7,
    COLOR_8,  COLOR_9,  COLOR_10, COLOR_11,
    COLOR_12, COLOR_13, COLOR_14, COLOR_15,
};

/* VT100 DEC Special Graphics: codepoints for 0x60..0x7e */
static const uint32_t DEC_SPECIAL[31] = {
    0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A,
    0x00B0, 0x00B1, 0x2424, 0x240B,
    0x2518, 0x2510, 0x250C, 0x2514, 0x253C,
    0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD,
    0x251C, 0x2524, 0x2534, 0x252C, 0x2502,
    0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,
};

typedef struct {
    uint8_t *data;      /* LZFSE-compressed bytes (or raw if is_raw) */
    size_t   data_sz;
    uint64_t bloom;     /* bloom filter */
    int      row_base;  /* first absolute scrollback row index */
    int      row_count;
    int      cols;      /* column width at compression time */
    uint8_t  is_raw;    /* true when not compressed (LZFSE failed or unavailable) */
} SbChunk;

#if !defined(__APPLE__)
typedef struct {
    SbChunk              *snap;
    int                   n_chunks;
    uint32_t             *qcps;
    int                   qlen;
    int                   pivot_off;
    uint64_t              qbloom;
    terminal_search_result_fn cb;
    void                 *arg;
    volatile int         *cancel;
    int                   q_next;
    int                   pending;
} PoolJob;

typedef struct SearchPool {
    pthread_t        *threads;
    int               n_threads;
    pthread_mutex_t   mu;
    pthread_cond_t    work_ready;
    pthread_cond_t    work_done;
    int               shutdown;
    PoolJob           job;
} SearchPool;
#endif

typedef struct {
    Cell    *cells;      /* decompressed cells; NULL = empty */
    int      chunk_idx;  /* -1 = empty */
    int      slot_cols;  /* col width of the allocated cells buffer */
    uint64_t lru_tick;
} SbCacheSlot;

typedef struct {
    SbChunk     *chunks;
    int          chunk_count;
    int          chunk_cap;
    Cell        *tail_raw;    /* SB_CHUNK_ROWS * cols cells (uncompressed) */
    int          tail_count;
    uint64_t     tail_bloom;
    int          total_rows;  /* tail rows included */
    int          cols;
    SbCacheSlot  cache[SB_DECOMP_CACHE];
    uint64_t     lru_tick;
    volatile int search_cancel;
#if defined(__APPLE__)
    dispatch_group_t search_group; /* block enters on start, leaves on finish */
#else
    SearchPool      *pool;
    pthread_mutex_t  search_mu;
    pthread_cond_t   search_cond;
    int              search_active;
#endif
} SbStore;

#if !defined(__APPLE__)
typedef struct {
    SbStore              *sb;
    SbChunk              *snap;
    int                   n_chunks;
    Cell                 *tail_snap;
    int                   tail_count;
    uint64_t              tail_bloom;
    int                   tail_base;
    Cell                 *live_snap;
    int                   live_rows;
    int                   live_cols;
    int                   live_base;
    uint32_t             *qcps;
    int                   qlen;
    int                   pivot_off;
    uint64_t              qbloom;
    terminal_search_result_fn cb;
    terminal_search_done_fn   done_cb;
    void                 *arg;
    int                   cols;
} CoordArgs;

static int row_contains_query(const Cell *row, int cols,
                               const uint32_t *qcps, int qlen,
                               int pivot_off);

static void *search_worker_fn(void *arg)
{
    SearchPool *pool = (SearchPool *)arg;
    static _Thread_local Cell  *scratch    = NULL;
    static _Thread_local size_t scratch_sz = 0;

    for (;;) {
        pthread_mutex_lock(&pool->mu);
        while (!pool->shutdown && pool->job.q_next >= pool->job.n_chunks)
            pthread_cond_wait(&pool->work_ready, &pool->mu);
        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->mu);
            break;
        }
        int       ci     = pool->job.q_next++;
        SbChunk  *ck     = &pool->job.snap[ci];
        uint32_t *qcps      = pool->job.qcps;
        int       qlen      = pool->job.qlen;
        int       pivot_off = pool->job.pivot_off;
        uint64_t  qbloom    = pool->job.qbloom;
        terminal_search_result_fn cb = pool->job.cb;
        void        *cb_arg  = pool->job.arg;
        volatile int *cancel = pool->job.cancel;
        pthread_mutex_unlock(&pool->mu);

        if (*cancel || (ck->bloom & qbloom) != qbloom) {
            pthread_mutex_lock(&pool->mu);
            if (--pool->job.pending == 0)
                pthread_cond_signal(&pool->work_done);
            pthread_mutex_unlock(&pool->mu);
            continue;
        }

        int    ck_cols = ck->cols;
        size_t raw_sz  = (size_t)(ck->row_count * ck_cols) * sizeof(Cell);

        if (raw_sz > scratch_sz) {
            Cell *p = realloc(scratch, raw_sz);
            if (!p) {
                pthread_mutex_lock(&pool->mu);
                if (--pool->job.pending == 0)
                    pthread_cond_signal(&pool->work_done);
                pthread_mutex_unlock(&pool->mu);
                continue;
            }
            scratch    = p;
            scratch_sz = raw_sz;
        }

        if (ck->is_raw) {
            memcpy(scratch, ck->data, raw_sz);
        } else {
            uLongf out_sz = (uLongf)raw_sz;
            uncompress((Bytef *)scratch, &out_sz,
                       ck->data, (uLong)ck->data_sz);
        }

        for (int r = 0; r < ck->row_count && !*cancel; r++) {
            if (row_contains_query(&scratch[r * ck_cols], ck_cols,
                                   qcps, qlen, pivot_off))
                cb(ck->row_base + r, cb_arg);
        }

        pthread_mutex_lock(&pool->mu);
        if (--pool->job.pending == 0)
            pthread_cond_signal(&pool->work_done);
        pthread_mutex_unlock(&pool->mu);
    }
    return NULL;
}

static SearchPool *search_pool_create(void)
{
    int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;

    SearchPool *pool = calloc(1, sizeof *pool);
    if (!pool) return NULL;

    pool->threads = malloc((size_t)n * sizeof(pthread_t));
    if (!pool->threads) { free(pool); return NULL; }

    pthread_mutex_init(&pool->mu, NULL);
    pthread_cond_init(&pool->work_ready, NULL);
    pthread_cond_init(&pool->work_done, NULL);

    for (int i = 0; i < n; i++) {
        if (0 != pthread_create(&pool->threads[i], NULL,
                                search_worker_fn, pool))
            break;
        pool->n_threads++;
    }
    return pool;
}

static void search_pool_destroy(SearchPool *pool)
{
    if (!pool) return;
    pthread_mutex_lock(&pool->mu);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->work_ready);
    pthread_mutex_unlock(&pool->mu);
    for (int i = 0; i < pool->n_threads; i++)
        pthread_join(pool->threads[i], NULL);
    free(pool->threads);
    pthread_mutex_destroy(&pool->mu);
    pthread_cond_destroy(&pool->work_ready);
    pthread_cond_destroy(&pool->work_done);
    free(pool);
}

static void *search_coord_fn(void *arg)
{
    CoordArgs  *a    = (CoordArgs *)arg;
    SearchPool *pool = a->sb->pool;

    /* Scan live screen and tail first  -  no decompression, results appear fast. */
    for (int r = 0; r < a->live_rows && !a->sb->search_cancel; r++) {
        if (a->live_snap &&
            row_contains_query(&a->live_snap[r * a->live_cols],
                               a->live_cols, a->qcps, a->qlen,
                               a->pivot_off))
            a->cb(a->live_base + r, a->arg);
    }

    if ((a->tail_bloom & a->qbloom) == a->qbloom) {
        for (int r = 0; r < a->tail_count && !a->sb->search_cancel; r++) {
            if (row_contains_query(&a->tail_snap[r * a->cols], a->cols,
                                   a->qcps, a->qlen, a->pivot_off))
                a->cb(a->tail_base + r, a->arg);
        }
    }

    if (a->n_chunks > 0) {
        if (pool) {
            pthread_mutex_lock(&pool->mu);
            pool->job.snap     = a->snap;
            pool->job.n_chunks = a->n_chunks;
            pool->job.qcps      = a->qcps;
            pool->job.qlen      = a->qlen;
            pool->job.pivot_off = a->pivot_off;
            pool->job.qbloom    = a->qbloom;
            pool->job.cb       = a->cb;
            pool->job.arg      = a->arg;
            pool->job.cancel   = &a->sb->search_cancel;
            pool->job.q_next   = 0;
            pool->job.pending  = a->n_chunks;
            pthread_cond_broadcast(&pool->work_ready);
            while (pool->job.pending > 0)
                pthread_cond_wait(&pool->work_done, &pool->mu);
            pthread_mutex_unlock(&pool->mu);
        } else {
            /* pool unavailable  -  serial chunk scan */
            for (int ci = 0; ci < a->n_chunks && !a->sb->search_cancel; ci++) {
                SbChunk *ck    = &a->snap[ci];
                if ((ck->bloom & a->qbloom) != a->qbloom) continue;
                int    ck_cols = ck->cols;
                size_t raw_sz  = (size_t)(ck->row_count * ck_cols) * sizeof(Cell);
                Cell  *buf     = malloc(raw_sz);
                if (!buf) continue;
                if (ck->is_raw) {
                    memcpy(buf, ck->data, raw_sz);
                } else {
                    uLongf out_sz = (uLongf)raw_sz;
                    uncompress((Bytef *)buf, &out_sz,
                               ck->data, (uLong)ck->data_sz);
                }
                for (int r = 0; r < ck->row_count && !a->sb->search_cancel; r++) {
                    if (row_contains_query(&buf[r * ck_cols], ck_cols,
                                           a->qcps, a->qlen,
                                           a->pivot_off))
                        a->cb(ck->row_base + r, a->arg);
                }
                free(buf);
            }
        }
    }

    for (int i = 0; i < a->n_chunks; i++) free(a->snap[i].data);
    free(a->snap);
    free(a->tail_snap);
    free(a->live_snap);
    free(a->qcps);

    if (a->done_cb) a->done_cb(a->arg);

    pthread_mutex_lock(&a->sb->search_mu);
    a->sb->search_active--;
    pthread_cond_signal(&a->sb->search_cond);
    pthread_mutex_unlock(&a->sb->search_mu);

    free(a);
    return NULL;
}
#endif /* !defined(__APPLE__) */

/* Absolute row index of the first row in the uncompressed tail buffer. */
static inline int sb_tail_base(const SbStore *s)
{
    return s->total_rows - s->tail_count;
}

#define SB_INITIAL_CHUNK_CAP 16

/* k=2 bloom: derive two independent 6-bit indices from one 64-bit multiply.
 * Uses the Fibonacci (golden-ratio) constant for good bit distribution. */
static uint64_t sb_bloom_bits(uint32_t cp)
{
    uint64_t h = (uint64_t)cp * 0x9E3779B97F4A7C15ULL;
    return ((uint64_t)1 << (h >> 58))
         | ((uint64_t)1 << ((h >> 52) & 63u));
}

static SbStore *sb_create(int cols)
{
    SbStore *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->chunks = malloc(
        (size_t)SB_INITIAL_CHUNK_CAP * sizeof(SbChunk));
    if (!s->chunks) { free(s); return NULL; }
    s->chunk_cap = SB_INITIAL_CHUNK_CAP;
    s->tail_raw  = malloc(
        (size_t)(SB_CHUNK_ROWS * cols) * sizeof(Cell));
    if (!s->tail_raw) { free(s->chunks); free(s); return NULL; }
    s->cols = cols;
    for (int i = 0; i < SB_DECOMP_CACHE; i++) s->cache[i].chunk_idx = -1;
#if defined(__APPLE__)
    s->search_group = dispatch_group_create();
#else
    s->pool = search_pool_create();
    pthread_mutex_init(&s->search_mu, NULL);
    pthread_cond_init(&s->search_cond, NULL);
    s->search_active = 0;
#endif
    return s;
}

static void sb_destroy(SbStore *s)
{
    if (!s) return;
#if defined(__APPLE__)
    dispatch_group_wait(s->search_group, DISPATCH_TIME_FOREVER);
    dispatch_release(s->search_group);
#else
    pthread_mutex_lock(&s->search_mu);
    while (s->search_active > 0)
        pthread_cond_wait(&s->search_cond, &s->search_mu);
    pthread_mutex_unlock(&s->search_mu);
    search_pool_destroy(s->pool);
    pthread_mutex_destroy(&s->search_mu);
    pthread_cond_destroy(&s->search_cond);
#endif
    for (int i = 0; i < s->chunk_count; i++) free(s->chunks[i].data);
    free(s->chunks);
    free(s->tail_raw);
    for (int i = 0; i < SB_DECOMP_CACHE; i++) free(s->cache[i].cells);
    free(s);
}

/* LZFSE-compresses accumutated tail buffer into a sealed SbChunk with a bloom filter.
 * Resets the tail buffer so new rows can accumulate again. */
static void sb_finalize_chunk(SbStore *s)
{
    if (0 == s->tail_count) return;

    size_t   raw_sz  = (size_t)(s->tail_count * s->cols) * sizeof(Cell);
    uint8_t *comp    = NULL;
    size_t   comp_sz = 0;
    uint8_t  is_raw  = 0;

#if defined(__APPLE__)
    size_t   out_cap  = raw_sz + 64;
    size_t   scr_sz   = compression_encode_scratch_buffer_size(
                             COMPRESSION_LZFSE);
    uint8_t *scratch  = malloc(scr_sz);
    comp = malloc(out_cap);
    if (scratch && comp) {
        comp_sz = compression_encode_buffer(
            comp, out_cap,
            (const uint8_t *)s->tail_raw, raw_sz,
            scratch, COMPRESSION_LZFSE);
    }
    free(scratch);
    if (0 == comp_sz || comp_sz >= raw_sz) {
        free(comp);
        comp    = malloc(raw_sz);
        if (comp) memcpy(comp, s->tail_raw, raw_sz);
        comp_sz = raw_sz;
        is_raw  = 1;
    }
#else
    uLongf out_cap = (uLongf)compressBound((uLong)raw_sz);
    comp = malloc(out_cap);
    if (comp) {
        uLongf out_sz = out_cap;
        if (Z_OK == compress2(comp, &out_sz, (const Bytef *)s->tail_raw,
                    (uLong)raw_sz, Z_DEFAULT_COMPRESSION) && out_sz < raw_sz)
            comp_sz = (size_t)out_sz;
        else {
            free(comp);
            comp    = malloc(raw_sz);
            if (comp) memcpy(comp, s->tail_raw, raw_sz);
            comp_sz = raw_sz;
            is_raw  = 1;
        }
    }
#endif

    if (!comp) { s->tail_count = 0; s->tail_bloom = 0; return; }

    if (s->chunk_count == s->chunk_cap) {
        int       new_cap  = s->chunk_cap * 2;
        SbChunk  *newbuf   = realloc(s->chunks,
                                (size_t)new_cap * sizeof(SbChunk));
        if (!newbuf) { free(comp); s->tail_count = 0; s->tail_bloom = 0;
                       return; }
        s->chunks    = newbuf;
        s->chunk_cap = new_cap;
    }

    SbChunk *ck  = &s->chunks[s->chunk_count++];
    ck->data      = comp;
    ck->data_sz   = comp_sz;
    ck->bloom     = s->tail_bloom;
    ck->row_base  = sb_tail_base(s);
    ck->row_count = s->tail_count;
    ck->cols      = s->cols;
    ck->is_raw    = is_raw;
    s->tail_count = 0;
    s->tail_bloom = 0;
#if PHANTOM_DEBUG
    if (is_raw) {
        VT_LOG("sb chunk #%d: rows=%d cols=%d stored raw %zu bytes",
               s->chunk_count - 1, ck->row_count, ck->cols, comp_sz);
    } else {
        double pct = raw_sz ? 100.0 * (double)comp_sz / (double)raw_sz : 0.0;
        VT_LOG("sb chunk #%d: rows=%d cols=%d compressed %zu->%zu bytes (%.1f%%)",
               s->chunk_count - 1, ck->row_count, ck->cols,
               raw_sz, comp_sz, pct);
    }
#endif
}

static void sb_push_row(SbStore *s, const Cell *row)
{
    memcpy(&s->tail_raw[s->tail_count * s->cols], row,
           (size_t)s->cols * sizeof(Cell));
    for (int i = 0; i < s->cols; i++)
        s->tail_bloom |= sb_bloom_bits(row[i].ch);
    s->tail_count++;
    s->total_rows++;
    if (SB_CHUNK_ROWS == s->tail_count) sb_finalize_chunk(s);
}

static Cell *sb_decomp_cache_get(SbStore *s, int chunk_idx)
{
    for (int i = 0; i < SB_DECOMP_CACHE; i++) {
        if (s->cache[i].chunk_idx == chunk_idx) {
            s->cache[i].lru_tick = ++s->lru_tick;
            return s->cache[i].cells;
        }
    }
    int lru = 0;
    for (int i = 1; i < SB_DECOMP_CACHE; i++) {
        if (s->cache[i].lru_tick < s->cache[lru].lru_tick) lru = i;
    }
    SbCacheSlot *slot   = &s->cache[lru];
    SbChunk     *ck     = &s->chunks[chunk_idx];
    size_t       raw_sz = (size_t)(ck->row_count * ck->cols) * sizeof(Cell);

    /* Resize slot buffer if this chunk has a different column width. */
    if (!slot->cells || slot->slot_cols != ck->cols) {
        free(slot->cells);
        slot->cells = malloc(
            (size_t)(SB_CHUNK_ROWS * ck->cols) * sizeof(Cell));
        if (!slot->cells) {
            slot->chunk_idx = -1;
            slot->slot_cols = 0;
            return NULL;
        }
        slot->slot_cols = ck->cols;
    }
#if defined(__APPLE__)
    if (ck->is_raw) {
        memcpy(slot->cells, ck->data, raw_sz);
    } else {
        compression_decode_buffer(
            (uint8_t *)slot->cells, raw_sz,
            ck->data, ck->data_sz, NULL, COMPRESSION_LZFSE);
    }
#else
    if (ck->is_raw) {
        memcpy(slot->cells, ck->data, raw_sz);
    } else {
        uLongf out_sz = (uLongf)raw_sz;
        uncompress((Bytef *)slot->cells, &out_sz, ck->data,
                (uLong)ck->data_sz);
    }
#endif
    slot->chunk_idx = chunk_idx;
    slot->lru_tick  = ++s->lru_tick;
    return slot->cells;
}

static int chunk_row_cmp(const void *key, const void *elem)
{
    int            abs_row = *(const int *)key;
    const SbChunk *ck      = elem;
    if (abs_row < ck->row_base)                  return -1;
    if (abs_row >= ck->row_base + ck->row_count) return  1;
    return 0;
}

static int sb_chunk_for_row(SbStore *s, int abs_row)
{
    SbChunk *ck = bsearch(&abs_row, s->chunks,
                          (size_t)s->chunk_count, sizeof(SbChunk),
                          chunk_row_cmp);
    return ck ? (int)(ck - s->chunks) : -1;
}

/* Returns a pointer to the row at abs_row at its native column width.
 * For tail rows that is s->cols; for chunk rows that is ck->cols. */
static const Cell *sb_get_row(SbStore *s, int abs_row)
{
    int tail_base = sb_tail_base(s);
    if (abs_row >= tail_base)
        return &s->tail_raw[(abs_row - tail_base) * s->cols];
    int ci = sb_chunk_for_row(s, abs_row);
    if (ci < 0) return NULL;
    Cell *decomp = sb_decomp_cache_get(s, ci);
    if (!decomp) return NULL;
    return &decomp[(abs_row - s->chunks[ci].row_base) * s->chunks[ci].cols];
}

/* Returns the native column width of the row at abs_row. */
static int sb_row_cols(SbStore *s, int abs_row)
{
    int tail_base = sb_tail_base(s);
    if (abs_row >= tail_base) return s->cols;
    int ci = sb_chunk_for_row(s, abs_row);
    return (ci >= 0) ? s->chunks[ci].cols : s->cols;
}

/* Copy row abs_row into out[0..out_cols-1], truncating or padding with blanks
 * as needed to match the requested column width. */
static void sb_get_row_into(SbStore *s, int abs_row,
                             Cell *out, int out_cols)
{
    const Cell *src  = sb_get_row(s, abs_row);
    int         ncols = sb_row_cols(s, abs_row);
    int         copy  = ncols < out_cols ? ncols : out_cols;
    if (src) memcpy(out, src, (size_t)copy * sizeof(Cell));
    if (copy < out_cols) {
        Cell blank = { .ch = ' ', .fg = COL_DEFAULT_FG,
                       .bg = COL_DEFAULT_BG, .attrs = 0 };
        for (int i = copy; i < out_cols; i++) out[i] = blank;
    }
}

/* Remove the count newest rows from scrollback (tail first, then chunks). */
static void sb_trim_newest(SbStore *s, int count)
{
    if (count <= 0) return;
    /* Trim from tail first. */
    if (count <= s->tail_count) {
        s->tail_count -= count;
        s->total_rows -= count;
        return;
    }
    count -= s->tail_count;
    s->total_rows -= s->tail_count;
    s->tail_count  = 0;
    /* Trim from finalized chunks, newest first. */
    while (count > 0 && s->chunk_count > 0) {
        SbChunk *ck = &s->chunks[s->chunk_count - 1];
        int      rm = count < ck->row_count ? count : ck->row_count;
        ck->row_count  -= rm;
        s->total_rows  -= rm;
        count          -= rm;
        if (0 == ck->row_count) {
            free(ck->data);
            /* Invalidate any cache slot holding this chunk. */
            for (int i = 0; i < SB_DECOMP_CACHE; i++) {
                if (s->cache[i].chunk_idx == s->chunk_count - 1)
                    s->cache[i].chunk_idx = -1;
            }
            s->chunk_count--;
        }
    }
}

/* Finalize the tail into a chunk (if non-empty) then reinitialize the tail
 * for new_cols.  Existing compressed chunks are untouched. */
static void sb_adapt_cols(SbStore *s, int new_cols)
{
    if (s->tail_count > 0) sb_finalize_chunk(s);
    if (new_cols == s->cols) return;
    free(s->tail_raw);
    s->tail_raw = malloc((size_t)(SB_CHUNK_ROWS * new_cols) * sizeof(Cell));
    s->cols     = new_cols;
}


typedef enum {
    PARSE_NORMAL,
    PARSE_ESC,
    PARSE_CSI,
    PARSE_OSC,         /* inside OSC body  -  accumulate until BEL or ST */
    PARSE_OSC_ESC,     /* ESC seen inside OSC  -  awaiting '\' (ST) */
    PARSE_ESC_CHARSET, /* after ESC ( or ESC )  -  next byte selects charset */
} ParseState;

struct Terminal {
    int      cols;
    int      rows;
    Cell    *cells;
    Cell    *cells_alt;       /* alternate screen buffer */
    int      on_alt_screen;

    SbStore *sb;
    int      scroll_offset;        /* rows above live view (0 = live) */
    int      cursor_col;
    int      cursor_row;
    int      cursor_col_main;   /* primary-screen cursor saved on alt-screen entry */
    int      cursor_row_main;
    int      cursor_shape_main; /* primary-screen shape saved on alt-screen entry */
    int      saved_col;         /* ESC 7 / ESC 8 save/restore */
    int      saved_row;
    uint32_t fg;
    uint32_t bg;
    uint8_t  attrs;
    int      cursor_visible;  /* 0 = hidden (\x1b[?25l) */
    int      cursor_shape;    /* DECSCUSR: 0=default(block),1=blink block,2=block,
                               *           3=blink underline,4=underline,
                               *           5=blink beam,6=beam */
    int      app_cursor_keys; /* 1 = DECCKM: arrows -> \x1bO[ABCD] */
    int      bracketed_paste; /* 1 = wrap pastes in \x1b[200~ ... \x1b[201~ */
    int      charset_g0;      /* 0 = ASCII, 1 = DEC special graphics */
    int      esc_charset_g1;  /* 1 if last ESC charset designator was for G1 */
    int      pending_wrap;    /* VT100 last-column wrap-pending flag */
    ParseState state;
    char     csi_buf[CSI_BUF_SIZE];
    int      csi_len;
    int      csi_mod;         /* CSI modifier char: 0, '?', or '>' */
    int      scroll_top;
    int      scroll_bottom;
    uint8_t  utf8_buf[4];     /* partial UTF-8 sequence accumulator */
    int      utf8_rem;        /* continuation bytes still expected */
    pthread_mutex_t lock;
    /* Selection (main-thread only). */
    int sel_active;
    int sel_col0, sel_row0; /* anchor */
    int sel_col1, sel_row1; /* current end */

    /* OSC body accumulator */
    char osc_buf[OSC_BUF_SIZE];
    int  osc_len;

    /* Alt-screen callback (registered once before PTY starts; never changed). */
    terminal_alt_screen_fn     alt_screen_cb;
    void                      *alt_screen_cb_arg;
    /* Search callback (main-thread writes; background thread reads). */
    terminal_search_result_fn  search_cb;
    void                      *search_arg;

    /* Exit-code tracking via OSC 133;D sequences. */
    int                        last_exit_code;  /* -1 = not yet received */
    terminal_exit_code_fn      exit_code_cb;
    void                      *exit_code_cb_arg;

    /* Shell CWD tracking via OSC 7 sequences. */
    terminal_cwd_fn            cwd_cb;
    void                      *cwd_cb_arg;

    /* Window title via OSC 0/2 sequences. */
    terminal_title_fn          title_cb;
    void                      *title_cb_arg;
};

static void fill_cells(Cell *buf, int from, int to,
                       uint32_t fg, uint32_t bg)
{
    Cell blank = { .ch = ' ', .fg = fg, .bg = bg, .attrs = 0 };
    for (int i = from; i < to; i++) buf[i] = blank;
}

static int row_is_blank(const Cell *row, int cols)
{
    for (int j = 0; j < cols; j++) {
        if (' ' != row[j].ch) return 0;
    }
    return 1;
}

static void clear_range(Terminal *t, int from, int to)
{
    fill_cells(t->cells, from, to, t->fg, t->bg);
}


static void scroll_up(Terminal *t, int lines)
{
    int region_h = t->scroll_bottom - t->scroll_top + 1;
    int push     = lines < region_h ? lines : region_h;

    if (0 == t->scroll_top && !t->on_alt_screen) {
        for (int i = 0; i < push; i++)
            sb_push_row(t->sb, &t->cells[(t->scroll_top + i) * t->cols]);
        /* If the user is reading scrollback, advance the offset so the viewed
         * lines stay on screen rather than the view jumping to new output. */
        if (t->scroll_offset > 0) {
            t->scroll_offset += push;
            if (t->scroll_offset > t->sb->total_rows)
                t->scroll_offset = t->sb->total_rows;
        }
    }

    if (lines >= region_h) {
        clear_range(t, t->scroll_top * t->cols,
                    (t->scroll_bottom + 1) * t->cols);
        return;
    }
    int src = (t->scroll_top + lines) * t->cols;
    int dst = t->scroll_top * t->cols;
    int sz  = (region_h - lines) * t->cols;
    memmove(&t->cells[dst], &t->cells[src], (size_t)sz * sizeof(Cell));
    clear_range(t, (t->scroll_bottom - lines + 1) * t->cols,
                (t->scroll_bottom + 1) * t->cols);
}

static void scroll_down(Terminal *t, int lines)
{
    int region_h = t->scroll_bottom - t->scroll_top + 1;
    if (lines >= region_h) {
        clear_range(t, t->scroll_top * t->cols,
                    (t->scroll_bottom + 1) * t->cols);
        return;
    }
    int src = t->scroll_top * t->cols;
    int dst = (t->scroll_top + lines) * t->cols;
    int sz  = (region_h - lines) * t->cols;
    memmove(&t->cells[dst], &t->cells[src], (size_t)sz * sizeof(Cell));
    clear_range(t, src, src + lines * t->cols);
}

static void erase_display(Terminal *t, int mode)
{
    if (0 == mode) {
        clear_range(t, t->cursor_row * t->cols + t->cursor_col,
                    t->rows * t->cols);
    } else if (1 == mode) {
        clear_range(t, 0, t->cursor_row * t->cols + t->cursor_col + 1);
    } else {  /* 2 and 3 both clear the visible area */
        /* Append non-empty visible rows to scrollback so `clear` preserves
         * history without padding blank lines at the end. */
        if (!t->on_alt_screen) {
            int last = t->rows - 1;
            while (last >= 0 &&
                   row_is_blank(&t->cells[last * t->cols], t->cols))
                last--;
            for (int i = 0; i <= last; i++)
                sb_push_row(t->sb, &t->cells[i * t->cols]);
        }
        clear_range(t, 0, t->rows * t->cols);
    }
}

static void erase_line(Terminal *t, int mode)
{
    int base = t->cursor_row * t->cols;
    if (0 == mode) {
        clear_range(t, base + t->cursor_col, base + t->cols);
    } else if (1 == mode) {
        clear_range(t, base, base + t->cursor_col + 1);
    } else {
        clear_range(t, base, base + t->cols);
    }
}


static void enter_alt_screen(Terminal *t)
{
    if (t->on_alt_screen) return;
    VT_LOG("enter_alt_screen");
    t->scroll_offset   = 0;
    t->cursor_col_main   = t->cursor_col;
    t->cursor_row_main   = t->cursor_row;
    t->cursor_shape_main = t->cursor_shape;
    Cell *tmp    = t->cells;
    t->cells     = t->cells_alt;
    t->cells_alt = tmp;
    t->on_alt_screen = 1;
    clear_range(t, 0, t->rows * t->cols);
    t->cursor_col = 0;
    t->cursor_row = 0;
    if (t->alt_screen_cb) t->alt_screen_cb(t->alt_screen_cb_arg, 1);
}

static void exit_alt_screen(Terminal *t)
{
    if (!t->on_alt_screen) {
        VT_LOG("exit_alt_screen (not on alt screen  -  no-op)");
        return;
    }
    VT_LOG("exit_alt_screen");
    /* Preserve alt-screen content in scrollback before restoring primary. */
    for (int i = 0; i < t->rows; i++)
        sb_push_row(t->sb, &t->cells[i * t->cols]);
    Cell *tmp    = t->cells;
    t->cells     = t->cells_alt;
    t->cells_alt = tmp;
    t->on_alt_screen = 0;
    t->cursor_shape  = t->cursor_shape_main;
    t->cursor_col    = t->cursor_col_main;
    t->cursor_row    = t->cursor_row_main;
    t->scroll_offset = 0;
    if (t->alt_screen_cb) t->alt_screen_cb(t->alt_screen_cb_arg, 0);
}

static void parse_csi_params(const char *buf, int len,
                               int *params, int *nparams)
{
    *nparams = 0;
    int cur = 0;
    int has_cur = 0;
    for (int i = 0; i < len && *nparams < CSI_PARAMS_MAX; i++) {
        char c = buf[i];
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + (c - '0');
            has_cur = 1;
        } else if (';' == c) {
            params[(*nparams)++] = has_cur ? cur : 0;
            cur = 0;
            has_cur = 0;
        }
    }
    if (has_cur || *nparams > 0) params[(*nparams)++] = has_cur ? cur : 0;
}

static int p(int *params, int nparams, int idx, int def)
{
    if (idx < nparams && params[idx] > 0) return params[idx];
    return def;
}

static uint32_t color256(int n)
{
    if (n < ANSI_COLOR_COUNT) return ANSI_COLORS[n];
    if (n < GRAYSCALE_START) {
        n -= ANSI_COLOR_COUNT;
        int b = n % COLOR_CUBE_DIM; n /= COLOR_CUBE_DIM;
        int g = n % COLOR_CUBE_DIM; n /= COLOR_CUBE_DIM;
        int r = n % COLOR_CUBE_DIM;
        uint8_t rv = r ? (uint8_t)(COLOR_CUBE_BASE + r * COLOR_CUBE_STEP) : 0;
        uint8_t gv = g ? (uint8_t)(COLOR_CUBE_BASE + g * COLOR_CUBE_STEP) : 0;
        uint8_t bv = b ? (uint8_t)(COLOR_CUBE_BASE + b * COLOR_CUBE_STEP) : 0;
        return ((uint32_t)rv << 24) | ((uint32_t)gv << 16)
             | ((uint32_t)bv << 8)  | RGBA_ALPHA_OPAQUE;
    }
    uint8_t v = (uint8_t)(GRAYSCALE_BASE + (n - GRAYSCALE_START) * GRAYSCALE_STEP);
    return ((uint32_t)v << 24) | ((uint32_t)v << 16)
         | ((uint32_t)v << 8)  | RGBA_ALPHA_OPAQUE;
}

/* Parse an SGR extended-color sub-sequence starting at params[i].
 * Handles both 256-color (38;5;n) and RGB (38;2;r;g;b) forms.
 * Returns the number of extra parameter slots consumed (2 or 4),
 * or -1 if the sub-sequence is malformed (caller leaves color unchanged). */
static int parse_extended_color(const int *params, int nparams,
                                 int i, uint32_t *out_color)
{
    if (i + 2 < nparams && SGR_COLOR256 == params[i + 1]) {
        *out_color = color256(params[i + 2]);
        return 2;
    }
    if (i + 4 < nparams && SGR_COLORRGB == params[i + 1]) {
        *out_color = ((uint32_t)params[i + 2] << 24)
                   | ((uint32_t)params[i + 3] << 16)
                   | ((uint32_t)params[i + 4] <<  8)
                   | RGBA_ALPHA_OPAQUE;
        return 4;
    }
    return -1;
}

static void apply_sgr(Terminal *t, int *params, int nparams)
{
    /* Bare \e[m (no params) is equivalent to \e[0m, reset all attributes. */
    if (0 == nparams) {
        t->fg    = COL_DEFAULT_FG;
        t->bg    = COL_DEFAULT_BG;
        t->attrs = 0;
        return;
    }
    for (int i = 0; i < nparams; i++) {
        int v = params[i];
        if (SGR_RESET == v) {
            t->fg    = COL_DEFAULT_FG;
            t->bg    = COL_DEFAULT_BG;
            t->attrs = 0;
        } else if (SGR_BOLD == v) {
            t->attrs |= ATTR_BOLD;
        } else if (SGR_ITALIC == v) {
            t->attrs |= ATTR_ITALIC;
        } else if (SGR_UNDERLINE == v) {
            t->attrs |= ATTR_UNDERLINE;
        } else if (SGR_REVERSE == v) {
            t->attrs |= ATTR_REVERSE;
        } else if (SGR_BOLD_OFF == v) {
            t->attrs &= (uint8_t)~ATTR_BOLD;
        } else if (SGR_ITALIC_OFF == v) {
            t->attrs &= (uint8_t)~ATTR_ITALIC;
        } else if (SGR_UNDERLINE_OFF == v) {
            t->attrs &= (uint8_t)~ATTR_UNDERLINE;
        } else if (SGR_REVERSE_OFF == v) {
            t->attrs &= (uint8_t)~ATTR_REVERSE;
        } else if (v >= SGR_FG_FIRST && v <= SGR_FG_LAST) {
            t->fg = ANSI_COLORS[v - SGR_FG_FIRST];
        } else if (SGR_FG_DEFAULT == v) {
            t->fg = COL_DEFAULT_FG;
        } else if (v >= SGR_BG_FIRST && v <= SGR_BG_LAST) {
            t->bg = ANSI_COLORS[v - SGR_BG_FIRST];
        } else if (SGR_BG_DEFAULT == v) {
            t->bg = COL_DEFAULT_BG;
        } else if (v >= SGR_FG_BRIGHT_FIRST && v <= SGR_FG_BRIGHT_LAST) {
            t->fg = ANSI_COLORS[ANSI_BRIGHT_OFFSET + v - SGR_FG_BRIGHT_FIRST];
        } else if (v >= SGR_BG_BRIGHT_FIRST && v <= SGR_BG_BRIGHT_LAST) {
            t->bg = ANSI_COLORS[ANSI_BRIGHT_OFFSET + v - SGR_BG_BRIGHT_FIRST];
        } else if (SGR_FG_EXT == v) {
            int adv = parse_extended_color(params, nparams, i, &t->fg);
            if (adv > 0) i += adv;
        } else if (SGR_BG_EXT == v) {
            int adv = parse_extended_color(params, nparams, i, &t->bg);
            if (adv > 0) i += adv;
        }
    }
}

static void clamp_cursor(Terminal *t)
{
    if (t->cursor_col < 0) t->cursor_col = 0;
    if (t->cursor_col >= t->cols) t->cursor_col = t->cols - 1;
    if (t->cursor_row < 0) t->cursor_row = 0;
    if (t->cursor_row >= t->rows) t->cursor_row = t->rows - 1;
}

static void csi_dispatch(Terminal *t, char cmd)
{
    int params[CSI_PARAMS_MAX];
    int nparams = 0;
    parse_csi_params(t->csi_buf, t->csi_len, params, &nparams);
    t->pending_wrap = 0;

    /* Sequences with '>' modifier (e.g. \x1b[>4;2m) are xterm-private and
     * must not be interpreted as standard CSI commands. */
    if ('>' == t->csi_mod) return;

    switch (cmd) {
    case 'A':
        t->cursor_row -= p(params, nparams, 0, 1);
        break;
    case 'B':
        t->cursor_row += p(params, nparams, 0, 1);
        break;
    case 'C':
        t->cursor_col += p(params, nparams, 0, 1);
        break;
    case 'D':
        t->cursor_col -= p(params, nparams, 0, 1);
        break;
    case 'E':
        t->cursor_row += p(params, nparams, 0, 1);
        t->cursor_col = 0;
        break;
    case 'F':
        t->cursor_row -= p(params, nparams, 0, 1);
        t->cursor_col = 0;
        break;
    case 'G':
        t->cursor_col = p(params, nparams, 0, 1) - 1;
        break;
    case 'H':
    case 'f':
        t->cursor_row = p(params, nparams, 0, 1) - 1;
        t->cursor_col = p(params, nparams, 1, 1) - 1;
        break;
    case 'J':
        erase_display(t, nparams ? params[0] : 0);
        break;
    case 'K':
        erase_line(t, nparams ? params[0] : 0);
        break;
    case 'L': {  /* IL: insert lines  -  push region down from cursor row */
        int n = p(params, nparams, 0, 1);
        int saved_top = t->scroll_top;
        t->scroll_top = t->cursor_row;
        scroll_down(t, n);
        t->scroll_top = saved_top;
        break;
    }
    case 'M': {  /* DL: delete lines  -  pull region up from cursor row */
        int n = p(params, nparams, 0, 1);
        int saved_top = t->scroll_top;
        t->scroll_top = t->cursor_row;
        scroll_up(t, n);
        t->scroll_top = saved_top;
        break;
    }
    case 'P': {
        int n = p(params, nparams, 0, 1);
        int row_base = t->cursor_row * t->cols;
        int end = row_base + t->cols;
        int src = row_base + t->cursor_col + n;
        int dst = row_base + t->cursor_col;
        if (src < end) {
            memmove(&t->cells[dst], &t->cells[src],
                    (size_t)(end - src) * sizeof(Cell));
        }
        int clear_from = end - n < dst ? dst : end - n;
        clear_range(t, clear_from, end);
        break;
    }
    case 'X': {  /* ECH: erase characters */
        int n = p(params, nparams, 0, 1);
        int from = t->cursor_row * t->cols + t->cursor_col;
        int to   = from + n;
        int max  = (t->cursor_row + 1) * t->cols;
        if (to > max) to = max;
        clear_range(t, from, to);
        break;
    }
    case '@': {  /* ICH: insert blank characters */
        int n = p(params, nparams, 0, 1);
        int row_base = t->cursor_row * t->cols;
        int end = row_base + t->cols;
        int src = row_base + t->cursor_col;
        int dst = src + n;
        if (dst < end) {
            memmove(&t->cells[dst], &t->cells[src],
                    (size_t)(end - dst) * sizeof(Cell));
        }
        int clear_to = dst < end ? dst : end;
        clear_range(t, src, clear_to);
        break;
    }
    case 'd':
        t->cursor_row = p(params, nparams, 0, 1) - 1;
        break;
    case 'h':
        if ('?' == t->csi_mod) {
            int mode = p(params, nparams, 0, 0);
            VT_LOG("CSI ?%dh", mode);
            switch (mode) {
            case DECPM_DECCKM:           t->app_cursor_keys = 1; break;
            case DECPM_CURSOR_VISIBLE:   t->cursor_visible  = 1; break;
            case DECPM_ALT_SCREEN:       /* fallthrough */
            case DECPM_ALT_SCREEN_2:     enter_alt_screen(t); break;
            case DECPM_ALT_SCREEN_3:     enter_alt_screen(t); break;
            case DECPM_BRACKETED_PASTE:  t->bracketed_paste = 1; break;
            default:                     break;
            }
        }
        break;
    case 'l':
        if ('?' == t->csi_mod) {
            int mode = p(params, nparams, 0, 0);
            VT_LOG("CSI ?%dl", mode);
            switch (mode) {
            case DECPM_DECCKM:
                t->app_cursor_keys = 0;
                /* Implicit rmcup: Vim resets DECCKM (rmkx) without
                 * sending ?1049l when exiting. Exit alt screen if active. */
                if (t->on_alt_screen) exit_alt_screen(t);
                break;
            case DECPM_CURSOR_VISIBLE:   t->cursor_visible  = 0; break;
            case DECPM_ALT_SCREEN:       /* fallthrough */
            case DECPM_ALT_SCREEN_2:     exit_alt_screen(t); break;
            case DECPM_ALT_SCREEN_3:     exit_alt_screen(t); break;
            case DECPM_BRACKETED_PASTE:  t->bracketed_paste = 0; break;
            default:                     break;
            }
        }
        break;
    case 'm':
        apply_sgr(t, params, nparams);
        break;
    case 'q':
        /* DECSCUSR (ESC [ Ps SP q), set cursor shape.
         * Only active when intermediate byte SP is present in the CSI buffer. */
        {
            int has_sp = 0;
            for (int i = 0; i < t->csi_len; i++) {
                if (' ' == t->csi_buf[i]) { has_sp = 1; break; }
            }
            if (has_sp)
                t->cursor_shape = p(params, nparams, 0, 0);
        }
        break;
    case 'r':
        t->scroll_top    = p(params, nparams, 0, 1) - 1;
        t->scroll_bottom = p(params, nparams, 1, t->rows) - 1;
        if (t->scroll_top < 0) t->scroll_top = 0;
        if (t->scroll_bottom >= t->rows) t->scroll_bottom = t->rows - 1;
        break;
    case 's':
        t->saved_col = t->cursor_col;
        t->saved_row = t->cursor_row;
        break;
    case 'u':
        t->cursor_col = t->saved_col;
        t->cursor_row = t->saved_row;
        break;
    default:
        break;
    }
    clamp_cursor(t);
}

static void write_glyph(Terminal *t, uint32_t cp)
{
    if (t->pending_wrap) {
        t->pending_wrap = 0;
        t->cursor_col = 0;
        if (t->cursor_row == t->scroll_bottom) {
            scroll_up(t, 1);
        } else {
            t->cursor_row++;
        }
    }
    if (t->cursor_col >= t->cols) t->cursor_col = t->cols - 1;
    int idx = t->cursor_row * t->cols + t->cursor_col;
    t->cells[idx].ch    = cp;
    t->cells[idx].fg    = t->fg;
    t->cells[idx].bg    = t->bg;
    t->cells[idx].attrs = t->attrs;
    if (t->cursor_col >= t->cols - 1) {
        t->pending_wrap = 1;
    } else {
        t->cursor_col++;
    }
}

/* URL-decodes a percent-encoded string into out (size cap).
 * Returns number of bytes written (excluding NUL). */
static size_t url_decode(const char *src, char *out, size_t cap)
{
    size_t n = 0;
    for (; *src && n + 1 < cap; src++) {
        if ('%' == src[0]
            && ((src[1] >= '0' && src[1] <= '9')
                || (src[1] >= 'A' && src[1] <= 'F')
                || (src[1] >= 'a' && src[1] <= 'f'))
            && ((src[2] >= '0' && src[2] <= '9')
                || (src[2] >= 'A' && src[2] <= 'F')
                || (src[2] >= 'a' && src[2] <= 'f'))) {
            char hex[3] = {src[1], src[2], '\0'};
            out[n++] = (char)(int)strtol(hex, NULL, 16);
            src += 2;
        } else {
            out[n++] = *src;
        }
    }
    out[n] = '\0';
    return n;
}

/* Called with t->lock held, before osc_len is cleared.
 * Parses OSC 7 (shell CWD) and OSC 133;D (exit code) sequences. */
static void dispatch_osc(Terminal *t)
{
    t->osc_buf[t->osc_len] = '\0';

    /* OSC 0/2: window title, "0;title" or "2;title" */
    if ((t->osc_buf[0] == '0' || t->osc_buf[0] == '2') &&
        t->osc_buf[1] == ';' && NULL != t->title_cb) {
        t->title_cb(t->osc_buf + 2, t->title_cb_arg);
        if (t->osc_buf[0] == '2') return;
        /* OSC 0 also sets icon name  -  fall through to check other OSCs if any */
        return;
    }

    /* OSC 7: shell CWD notification, "7;file://[host]/path" */
    if (0 == strncmp(t->osc_buf, "7;file://", 9) && NULL != t->cwd_cb) {
        /* Skip optional hostname (everything up to the next '/'). */
        const char *p = t->osc_buf + 9;
        if ('/' != p[0]) {
            const char *slash = strchr(p, '/');
            if (NULL != slash) p = slash;
        }
        if ('/' == p[0]) {
            char decoded[4096];
            if (url_decode(p, decoded, sizeof decoded) > 0)
                t->cwd_cb(decoded, t->cwd_cb_arg);
        }
        return;
    }

    /* OSC 133;D: command end / exit code. */
    if (0 != strncmp(t->osc_buf, "133;D", 5)) return;

    int code = 0;
    if (';' == t->osc_buf[5])
        code = (int)strtol(t->osc_buf + 6, NULL, 10);

    t->last_exit_code = code;

    if (NULL != t->exit_code_cb)
        t->exit_code_cb(code, t->exit_code_cb_arg);
}

static void process_byte(Terminal *t, unsigned char c)
{
    switch (t->state) {
    case PARSE_NORMAL:
        if (ASCII_ESC == c) {
            t->utf8_rem    = 0;
            t->pending_wrap = 0;
            t->state = PARSE_ESC;
        } else if (ASCII_CR == c) {
            t->utf8_rem    = 0;
            t->pending_wrap = 0;
            t->cursor_col  = 0;
        } else if (ASCII_LF == c || ASCII_VT == c || ASCII_FF == c) {
            t->utf8_rem    = 0;
            t->pending_wrap = 0;
            if (t->cursor_row == t->scroll_bottom) {
                scroll_up(t, 1);
            } else {
                t->cursor_row++;
            }
        } else if (ASCII_BS == c) {
            t->utf8_rem    = 0;
            t->pending_wrap = 0;
            if (t->cursor_col > 0) t->cursor_col--;
        } else if (ASCII_HT == c) {
            t->utf8_rem    = 0;
            t->pending_wrap = 0;
            t->cursor_col  = (t->cursor_col + TAB_STOP_WIDTH) & ~(TAB_STOP_WIDTH - 1);
            if (t->cursor_col >= t->cols) t->cursor_col = t->cols - 1;
        } else if (c >= UTF8_2BYTE_LEAD_MIN) {
            /* UTF-8 phase 1  -  lead byte: record it and set utf8_rem to the
             * number of continuation bytes still expected (1, 2, or 3). */
            t->utf8_buf[0] = c;
            t->utf8_rem = (c >= UTF8_4BYTE_LEAD_MIN) ? 3
                        : (c >= UTF8_3BYTE_LEAD_MIN)  ? 2 : 1;
        } else if (c >= UTF8_CONT_BYTE_MIN) {
            /* UTF-8 phase 2  -  continuation byte: store it and decrement
             * utf8_rem.  Phase 3 triggers when utf8_rem reaches zero:
             * assemble the codepoint from the buffered bytes and call write_glyph.
             * Stray continuations (utf8_rem == 0) are silently discarded per the Unicode replacement policy. */
            if (t->utf8_rem > 0) {
                uint8_t lead = t->utf8_buf[0];
                int total = (lead >= UTF8_4BYTE_LEAD_MIN) ? 4
                          : (lead >= UTF8_3BYTE_LEAD_MIN)  ? 3 : 2;
                t->utf8_buf[total - t->utf8_rem] = c;
                t->utf8_rem--;
                if (0 == t->utf8_rem) {
                    uint32_t cp;
                    if (lead >= UTF8_4BYTE_LEAD_MIN) {
                        cp = ((uint32_t)(lead             & UTF8_4BYTE_DATA_MASK) << 18)
                           | ((uint32_t)(t->utf8_buf[1]  & UTF8_CONT_DATA_MASK)  << 12)
                           | ((uint32_t)(t->utf8_buf[2]  & UTF8_CONT_DATA_MASK)  <<  6)
                           | (uint32_t)(t->utf8_buf[3]   & UTF8_CONT_DATA_MASK);
                    } else if (lead >= UTF8_3BYTE_LEAD_MIN) {
                        cp = ((uint32_t)(lead             & UTF8_3BYTE_DATA_MASK) << 12)
                           | ((uint32_t)(t->utf8_buf[1]  & UTF8_CONT_DATA_MASK)  <<  6)
                           | (uint32_t)(t->utf8_buf[2]   & UTF8_CONT_DATA_MASK);
                    } else {
                        cp = ((uint32_t)(lead             & UTF8_2BYTE_DATA_MASK) <<  6)
                           | (uint32_t)(t->utf8_buf[1]   & UTF8_CONT_DATA_MASK);
                    }
                    write_glyph(t, cp);
                }
            }
            /* else: stray continuation, ignore */
        } else if (c >= ASCII_PRINTABLE_FIRST) {
            t->utf8_rem = 0;
            uint32_t glyph = c;
            if (t->charset_g0 && c >= DEC_SPECIAL_FIRST && c <= ASCII_PRINTABLE_LAST) {
                glyph = DEC_SPECIAL[c - DEC_SPECIAL_FIRST];
            }
            write_glyph(t, glyph);
        }
        break;

    case PARSE_ESC:
        t->state        = PARSE_NORMAL;
        t->pending_wrap = 0;
        switch ((char)c) {
        case '[':
            t->state   = PARSE_CSI;
            t->csi_len = 0;
            t->csi_mod = 0;
            memset(t->csi_buf, 0, sizeof t->csi_buf);
            break;
        case ']':
            t->osc_len = 0;
            t->state   = PARSE_OSC;
            break;
        case '(':
            t->esc_charset_g1 = 0;
            t->state = PARSE_ESC_CHARSET;
            break;
        case ')': case '*': case '+':
            t->esc_charset_g1 = 1;
            t->state = PARSE_ESC_CHARSET;
            break;
        case 'c':  /* RIS: full reset */
            clear_range(t, 0, t->rows * t->cols);
            t->cursor_col      = 0;
            t->cursor_row      = 0;
            t->fg              = COL_DEFAULT_FG;
            t->bg              = COL_DEFAULT_BG;
            t->attrs           = 0;
            t->scroll_top      = 0;
            t->scroll_bottom   = t->rows - 1;
            t->charset_g0      = 0;
            t->app_cursor_keys = 0;
            t->cursor_visible  = 1;
            break;
        case '7':  /* DECSC: save cursor */
            t->saved_col = t->cursor_col;
            t->saved_row = t->cursor_row;
            break;
        case '8':  /* DECRC: restore cursor */
            t->cursor_col = t->saved_col;
            t->cursor_row = t->saved_row;
            break;
        case 'M':  /* RI: reverse index (scroll down if at top margin) */
            if (t->cursor_row == t->scroll_top) {
                scroll_down(t, 1);
            } else {
                t->cursor_row--;
            }
            break;
        case 'E':  /* NEL: next line */
            t->cursor_col = 0;
            if (t->cursor_row == t->scroll_bottom) {
                scroll_up(t, 1);
            } else {
                t->cursor_row++;
            }
            break;
        default:   /* unknown / keypad mode switches (= >), silently ignored */
            break;
        }
        break;

    case PARSE_CSI:
        if (('?' == c || '>' == c) && 0 == t->csi_len && 0 == t->csi_mod) {
            t->csi_mod = c;
        } else if (c >= CSI_FINAL_FIRST && c <= CSI_FINAL_LAST) {
            csi_dispatch(t, (char)c);
            t->state = PARSE_NORMAL;
        } else if (t->csi_len < (int)sizeof(t->csi_buf) - 1) {
            t->csi_buf[t->csi_len++] = (char)c;
        }
        break;

    case PARSE_OSC:
        if (ASCII_BEL == c) {
            dispatch_osc(t);
            t->osc_len = 0;
            t->state   = PARSE_NORMAL;
        } else if (ASCII_ESC == c) {
            t->state = PARSE_OSC_ESC;
        } else {
            if (t->osc_len < OSC_BUF_SIZE - 1)
                t->osc_buf[t->osc_len++] = (char)c;
        }
        break;

    case PARSE_OSC_ESC:
        /* Awaiting '\' (String Terminator). */
        if ('\\' == c) {
            dispatch_osc(t);
            t->osc_len = 0;
        }
        t->state = PARSE_NORMAL;
        break;

    case PARSE_ESC_CHARSET:
        /* Only apply to G0; silently ignore G1/G2/G3 designations. */
        if (!t->esc_charset_g1) {
            t->charset_g0 = (CHARSET_DEC_SPECIAL == c) ? 1 : 0;
        }
        t->state = PARSE_NORMAL;
        break;
    }
}

Terminal *terminal_create(int cols, int rows)
{
    Terminal *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->cols      = cols;
    t->rows      = rows;
    t->cells     = malloc((size_t)(cols * rows) * sizeof(Cell));
    t->cells_alt = malloc((size_t)(cols * rows) * sizeof(Cell));
    t->sb        = sb_create(cols);
    if (!t->cells || !t->cells_alt || !t->sb) {
        free(t->cells); free(t->cells_alt);
        sb_destroy(t->sb); free(t); return NULL;
    }
    t->fg            = COL_DEFAULT_FG;
    t->bg            = COL_DEFAULT_BG;
    t->scroll_top    = 0;
    t->scroll_bottom = rows - 1;
    t->cursor_visible = 1;
    t->state         = PARSE_NORMAL;
    t->last_exit_code = -1;
    pthread_mutex_init(&t->lock, NULL);
    fill_cells(t->cells,     0, cols * rows, COL_DEFAULT_FG, COL_DEFAULT_BG);
    fill_cells(t->cells_alt, 0, cols * rows, COL_DEFAULT_FG, COL_DEFAULT_BG);
    return t;
}

void terminal_destroy(Terminal *t)
{
    if (!t) return;
    terminal_search_cancel(t);
    pthread_mutex_destroy(&t->lock);
    free(t->cells);
    free(t->cells_alt);
    sb_destroy(t->sb);
    free(t);
}

void terminal_feed(Terminal *t, const char *buf, size_t len)
{
#if PHANTOM_DEBUG && LOG_BUFFER
    fprintf(stderr, "phantom: feed %zu bytes:", len);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c >= ASCII_PRINTABLE_FIRST && c <= ASCII_PRINTABLE_LAST) {
            fprintf(stderr, " %c", c);
        } else {
            fprintf(stderr, " %02x", c);
        }
    }
    fprintf(stderr, "\n");
#endif
    pthread_mutex_lock(&t->lock);
    for (size_t i = 0; i < len; i++) {
        process_byte(t, (unsigned char)buf[i]);
    }
    pthread_mutex_unlock(&t->lock);
}

/* Copy a rectangle of cells from src to dst
 * src_row_start: first row in src to copy from.
 * dst_row_start: first row in dst to copy into.
 * copy_cols: number of columns to copy per row (pre-clamped by caller). */
static void copy_cells_resized(const Cell *src, int src_cols,
                                Cell *dst, int dst_cols,
                                int src_row_start, int dst_row_start,
                                int copy_rows, int copy_cols)
{
    for (int row = 0; row < copy_rows; row++) {
        memcpy(&dst[(dst_row_start + row) * dst_cols],
               &src[(src_row_start + row) * src_cols],
               (size_t)copy_cols * sizeof(Cell));
    }
}

void terminal_resize(Terminal *t, int cols, int rows)
{
    pthread_mutex_lock(&t->lock);

    int old_cols = t->cols;
    int old_rows = t->rows;
    int copy_cols = cols < old_cols ? cols : old_cols;

    Cell *new_cells     = malloc((size_t)(cols * rows) * sizeof(Cell));
    Cell *new_cells_alt = malloc((size_t)(cols * rows) * sizeof(Cell));
    if (!new_cells || !new_cells_alt) {
        free(new_cells); free(new_cells_alt);
        pthread_mutex_unlock(&t->lock);
        return;
    }
    fill_cells(new_cells,     0, cols * rows, COL_DEFAULT_FG, COL_DEFAULT_BG);
    fill_cells(new_cells_alt, 0, cols * rows, COL_DEFAULT_FG, COL_DEFAULT_BG);

    /* Sync visible grid with scrollback on resize (Kitty rewrap approach):
     * enlarge → pull newest sb rows into viewport top;
     * shrink  → push top visible rows back into scrollback. */
    if (!t->on_alt_screen) {
        int delta = rows - old_rows;

        if (delta > 0) {
            int pull = delta < t->sb->total_rows ? delta : t->sb->total_rows;
            int sb_base = t->sb->total_rows - pull;
            for (int i = 0; i < pull; i++)
                sb_get_row_into(t->sb, sb_base + i,
                                &new_cells[i * cols], cols);
            int copy_rows = old_rows < rows - pull ? old_rows : rows - pull;
            copy_cells_resized(t->cells, old_cols,
                               new_cells, cols,
                               0, pull, copy_rows, copy_cols);
            sb_trim_newest(t->sb, pull);
            t->cursor_row += pull;
            t->cursor_col_main += 0; /* unchanged */
        } else if (delta < 0) {
            int want_push = -delta;
            int push = want_push < t->cursor_row ? want_push : t->cursor_row;
            for (int i = 0; i < push; i++)
                sb_push_row(t->sb, &t->cells[i * old_cols]);
            int src_start = push;
            int copy_rows = old_rows - src_start < rows
                          ? old_rows - src_start : rows;
            copy_cells_resized(t->cells, old_cols,
                               new_cells, cols,
                               src_start, 0, copy_rows, copy_cols);
            t->cursor_row -= push;
        } else {
            copy_cells_resized(t->cells, old_cols,
                               new_cells, cols,
                               0, 0, old_rows, copy_cols);
        }
    } else {
        /* Copy both buffers without touching the primary-screen scrollback. */
        int copy_rows = old_rows < rows ? old_rows : rows;
        copy_cells_resized(t->cells, old_cols,
                           new_cells, cols,
                           0, 0, copy_rows, copy_cols);
        copy_cells_resized(t->cells_alt, old_cols,
                           new_cells_alt, cols,
                           0, 0, copy_rows, copy_cols);
        /* Clamp the saved primary-screen cursor. */
        if (t->cursor_col_main >= cols)  t->cursor_col_main = cols - 1;
        if (t->cursor_row_main >= rows)  t->cursor_row_main = rows - 1;
    }

    /* Copy alt-screen buffer for non-alt-screen case (primary buffer copied
     * above; alt buffer just needs a width/height-safe copy). */
    if (!t->on_alt_screen) {
        int copy_rows = old_rows < rows ? old_rows : rows;
        copy_cells_resized(t->cells_alt, old_cols,
                           new_cells_alt, cols,
                           0, 0, copy_rows, copy_cols);
    }

    free(t->cells);
    free(t->cells_alt);
    t->cells     = new_cells;
    t->cells_alt = new_cells_alt;

    /* Adapt scrollback to the new column width (finalises the tail at the
     * old width and opens a new empty tail at the new width).  Existing
     * compressed chunks are preserved verbatim at their own widths. */
    if (cols != old_cols) sb_adapt_cols(t->sb, cols);

    t->scroll_offset = 0;
    t->cols          = cols;
    t->rows          = rows;
    t->scroll_top    = 0;
    t->scroll_bottom = rows - 1;
    clamp_cursor(t);
    pthread_mutex_unlock(&t->lock);
}

int terminal_cols(const Terminal *t) { return t->cols; }
int terminal_rows(const Terminal *t) { return t->rows; }
int terminal_app_cursor_keys(const Terminal *t) { return t->app_cursor_keys; }
int terminal_cursor_visible(const Terminal *t)  { return t->cursor_visible;  }
int terminal_on_alt_screen(const Terminal *t)   { return t->on_alt_screen;   }
int terminal_cursor_shape(const Terminal *t)    { return t->cursor_shape;    }
int terminal_bracketed_paste(const Terminal *t) { return t->bracketed_paste; }
int terminal_scroll_offset(const Terminal *t)   { return t->scroll_offset;   }
int terminal_sb_total_rows(const Terminal *t)   { return t->sb->total_rows;  }

void terminal_scroll_to_row(Terminal *t, int abs_row)
{
    pthread_mutex_lock(&t->lock);
    int total = t->sb->total_rows;
    t->scroll_offset = total - abs_row;
    if (t->scroll_offset < 0) t->scroll_offset = 0;
    if (t->scroll_offset > total) t->scroll_offset = total;
    pthread_mutex_unlock(&t->lock);
}

void terminal_set_selection(Terminal *t, int c0, int r0, int c1, int r1)
{
    t->sel_col0 = c0; t->sel_row0 = r0;
    t->sel_col1 = c1; t->sel_row1 = r1;
    t->sel_active = 1;
}

void terminal_clear_selection(Terminal *t)
{
    t->sel_active = 0;
}

static void sel_norm(const Terminal *t,
                     int *c0, int *r0, int *c1, int *r1)
{
    int ac = t->sel_col0, ar = t->sel_row0;
    int bc = t->sel_col1, br = t->sel_row1;
    if (ar > br || (ar == br && ac > bc)) {
        int tc = ac; ac = bc; bc = tc;
        int tr = ar; ar = br; br = tr;
    }
    *c0 = ac; *r0 = ar; *c1 = bc; *r1 = br;
}

int terminal_cell_selected(const Terminal *t, int col, int row)
{
    if (!t->sel_active) return 0;
    int c0, r0, c1, r1;
    sel_norm(t, &c0, &r0, &c1, &r1);
    if (row < r0 || row > r1) return 0;
    if (row == r0 && col < c0) return 0;
    if (row == r1 && col > c1) return 0;
    return 1;
}

/* Returns a newly malloc'd UTF-8 string for the current selection.
 * Caller must free(). Returns NULL when there is no selection. */
char *terminal_get_selected_text(Terminal *t)
{
    if (!t->sel_active) return NULL;
    int c0, r0, c1, r1;
    sel_norm(t, &c0, &r0, &c1, &r1);

    int cols = t->cols, rows = t->rows;
    Cell *snap = malloc((size_t)(cols * rows) * sizeof *snap);
    if (!snap) return NULL;
    int cc, cr;
    terminal_get_state(t, snap, &cc, &cr);

    size_t bufsz = (size_t)(r1 - r0 + 1) * ((size_t)cols * UTF8_MAX_BYTES + 2);
    char *buf = malloc(bufsz);
    if (!buf) { free(snap); return NULL; }

    char *p = buf;
    for (int row = r0; row <= r1 && row < rows; row++) {
        int from = (row == r0) ? c0 : 0;
        int to   = (row == r1) ? c1 : cols - 1;
        /* Strip trailing spaces from all but the last selected row. */
        if (row < r1) {
            while (to >= from && snap[row * cols + to].ch == ' ') to--;
        }
        for (int col = from; col <= to; col++) {
            uint32_t cp = snap[row * cols + col].ch;
            if (0 == cp) cp = ' ';
            if (cp < UTF8_CONT_BYTE_MIN) {
                *p++ = (char)cp;
            } else if (cp < UTF8_2BYTE_LIMIT) {
                *p++ = (char)(UTF8_2BYTE_LEAD_MIN | (cp >> 6));
                *p++ = (char)(UTF8_CONT_BYTE_MIN  | (cp & UTF8_CONT_DATA_MASK));
            } else if (cp < UTF8_3BYTE_LIMIT) {
                *p++ = (char)(UTF8_3BYTE_LEAD_MIN | (cp >> 12));
                *p++ = (char)(UTF8_CONT_BYTE_MIN  | ((cp >> 6) & UTF8_CONT_DATA_MASK));
                *p++ = (char)(UTF8_CONT_BYTE_MIN  | (cp & UTF8_CONT_DATA_MASK));
            } else {
                *p++ = (char)(UTF8_4BYTE_LEAD_MIN | (cp >> 18));
                *p++ = (char)(UTF8_CONT_BYTE_MIN  | ((cp >> 12) & UTF8_CONT_DATA_MASK));
                *p++ = (char)(UTF8_CONT_BYTE_MIN  | ((cp >> 6) & UTF8_CONT_DATA_MASK));
                *p++ = (char)(UTF8_CONT_BYTE_MIN  | (cp & UTF8_CONT_DATA_MASK));
            }
        }
        if (row < r1) *p++ = '\n';
    }
    *p = '\0';
    free(snap);
    return buf;
}

void terminal_get_state(Terminal *t, Cell *cells_out,
                         int *cursor_col, int *cursor_row)
{
    pthread_mutex_lock(&t->lock);
    if (t->scroll_offset > 0) {
        int  sb_rows = t->sb->total_rows;
        Cell blank   = { .ch = ' ', .fg = COL_DEFAULT_FG,
                         .bg = COL_DEFAULT_BG, .attrs = 0 };
        for (int row = 0; row < t->rows; row++) {
            Cell *dest       = &cells_out[row * t->cols];
            int   lines_back = t->scroll_offset - row;
            if (lines_back > 0) {
                int abs_row = sb_rows - lines_back;
                if (abs_row >= 0) {
                    sb_get_row_into(t->sb, abs_row, dest, t->cols);
                } else {
                    for (int col = 0; col < t->cols; col++) dest[col] = blank;
                }
            } else {
                memcpy(dest, &t->cells[(row - t->scroll_offset) * t->cols],
                       (size_t)t->cols * sizeof(Cell));
            }
        }
        /* Don't draw the cursor while scrolled back. */
        *cursor_col = -1;
        *cursor_row = -1;
    } else {
        memcpy(cells_out, t->cells,
               (size_t)(t->cols * t->rows) * sizeof(Cell));
        *cursor_col = t->cursor_col;
        *cursor_row = t->cursor_row;
    }
    pthread_mutex_unlock(&t->lock);
}

void terminal_scroll(Terminal *t, int delta)
{
    pthread_mutex_lock(&t->lock);
    t->scroll_offset += delta;
    if (t->scroll_offset < 0) t->scroll_offset = 0;
    if (t->scroll_offset > t->sb->total_rows)
        t->scroll_offset = t->sb->total_rows;
    pthread_mutex_unlock(&t->lock);
}

void terminal_scroll_bottom(Terminal *t)
{
    pthread_mutex_lock(&t->lock);
    t->scroll_offset = 0;
    pthread_mutex_unlock(&t->lock);
}

void terminal_set_alt_screen_callback(Terminal *t,
                                      terminal_alt_screen_fn fn, void *arg)
{
    t->alt_screen_cb     = fn;
    t->alt_screen_cb_arg = arg;
}

int terminal_exit_code(const Terminal *t)
{
    if (NULL == t) return -1;
    pthread_mutex_lock((pthread_mutex_t *)&t->lock);
    int code = t->last_exit_code;
    pthread_mutex_unlock((pthread_mutex_t *)&t->lock);
    return code;
}

void terminal_set_exit_code_callback(Terminal *t,
                                     terminal_exit_code_fn fn,
                                     void *arg)
{
    if (NULL == t) return;
    t->exit_code_cb     = fn;
    t->exit_code_cb_arg = arg;
}

void terminal_set_cwd_callback(Terminal *t, terminal_cwd_fn fn, void *arg)
{
    if (NULL == t) return;
    t->cwd_cb     = fn;
    t->cwd_cb_arg = arg;
}

void terminal_set_title_callback(Terminal *t, terminal_title_fn fn, void *arg)
{
    if (NULL == t) return;
    t->title_cb     = fn;
    t->title_cb_arg = arg;
}

void terminal_search_cancel(Terminal *t)
{
    if (t && t->sb) {
        t->sb->search_cancel = 1;
        VT_LOG("search: cancel");
    }
}

/* Decode one UTF-8 codepoint from *p, advancing *p past it.
 * Returns 0 at end of string. */
static uint32_t utf8_decode_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    if (0 == *s) return 0;
    uint32_t cp;
    if (*s < UTF8_2BYTE_LEAD_MIN) {
        cp  = *s++;
    } else if (*s < UTF8_3BYTE_LEAD_MIN) {
        cp  = (*s++ & UTF8_2BYTE_DATA_MASK) << 6;
        cp |= (*s++ & UTF8_CONT_DATA_MASK);
    } else if (*s < UTF8_4BYTE_LEAD_MIN) {
        cp  = (*s++ & UTF8_3BYTE_DATA_MASK) << 12;
        cp |= (*s++ & UTF8_CONT_DATA_MASK) << 6;
        cp |= (*s++ & UTF8_CONT_DATA_MASK);
    } else {
        cp  = (*s++ & UTF8_4BYTE_DATA_MASK) << 18;
        cp |= (*s++ & UTF8_CONT_DATA_MASK) << 12;
        cp |= (*s++ & UTF8_CONT_DATA_MASK) << 6;
        cp |= (*s++ & UTF8_CONT_DATA_MASK);
    }
    *p = (const char *)s;
    return cp;
}

/* Check if the query codepoint sequence qcps[0..qlen-1] appears starting at
 * row[col].  cells_avail is the number of cells to the right of col. */
static int cells_match_query(const Cell *row, int cells_avail,
                              const uint32_t *qcps, int qlen)
{
    if (cells_avail < qlen) return 0;
    for (int i = 0; i < qlen; i++) {
        if (row[i].ch != qcps[i]) return 0;
    }
    return 1;
}

/* SWAR helpers  -  always compiled so tests can reach row_swar_scan on any arch. */
#define SWAR_ONES  0x0101010101010101ULL
#define SWAR_HIGHS 0x8080808080808080ULL

static int swar_has_match(uint64_t v)
{
    return ((v - SWAR_ONES) & ~v & SWAR_HIGHS) != 0;
}

/* Frequency weights for terminal/log content: lower = rarer = better pivot.
 * Zero entries fall back to a default of 5. */
static const uint8_t FREQ_TABLE[256] = {
    [' ']=150,
    ['e']=127, ['t']=91, ['a']=82, ['o']=75, ['i']=70, ['n']=68, ['s']=63,
    ['r']=60,  ['h']=50, ['l']=40, ['d']=38, ['c']=28, ['u']=27, ['m']=24,
    ['f']=22,  ['p']=19, ['g']=18, ['w']=15, ['y']=15, ['b']=13, ['v']=10,
    ['k']=8,   ['[']=50, [']']=50, [':']=40, ['-']=40, ['.']=30,
    ['x']=2,   ['j']=1,  ['q']=1,  ['z']=1,
};

/* Returns the index of the rarest (lowest-frequency) codepoint in qcps. */
static int get_rarest_idx(const uint32_t *qcps, int qlen)
{
    int     rarest   = 0;
    uint8_t min_freq = 255;
    for (int i = 0; i < qlen; i++) {
        uint8_t f;
        if (qcps[i] > 0xFFu) {
            f = 1;
        } else {
            f = FREQ_TABLE[qcps[i]];
            if (0 == f) f = 5;
        }
        if (f < min_freq) {
            min_freq = f;
            rarest   = i;
        }
    }
    return rarest;
}

/* SWAR scan with rarest-character pivot.
 * Picks the lowest-frequency codepoint in qcps as the anchor, scans for it
 * using SWAR zero-detection, then verifies full matches via cells_match_query.
 * False positives (ch > 0xFF, same low byte) are rejected by the verifier. */
static int row_swar_scan_impl(const Cell *row, int cols,
                               const uint32_t *qcps, int qlen,
                               int pivot_off)
{
    int          offset     = pivot_off;
    uint8_t      lo         = (uint8_t)(qcps[offset] & 0xFFu);
    uint64_t     broadcast  = SWAR_ONES * lo;
    /* anchor_row[j] == row[offset + j]; hit at j -> full match starts at row[j]. */
    const Cell  *anchor_row = row + offset;
    int          anchor_end = cols - qlen + 1; /* exclusive upper bound */
    int i = 0;
    for (; i + 8 <= anchor_end; i += 8) {
        uint64_t word =
              (uint64_t)((uint8_t)anchor_row[i+0].ch)
            | (uint64_t)((uint8_t)anchor_row[i+1].ch) <<  8
            | (uint64_t)((uint8_t)anchor_row[i+2].ch) << 16
            | (uint64_t)((uint8_t)anchor_row[i+3].ch) << 24
            | (uint64_t)((uint8_t)anchor_row[i+4].ch) << 32
            | (uint64_t)((uint8_t)anchor_row[i+5].ch) << 40
            | (uint64_t)((uint8_t)anchor_row[i+6].ch) << 48
            | (uint64_t)((uint8_t)anchor_row[i+7].ch) << 56;
        if (swar_has_match(word ^ broadcast)) {
            for (int k = 0; k < 8; k++) {
                if ((uint8_t)anchor_row[i+k].ch == lo
                    && cells_match_query(&row[i+k], cols-(i+k), qcps, qlen))
                    return 1;
            }
        }
    }
    for (; i < anchor_end; i++) {
        if (anchor_row[i].ch == qcps[offset]
            && cells_match_query(&row[i], cols-i, qcps, qlen))
            return 1;
    }
    return 0;
}

/* Public wrapper  -  computes pivot on behalf of callers (e.g. tests). */
int row_swar_scan(const Cell *row, int cols,
                  const uint32_t *qcps, int qlen)
{
    return row_swar_scan_impl(row, cols, qcps, qlen,
                               get_rarest_idx(qcps, qlen));
}

/* Scan one row for the query.  Returns 1 if found. */
static int row_contains_query(const Cell *row, int cols,
                               const uint32_t *qcps, int qlen,
                               int pivot_off)
{
    if (0 == qlen) return 0;

#if defined(__aarch64__)
    {
        int         offset     = pivot_off;
        uint32_t    pivot      = qcps[offset];
        const Cell *anchor     = row + offset;
        int         anchor_end = cols - qlen + 1;
        uint32x4_t  needle     = vdupq_n_u32(pivot);
        int i = 0;
        for (; i + 4 <= anchor_end; i += 4) {
            uint32x4x4_t fields = vld4q_u32(
                (const uint32_t *)&anchor[i]);
            uint32x4_t cmp = vceqq_u32(fields.val[0], needle);
            if (vmaxvq_u32(cmp)) {
                uint32_t lanes[4];
                vst1q_u32(lanes, cmp);
                for (int k = 0; k < 4; k++) {
                    if (!lanes[k]) continue;
                    int pos = i + k;
                    if (cells_match_query(&row[pos], cols - pos,
                                         qcps, qlen))
                        return 1;
                }
            }
        }
        for (; i < anchor_end; i++) {
            if (anchor[i].ch == pivot
                && cells_match_query(&row[i], cols - i, qcps, qlen))
                return 1;
        }
        return 0;
    }
#elif defined(__x86_64__) && PHANTOM_HAVE_AVX2
    {
        int         offset     = pivot_off;
        uint32_t    pivot      = qcps[offset];
        const Cell *anchor     = row + offset;
        int         anchor_end = cols - qlen + 1;
        __m256i     needle     = _mm256_set1_epi32((int32_t)pivot);
        /* stride 16 bytes (sizeof Cell) = 4 int32 units; scale=4 -> step=16 */
        __m256i idx = _mm256_set_epi32(28, 24, 20, 16, 12, 8, 4, 0);
        int i = 0;
        for (; i + 8 <= anchor_end; i += 8) {
            __m256i gathered = _mm256_i32gather_epi32(
                (const int *)&anchor[i], idx, 4);
            __m256i cmp  = _mm256_cmpeq_epi32(gathered, needle);
            int     mask = _mm256_movemask_ps(_mm256_castsi256_ps(cmp));
            if (mask) {
                for (int k = 0; k < 8; k++) {
                    if (!((mask >> k) & 1)) continue;
                    int pos = i + k;
                    if (cells_match_query(&row[pos], cols - pos,
                                         qcps, qlen))
                        return 1;
                }
            }
        }
        for (; i < anchor_end; i++) {
            if (anchor[i].ch == pivot
                && cells_match_query(&row[i], cols - i, qcps, qlen))
                return 1;
        }
        return 0;
    }
#else
    return row_swar_scan_impl(row, cols, qcps, qlen, pivot_off);
#endif
}

void terminal_search(Terminal *t, const char *query,
                     terminal_search_result_fn cb,
                     terminal_search_done_fn   done_cb,
                     void *arg)
{
    if (!query || '\0' == query[0] || !cb) return;

    /* Decode the query into a codepoint array (stack-local; 256 cps max). */
    uint32_t qcps[256];
    int      qlen    = 0;
    uint64_t qbloom  = 0;
    const char *p    = query;
    uint32_t cp;
    while (qlen < 256 && (cp = utf8_decode_next(&p)) != 0) {
        qcps[qlen++] = cp;
        qbloom |= sb_bloom_bits(cp);
    }
    if (0 == qlen) return;

    terminal_search_cancel(t);

    pthread_mutex_lock(&t->lock);
    t->sb->search_cancel = 0;
    t->search_cb         = cb;
    t->search_arg        = arg;
    int n_chunks = t->sb->chunk_count;
    int cols     = t->sb->cols;
    /* Deep-copy chunk metadata AND compressed data so the search block owns
     * its buffers independently of SbStore.  sb_reset (terminal_resize) can
     * then free the originals without racing the background search thread. */
    SbChunk *snap = NULL;
    if (n_chunks > 0) {
        snap = malloc((size_t)n_chunks * sizeof(SbChunk));
        if (snap) {
            memcpy(snap, t->sb->chunks,
                   (size_t)n_chunks * sizeof(SbChunk));
            for (int i = 0; i < n_chunks; i++) {
                if (!snap[i].data) continue;
                uint8_t *buf = malloc(snap[i].data_sz);
                if (!buf) {
                    for (int j = 0; j < i; j++) free(snap[j].data);
                    free(snap); snap = NULL; break;
                }
                memcpy(buf, snap[i].data, snap[i].data_sz);
                snap[i].data = buf;
            }
        }
    }
    pthread_mutex_unlock(&t->lock);

    if (n_chunks > 0 && !snap) return; /* OOM */

    /* Snapshot the tail and live cell buffer under a single lock acquisition. */
    int      tail_count = 0;
    uint64_t tail_bloom = 0;
    Cell    *tail_snap  = NULL;
    Cell    *live_snap  = NULL;
    int      live_rows  = 0;
    int      live_cols  = 0;
    int      live_base  = 0;
    pthread_mutex_lock(&t->lock);
    tail_count = t->sb->tail_count;
    tail_bloom = t->sb->tail_bloom;
    if (tail_count > 0) {
        size_t tail_sz = (size_t)(tail_count * cols) * sizeof(Cell);
        tail_snap = malloc(tail_sz);
        if (tail_snap) memcpy(tail_snap, t->sb->tail_raw, tail_sz);
    }
    int tail_base = sb_tail_base(t->sb);
    live_rows = t->rows;
    live_cols = t->cols;
    live_base = t->sb->total_rows;  /* abs_row base for live grid rows */
    if (live_rows > 0 && live_cols > 0) {
        size_t live_sz = (size_t)(live_rows * live_cols) * sizeof(Cell);
        live_snap = malloc(live_sz);
        if (live_snap) memcpy(live_snap, t->cells, live_sz);
    }
    pthread_mutex_unlock(&t->lock);

    VT_LOG("search: start query=\"%.48s\" chunks=%d tail=%d live=%d",
           query, n_chunks, tail_count, live_rows);

    /* Again, Gemini was heavily involved in writing this */
#if defined(__APPLE__)
    /* Capture needed state for the block. */
    terminal_search_result_fn bcb  = cb;
    void                     *barg = arg;

    /* Local copies of query data to ensure thread safety and avoid stack corruption. */
    uint32_t *bqcps = malloc((size_t)qlen * sizeof(uint32_t));
    if (!bqcps) {
        free(snap); free(tail_snap); free(live_snap); return;
    }
    memcpy(bqcps, qcps, (size_t)qlen * sizeof(uint32_t));
    int bpivot_off = get_rarest_idx(bqcps, qlen);

    SbChunk *bsnap        = snap;
    Cell    *btail_snap   = tail_snap;
    uint64_t btail_bloom  = tail_bloom;
    uint64_t bqbloom      = qbloom;
    Cell    *blive_snap   = live_snap;
    int      blive_rows   = live_rows;
    int      blive_cols   = live_cols;
    int      blive_base   = live_base;
    SbStore *bs           = t->sb;

    dispatch_group_enter(bs->search_group);
    dispatch_async(
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{

        /* Scan live screen and tail first  -  no decompression, results fast. */
        for (int r = 0; r < blive_rows && !bs->search_cancel; r++) {
            if (blive_snap &&
                row_contains_query(&blive_snap[r * blive_cols],
                                   blive_cols, bqcps, qlen, bpivot_off))
                bcb(blive_base + r, barg);
        }

        if ((btail_bloom & bqbloom) == bqbloom) {
            for (int r = 0; r < tail_count && !bs->search_cancel; r++) {
                if (row_contains_query(&btail_snap[r * cols], cols,
                                       bqcps, qlen, bpivot_off))
                    bcb(tail_base + r, barg);
            }
        }

        /* dispatch_apply fans out chunk scanning across CPU cores. */
        dispatch_apply((size_t)n_chunks,
                       dispatch_get_global_queue(QOS_CLASS_UTILITY, 0),
                       ^(size_t ci) {
            if (bs->search_cancel) return;

            SbChunk *ck      = &bsnap[ci];
            int      ck_cols = ck->cols;

            if ((ck->bloom & qbloom) != qbloom) return;

            size_t raw_sz = (size_t)(ck->row_count * ck_cols) * sizeof(Cell);

            static _Thread_local Cell  *thread_scratch    = NULL;
            static _Thread_local size_t thread_scratch_sz = 0;
            if (raw_sz > thread_scratch_sz) {
                Cell *p = realloc(thread_scratch, raw_sz);
                if (!p) return;
                thread_scratch    = p;
                thread_scratch_sz = raw_sz;
            }

            if (ck->is_raw) {
                memcpy(thread_scratch, ck->data, raw_sz);
            } else {
                compression_decode_buffer(
                    (uint8_t *)thread_scratch, raw_sz,
                    ck->data, ck->data_sz, NULL, COMPRESSION_LZFSE);
            }

            for (int r = 0; r < ck->row_count; r++) {
                if (bs->search_cancel) break;
                if (row_contains_query(&thread_scratch[r * ck_cols], ck_cols,
                                       bqcps, qlen, bpivot_off))
                    bcb(ck->row_base + r, barg);
            }
        });

        /* Final cleanup of captured state. */
        for (int i = 0; i < n_chunks; i++) free(bsnap[i].data);
        free(bsnap);
        free(btail_snap);
        free(blive_snap);
        free(bqcps);
        if (done_cb) done_cb(barg);
        dispatch_group_leave(bs->search_group);
    });
    return; /* block owns all allocations */
#else
    uint32_t *bqcps = malloc((size_t)qlen * sizeof(uint32_t));
    if (!bqcps) {
        free(tail_snap); free(live_snap);
        if (snap) {
            for (int i = 0; i < n_chunks; i++) free(snap[i].data);
            free(snap);
        }
        return;
    }
    memcpy(bqcps, qcps, (size_t)qlen * sizeof(uint32_t));

    CoordArgs *ca = malloc(sizeof *ca);
    if (!ca) {
        free(bqcps); free(tail_snap); free(live_snap);
        if (snap) {
            for (int i = 0; i < n_chunks; i++) free(snap[i].data);
            free(snap);
        }
        return;
    }
    ca->sb         = t->sb;
    ca->snap       = snap;        ca->n_chunks   = n_chunks;
    ca->tail_snap  = tail_snap;   ca->tail_count = tail_count;
    ca->tail_bloom = tail_bloom;  ca->tail_base  = tail_base;
    ca->live_snap  = live_snap;   ca->live_rows  = live_rows;
    ca->live_cols  = live_cols;   ca->live_base  = live_base;
    ca->qcps       = bqcps;       ca->qlen       = qlen;
    ca->pivot_off  = get_rarest_idx(bqcps, qlen);
    ca->qbloom     = qbloom;
    ca->cb         = cb;          ca->done_cb    = done_cb;
    ca->arg        = arg;         ca->cols       = cols;

    pthread_mutex_lock(&t->sb->search_mu);
    t->sb->search_active++;
    pthread_mutex_unlock(&t->sb->search_mu);

    pthread_t coord;
    if (0 != pthread_create(&coord, NULL, search_coord_fn, ca)) {
        pthread_mutex_lock(&t->sb->search_mu);
        t->sb->search_active--;
        pthread_mutex_unlock(&t->sb->search_mu);
        for (int r = 0; r < live_rows && live_snap; r++) {
            if (row_contains_query(&live_snap[r * live_cols], live_cols,
                                   qcps, qlen, ca->pivot_off))
                cb(live_base + r, arg);
        }
        free(bqcps); free(ca); free(tail_snap); free(live_snap);
        if (snap) {
            for (int i = 0; i < n_chunks; i++) free(snap[i].data);
            free(snap);
        }
        return;
    }
    pthread_detach(coord);
#endif
}
