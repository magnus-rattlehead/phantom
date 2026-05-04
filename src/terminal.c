#include "terminal.h"
#include "config.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lz4.h>
#include <stdatomic.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#else
#if defined(__x86_64__) && PHANTOM_HAVE_AVX2
#include <immintrin.h>
#endif
#endif
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#if PHANTOM_DEBUG
#include <stdarg.h>
static void vt_log(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "phantom: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
#define VT_LOG vt_log
#else
static void vt_log_noop(const char *fmt, ...) { (void)fmt; }
#define VT_LOG vt_log_noop
#endif

#define COL_DEFAULT_FG TERM_DEFAULT_FG
#define COL_DEFAULT_BG TERM_DEFAULT_BG

#define CSI_PARAMS_MAX 16 /* max parsed params in a single CSI sequence */
#define TAB_STOP_WIDTH 8  /* columns between tab stops */

/* xterm 256-color palette layout: 0-15 = ANSI, 16-231 = 6x6x6 RGB cube,
 * 232-255 = grayscale ramp. */
#define ANSI_COLOR_COUNT 16 /* first N entries are the named ANSI colors */
#define GRAYSCALE_START 232 /* index where the grayscale ramp begins */
#define COLOR_CUBE_DIM 6    /* one axis of the 6x6x6 RGB cube */
#define COLOR_CUBE_BASE 55  /* minimum nonzero component value in the cube */
#define COLOR_CUBE_STEP 40  /* component value increment per cube level */
#define GRAYSCALE_BASE 8    /* darkest gray component value */
#define GRAYSCALE_STEP 10   /* component increment between grayscale levels */

#define RGBA_ALPHA_OPAQUE 0xFF

/* UTF-8 byte classification thresholds and payload masks */
#define UTF8_CONT_BYTE_MIN 0x80   /* lower bound for continuation bytes */
#define UTF8_2BYTE_LEAD_MIN 0xC0  /* lower bound for 2-byte sequence lead */
#define UTF8_3BYTE_LEAD_MIN 0xE0  /* lower bound for 3-byte sequence lead */
#define UTF8_4BYTE_LEAD_MIN 0xF0  /* lower bound for 4-byte sequence lead */
#define UTF8_4BYTE_DATA_MASK 0x07 /* payload bits in a 4-byte lead byte */
#define UTF8_3BYTE_DATA_MASK 0x0F /* payload bits in a 3-byte lead byte */
#define UTF8_2BYTE_DATA_MASK 0x1F /* payload bits in a 2-byte lead byte */
#define UTF8_CONT_DATA_MASK 0x3F  /* payload bits in a continuation byte */

/* Codepoint thresholds for UTF-8 sequence length (used in encoding) */
#define UTF8_2BYTE_LIMIT 0x800u   /* codepoints >= this need 2+ bytes */
#define UTF8_3BYTE_LIMIT 0x10000u /* codepoints >= this need 3+ bytes */
#define UTF8_MAX_BYTES 4

/* ASCII / CSI byte range constants */
#define ASCII_PRINTABLE_FIRST 0x20
#define ASCII_PRINTABLE_LAST 0x7E
#define DEC_SPECIAL_FIRST 0x60 /* first DEC special-graphics char (`) */
#define CSI_FINAL_FIRST 0x40
#define CSI_FINAL_LAST 0x7E
#define CSI_BUF_SIZE 64
#define OSC_BUF_SIZE 4096

/* ASCII control character codes */
#define ASCII_BEL '\x07'
#define ASCII_BS '\b'
#define ASCII_HT '\t'
#define ASCII_LF '\n'
#define ASCII_VT '\v'
#define ASCII_FF '\f'
#define ASCII_CR '\r'
#define ASCII_ESC '\x1b'

/* SGR (Select Graphic Rendition) parameter codes */
#define SGR_RESET 0
#define SGR_BOLD 1
#define SGR_ITALIC 3
#define SGR_UNDERLINE 4
#define SGR_REVERSE 7
#define SGR_BOLD_OFF 22
#define SGR_ITALIC_OFF 23
#define SGR_UNDERLINE_OFF 24
#define SGR_REVERSE_OFF 27
#define SGR_FG_FIRST 30
#define SGR_FG_LAST 37
#define SGR_FG_EXT 38 /* extended FG color (256/RGB subcommand) */
#define SGR_FG_DEFAULT 39
#define SGR_BG_FIRST 40
#define SGR_BG_LAST 47
#define SGR_BG_EXT 48 /* extended BG color (256/RGB subcommand) */
#define SGR_BG_DEFAULT 49
#define SGR_FG_BRIGHT_FIRST 90
#define SGR_FG_BRIGHT_LAST 97
#define SGR_BG_BRIGHT_FIRST 100
#define SGR_BG_BRIGHT_LAST 107
#define SGR_COLOR256 5       /* subcommand: 256-color palette index follows */
#define SGR_COLORRGB 2       /* subcommand: R;G;B components follow */
#define ANSI_BRIGHT_OFFSET 8 /* bright colors start at palette index 8 */

/* DECPM (DEC private mode) numbers used in ?h / ?l sequences */
#define DECPM_DECCKM 1
#define DECPM_CURSOR_VISIBLE 25
#define DECPM_ALT_SCREEN 47
#define DECPM_ALT_SCREEN_2 1047
#define DECPM_ALT_SCREEN_3 1049
#define DECPM_BRACKETED_PASTE 2004

#define CHARSET_DEC_SPECIAL '0'

/* Standard 16 ANSI colors (RGBA) */
static const uint32_t ANSI_COLORS[16] = {
    COLOR_0,  COLOR_1,  COLOR_2,  COLOR_3,  COLOR_4,  COLOR_5,
    COLOR_6,  COLOR_7,  COLOR_8,  COLOR_9,  COLOR_10, COLOR_11,
    COLOR_12, COLOR_13, COLOR_14, COLOR_15,
};

/* VT100 DEC Special Graphics: codepoints for 0x60..0x7e */
static const uint32_t DEC_SPECIAL[31] = {
    0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1,
    0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA,
    0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, 0x252C,
    0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,
};

/* Ref-counted compressed data buffer shared between chunks[] and search
 * snapshots; avoids deep-copying chunk data on the main thread at search
 * start. */
typedef struct {
    uint8_t *data;
    size_t data_sz;
    _Atomic int refs;
} SbChunkBuf;

static void sb_chunk_buf_release(SbChunkBuf *buf) {
    if (!buf)
        return;
    if (1 == atomic_fetch_sub_explicit(&buf->refs, 1, memory_order_acq_rel)) {
        free(buf->data);
        free(buf);
    }
}

typedef struct {
    int row_base;       /* absolute scrollback row of first row */
    int row_count;      /* rows currently valid (may be trimmed) */
    int orig_row_count; /* row_count at compression time; never changes */
    int cols;           /* column width when these rows were captured */
    uint64_t wrap_bits; /* bit i = 1 if row i is a soft-wrap continuation */
    SbChunkBuf *buf;    /* ref-counted compressed Cell data */
    uint64_t bloom;
    uint8_t is_raw; /* 1 = buf->data is raw (no compression gain) */
} SbChunk;

#define COMP_POOL_N 4 /* max parallel compression workers */

/* tracks in-progress scrollback compression job owned by worker */
typedef struct {
    Cell *cells;   /* pointer to raw cell array the worker is currently
                      compressing */
    int row_base;  /* absolute scrollback row index where this chunk starts */
    int row_count; /* number of rows in chunk */
    int cols; /* column width at dispatch time, terminal can resize mid-flight;
                 worker must compress with the cols that were valid when the
                 data was captured */
    uint64_t bloom;     /* 64-bit bloom filter over codepoints in these rows.
                           Pre-computed before dispatch so search can skip entire
                           chunks */
    uint64_t wrap_bits; /* bit i = 1 if row i in this chunk is a soft-wrap
                           continuation of the preceding row */
    int epoch;  /* value of clear_epoch at dispatch time. If clear_epoch has
                   advanced by the time the worker finishes, the terminal was
                   cleared mid-flight; result is stale */
    int in_use; /* 1 = worker holds this slot */
} CompInflight;

/* a search request to be consumed by a search worker */
#if !defined(__APPLE__)
typedef struct {
    SbChunk *snap;  /* snapshot of compressed scrollback chunk array at search
                       start */
    int n_chunks;   /* number of chunks in snap */
    uint32_t *qcps; /* query codepoints array; search string pre-decoded to
                       UTF32; workers compare against this rather than
                       re-decoding bytes seach time */
    int qlen; /* number of codepoints in qcps; length of query in characters */
    int pivot_off;   /* byte offset within a chunk where the search should
                        pivot/search; allows resuming mid-chunk */
    uint64_t qbloom; /* bloom filter of query's codepoints */
    terminal_search_result_fn cb; /* callback to invoke for each matched row */
    void *arg;                    /* SearchState passed through to callback */
    volatile int
        *cancel; /* pointer to SbStore.search_cancel; worker polls this each
                    chunk; main thread can write it without holding a lock */
    int q_next;  /* index of next chunk to hand out; workers atomically claim
                    chunks by incrementing */
    int pending; /* count of workers that haven't finished yet */
    int cols;    /* column width at search dispatch time (for re-wrapping) */
} PoolJob;

/* search thread pool */
typedef struct SearchPool {
    pthread_t *threads;        /* heap array of pthread handles */
    int n_threads;             /* length of threads */
    pthread_mutex_t mu;        /* mutex protecting everything in this struct and
                                  job.q_next/pending */
    pthread_cond_t work_ready; /* condition variable main thread signals when a
                                  new job is loaded */
    pthread_cond_t work_done;  /* condition variable last worker signals when
                                  job.pending hits zero */
    int shutdown; /* workers check this after waking on work_ready: 1 = exit */
    PoolJob job;  /* the single PoolJob all workers share */
} SearchPool;
#endif

/* slot in decompressed chunk LRU cache */
typedef struct {
    Cell *cells;       /* decompressed cells; NULL = empty */
    size_t cells_cap;  /* byte capacity of cells buffer */
    int chunk_idx;     /* which chunk in SbStore.chunks this slot holds;
                          -1 = empty */
    int slot_cols;     /* ck->cols when cached */
    uint64_t lru_tick; /* timestamp from SbStore.lru_tick counter */
} SbCacheSlot;

typedef struct {
    Cell *cells;   /* malloc'd: row_count * cols cells; NULL = empty slot */
    int row_base;  /* absolute scrollback row index of the first row in this
                      chunk */
    int row_count; /* number of rows in cells */
    int cols; /* column width when these cells were captured; resize can happen
                 between en/dequeue */
    uint64_t bloom;     /* bloom filter */
    uint64_t wrap_bits; /* bit i = 1 if row i is a soft-wrap continuation */
} SbRawChunk;

#define SB_RAW_QUEUE_CAP 64

typedef struct {
    SbChunk *chunks; /* array of compressed scrollback chunks */
    int chunk_count; /* count of chunks */
    int chunk_cap;   /* allocated capacity of chunks */
    Cell *tail_raw;  /* SB_CHUNK_ROWS * cols cells (uncompressed) */
    int tail_count;  /* how many rows are currently in tail_raw */
    uint64_t
        tail_bloom; /* bloom filter accumulating codepoints seen in tail_raw */
    uint64_t tail_wrap_bits; /* bit i = 1 if tail row i is a soft-wrap
                                continuation */
    int total_rows;          /* tail rows included */
    int cols;                /* current terminal column width */
    SbCacheSlot cache[SB_DECOMP_CACHE]; /* LRU cache of recently decompressed
                                           chunks; avoids re-decompressing the
                                           same chunk on every render frame */
    uint64_t lru_tick; /* monotonically incrementing couonter; bumped on cache
                          access to drive LRU eviction */
    volatile int search_cancel; /* 1 = abort in-flight search */
#if defined(__APPLE__)
    dispatch_group_t search_group; /* GCD dispatch group; block enters on start,
                                      leaves on finish */
#else
    SearchPool *pool; /* thread pool owning worker threads */
    /* mutex + condition variable + flag for main thread to wait on search
     * completion */
    pthread_mutex_t search_mu;
    pthread_cond_t search_cond;
    int search_active;
#endif
    SbRawChunk *raw_queue;   /* work queue; SB_RAW_QUEUE_CAP entries; comp_mu */
    int rq_head;             /* index of oldest occupied entry */
    int rq_count;            /* number of occupied entries */
    pthread_mutex_t comp_mu; /* protects rq_*, inflight, dispatch/commit seqs */
    pthread_cond_t comp_need;    /* signaled when job enqueued or slot freed */
    pthread_cond_t commit_ready; /* signaled when commit_seq advances */
    int comp_shutdown;           /* set to 1 before joining workers */
    int clear_epoch;             /* incremented by sb_clear; comp_mu protects */
    uint64_t dispatch_seq;       /* next seq to hand out (comp_mu) */
    uint64_t commit_seq;         /* next seq to commit (comp_mu) */
    CompInflight inflight[COMP_POOL_N]; /* one slot per worker; comp_mu */
    pthread_t workers[COMP_POOL_N];     /* pthread_t handles for compresion
                                           background threads */
    int n_workers; /* number of active compression workers */
    pthread_rwlock_t
        chunk_mu;    /* guards chunks[]; rdlock=read, wrlock=write/append */
    Cell *tail_pool; /* recycled tail buffer (1 slot); NULL if empty; comp_mu */
    int tail_pool_cols; /* cols dimension of tail_pool */
} SbStore;

/* argument bundle passed to search coordinator thread at search start */
#if !defined(__APPLE__)
typedef struct {
    SbStore *
        sb; /* SbStore; coordinator accesses SearchPool and signal completion */
    SbChunk
        *snap; /* snapshot of chunks taken at search start; handed to PoolJob */
    int n_chunks;        /* length of snapshot */
    Cell *tail_snap;     /* copy of tail_raw cells taken at search start */
    int tail_count;      /* number of rows in tail_snap */
    uint64_t tail_bloom; /* bloom filter of tail_snap */
    int tail_base;   /* absolute row index of the first row in tail_snap, passed
                        to callback when tail hit is found */
    Cell *live_snap; /* copy of terminal's active screen cells, searched last */
    int live_rows;   /* number of rows in live snapshot */
    int live_cols;   /* number of columns in live snapshot */
    int live_base;   /* absolute row index of the first live screen row */
    uint32_t *qcps;  /* query decoded to UTF32 codepoints */
    int qlen;        /* codepoint count of query */
    int pivot_off; /* byte offset within chunks where scanning starts; supports
                      mid chunk resume */
    uint64_t qbloom;              /* bloom filter of query codepoints */
    terminal_search_result_fn cb; /* result callback; fired on matching row */
    terminal_search_done_fn
        done_cb; /* completion callback; fired once all regions searched */
    void *arg;   /* pointer passed to callbacks, points to SearchState */
    int cols;    /* column width at dispatch time */
    SbRawChunk *rq_snap; /* snapshot of raw queue entires (queued for
                            compression but not yet compressed), searched before
                            compression completes */
    int rq_snap_count;   /* number of chunks in rq_snap */
} CoordArgs;

static int row_contains_query(const Cell *row, int cols, const uint32_t *qcps,
                              int qlen, int pivot_off);

static void *search_worker_fn(void *arg) {
    SearchPool *pool = (SearchPool *)arg;

    /* Per-thread scratch buffer reused across jobs; avoids realloc when next
     * chunk fits. */
    static _Thread_local Cell *scratch = NULL;
    static _Thread_local size_t scratch_sz = 0;

    for (;;) {
        pthread_mutex_lock(&pool->mu);
        while (!pool->shutdown && pool->job.q_next >= pool->job.n_chunks)
            pthread_cond_wait(&pool->work_ready, &pool->mu);
        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->mu);
            break;
        }

        int ci = pool->job.q_next++;
        SbChunk *ck = &pool->job.snap[ci];
        /* Copy job params locally before releasing lock. */
        uint32_t *qcps = pool->job.qcps;
        int qlen = pool->job.qlen;
        int pivot_off = pool->job.pivot_off;
        uint64_t qbloom = pool->job.qbloom;
        terminal_search_result_fn cb = pool->job.cb;
        void *cb_arg = pool->job.arg;
        volatile int *cancel = pool->job.cancel;
        pthread_mutex_unlock(&pool->mu);

        /* Skip if cancelled or bloom filter rules out any match. */
        if (*cancel || (ck->bloom & qbloom) != qbloom) {
            pthread_mutex_lock(&pool->mu);
            if (--pool->job.pending == 0)
                pthread_cond_signal(&pool->work_done);
            pthread_mutex_unlock(&pool->mu);
            continue;
        }

        size_t raw_sz = (size_t)(ck->row_count * ck->cols) * sizeof(Cell);

        if (raw_sz > scratch_sz) {
            Cell *p = realloc(scratch, raw_sz > 0 ? raw_sz : 1);
            if (!p) {
                pthread_mutex_lock(&pool->mu);
                if (--pool->job.pending == 0)
                    pthread_cond_signal(&pool->work_done);
                pthread_mutex_unlock(&pool->mu);
                continue;
            }
            scratch = p;
            scratch_sz = raw_sz;
        }

        if (ck->is_raw) {
            memcpy(scratch, ck->buf->data, raw_sz);
        } else {
            LZ4_decompress_safe((const char *)ck->buf->data, (char *)scratch,
                                (int)ck->buf->data_sz, (int)raw_sz);
        }

        for (int r = 0; r < ck->row_count && !*cancel; r++) {
            if (row_contains_query(scratch + (size_t)r * ck->cols, ck->cols,
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

static SearchPool *search_pool_create(void) {
    int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1)
        n = 1;

    SearchPool *pool = calloc(1, sizeof *pool);
    if (!pool)
        return NULL;

    pool->threads = malloc((size_t)n * sizeof(pthread_t));
    if (!pool->threads) {
        free(pool);
        return NULL;
    }

    pthread_mutex_init(&pool->mu, NULL);
    pthread_cond_init(&pool->work_ready, NULL);
    pthread_cond_init(&pool->work_done, NULL);

    for (int i = 0; i < n; i++) {
        if (0 !=
            pthread_create(&pool->threads[i], NULL, search_worker_fn, pool))
            break;
        pool->n_threads++;
    }
    return pool;
}

static void search_pool_destroy(SearchPool *pool) {
    if (!pool)
        return;
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

static void *search_coord_fn(void *arg) {
    CoordArgs *a = (CoordArgs *)arg;
    SearchPool *pool = a->sb->pool;

    /* Live screen and tail have no decompression cost; scan first. */
    for (int r = 0; r < a->live_rows && !a->sb->search_cancel; r++) {
        if (a->live_snap &&
            row_contains_query(&a->live_snap[r * a->live_cols], a->live_cols,
                               a->qcps, a->qlen, a->pivot_off))
            a->cb(a->live_base + r, a->arg);
    }

    if ((a->tail_bloom & a->qbloom) == a->qbloom) {
        for (int r = 0; r < a->tail_count && !a->sb->search_cancel; r++) {
            if (row_contains_query(&a->tail_snap[r * a->cols], a->cols, a->qcps,
                                   a->qlen, a->pivot_off))
                a->cb(a->tail_base + r, a->arg);
        }
    }

    /* raw_queue entries: flushed from tail but not yet in chunks[]. */
    for (int qi = 0; qi < a->rq_snap_count && !a->sb->search_cancel; qi++) {
        SbRawChunk *rq = &a->rq_snap[qi];
        if (!rq->cells || (rq->bloom & a->qbloom) != a->qbloom)
            continue;
        for (int r = 0; r < rq->row_count && !a->sb->search_cancel; r++) {
            if (row_contains_query(&rq->cells[r * rq->cols], rq->cols, a->qcps,
                                   a->qlen, a->pivot_off))
                a->cb(rq->row_base + r, a->arg);
        }
    }

    if (a->n_chunks > 0) {
        if (pool) {
            pthread_mutex_lock(&pool->mu);
            pool->job.snap = a->snap;
            pool->job.n_chunks = a->n_chunks;
            pool->job.qcps = a->qcps;
            pool->job.qlen = a->qlen;
            pool->job.pivot_off = a->pivot_off;
            pool->job.qbloom = a->qbloom;
            pool->job.cb = a->cb;
            pool->job.arg = a->arg;
            pool->job.cancel = &a->sb->search_cancel;
            pool->job.q_next = 0;
            pool->job.pending = a->n_chunks;
            pool->job.cols = a->cols;
            pthread_cond_broadcast(&pool->work_ready);
            while (pool->job.pending > 0)
                pthread_cond_wait(&pool->work_done, &pool->mu);
            pthread_mutex_unlock(&pool->mu);
        } else {
            /* Pool unavailable; serial fallback. */
            for (int ci = 0; ci < a->n_chunks && !a->sb->search_cancel; ci++) {
                SbChunk *ck = &a->snap[ci];
                if ((ck->bloom & a->qbloom) != a->qbloom)
                    continue;
                size_t raw_sz =
                    (size_t)(ck->row_count * ck->cols) * sizeof(Cell);
                Cell *buf = malloc(raw_sz > 0 ? raw_sz : 1);
                if (!buf)
                    continue;
                if (ck->is_raw) {
                    memcpy(buf, ck->buf->data, raw_sz);
                } else {
                    LZ4_decompress_safe((const char *)ck->buf->data,
                                        (char *)buf, (int)ck->buf->data_sz,
                                        (int)raw_sz);
                }
                for (int r = 0; r < ck->row_count && !a->sb->search_cancel;
                     r++) {
                    if (row_contains_query(buf + (size_t)r * ck->cols, ck->cols,
                                           a->qcps, a->qlen, a->pivot_off))
                        a->cb(ck->row_base + r, a->arg);
                }
                free(buf);
            }
        }
    }

    for (int i = 0; i < a->n_chunks; i++)
        sb_chunk_buf_release(a->snap[i].buf);
    free(a->snap);
    free(a->tail_snap);
    free(a->live_snap);
    for (int i = 0; i < a->rq_snap_count; i++)
        free(a->rq_snap[i].cells);
    free(a->rq_snap);
    free(a->qcps);

    if (a->done_cb)
        a->done_cb(a->arg);

    pthread_mutex_lock(&a->sb->search_mu);
    a->sb->search_active--;
    pthread_cond_signal(&a->sb->search_cond);
    pthread_mutex_unlock(&a->sb->search_mu);

    free(a);
    return NULL;
}
#endif /* !defined(__APPLE__) */

/* Absolute row index of the first row in the uncompressed tail buffer. */
static inline int sb_tail_base(const SbStore *s) {
    return s->total_rows - s->tail_count;
}

#define SB_INITIAL_CHUNK_CAP 16

/* k=2 bloom: derive two independent 6-bit indices from one 64-bit multiply.
 * Uses the Fibonacci (golden-ratio) constant for good bit distribution. */
static uint64_t sb_bloom_bits(uint32_t cp) {
    uint64_t h = (uint64_t)cp * 0x9E3779B97F4A7C15ULL;
    return ((uint64_t)1 << (h >> 58)) | ((uint64_t)1 << ((h >> 52) & 63u));
}

typedef struct {
    SbStore *s;
    int slot;
} WorkerArg;
static void *sb_worker_thread(void *arg);

static SbStore *sb_create(int cols) {
    SbStore *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->chunks = malloc((size_t)SB_INITIAL_CHUNK_CAP * sizeof(SbChunk));
    if (!s->chunks) {
        free(s);
        return NULL;
    }
    s->chunk_cap = SB_INITIAL_CHUNK_CAP;
    s->tail_raw = malloc((size_t)(SB_CHUNK_ROWS * cols) * sizeof(Cell));
    if (!s->tail_raw) {
        free(s->chunks);
        free(s);
        return NULL;
    }
    s->raw_queue = calloc(SB_RAW_QUEUE_CAP, sizeof(SbRawChunk));
    if (!s->raw_queue) {
        free(s->tail_raw);
        free(s->chunks);
        free(s);
        return NULL;
    }
    s->cols = cols;
    for (int i = 0; i < SB_DECOMP_CACHE; i++)
        s->cache[i].chunk_idx = -1;
    pthread_mutex_init(&s->comp_mu, NULL);
    pthread_cond_init(&s->comp_need, NULL);
    pthread_cond_init(&s->commit_ready, NULL);
    pthread_rwlock_init(&s->chunk_mu, NULL);

    int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1)
        ncpu = 1;
    int nw = ncpu / 2;
    if (nw < 1)
        nw = 1;
    if (nw > COMP_POOL_N)
        nw = COMP_POOL_N;
    s->n_workers = nw;
    for (int i = 0; i < nw; i++) {
        WorkerArg *a = malloc(sizeof *a);
        if (!a) {
            s->n_workers = i;
            break;
        }
        a->s = s;
        a->slot = i;
#if defined(__APPLE__)
        {
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
            pthread_create(&s->workers[i], &attr, sb_worker_thread, a);
            pthread_attr_destroy(&attr);
        }
#else
        pthread_create(&s->workers[i], NULL, sb_worker_thread, a);
#endif
    }

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

/* Clear all scrollback state. Safe to call while workers are compressing;
 * clear_epoch causes in-flight workers to discard their results. */
static void sb_clear(SbStore *s) {
    pthread_mutex_lock(&s->comp_mu);
    s->clear_epoch++;
    for (int i = 0; i < s->rq_count; i++) {
        int idx = (s->rq_head + i) % SB_RAW_QUEUE_CAP;
        free(s->raw_queue[idx].cells);
        s->raw_queue[idx].cells = NULL;
    }
    s->rq_count = 0;
    s->rq_head = 0;
    /* Inflight workers detect the epoch change and discard results. */
    pthread_mutex_unlock(&s->comp_mu);

    pthread_rwlock_wrlock(&s->chunk_mu);
    for (int i = 0; i < s->chunk_count; i++)
        sb_chunk_buf_release(s->chunks[i].buf);
    s->chunk_count = 0;
    pthread_rwlock_unlock(&s->chunk_mu);

    for (int i = 0; i < SB_DECOMP_CACHE; i++) {
        free(s->cache[i].cells);
        s->cache[i].cells = NULL;
        s->cache[i].chunk_idx = -1;
    }
    s->lru_tick = 0;
    s->tail_count = 0;
    s->tail_bloom = 0;
    s->total_rows = 0;
}

static void sb_destroy(SbStore *s) {
    if (!s)
        return;
    pthread_mutex_lock(&s->comp_mu);
    s->comp_shutdown = 1;
    pthread_cond_broadcast(&s->comp_need);
    pthread_cond_broadcast(&s->commit_ready);
    pthread_mutex_unlock(&s->comp_mu);
    for (int i = 0; i < s->n_workers; i++)
        pthread_join(s->workers[i], NULL);
    /* Queue should be empty after join, but free defensively. */
    for (int i = 0; i < s->rq_count; i++) {
        int idx = (s->rq_head + i) % SB_RAW_QUEUE_CAP;
        free(s->raw_queue[idx].cells);
    }
    free(s->raw_queue);
    pthread_mutex_destroy(&s->comp_mu);
    pthread_cond_destroy(&s->comp_need);
    pthread_cond_destroy(&s->commit_ready);
    pthread_rwlock_destroy(&s->chunk_mu);
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
    for (int i = 0; i < s->chunk_count; i++)
        sb_chunk_buf_release(s->chunks[i].buf);
    free(s->chunks);
    free(s->tail_raw);
    free(s->tail_pool);
    for (int i = 0; i < SB_DECOMP_CACHE; i++)
        free(s->cache[i].cells);
    free(s);
}

/* Compress cells into a newly malloc'd buffer; fall back to raw copy if
 * compression does not shrink the data.  Sets *out_comp to the allocated
 * buffer (caller owns it), *out_comp_sz to its byte length, and
 * *out_is_raw to 1 when raw fallback was used.  Returns without setting
 * *out_comp on OOM (caller must check for NULL). */
static void sb_lz4_compress(const Cell *cells, int raw_sz, uint8_t **out_comp,
                            int *out_comp_sz, int *out_is_raw) {
    int max_dst = LZ4_compressBound(raw_sz);
    uint8_t *comp = malloc((size_t)max_dst);
    if (!comp) {
        *out_comp = NULL;
        return;
    }
    int n = LZ4_compress_default((const char *)cells, (char *)comp, raw_sz,
                                 max_dst);
    if (n > 0 && (size_t)n < (size_t)raw_sz) {
        *out_comp = comp;
        *out_comp_sz = n;
        *out_is_raw = 0;
    } else {
        free(comp);
        comp = malloc((size_t)raw_sz > 0 ? (size_t)raw_sz : 1);
        if (!comp) {
            *out_comp = NULL;
            return;
        }
        memcpy(comp, cells, (size_t)raw_sz);
        *out_comp = comp;
        *out_comp_sz = raw_sz;
        *out_is_raw = 1;
    }
}

/* Inline fallback: LZ4-compress n_rows rows and commit to chunks[].
 * Used only when the async queue is saturated (extremely rare).
 * Caller must NOT hold chunk_mu. Returns 1 on success, 0 on OOM. */
static int sb_compress_and_commit(SbStore *s, const Cell *cells_src, int n_rows,
                                  int cols, uint64_t wrap_bits, uint64_t bloom,
                                  int row_base) {
    int raw_sz = (int)((size_t)(n_rows * cols) * sizeof(Cell));
    uint8_t *comp = NULL;
    int comp_sz = 0;
    int is_raw = 0;

    sb_lz4_compress(cells_src, raw_sz, &comp, &comp_sz, &is_raw);
    if (!comp)
        return 0;

    pthread_rwlock_wrlock(&s->chunk_mu);
    if (s->chunk_count == s->chunk_cap) {
        int new_cap = s->chunk_cap * 2;
        SbChunk *nb = realloc(s->chunks, (size_t)new_cap * sizeof(SbChunk));
        if (!nb) {
            free(comp);
            pthread_rwlock_unlock(&s->chunk_mu);
            return 0;
        }
        s->chunks = nb;
        s->chunk_cap = new_cap;
    }
    SbChunkBuf *cbuf = malloc(sizeof *cbuf);
    if (!cbuf) {
        free(comp);
        pthread_rwlock_unlock(&s->chunk_mu);
        return 0;
    }
    atomic_init(&cbuf->refs, 1);
    cbuf->data = comp;
    cbuf->data_sz = (size_t)comp_sz;
    SbChunk *ck = &s->chunks[s->chunk_count++];
    ck->row_base = row_base;
    ck->row_count = n_rows;
    ck->orig_row_count = n_rows;
    ck->cols = cols;
    ck->wrap_bits = wrap_bits;
    ck->buf = cbuf;
    ck->bloom = bloom;
    ck->is_raw = is_raw;
    pthread_rwlock_unlock(&s->chunk_mu);
    return 1;
}

/* Compression worker: dequeues jobs, LZ4-compresses them in parallel,
 * then commits to chunks[] in dispatch order (via commit_seq). */
static void *sb_worker_thread(void *arg) {
    WorkerArg *wa = (WorkerArg *)arg;
    SbStore *s = wa->s;
    int slot = wa->slot;
    free(wa);

    for (;;) {
        pthread_mutex_lock(&s->comp_mu);
        while (!s->comp_shutdown && 0 == s->rq_count)
            pthread_cond_wait(&s->comp_need, &s->comp_mu);
        if (s->comp_shutdown && 0 == s->rq_count) {
            pthread_mutex_unlock(&s->comp_mu);
            break;
        }

        SbRawChunk entry = s->raw_queue[s->rq_head];
        s->rq_head = (s->rq_head + 1) % SB_RAW_QUEUE_CAP;
        s->rq_count--;
        if (s->rq_count > 0)
            pthread_cond_signal(&s->comp_need);

        uint64_t my_seq = s->dispatch_seq++;
        int my_epoch = s->clear_epoch;

        /* Expose cells in inflight so sb_get_row_into can read them. */
        s->inflight[slot].cells = entry.cells;
        s->inflight[slot].row_base = entry.row_base;
        s->inflight[slot].row_count = entry.row_count;
        s->inflight[slot].cols = entry.cols;
        s->inflight[slot].bloom = entry.bloom;
        s->inflight[slot].wrap_bits = entry.wrap_bits;
        s->inflight[slot].epoch = my_epoch;
        s->inflight[slot].in_use = 1;
        pthread_mutex_unlock(&s->comp_mu);

        int raw_sz =
            (int)((size_t)(entry.row_count * entry.cols) * sizeof(Cell));
        uint8_t *comp = NULL;
        int comp_sz = 0;
        int is_raw = 0;
        sb_lz4_compress(entry.cells, raw_sz, &comp, &comp_sz, &is_raw);

        /* Wait for our turn to commit (preserve chunk ordering). */
        pthread_mutex_lock(&s->comp_mu);
        while (s->commit_seq != my_seq && !s->comp_shutdown)
            pthread_cond_wait(&s->commit_ready, &s->comp_mu);

        int was_cleared = (s->clear_epoch != my_epoch);

        /* Return buffer to pool instead of freeing; avoids malloc in
         * sb_enqueue_tail every 64 rows. Only pool if cols still match. */
        if (!s->tail_pool && s->inflight[slot].cols == s->cols) {
            s->tail_pool = s->inflight[slot].cells;
            s->tail_pool_cols = s->cols;
        } else {
            free(s->inflight[slot].cells);
        }
        s->inflight[slot].cells = NULL;
        s->inflight[slot].in_use = 0;
        s->commit_seq++;
        pthread_cond_broadcast(&s->commit_ready);
        pthread_mutex_unlock(&s->comp_mu);

        if (was_cleared || !comp) {
            free(comp);
            continue;
        }

        pthread_rwlock_wrlock(&s->chunk_mu);
        if (s->chunk_count == s->chunk_cap) {
            int nc = s->chunk_cap * 2;
            SbChunk *nb = realloc(s->chunks, (size_t)nc * sizeof(SbChunk));
            if (!nb) {
                free(comp);
                pthread_rwlock_unlock(&s->chunk_mu);
                continue;
            }
            s->chunks = nb;
            s->chunk_cap = nc;
        }
        SbChunkBuf *cbuf2 = malloc(sizeof *cbuf2);
        if (!cbuf2) {
            free(comp);
            pthread_rwlock_unlock(&s->chunk_mu);
            continue;
        }
        atomic_init(&cbuf2->refs, 1);
        cbuf2->data = comp;
        cbuf2->data_sz = (size_t)comp_sz;
        SbChunk *ck = &s->chunks[s->chunk_count++];
        ck->row_base = entry.row_base;
        ck->row_count = entry.row_count;
        ck->orig_row_count = entry.row_count;
        ck->cols = entry.cols;
        ck->wrap_bits = entry.wrap_bits;
        ck->buf = cbuf2;
        ck->bloom = entry.bloom;
        ck->is_raw = is_raw;
        VT_LOG("comp: committed chunk row_base=%d rows=%d "
               "raw=%zu comp=%d ratio=%.2f is_raw=%d chunks=%d",
               entry.row_base, entry.row_count,
               (size_t)(entry.row_count * entry.cols) * sizeof(Cell), comp_sz,
               comp_sz > 0 ? (float)((size_t)(entry.row_count * entry.cols) *
                                     sizeof(Cell)) /
                                 (float)comp_sz
                           : 0.0f,
               is_raw, s->chunk_count);
        pthread_rwlock_unlock(&s->chunk_mu);
    }
    return NULL;
}

/* Inline fallback: compress the current tail synchronously.
 * Used only when the async queue is saturated (extremely rare).
 * Caller holds t->lock throughout; chunk_mu is acquired internally. */
static void sb_finalize_chunk_inline(SbStore *s) {
    if (0 == s->tail_count)
        return;
    int row_base = sb_tail_base(s);
    int rows = s->tail_count;
    uint64_t bloom = s->tail_bloom;
    uint64_t wrap_bits = s->tail_wrap_bits;
    int cols = s->cols;
    s->tail_count = 0;
    s->tail_bloom = 0;
    s->tail_wrap_bits = 0;
    sb_compress_and_commit(s, s->tail_raw, rows, cols, wrap_bits, bloom,
                           row_base);
}

/* Enqueue the current tail as a raw (uncompressed) chunk for the background
 * compression thread.  Falls back to inline compression if the queue is full.
 * Caller holds t->lock. */
static void sb_enqueue_tail(SbStore *s) {
    if (0 == s->tail_count)
        return;

    pthread_mutex_lock(&s->comp_mu);

    if (s->rq_count >= SB_RAW_QUEUE_CAP) {
        pthread_mutex_unlock(&s->comp_mu);
        sb_finalize_chunk_inline(s);
        return;
    }

    /* Hot path: take replacement buffer from pool (no malloc). */
    Cell *new_raw = NULL;
    if (s->tail_pool && s->tail_pool_cols == s->cols) {
        new_raw = s->tail_pool;
        s->tail_pool = NULL;
    }

    pthread_mutex_unlock(&s->comp_mu);

    /* Cold path: pool miss - allocate outside the lock. */
    if (!new_raw) {
        new_raw = malloc((size_t)(SB_CHUNK_ROWS * s->cols) * sizeof(Cell));
        if (!new_raw) {
            s->tail_count = 0;
            s->tail_bloom = 0;
            s->tail_wrap_bits = 0;
            return;
        }
        /* Re-acquire lock; re-check queue capacity. */
        pthread_mutex_lock(&s->comp_mu);
        if (s->rq_count >= SB_RAW_QUEUE_CAP) {
            /* Stash the just-allocated buffer for next time. */
            if (!s->tail_pool) {
                s->tail_pool = new_raw;
                s->tail_pool_cols = s->cols;
            } else {
                free(new_raw);
            }
            pthread_mutex_unlock(&s->comp_mu);
            sb_finalize_chunk_inline(s);
            return;
        }
        pthread_mutex_unlock(&s->comp_mu);
    }

    pthread_mutex_lock(&s->comp_mu);
    int slot = (s->rq_head + s->rq_count) % SB_RAW_QUEUE_CAP;
    s->raw_queue[slot].cells = s->tail_raw; /* transfer ownership */
    s->raw_queue[slot].row_base = sb_tail_base(s);
    s->raw_queue[slot].row_count = s->tail_count;
    s->raw_queue[slot].cols = s->cols;
    s->raw_queue[slot].bloom = s->tail_bloom;
    s->raw_queue[slot].wrap_bits = s->tail_wrap_bits;
    VT_LOG("comp: enqueue row_base=%d rows=%d cols=%d total=%d",
           s->raw_queue[slot].row_base, s->tail_count, s->cols, s->total_rows);
    s->rq_count++;
    pthread_cond_signal(&s->comp_need);
    pthread_mutex_unlock(&s->comp_mu);

    s->tail_raw = new_raw;
    s->tail_count = 0;
    s->tail_bloom = 0;
    s->tail_wrap_bits = 0;
}

/* appends row to scrollback tail; triggers compression flush when tail is full
 */
static void sb_push_row(SbStore *s, const Cell *row, int row_occ, int is_wrap) {
    Cell *dst = &s->tail_raw[s->tail_count * s->cols];
    /* Copy only occupied cells from source; blank-fill the rest.  For sparse
     * rows this cuts source memory reads by (cols - occ)/cols, and produces a
     * uniform blank pattern that LZ4 compresses far better. */
    int copy_cols = row_occ < s->cols ? row_occ : s->cols;
    memcpy(dst, row, (size_t)copy_cols * sizeof(Cell));
    if (copy_cols < s->cols) {
        static const Cell blank = {.ch = ' ',
                                   .fg = TERM_DEFAULT_FG,
                                   .bg = TERM_DEFAULT_BG,
                                   .attrs = 0};
        for (int i = copy_cols; i < s->cols; i++)
            dst[i] = blank;
    }
    /* Update tail bloom filter. Batch all blank cells into one sb_bloom_bits
     * call instead of repeating the multiply chain for every ' ' (rows pushed
     * during scrolling are typically one non-blank cell + many blanks). */
    int bloom_end = row_occ < s->cols ? row_occ : s->cols;
    int saw_blank = 0;
    for (int i = 0; i < bloom_end; i++) {
        uint32_t cp = row[i].ch;
        if (cp == ' ') {
            saw_blank = 1;
        } else {
            s->tail_bloom |= sb_bloom_bits(cp);
        }
    }
    if (saw_blank)
        s->tail_bloom |= sb_bloom_bits(' ');
    if (is_wrap)
        s->tail_wrap_bits |= (1ULL << s->tail_count);
    s->tail_count++;
    s->total_rows++;
    if (SB_CHUNK_ROWS == s->tail_count)
        sb_enqueue_tail(s);
}

static Cell *sb_decomp_cache_get(SbStore *s, int chunk_idx) {
    for (int i = 0; i < SB_DECOMP_CACHE; i++) {
        if (s->cache[i].chunk_idx == chunk_idx) {
            s->cache[i].lru_tick = ++s->lru_tick;
            return s->cache[i].cells;
        }
    }

    int lru = 0;
    for (int i = 1; i < SB_DECOMP_CACHE; i++) {
        if (s->cache[i].lru_tick < s->cache[lru].lru_tick)
            lru = i;
    }

    SbCacheSlot *slot = &s->cache[lru];
    SbChunk *ck = &s->chunks[chunk_idx];

    /* orig_row_count is the row count at compression time and never changes.
     * row_count may be smaller after sb_trim_newest.  We must decompress using
     * the full original size: LZ4_decompress_safe returns an error when
     * dstCapacity is smaller than the actual decompressed stream, leaving the
     * output buffer with garbage. */
    size_t orig_sz = (size_t)(ck->orig_row_count * ck->cols) * sizeof(Cell);
    size_t alloc_sz = orig_sz > 0 ? orig_sz : 1;

    /* Resize slot buffer if cols changed or buffer is too small. */
    if (!slot->cells || slot->slot_cols != ck->cols ||
        slot->cells_cap < alloc_sz) {
        free(slot->cells);
        slot->cells = malloc(alloc_sz);
        if (!slot->cells) {
            slot->chunk_idx = -1;
            slot->slot_cols = 0;
            slot->cells_cap = 0;
            return NULL;
        }
        slot->slot_cols = ck->cols;
        slot->cells_cap = alloc_sz;
    }
    /* Decompress into the full original-size buffer.  Callers index by
     * [off * ck->cols] where off < ck->row_count <= ck->orig_row_count,
     * so accesses stay within the valid decompressed prefix. */
    if (ck->is_raw) {
        memcpy(slot->cells, ck->buf->data, orig_sz);
    } else {
        LZ4_decompress_safe((const char *)ck->buf->data, (char *)slot->cells,
                            (int)ck->buf->data_sz, (int)orig_sz);
    }
    slot->chunk_idx = chunk_idx;
    slot->lru_tick = ++s->lru_tick;
    return slot->cells;
}

/* Shared traversal helper: locate abs_row in the three-tier hierarchy.
 * Returns the stored column width for the row, or 0 if not found.
 * If out_cells != NULL, copies min(stored_cols, out_cols) cells into
 * out_cells[0..out_cols) and blank-fills any remainder. */
static int sb_find_row(SbStore *s, int abs_row, Cell *out_cells, int out_cols) {
    static const Cell BLANK = {
        .ch = ' ', .fg = COL_DEFAULT_FG, .bg = COL_DEFAULT_BG, .attrs = 0};

    /* Tier 1: uncompressed tail (hot; no extra lock needed). */
    int tail_base = sb_tail_base(s);
    if (abs_row >= tail_base) {
        int src_cols = s->cols;
        if (out_cells) {
            const Cell *src = &s->tail_raw[(abs_row - tail_base) * src_cols];
            int copy = src_cols < out_cols ? src_cols : out_cols;
            memcpy(out_cells, src, (size_t)copy * sizeof(Cell));
            for (int i = copy; i < out_cols; i++)
                out_cells[i] = BLANK;
        }
        return src_cols;
    }

    /* Tier 2: raw queue (flushed from tail, pending compression) and
     * inflight (dequeued by worker, currently being compressed).
     * Both live under comp_mu. */
    pthread_mutex_lock(&s->comp_mu);
    for (int i = 0; i < s->rq_count; i++) {
        int idx = (s->rq_head + i) % SB_RAW_QUEUE_CAP;
        SbRawChunk *rq = &s->raw_queue[idx];
        if (!rq->cells)
            continue;
        if (abs_row >= rq->row_base && abs_row < rq->row_base + rq->row_count) {
            int src_cols = rq->cols;
            if (out_cells) {
                const Cell *src =
                    &rq->cells[(abs_row - rq->row_base) * src_cols];
                int copy = src_cols < out_cols ? src_cols : out_cols;
                memcpy(out_cells, src, (size_t)copy * sizeof(Cell));
                for (int j = copy; j < out_cols; j++)
                    out_cells[j] = BLANK;
            }
            pthread_mutex_unlock(&s->comp_mu);
            return src_cols;
        }
    }
    for (int i = 0; i < COMP_POOL_N; i++) {
        CompInflight *ifl = &s->inflight[i];
        if (!ifl->in_use || !ifl->cells)
            continue;
        if (abs_row >= ifl->row_base &&
            abs_row < ifl->row_base + ifl->row_count) {
            int src_cols = ifl->cols;
            if (out_cells) {
                const Cell *src =
                    &ifl->cells[(abs_row - ifl->row_base) * src_cols];
                int copy = src_cols < out_cols ? src_cols : out_cols;
                memcpy(out_cells, src, (size_t)copy * sizeof(Cell));
                for (int j = copy; j < out_cols; j++)
                    out_cells[j] = BLANK;
            }
            pthread_mutex_unlock(&s->comp_mu);
            return src_cols;
        }
    }
    pthread_mutex_unlock(&s->comp_mu);

    /* Tier 3: committed compressed chunks (chunk_mu). */
    pthread_rwlock_rdlock(&s->chunk_mu);
    for (int i = s->chunk_count - 1; i >= 0; i--) {
        SbChunk *ck = &s->chunks[i];
        if (abs_row >= ck->row_base && abs_row < ck->row_base + ck->row_count) {
            int src_cols = ck->cols;
            if (out_cells) {
                int off = abs_row - ck->row_base;
                Cell *decomp = sb_decomp_cache_get(s, i);
                if (decomp) {
                    int copy = src_cols < out_cols ? src_cols : out_cols;
                    memcpy(out_cells, decomp + (size_t)off * src_cols,
                           (size_t)copy * sizeof(Cell));
                    for (int j = copy; j < out_cols; j++)
                        out_cells[j] = BLANK;
                } else {
                    for (int j = 0; j < out_cols; j++)
                        out_cells[j] = BLANK;
                }
            }
            pthread_rwlock_unlock(&s->chunk_mu);
            return src_cols;
        }
    }
    pthread_rwlock_unlock(&s->chunk_mu);
    return 0;
}

/* Copy row abs_row into out[0..out_cols-1], truncating or padding with blanks.
 * Lookup order matches recency: tail (hot, no extra lock) then raw queue
 * (pending compression, comp_mu) then compressed chunks (chunk_mu). */
static void sb_get_row_into(SbStore *s, int abs_row, Cell *out, int out_cols) {
    static const Cell BLANK = {
        .ch = ' ', .fg = COL_DEFAULT_FG, .bg = COL_DEFAULT_BG, .attrs = 0};
    if (0 == sb_find_row(s, abs_row, out, out_cols)) {
        for (int i = 0; i < out_cols; i++)
            out[i] = BLANK;
    }
}

/* Returns the column width at which scrollback row abs_row was stored.
 * Used by terminal_get_state to visually wrap rows wider than current cols. */
static int sb_get_row_stored_cols(SbStore *s, int abs_row) {
    int found = sb_find_row(s, abs_row, NULL, 0);
    return found > 0 ? found : s->cols;
}

/* Remove count newest rows from scrollback (tail, then rq, then chunks). */
static void sb_trim_newest(SbStore *s, int count) {
    if (count <= 0)
        return;
    if (count <= s->tail_count) {
        s->tail_count -= count;
        s->total_rows -= count;
        return;
    }
    count -= s->tail_count;
    s->total_rows -= s->tail_count;
    s->tail_count = 0;

    /* Trim from the raw queue (newest entries are at the back). */
    pthread_mutex_lock(&s->comp_mu);
    while (count > 0 && s->rq_count > 0) {
        int idx = (s->rq_head + s->rq_count - 1) % SB_RAW_QUEUE_CAP;
        SbRawChunk *rq = &s->raw_queue[idx];
        int rm = count < rq->row_count ? count : rq->row_count;
        rq->row_count -= rm;
        s->total_rows -= rm;
        count -= rm;
        if (0 == rq->row_count) {
            free(rq->cells);
            rq->cells = NULL;
            s->rq_count--;
        }
    }
    pthread_mutex_unlock(&s->comp_mu);

    /* Trim from committed compressed chunks, newest first. */
    pthread_rwlock_wrlock(&s->chunk_mu);
    while (count > 0 && s->chunk_count > 0) {
        SbChunk *ck = &s->chunks[s->chunk_count - 1];
        int rm = count < ck->row_count ? count : ck->row_count;
        ck->row_count -= rm;
        s->total_rows -= rm;
        count -= rm;
        /* Invalidate cache for this chunk. */
        for (int i = 0; i < SB_DECOMP_CACHE; i++) {
            if (s->cache[i].chunk_idx == s->chunk_count - 1)
                s->cache[i].chunk_idx = -1;
        }
        if (0 == ck->row_count) {
            sb_chunk_buf_release(ck->buf);
            ck->buf = NULL;
            s->chunk_count--;
        }
    }
    pthread_rwlock_unlock(&s->chunk_mu);
}

/* Finalize the tail into a chunk (if non-empty) then reinitialize the tail
 * for new_cols.  Existing compressed chunks are untouched. */
static void sb_adapt_cols(SbStore *s, int new_cols) {
    if (s->tail_count > 0)
        sb_enqueue_tail(s);
    if (new_cols == s->cols)
        return;
    free(s->tail_raw);
    s->tail_raw = malloc((size_t)(SB_CHUNK_ROWS * new_cols) * sizeof(Cell));
    s->cols = new_cols;
}

typedef enum {
    PARSE_NORMAL,
    PARSE_ESC,
    PARSE_CSI,
    PARSE_OSC,         /* inside OSC body, accumulate until BEL or ST */
    PARSE_OSC_ESC,     /* ESC seen inside OSC, awaiting '\' (ST) */
    PARSE_ESC_CHARSET, /* after ESC ( or ESC ), next byte selects charset */
} ParseState;

struct Terminal {
    int cols;
    int rows;
    Cell *cells;
    Cell *cells_alt; /* alternate screen buffer */
    int on_alt_screen;
    int *row_perm;     /* row_perm[logical] = physical index in cells[] */
    int *row_perm_alt; /* row_perm for the inactive screen */
    int *occ;          /* occ[phys] = rightmost written col + 1, cells[] */
    int *occ_alt;      /* same for cells_alt[] */
    int *wrap;         /* wrap[phys] = 1 if row is soft-wrap continuation */
    int *wrap_alt;     /* same for cells_alt[] */

    SbStore *sb;
    int scroll_offset; /* rows above live view (0 = live) */
    int cursor_col;
    int cursor_row;
    int cursor_col_main; /* primary-screen cursor saved on alt-screen entry */
    int cursor_row_main;
    int cursor_shape_main; /* primary-screen shape saved on alt-screen entry */
    int saved_col;         /* ESC 7 / ESC 8 save/restore */
    int saved_row;
    uint32_t fg;
    uint32_t bg;
    uint8_t attrs;
    int cursor_visible;  /* 0 = hidden (\x1b[?25l) */
    int cursor_shape;    /* DECSCUSR: 0=default(block),1=blink block,2=block,
                          *           3=blink underline,4=underline,
                          *           5=blink beam,6=beam */
    int app_cursor_keys; /* 1 = DECCKM: arrows -> \x1bO[ABCD] */
    int bracketed_paste; /* 1 = wrap pastes in \x1b[200~ ... \x1b[201~ */
    int charset_g0;      /* 0 = ASCII, 1 = DEC special graphics */
    int esc_charset_g1;  /* 1 if last ESC charset designator was for G1 */
    int pending_wrap;    /* VT100 last-column wrap-pending flag */
    ParseState state;
    char csi_buf[CSI_BUF_SIZE];
    int csi_len;
    int csi_mod; /* CSI modifier char: 0, '?', or '>' */
    int scroll_top;
    int scroll_bottom;
    uint8_t utf8_buf[4]; /* partial UTF-8 sequence accumulator */
    int utf8_rem;        /* continuation bytes still expected */
    pthread_mutex_t lock;
    /* Selection (main-thread only). */
    int sel_active;
    int sel_col0, sel_row0; /* anchor */
    int sel_col1, sel_row1; /* current end */

    /* OSC body accumulator */
    char osc_buf[OSC_BUF_SIZE];
    int osc_len;

    /* Alt-screen callback (registered once before PTY starts; never changed).
     */
    terminal_alt_screen_fn alt_screen_cb;
    void *alt_screen_cb_arg;

    /* Exit-code tracking via OSC 133;D sequences. */
    int last_exit_code; /* -1 = not yet received */
    terminal_exit_code_fn exit_code_cb;
    void *exit_code_cb_arg;

    /* Shell CWD tracking via OSC 7 sequences. */
    terminal_cwd_fn cwd_cb;
    void *cwd_cb_arg;

    /* Window title via OSC 0/2 sequences. */
    terminal_title_fn title_cb;
    void *title_cb_arg;

    /* OSC 8 hyperlink URL intern pool; url_idx in each Cell is 1-based. */
    char **url_pool;
    int url_pool_len;
    int url_pool_cap;
    uint16_t cur_url_idx; /* applied to every glyph written while non-zero */
};

static void fill_cells(Cell *buf, int from, int to, uint32_t fg, uint32_t bg) {
    Cell blank = {.ch = ' ', .fg = fg, .bg = bg, .attrs = 0};
    for (int i = from; i < to; i++)
        buf[i] = blank;
}

static int row_is_blank(const Cell *row, int cols) {
    for (int j = 0; j < cols; j++) {
        if (' ' != row[j].ch)
            return 0;
    }
    return 1;
}

/* Return pointer to the first cell of logical row r. */
static inline Cell *grid_row(const Terminal *t, int r) {
    return &t->cells[t->row_perm[r] * t->cols];
}

/* Clear logical cells [from, to) using row_perm lookup.
 * Skips cells past occ[phys] (already blank); updates occ[phys] after. */
static void clear_range(Terminal *t, int from, int to) {
    if (from >= to)
        return;
    Cell blank = {.ch = ' ', .fg = t->fg, .bg = t->bg, .attrs = 0};
    int r = from / t->cols;
    int c_off = from % t->cols;
    int end_r = (to - 1) / t->cols;
    for (; r <= end_r; r++) {
        int phys = t->row_perm[r];
        int c0 = c_off;
        int c1 = (r == end_r) ? ((to - 1) % t->cols + 1) : t->cols;
        int o = t->occ[phys];
        int eff1 = c1 < o ? c1 : o; /* don't write past occupied end */
        Cell *row = t->cells + (size_t)phys * t->cols;
        for (int c = c0; c < eff1; c++)
            row[c] = blank;
        /* If clear reached or passed old occupied end, shrink occ. */
        if (c1 >= o && c0 < o)
            t->occ[phys] = c0;
        c_off = 0;
    }
}

/* Fast-path: clear n complete logical rows starting at logical row start.
 * Avoids the per-row division and modulo in clear_range. */
static void clear_rows_full(Terminal *t, int start, int n) {
    Cell blank = {.ch = ' ', .fg = t->fg, .bg = t->bg, .attrs = 0};
    for (int li = start; li < start + n; li++) {
        int phys = t->row_perm[li];
        int o = t->occ[phys];
        if (o > 0) {
            Cell *row = t->cells + (size_t)phys * t->cols;
            for (int c = 0; c < o; c++)
                row[c] = blank;
            t->occ[phys] = 0;
        }
    }
}

/* Reverse perm[lo..hi] (inclusive) in-place. */
static void perm_reverse(int *perm, int lo, int hi) {
    while (lo < hi) {
        int t = perm[lo];
        perm[lo] = perm[hi];
        perm[hi] = t;
        lo++;
        hi--;
    }
}

/* Rotate row_perm[a..b] left by `n` positions (shift rows up in the region).
 * O(n) in-place 3-reversal: no heap or stack buffer required. */
static void perm_rotate_left(int *perm, int a, int b, int n) {
    int region = b - a + 1;
    if (n <= 0 || n >= region)
        return;
    /* rotate left k: reverse [a..a+k-1], reverse [a+k..b], reverse [a..b] */
    perm_reverse(perm, a, a + n - 1);
    perm_reverse(perm, a + n, b);
    perm_reverse(perm, a, b);
}

/* Rotate row_perm[a..b] right by `n` positions (shift rows down).
 * O(n) in-place 3-reversal: no heap or stack buffer required. */
static void perm_rotate_right(int *perm, int a, int b, int n) {
    int region = b - a + 1;
    if (n <= 0 || n >= region)
        return;
    /* rotate right k = rotate left (region - k) */
    perm_rotate_left(perm, a, b, region - n);
}

static void scroll_up(Terminal *t, int lines) {
    int region_h = t->scroll_bottom - t->scroll_top + 1;
    int push = lines < region_h ? lines : region_h;

    if (0 == t->scroll_top && !t->on_alt_screen) {
        for (int i = 0; i < push; i++)
            sb_push_row(t->sb, grid_row(t, i), t->occ[t->row_perm[i]],
                        t->wrap[t->row_perm[i]]);
        if (t->scroll_offset > 0) {
            t->scroll_offset += push;
            int total_sb = t->sb->total_rows;
            if (t->scroll_offset > total_sb)
                t->scroll_offset = total_sb;
        }
    }

    if (lines >= region_h) {
        for (int li = t->scroll_top; li <= t->scroll_bottom; li++)
            t->wrap[t->row_perm[li]] = 0;
        clear_rows_full(t, t->scroll_top, t->scroll_bottom - t->scroll_top + 1);
        return;
    }

    /* Rotate row_perm instead of copying cell data: O(region_h * 4 bytes)
     * vs O(region_h * cols * sizeof(Cell)). */
    perm_rotate_left(t->row_perm, t->scroll_top, t->scroll_bottom, lines);
    for (int li = t->scroll_bottom - lines + 1; li <= t->scroll_bottom; li++)
        t->wrap[t->row_perm[li]] = 0;
    clear_rows_full(t, t->scroll_bottom - lines + 1, lines);
}

static void scroll_down(Terminal *t, int lines) {
    int region_h = t->scroll_bottom - t->scroll_top + 1;
    if (lines >= region_h) {
        clear_rows_full(t, t->scroll_top, t->scroll_bottom - t->scroll_top + 1);
        return;
    }

    perm_rotate_right(t->row_perm, t->scroll_top, t->scroll_bottom, lines);
    for (int li = t->scroll_top; li < t->scroll_top + lines; li++)
        t->wrap[t->row_perm[li]] = 0;
    clear_rows_full(t, t->scroll_top, lines);
}

static void erase_display(Terminal *t, int mode) {
    if (0 == mode) {
        clear_range(t, t->cursor_row * t->cols + t->cursor_col,
                    t->rows * t->cols);
    } else if (1 == mode) {
        clear_range(t, 0, t->cursor_row * t->cols + t->cursor_col + 1);
    } else { /* 2 and 3 both clear the visible area */
        /* Append non-empty visible rows to scrollback so `clear` preserves
         * history without padding blank lines at the end. */
        if (!t->on_alt_screen) {
            int last = t->rows - 1;
            while (last >= 0 && row_is_blank(grid_row(t, last), t->cols))
                last--;
            for (int i = 0; i <= last; i++)
                sb_push_row(t->sb, grid_row(t, i), t->occ[t->row_perm[i]],
                            t->wrap[t->row_perm[i]]);
        }
        clear_range(t, 0, t->rows * t->cols);
    }
}

static void erase_line(Terminal *t, int mode) {
    int base = t->cursor_row * t->cols;
    if (0 == mode) {
        clear_range(t, base + t->cursor_col, base + t->cols);
    } else if (1 == mode) {
        clear_range(t, base, base + t->cursor_col + 1);
    } else {
        clear_range(t, base, base + t->cols);
    }
}

/* Swap the three pointer pairs that distinguish primary from alt screen. */
static void swap_screen_buffers(Terminal *t) {
    Cell *ctmp = t->cells;
    t->cells = t->cells_alt;
    t->cells_alt = ctmp;
    int *ptmp = t->row_perm;
    t->row_perm = t->row_perm_alt;
    t->row_perm_alt = ptmp;
    int *otmp = t->occ;
    t->occ = t->occ_alt;
    t->occ_alt = otmp;
}

static void enter_alt_screen(Terminal *t) {
    if (t->on_alt_screen)
        return;
    VT_LOG("enter_alt_screen");
    t->scroll_offset = 0;
    t->cursor_col_main = t->cursor_col;
    t->cursor_row_main = t->cursor_row;
    t->cursor_shape_main = t->cursor_shape;
    swap_screen_buffers(t);
    /* Reset alt-screen perm to identity; mark all rows occupied so
     * clear_range below wipes them fully regardless of prior content. */
    for (int i = 0; i < t->rows; i++) {
        t->row_perm[i] = i;
        t->occ[i] = t->cols;
    }
    t->on_alt_screen = 1;
    clear_range(t, 0, t->rows * t->cols);
    t->cursor_col = 0;
    t->cursor_row = 0;
    if (t->alt_screen_cb)
        t->alt_screen_cb(t->alt_screen_cb_arg, 1);
}

static void exit_alt_screen(Terminal *t) {
    if (!t->on_alt_screen) {
        VT_LOG("exit_alt_screen (not on alt screen  -  no-op)");
        return;
    }
    VT_LOG("exit_alt_screen");
    /* Preserve alt-screen content in scrollback before restoring primary. */
    for (int i = 0; i < t->rows; i++)
        sb_push_row(t->sb, grid_row(t, i), t->occ[t->row_perm[i]],
                    t->wrap[t->row_perm[i]]);
    swap_screen_buffers(t);
    t->on_alt_screen = 0;
    t->cursor_shape = t->cursor_shape_main;
    t->cursor_col = t->cursor_col_main;
    t->cursor_row = t->cursor_row_main;
    t->scroll_offset = 0;
    if (t->alt_screen_cb)
        t->alt_screen_cb(t->alt_screen_cb_arg, 0);
}

static void parse_csi_params(const char *buf, int len, int *params,
                             int *nparams) {
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
    if (has_cur || *nparams > 0)
        params[(*nparams)++] = has_cur ? cur : 0;
}

static int p(int *params, int nparams, int idx, int def) {
    if (idx < nparams && params[idx] > 0)
        return params[idx];
    return def;
}

static uint32_t color256(int n) {
    if (n < ANSI_COLOR_COUNT)
        return ANSI_COLORS[n];
    if (n < GRAYSCALE_START) {
        n -= ANSI_COLOR_COUNT;
        int b = n % COLOR_CUBE_DIM;
        n /= COLOR_CUBE_DIM;
        int g = n % COLOR_CUBE_DIM;
        n /= COLOR_CUBE_DIM;
        int r = n % COLOR_CUBE_DIM;
        uint8_t rv = r ? (uint8_t)(COLOR_CUBE_BASE + r * COLOR_CUBE_STEP) : 0;
        uint8_t gv = g ? (uint8_t)(COLOR_CUBE_BASE + g * COLOR_CUBE_STEP) : 0;
        uint8_t bv = b ? (uint8_t)(COLOR_CUBE_BASE + b * COLOR_CUBE_STEP) : 0;
        return ((uint32_t)rv << 24) | ((uint32_t)gv << 16) |
               ((uint32_t)bv << 8) | RGBA_ALPHA_OPAQUE;
    }
    uint8_t v =
        (uint8_t)(GRAYSCALE_BASE + (n - GRAYSCALE_START) * GRAYSCALE_STEP);
    return ((uint32_t)v << 24) | ((uint32_t)v << 16) | ((uint32_t)v << 8) |
           RGBA_ALPHA_OPAQUE;
}

/* Parse an SGR extended-color sub-sequence starting at params[i].
 * Handles both 256-color (38;5;n) and RGB (38;2;r;g;b) forms.
 * Returns the number of extra parameter slots consumed (2 or 4),
 * or -1 if the sub-sequence is malformed (caller leaves color unchanged). */
static int parse_extended_color(const int *params, int nparams, int i,
                                uint32_t *out_color) {
    if (i + 3 <= nparams && SGR_COLOR256 == params[i + 1]) {
        *out_color = color256(params[i + 2]);
        return 2;
    }
    if (i + 5 <= nparams && SGR_COLORRGB == params[i + 1]) {
        *out_color = ((uint32_t)params[i + 2] << 24) |
                     ((uint32_t)params[i + 3] << 16) |
                     ((uint32_t)params[i + 4] << 8) | RGBA_ALPHA_OPAQUE;
        return 4;
    }
    return -1;
}

static void apply_sgr(Terminal *t, int *params, int nparams) {
    /* Bare \e[m (no params) is equivalent to \e[0m, reset all attributes. */
    if (0 == nparams) {
        t->fg = COL_DEFAULT_FG;
        t->bg = COL_DEFAULT_BG;
        t->attrs = 0;
        return;
    }
    for (int i = 0; i < nparams; i++) {
        int v = params[i];
        if (SGR_RESET == v) {
            t->fg = COL_DEFAULT_FG;
            t->bg = COL_DEFAULT_BG;
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
            if (adv > 0)
                i += adv;
        } else if (SGR_BG_EXT == v) {
            int adv = parse_extended_color(params, nparams, i, &t->bg);
            if (adv > 0)
                i += adv;
        }
    }
}

static void clamp_cursor(Terminal *t) {
    if (t->cursor_col < 0)
        t->cursor_col = 0;
    if (t->cursor_col >= t->cols)
        t->cursor_col = t->cols - 1;
    if (t->cursor_row < 0)
        t->cursor_row = 0;
    if (t->cursor_row >= t->rows)
        t->cursor_row = t->rows - 1;
}

static void csi_dispatch(Terminal *t, char cmd) {
    int params[CSI_PARAMS_MAX];
    int nparams = 0;
    parse_csi_params(t->csi_buf, t->csi_len, params, &nparams);
    t->pending_wrap = 0;

    /* Sequences with '>' modifier (e.g. \x1b[>4;2m) are xterm-private and
     * must not be interpreted as standard CSI commands. */
    if ('>' == t->csi_mod)
        return;

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
    case 'L': { /* IL: insert lines  -  push region down from cursor row */
        int n = p(params, nparams, 0, 1);
        int saved_top = t->scroll_top;
        t->scroll_top = t->cursor_row;
        scroll_down(t, n);
        t->scroll_top = saved_top;
        break;
    }
    case 'M': { /* DL: delete lines  -  pull region up from cursor row */
        int n = p(params, nparams, 0, 1);
        int saved_top = t->scroll_top;
        t->scroll_top = t->cursor_row;
        scroll_up(t, n);
        t->scroll_top = saved_top;
        break;
    }
    case 'P': {
        int n = p(params, nparams, 0, 1);
        Cell *row = grid_row(t, t->cursor_row);
        int src = t->cursor_col + n;
        int dst = t->cursor_col;
        t->occ[t->row_perm[t->cursor_row]] = t->cols; /* conservative */
        if (src < t->cols)
            memmove(&row[dst], &row[src],
                    (size_t)(t->cols - src) * sizeof(Cell));
        int base = t->cursor_row * t->cols;
        int cf = t->cols - n < dst ? dst : t->cols - n;
        clear_range(t, base + cf, base + t->cols);
        break;
    }
    case 'X': { /* ECH: erase characters */
        int n = p(params, nparams, 0, 1);
        int from = t->cursor_row * t->cols + t->cursor_col;
        int to = from + n;
        int max = (t->cursor_row + 1) * t->cols;
        if (to > max)
            to = max;
        clear_range(t, from, to);
        break;
    }
    case '@': { /* ICH: insert blank characters */
        int n = p(params, nparams, 0, 1);
        Cell *row = grid_row(t, t->cursor_row);
        int src_col = t->cursor_col;
        int dst_col = src_col + n;
        t->occ[t->row_perm[t->cursor_row]] = t->cols; /* conservative */
        if (dst_col < t->cols)
            memmove(&row[dst_col], &row[src_col],
                    (size_t)(t->cols - dst_col) * sizeof(Cell));
        int base = t->cursor_row * t->cols;
        int ct = dst_col < t->cols ? dst_col : t->cols;
        clear_range(t, base + src_col, base + ct);
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
            case DECPM_DECCKM:
                t->app_cursor_keys = 1;
                break;
            case DECPM_CURSOR_VISIBLE:
                t->cursor_visible = 1;
                break;
            case DECPM_ALT_SCREEN: /* fallthrough */
            case DECPM_ALT_SCREEN_2:
                enter_alt_screen(t);
                break;
            case DECPM_ALT_SCREEN_3:
                enter_alt_screen(t);
                break;
            case DECPM_BRACKETED_PASTE:
                t->bracketed_paste = 1;
                break;
            default:
                break;
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
                if (t->on_alt_screen)
                    exit_alt_screen(t);
                break;
            case DECPM_CURSOR_VISIBLE:
                t->cursor_visible = 0;
                break;
            case DECPM_ALT_SCREEN: /* fallthrough */
            case DECPM_ALT_SCREEN_2:
                exit_alt_screen(t);
                break;
            case DECPM_ALT_SCREEN_3:
                exit_alt_screen(t);
                break;
            case DECPM_BRACKETED_PASTE:
                t->bracketed_paste = 0;
                break;
            default:
                break;
            }
        }
        break;
    case 'm':
        apply_sgr(t, params, nparams);
        break;
    case 'q':
        /* DECSCUSR (ESC [ Ps SP q), set cursor shape.
         * Only active when intermediate byte SP is present in the CSI buffer.
         */
        {
            int has_sp = 0;
            for (int i = 0; i < t->csi_len; i++) {
                if (' ' == t->csi_buf[i]) {
                    has_sp = 1;
                    break;
                }
            }
            if (has_sp)
                t->cursor_shape = p(params, nparams, 0, 0);
        }
        break;
    case 'r':
        t->scroll_top = p(params, nparams, 0, 1) - 1;
        t->scroll_bottom = p(params, nparams, 1, t->rows) - 1;
        if (t->scroll_top < 0)
            t->scroll_top = 0;
        if (t->scroll_bottom >= t->rows)
            t->scroll_bottom = t->rows - 1;
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

static __attribute__((always_inline)) void write_glyph(Terminal *t,
                                                       uint32_t cp) {
    /* VT100 pending-wrap: defer the actual newline until the next character
     * so printing exactly cols characters doesn't add a spurious blank line. */
    if (t->pending_wrap) {
        t->pending_wrap = 0;
        t->cursor_col = 0;
        if (t->cursor_row == t->scroll_bottom) {
            scroll_up(t, 1);
        } else {
            t->cursor_row++;
        }
        /* Mark new row as soft-wrap continuation (physical-indexed). */
        t->wrap[t->row_perm[t->cursor_row]] = 1;
    }
    if (t->cursor_col >= t->cols)
        t->cursor_col = t->cols - 1;
    {
        int phys = t->row_perm[t->cursor_row];
        Cell *row = t->cells + (size_t)phys * t->cols;
        row[t->cursor_col].ch = cp;
        row[t->cursor_col].fg = t->fg;
        row[t->cursor_col].bg = t->bg;
        row[t->cursor_col].attrs = t->attrs;
        /* occ[phys] tracks rightmost written column + 1; used to skip
         * blank cells during render and bloom filter updates. */
        if (t->cursor_col + 1 > t->occ[phys])
            t->occ[phys] = t->cursor_col + 1;
    }

    if (t->cursor_col >= t->cols - 1) {
        t->pending_wrap = 1;
    } else {
        t->cursor_col++;
    }
}

/* URL-decodes a percent-encoded string into out (size cap).
 * Returns number of bytes written (excluding NUL). */
static size_t url_decode(const char *src, char *out, size_t cap) {
    size_t n = 0;
    for (; *src && n + 1 < cap; src++) {
        if ('%' == src[0] &&
            ((src[1] >= '0' && src[1] <= '9') ||
             (src[1] >= 'A' && src[1] <= 'F') ||
             (src[1] >= 'a' && src[1] <= 'f')) &&
            ((src[2] >= '0' && src[2] <= '9') ||
             (src[2] >= 'A' && src[2] <= 'F') ||
             (src[2] >= 'a' && src[2] <= 'f'))) {
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
 * Handles OSC 0/2 (title), OSC 7 (CWD), and OSC 133;D (exit code). */
static void dispatch_osc(Terminal *t) {
    t->osc_buf[t->osc_len] = '\0';

    /* OSC 0/2: "0;title" or "2;title" */
    if ((t->osc_buf[0] == '0' || t->osc_buf[0] == '2') &&
        t->osc_buf[1] == ';' && NULL != t->title_cb) {
        t->title_cb(t->osc_buf + 2, t->title_cb_arg);
        return;
    }

    /* OSC 7: "7;file://[host]/path" */
    if (0 == strncmp(t->osc_buf, "7;file://", 9) && NULL != t->cwd_cb) {
        /* Skip optional hostname (up to the next '/'). */
        const char *p = t->osc_buf + 9;
        if ('/' != p[0]) {
            const char *slash = strchr(p, '/');
            if (NULL != slash)
                p = slash;
        }
        if ('/' == p[0]) {
            char decoded[4096];
            if (url_decode(p, decoded, sizeof decoded) > 0)
                t->cwd_cb(decoded, t->cwd_cb_arg);
        }
        return;
    }

    if (0 != strncmp(t->osc_buf, "133;D", 5))
        return;

    int code = 0;
    if (';' == t->osc_buf[5])
        code = (int)strtol(t->osc_buf + 6, NULL, 10);

    t->last_exit_code = code;

    if (NULL != t->exit_code_cb)
        t->exit_code_cb(code, t->exit_code_cb_arg);
}

static void process_byte(Terminal *t, unsigned char c) {
    switch (t->state) {
    case PARSE_NORMAL:
        if (ASCII_ESC == c) {
            t->utf8_rem = 0;
            t->pending_wrap = 0;
            t->state = PARSE_ESC;
        } else if (ASCII_CR == c) {
            t->utf8_rem = 0;
            t->pending_wrap = 0;
            t->cursor_col = 0;
        } else if (ASCII_LF == c || ASCII_VT == c || ASCII_FF == c) {
            t->utf8_rem = 0;
            t->pending_wrap = 0;
            if (t->cursor_row == t->scroll_bottom) {
                scroll_up(t, 1);
            } else {
                t->cursor_row++;
            }
        } else if (ASCII_BS == c) {
            t->utf8_rem = 0;
            t->pending_wrap = 0;
            if (t->cursor_col > 0)
                t->cursor_col--;
        } else if (ASCII_HT == c) {
            t->utf8_rem = 0;
            t->pending_wrap = 0;
            t->cursor_col =
                (t->cursor_col + TAB_STOP_WIDTH) & ~(TAB_STOP_WIDTH - 1);
            if (t->cursor_col >= t->cols)
                t->cursor_col = t->cols - 1;
        } else if (c >= UTF8_2BYTE_LEAD_MIN) {
            /* Multi-byte lead: record it and count remaining continuation
             * bytes. */
            t->utf8_buf[0] = c;
            t->utf8_rem = (c >= UTF8_4BYTE_LEAD_MIN)   ? 3
                          : (c >= UTF8_3BYTE_LEAD_MIN) ? 2
                                                       : 1;
        } else if (c >= UTF8_CONT_BYTE_MIN) {
            /* Continuation byte: accumulate; emit glyph when utf8_rem hits 0.
             * Stray continuations (utf8_rem == 0) are silently discarded per
             * the Unicode replacement policy. */
            if (t->utf8_rem > 0) {
                uint8_t lead = t->utf8_buf[0];
                int total = (lead >= UTF8_4BYTE_LEAD_MIN)   ? 4
                            : (lead >= UTF8_3BYTE_LEAD_MIN) ? 3
                                                            : 2;
                t->utf8_buf[total - t->utf8_rem] = c;
                t->utf8_rem--;
                if (0 == t->utf8_rem) {
                    uint32_t cp;
                    if (lead >= UTF8_4BYTE_LEAD_MIN) {
                        cp = ((uint32_t)(lead & UTF8_4BYTE_DATA_MASK) << 18) |
                             ((uint32_t)(t->utf8_buf[1] & UTF8_CONT_DATA_MASK)
                              << 12) |
                             ((uint32_t)(t->utf8_buf[2] & UTF8_CONT_DATA_MASK)
                              << 6) |
                             (uint32_t)(t->utf8_buf[3] & UTF8_CONT_DATA_MASK);
                    } else if (lead >= UTF8_3BYTE_LEAD_MIN) {
                        cp = ((uint32_t)(lead & UTF8_3BYTE_DATA_MASK) << 12) |
                             ((uint32_t)(t->utf8_buf[1] & UTF8_CONT_DATA_MASK)
                              << 6) |
                             (uint32_t)(t->utf8_buf[2] & UTF8_CONT_DATA_MASK);
                    } else {
                        cp = ((uint32_t)(lead & UTF8_2BYTE_DATA_MASK) << 6) |
                             (uint32_t)(t->utf8_buf[1] & UTF8_CONT_DATA_MASK);
                    }
                    write_glyph(t, cp);
                }
            }
            /* else: stray continuation, ignore */
        } else if (c >= ASCII_PRINTABLE_FIRST) {
            t->utf8_rem = 0;
            uint32_t glyph = c;
            if (t->charset_g0 && c >= DEC_SPECIAL_FIRST &&
                c <= ASCII_PRINTABLE_LAST) {
                glyph = DEC_SPECIAL[c - DEC_SPECIAL_FIRST];
            }
            write_glyph(t, glyph);
        }
        break;

    case PARSE_ESC:
        t->state = PARSE_NORMAL;
        t->pending_wrap = 0;
        switch ((char)c) {
        case '[':
            t->state = PARSE_CSI;
            t->csi_len = 0;
            t->csi_mod = 0;
            memset(t->csi_buf, 0, sizeof t->csi_buf);
            break;
        case ']':
            t->osc_len = 0;
            t->state = PARSE_OSC;
            break;
        case '(':
            t->esc_charset_g1 = 0;
            t->state = PARSE_ESC_CHARSET;
            break;
        case ')':
        case '*':
        case '+':
            t->esc_charset_g1 = 1;
            t->state = PARSE_ESC_CHARSET;
            break;
        case 'c': /* RIS: full reset */
            sb_clear(t->sb);
            t->scroll_offset = 0;
            clear_range(t, 0, t->rows * t->cols);
            t->cursor_col = 0;
            t->cursor_row = 0;
            t->fg = COL_DEFAULT_FG;
            t->bg = COL_DEFAULT_BG;
            t->attrs = 0;
            t->scroll_top = 0;
            t->scroll_bottom = t->rows - 1;
            t->charset_g0 = 0;
            t->app_cursor_keys = 0;
            t->cursor_visible = 1;
            break;
        case '7': /* DECSC: save cursor */
            t->saved_col = t->cursor_col;
            t->saved_row = t->cursor_row;
            break;
        case '8': /* DECRC: restore cursor */
            t->cursor_col = t->saved_col;
            t->cursor_row = t->saved_row;
            break;
        case 'M': /* RI: reverse index (scroll down if at top margin) */
            if (t->cursor_row == t->scroll_top) {
                scroll_down(t, 1);
            } else {
                t->cursor_row--;
            }
            break;
        case 'E': /* NEL: next line */
            t->cursor_col = 0;
            if (t->cursor_row == t->scroll_bottom) {
                scroll_up(t, 1);
            } else {
                t->cursor_row++;
            }
            break;
        default: /* unknown / keypad mode switches (= >), silently ignored */
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
            t->state = PARSE_NORMAL;
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

Terminal *terminal_create(int cols, int rows) {
    Terminal *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->cols = cols;
    t->rows = rows;
    t->cells = malloc((size_t)(cols * rows) * sizeof(Cell));
    t->cells_alt = malloc((size_t)(cols * rows) * sizeof(Cell));
    t->row_perm = malloc((size_t)rows * sizeof(int));
    t->row_perm_alt = malloc((size_t)rows * sizeof(int));
    t->occ = calloc((size_t)rows, sizeof(int));
    t->occ_alt = calloc((size_t)rows, sizeof(int));
    t->wrap = calloc((size_t)rows, sizeof(int));
    t->wrap_alt = calloc((size_t)rows, sizeof(int));
    t->sb = sb_create(cols);
    if (!t->cells || !t->cells_alt || !t->row_perm || !t->row_perm_alt ||
        !t->occ || !t->occ_alt || !t->wrap || !t->wrap_alt || !t->sb) {
        free(t->cells);
        free(t->cells_alt);
        free(t->row_perm);
        free(t->row_perm_alt);
        free(t->occ);
        free(t->occ_alt);
        free(t->wrap);
        free(t->wrap_alt);
        sb_destroy(t->sb);
        free(t);
        return NULL;
    }
    for (int i = 0; i < rows; i++)
        t->row_perm[i] = i;
    for (int i = 0; i < rows; i++)
        t->row_perm_alt[i] = i;
    t->fg = COL_DEFAULT_FG;
    t->bg = COL_DEFAULT_BG;
    t->scroll_top = 0;
    t->scroll_bottom = rows - 1;
    t->cursor_visible = 1;
    t->state = PARSE_NORMAL;
    t->last_exit_code = -1;
    pthread_mutex_init(&t->lock, NULL);
    fill_cells(t->cells, 0, cols * rows, COL_DEFAULT_FG, COL_DEFAULT_BG);
    fill_cells(t->cells_alt, 0, cols * rows, COL_DEFAULT_FG, COL_DEFAULT_BG);
    return t;
}

void terminal_destroy(Terminal *t) {
    if (!t)
        return;
    terminal_search_cancel(t);
    pthread_mutex_destroy(&t->lock);
    free(t->cells);
    free(t->cells_alt);
    free(t->row_perm);
    free(t->row_perm_alt);
    free(t->occ);
    free(t->occ_alt);
    free(t->wrap);
    free(t->wrap_alt);
    sb_destroy(t->sb);
    free(t);
}

void terminal_feed(Terminal *t, const char *buf, size_t len) {
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
    const unsigned char *p = (const unsigned char *)buf;
    const unsigned char *end = p + len;
    while (p < end) {
        unsigned char c = *p;
        /* Fast path: bypass the full switch for plain printable ASCII in the
         * normal parse state.  Covers the common case of dense text output. */
        if (PARSE_NORMAL == t->state && c >= ASCII_PRINTABLE_FIRST &&
            c <= ASCII_PRINTABLE_LAST && !t->charset_g0) {
            t->utf8_rem = 0;
            do {
                write_glyph(t, (uint32_t)c);
                c = *++p;
            } while (p < end && c >= ASCII_PRINTABLE_FIRST &&
                     c <= ASCII_PRINTABLE_LAST);
        } else {
            process_byte(t, c);
            p++;
        }
    }
    pthread_mutex_unlock(&t->lock);
}

/* copy_cols must be pre-clamped by caller to min(src_cols, dst_cols). */
static void copy_cells_resized(const Cell *src, int src_cols, Cell *dst,
                               int dst_cols, int src_row_start,
                               int dst_row_start, int copy_rows,
                               int copy_cols) {
    for (int row = 0; row < copy_rows; row++) {
        memcpy(&dst[(dst_row_start + row) * dst_cols],
               &src[(src_row_start + row) * src_cols],
               (size_t)copy_cols * sizeof(Cell));
    }
}

/* Like copy_cells_resized but src rows are looked up via a row_perm array. */
static void copy_cells_via_perm(const Cell *src, const int *perm, int src_cols,
                                Cell *dst, int dst_cols, int src_row_start,
                                int dst_row_start, int copy_rows,
                                int copy_cols) {
    for (int row = 0; row < copy_rows; row++) {
        int phys = perm[src_row_start + row];
        memcpy(&dst[(dst_row_start + row) * dst_cols], &src[phys * src_cols],
               (size_t)copy_cols * sizeof(Cell));
    }
}

/* Reallocates the cell grid; content is truncated/padded, not reflowed. */
void terminal_resize(Terminal *t, int cols, int rows) {
    pthread_mutex_lock(&t->lock);

    int old_cols = t->cols;
    int old_rows = t->rows;
    int copy_cols = cols < old_cols ? cols : old_cols;

    Cell *new_cells = malloc((size_t)(cols * rows) * sizeof(Cell));
    Cell *new_cells_alt = malloc((size_t)(cols * rows) * sizeof(Cell));
    int *new_perm = malloc((size_t)rows * sizeof(int));
    int *new_perm_alt = malloc((size_t)rows * sizeof(int));
    int *new_occ = malloc((size_t)rows * sizeof(int));
    int *new_occ_alt = malloc((size_t)rows * sizeof(int));
    int *new_wrap = calloc((size_t)rows, sizeof(int));
    int *new_wrap_alt = calloc((size_t)rows, sizeof(int));
    if (!new_cells || !new_cells_alt || !new_perm || !new_perm_alt ||
        !new_occ || !new_occ_alt || !new_wrap || !new_wrap_alt) {
        free(new_cells);
        free(new_cells_alt);
        free(new_perm);
        free(new_perm_alt);
        free(new_occ);
        free(new_occ_alt);
        free(new_wrap);
        free(new_wrap_alt);
        pthread_mutex_unlock(&t->lock);
        return;
    }
    fill_cells(new_cells, 0, cols * rows, COL_DEFAULT_FG, COL_DEFAULT_BG);
    fill_cells(new_cells_alt, 0, cols * rows, COL_DEFAULT_FG, COL_DEFAULT_BG);
    for (int i = 0; i < rows; i++)
        new_perm[i] = i;
    for (int i = 0; i < rows; i++)
        new_perm_alt[i] = i;
    /* Conservative: assume all rows have content after copy. */
    for (int i = 0; i < rows; i++)
        new_occ[i] = cols;
    for (int i = 0; i < rows; i++)
        new_occ_alt[i] = cols;

    if (!t->on_alt_screen) {
        int delta = rows - old_rows;

        if (cols != old_cols) {
            int copy_rows = old_rows < rows ? old_rows : rows;
            copy_cells_via_perm(t->cells, t->row_perm, old_cols, new_cells,
                                cols, 0, 0, copy_rows, copy_cols);
        } else if (delta > 0) {
            /* Row-only expand: pull newest scrollback rows into viewport. */
            int total_sb = t->sb->total_rows;
            int pull = delta < total_sb ? delta : total_sb;
            int sb_base_idx = total_sb - pull;
            for (int i = 0; i < pull; i++)
                sb_get_row_into(t->sb, sb_base_idx + i, &new_cells[i * cols],
                                cols);
            int copy_rows = old_rows < rows - pull ? old_rows : rows - pull;
            copy_cells_via_perm(t->cells, t->row_perm, old_cols, new_cells,
                                cols, 0, pull, copy_rows, copy_cols);
            sb_trim_newest(t->sb, pull);
            t->cursor_row += pull;
        } else if (delta < 0) {
            /* Row-only shrink: push top rows into scrollback. */
            int want_push = -delta;
            int push = want_push < t->cursor_row ? want_push : t->cursor_row;
            for (int i = 0; i < push; i++)
                sb_push_row(t->sb, &t->cells[t->row_perm[i] * old_cols],
                            t->occ[t->row_perm[i]], t->wrap[t->row_perm[i]]);
            int src_start = push;
            int copy_rows =
                old_rows - src_start < rows ? old_rows - src_start : rows;
            copy_cells_via_perm(t->cells, t->row_perm, old_cols, new_cells,
                                cols, src_start, 0, copy_rows, copy_cols);
            t->cursor_row -= push;
        } else {
            copy_cells_via_perm(t->cells, t->row_perm, old_cols, new_cells,
                                cols, 0, 0, old_rows, copy_cols);
        }
    } else {
        /* On alt screen: t->cells = alt buffer (row_perm = identity),
         *                t->cells_alt = main buffer (row_perm_alt = main perm).
         */
        int copy_rows = old_rows < rows ? old_rows : rows;
        copy_cells_resized(t->cells, old_cols, new_cells, cols, 0, 0, copy_rows,
                           copy_cols);
        copy_cells_via_perm(t->cells_alt, t->row_perm_alt, old_cols,
                            new_cells_alt, cols, 0, 0, copy_rows, copy_cols);
        /* Clamp the saved primary-screen cursor. */
        if (t->cursor_col_main >= cols)
            t->cursor_col_main = cols - 1;
        if (t->cursor_row_main >= rows)
            t->cursor_row_main = rows - 1;
    }

    /* Copy alt-screen buffer for non-alt-screen case. Alt buffer row_perm_alt
     * is always identity (reset on alt-screen entry). */
    if (!t->on_alt_screen) {
        int copy_rows = old_rows < rows ? old_rows : rows;
        copy_cells_resized(t->cells_alt, old_cols, new_cells_alt, cols, 0, 0,
                           copy_rows, copy_cols);
    }

    free(t->cells);
    free(t->cells_alt);
    free(t->row_perm);
    free(t->row_perm_alt);
    free(t->occ);
    free(t->occ_alt);
    free(t->wrap);
    free(t->wrap_alt);
    t->cells = new_cells;
    t->cells_alt = new_cells_alt;
    t->row_perm = new_perm;
    t->row_perm_alt = new_perm_alt;
    t->occ = new_occ;
    t->occ_alt = new_occ_alt;
    t->wrap = new_wrap;
    t->wrap_alt = new_wrap_alt;

    /* Adapt scrollback to the new column width (finalises the tail at the
     * old width and opens a new empty tail at the new width).  Existing
     * compressed chunks are preserved verbatim at their own widths. */
    if (cols != old_cols)
        sb_adapt_cols(t->sb, cols);

    t->scroll_offset = 0;
    t->sel_active = 0; /* resize invalidates virtual row coordinates */
    t->cols = cols;
    t->rows = rows;
    t->scroll_top = 0;
    t->scroll_bottom = rows - 1;
    clamp_cursor(t);
    pthread_mutex_unlock(&t->lock);
}

int terminal_cols(const Terminal *t) { return t->cols; }
int terminal_rows(const Terminal *t) { return t->rows; }
int terminal_app_cursor_keys(const Terminal *t) { return t->app_cursor_keys; }
int terminal_cursor_visible(const Terminal *t) { return t->cursor_visible; }
int terminal_on_alt_screen(const Terminal *t) { return t->on_alt_screen; }
int terminal_cursor_shape(const Terminal *t) { return t->cursor_shape; }
int terminal_bracketed_paste(const Terminal *t) { return t->bracketed_paste; }
int terminal_scroll_offset(const Terminal *t) { return t->scroll_offset; }

void terminal_cursor_pos(Terminal *term, int *col, int *row) {
    pthread_mutex_lock(&term->lock);
    *col = term->cursor_col;
    *row = term->cursor_row;
    pthread_mutex_unlock(&term->lock);
}
int terminal_sb_total_rows(const Terminal *t) { return t->sb->total_rows; }

void terminal_scroll_to_row(Terminal *t, int abs_row) {
    pthread_mutex_lock(&t->lock);
    int total = t->sb->total_rows;
    t->scroll_offset = total - abs_row;
    if (t->scroll_offset < 0)
        t->scroll_offset = 0;
    if (t->scroll_offset > total)
        t->scroll_offset = total;
    pthread_mutex_unlock(&t->lock);
}

void terminal_set_selection(Terminal *t, int c0, int r0, int c1, int r1) {
    t->sel_col0 = c0;
    t->sel_row0 = r0;
    t->sel_col1 = c1;
    t->sel_row1 = r1;
    t->sel_active = 1;
}

void terminal_clear_selection(Terminal *t) { t->sel_active = 0; }

static void sel_norm(const Terminal *t, int *c0, int *r0, int *c1, int *r1) {
    int ac = t->sel_col0, ar = t->sel_row0;
    int bc = t->sel_col1, br = t->sel_row1;
    if (ar > br || (ar == br && ac > bc)) {
        int tc = ac;
        ac = bc;
        bc = tc;
        int tr = ar;
        ar = br;
        br = tr;
    }
    *c0 = ac;
    *r0 = ar;
    *c1 = bc;
    *r1 = br;
}

int terminal_cell_selected(const Terminal *t, int col, int row) {
    if (!t->sel_active)
        return 0;
    int c0, r0, c1, r1;
    sel_norm(t, &c0, &r0, &c1, &r1);
    /* row is viewport-relative; convert to virtual (grid-relative, negative
     * = scrollback) so selection stays anchored to content on scroll. */
    int vrow = row - t->scroll_offset;
    if (vrow < r0 || vrow > r1)
        return 0;
    if (vrow == r0 && col < c0)
        return 0;
    if (vrow == r1 && col > c1)
        return 0;
    return 1;
}

/* r0/r1 are virtual rows (grid-relative; negative = scrollback). */
char *terminal_get_selected_text(Terminal *t) {
    if (!t->sel_active)
        return NULL;
    int c0, r0, c1, r1;
    sel_norm(t, &c0, &r0, &c1, &r1);

    int cols = t->cols;
    size_t bufsz = (size_t)(r1 - r0 + 1) * ((size_t)cols * UTF8_MAX_BYTES + 2);
    char *buf = malloc(bufsz);
    if (!buf)
        return NULL;

    Cell *row_buf = malloc((size_t)cols * sizeof(Cell));
    if (!row_buf) {
        free(buf);
        return NULL;
    }

    static const Cell BLANK_CELL = {
        .ch = ' ', .fg = COL_DEFAULT_FG, .bg = COL_DEFAULT_BG, .attrs = 0};

    pthread_mutex_lock(&t->lock);
    int sb_rows = t->sb->total_rows;
    char *p = buf;

    for (int vrow = r0; vrow <= r1; vrow++) {
        int from = (vrow == r0) ? c0 : 0;
        int to = (vrow == r1) ? c1 : cols - 1;

        if (vrow < 0) {
            /* Scrollback: virtual -1 = most recent sb row. */
            int abs_row = sb_rows + vrow;
            if (abs_row >= 0) {
                sb_get_row_into(t->sb, abs_row, row_buf, cols);
            } else {
                for (int c = 0; c < cols; c++)
                    row_buf[c] = BLANK_CELL;
            }
        } else if (vrow < t->rows) {
            memcpy(row_buf, grid_row(t, vrow), (size_t)cols * sizeof(Cell));
        } else {
            for (int c = 0; c < cols; c++)
                row_buf[c] = BLANK_CELL;
        }

        /* Strip trailing spaces from all but the last selected row. */
        if (vrow < r1) {
            while (to >= from && row_buf[to].ch == ' ')
                to--;
        }

        for (int col = from; col <= to; col++) {
            uint32_t cp = row_buf[col].ch;
            if (0 == cp)
                cp = ' ';
            if (cp < UTF8_CONT_BYTE_MIN) {
                *p++ = (char)cp;
            } else if (cp < UTF8_2BYTE_LIMIT) {
                *p++ = (char)(UTF8_2BYTE_LEAD_MIN | (cp >> 6));
                *p++ = (char)(UTF8_CONT_BYTE_MIN | (cp & UTF8_CONT_DATA_MASK));
            } else if (cp < UTF8_3BYTE_LIMIT) {
                *p++ = (char)(UTF8_3BYTE_LEAD_MIN | (cp >> 12));
                *p++ = (char)(UTF8_CONT_BYTE_MIN |
                              ((cp >> 6) & UTF8_CONT_DATA_MASK));
                *p++ = (char)(UTF8_CONT_BYTE_MIN | (cp & UTF8_CONT_DATA_MASK));
            } else {
                *p++ = (char)(UTF8_4BYTE_LEAD_MIN | (cp >> 18));
                *p++ = (char)(UTF8_CONT_BYTE_MIN |
                              ((cp >> 12) & UTF8_CONT_DATA_MASK));
                *p++ = (char)(UTF8_CONT_BYTE_MIN |
                              ((cp >> 6) & UTF8_CONT_DATA_MASK));
                *p++ = (char)(UTF8_CONT_BYTE_MIN | (cp & UTF8_CONT_DATA_MASK));
            }
        }
        if (vrow < r1)
            *p++ = '\n';
    }

    pthread_mutex_unlock(&t->lock);
    *p = '\0';
    free(row_buf);
    return buf;
}

void terminal_get_state(Terminal *t, Cell *cells_out, int *cursor_col,
                        int *cursor_row) {
    pthread_mutex_lock(&t->lock);
    if (t->scroll_offset > 0) {
        int cols = t->cols;
        int rows = t->rows;
        int so = t->scroll_offset;
        int total_sb = t->sb->total_rows;
        Cell blank = {
            .ch = ' ', .fg = COL_DEFAULT_FG, .bg = COL_DEFAULT_BG, .attrs = 0};

        int sb_start = total_sb - so;
        if (sb_start < 0)
            sb_start = 0;

        /* Fill visual rows 0..so-1 from stored scrollback rows.
         * Rows stored wider than cols are visually wrapped at column
         * boundary; narrower rows are blank-padded. */
        int vrow = 0;
        int sr = sb_start;
        int sr_off = 0;    /* column offset within current stored row */
        int cur_scols = 0; /* stored cols of current sb row */
        Cell *native_buf = NULL;
        int native_cap = 0;

        while (vrow < so && vrow < rows) {
            Cell *dest = cells_out + (size_t)vrow * cols;
            if (sr >= total_sb) {
                for (int c = 0; c < cols; c++)
                    dest[c] = blank;
                vrow++;
                continue;
            }
            if (0 == sr_off) {
                cur_scols = sb_get_row_stored_cols(t->sb, sr);
                if (cur_scols <= 0)
                    cur_scols = 1;
                if (cur_scols > native_cap) {
                    free(native_buf);
                    native_buf = malloc((size_t)cur_scols * sizeof(Cell));
                    if (!native_buf) {
                        /* OOM fallback: read truncated to cols. */
                        sb_get_row_into(t->sb, sr, dest, cols);
                        vrow++;
                        sr++;
                        continue;
                    }
                    native_cap = cur_scols;
                }
                sb_get_row_into(t->sb, sr, native_buf, cur_scols);
            }
            int chunk = cur_scols - sr_off;
            if (chunk > cols)
                chunk = cols;
            if (chunk > 0)
                memcpy(dest, native_buf + sr_off, (size_t)chunk * sizeof(Cell));
            for (int c = chunk; c < cols; c++)
                dest[c] = blank;
            sr_off += cols;
            vrow++;
            if (sr_off >= cur_scols) {
                sr++;
                sr_off = 0;
            }
        }
        free(native_buf);

        for (int r = so; r < rows; r++) {
            Cell *dest = cells_out + (size_t)r * cols;
            int live_row = r - so;
            if (live_row >= 0 && live_row < t->rows)
                memcpy(dest, grid_row(t, live_row),
                       (size_t)cols * sizeof(Cell));
            else
                for (int c = 0; c < cols; c++)
                    dest[c] = blank;
        }
        *cursor_col = -1;
        *cursor_row = -1;
    } else {
        for (int r = 0; r < t->rows; r++)
            memcpy(&cells_out[r * t->cols], grid_row(t, r),
                   (size_t)t->cols * sizeof(Cell));
        *cursor_col = t->cursor_col;
        *cursor_row = t->cursor_row;
    }
    pthread_mutex_unlock(&t->lock);
}

void terminal_scroll(Terminal *t, int delta) {
    pthread_mutex_lock(&t->lock);
    t->scroll_offset += delta;
    if (t->scroll_offset < 0)
        t->scroll_offset = 0;
    int total = t->sb->total_rows;
    if (t->scroll_offset > total)
        t->scroll_offset = total;
    pthread_mutex_unlock(&t->lock);
}

void terminal_scroll_bottom(Terminal *t) {
    pthread_mutex_lock(&t->lock);
    t->scroll_offset = 0;
    pthread_mutex_unlock(&t->lock);
}

void terminal_clear_scrollback(Terminal *t) {
    pthread_mutex_lock(&t->lock);
    sb_clear(t->sb);
    t->scroll_offset = 0;
    pthread_mutex_unlock(&t->lock);
}

void terminal_set_alt_screen_callback(Terminal *t, terminal_alt_screen_fn fn,
                                      void *arg) {
    t->alt_screen_cb = fn;
    t->alt_screen_cb_arg = arg;
}

int terminal_exit_code(const Terminal *t) {
    if (NULL == t)
        return -1;
    pthread_mutex_lock((pthread_mutex_t *)&t->lock);
    int code = t->last_exit_code;
    pthread_mutex_unlock((pthread_mutex_t *)&t->lock);
    return code;
}

void terminal_set_exit_code_callback(Terminal *t, terminal_exit_code_fn fn,
                                     void *arg) {
    if (NULL == t)
        return;
    t->exit_code_cb = fn;
    t->exit_code_cb_arg = arg;
}

void terminal_set_cwd_callback(Terminal *t, terminal_cwd_fn fn, void *arg) {
    if (NULL == t)
        return;
    t->cwd_cb = fn;
    t->cwd_cb_arg = arg;
}

void terminal_set_title_callback(Terminal *t, terminal_title_fn fn, void *arg) {
    if (NULL == t)
        return;
    t->title_cb = fn;
    t->title_cb_arg = arg;
}

void terminal_search_cancel(Terminal *t) {
    if (t && t->sb) {
        t->sb->search_cancel = 1;
        VT_LOG("search: cancel");
    }
}

/* Decode one UTF-8 codepoint from *p, advancing *p past it.
 * Returns 0 at end of string. */
static uint32_t utf8_decode_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    if (0 == *s)
        return 0;
    uint32_t cp;
    if (*s < UTF8_2BYTE_LEAD_MIN) {
        cp = *s++;
    } else if (*s < UTF8_3BYTE_LEAD_MIN) {
        cp = (*s++ & UTF8_2BYTE_DATA_MASK) << 6;
        cp |= (*s++ & UTF8_CONT_DATA_MASK);
    } else if (*s < UTF8_4BYTE_LEAD_MIN) {
        cp = (*s++ & UTF8_3BYTE_DATA_MASK) << 12;
        cp |= (*s++ & UTF8_CONT_DATA_MASK) << 6;
        cp |= (*s++ & UTF8_CONT_DATA_MASK);
    } else {
        cp = (*s++ & UTF8_4BYTE_DATA_MASK) << 18;
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
                             const uint32_t *qcps, int qlen) {
    if (cells_avail < qlen)
        return 0;
    for (int i = 0; i < qlen; i++) {
        if (row[i].ch != qcps[i])
            return 0;
    }
    return 1;
}

/* SWAR helpers, always compiled so tests can reach row_swar_scan on any
 * arch. */
#define SWAR_ONES 0x0101010101010101ULL
#define SWAR_HIGHS 0x8080808080808080ULL

static int swar_has_match(uint64_t v) {
    return ((v - SWAR_ONES) & ~v & SWAR_HIGHS) != 0;
}

/* Frequency weights for terminal/log content: lower = rarer = better pivot.
 * Zero entries fall back to a default of 5. */
static const uint8_t FREQ_TABLE[256] = {
    [' '] = 150, ['e'] = 127, ['t'] = 91, ['a'] = 82, ['o'] = 75, ['i'] = 70,
    ['n'] = 68,  ['s'] = 63,  ['r'] = 60, ['h'] = 50, ['l'] = 40, ['d'] = 38,
    ['c'] = 28,  ['u'] = 27,  ['m'] = 24, ['f'] = 22, ['p'] = 19, ['g'] = 18,
    ['w'] = 15,  ['y'] = 15,  ['b'] = 13, ['v'] = 10, ['k'] = 8,  ['['] = 50,
    [']'] = 50,  [':'] = 40,  ['-'] = 40, ['.'] = 30, ['x'] = 2,  ['j'] = 1,
    ['q'] = 1,   ['z'] = 1,
};

/* Returns the index of the rarest (lowest-frequency) codepoint in qcps. */
static int get_rarest_idx(const uint32_t *qcps, int qlen) {
    int rarest = 0;
    uint8_t min_freq = 255;
    for (int i = 0; i < qlen; i++) {
        uint8_t f;
        if (qcps[i] > 0xFFu) {
            f = 1;
        } else {
            f = FREQ_TABLE[qcps[i]];
            if (0 == f)
                f = 5;
        }
        if (f < min_freq) {
            min_freq = f;
            rarest = i;
        }
    }
    return rarest;
}

/* SWAR scan with rarest-character pivot.
 * Picks the lowest-frequency codepoint in qcps as the anchor, scans for it
 * using SWAR zero-detection, then verifies full matches via cells_match_query.
 * False positives (ch > 0xFF, same low byte) are rejected by the verifier. */
static int row_swar_scan_impl(const Cell *row, int cols, const uint32_t *qcps,
                              int qlen, int pivot_off) {
    int offset = pivot_off;
    uint8_t lo = (uint8_t)(qcps[offset] & 0xFFu);
    uint64_t broadcast = SWAR_ONES * lo;
    /* anchor_row[j] == row[offset + j]; hit at j -> full match starts at
     * row[j]. */
    const Cell *anchor_row = row + offset;
    int anchor_end = cols - qlen + 1; /* exclusive upper bound */
    int i = 0;
    for (; i + 8 <= anchor_end; i += 8) {
        uint64_t word = (uint64_t)((uint8_t)anchor_row[i + 0].ch) |
                        (uint64_t)((uint8_t)anchor_row[i + 1].ch) << 8 |
                        (uint64_t)((uint8_t)anchor_row[i + 2].ch) << 16 |
                        (uint64_t)((uint8_t)anchor_row[i + 3].ch) << 24 |
                        (uint64_t)((uint8_t)anchor_row[i + 4].ch) << 32 |
                        (uint64_t)((uint8_t)anchor_row[i + 5].ch) << 40 |
                        (uint64_t)((uint8_t)anchor_row[i + 6].ch) << 48 |
                        (uint64_t)((uint8_t)anchor_row[i + 7].ch) << 56;
        if (swar_has_match(word ^ broadcast)) {
            for (int k = 0; k < 8; k++) {
                if ((uint8_t)anchor_row[i + k].ch == lo &&
                    cells_match_query(&row[i + k], cols - (i + k), qcps, qlen))
                    return 1;
            }
        }
    }
    for (; i < anchor_end; i++) {
        if (anchor_row[i].ch == qcps[offset] &&
            cells_match_query(&row[i], cols - i, qcps, qlen))
            return 1;
    }
    return 0;
}

/* Public wrapper, computes pivot on behalf of callers (e.g. tests). */
int row_swar_scan(const Cell *row, int cols, const uint32_t *qcps, int qlen) {
    return row_swar_scan_impl(row, cols, qcps, qlen,
                              get_rarest_idx(qcps, qlen));
}

/* Each find_anchor_* scans anchor_row[0..anchor_end) for cells whose .ch
 * field equals `anchor`, recording the position (relative to anchor_row)
 * of each match into hits[0..cap).  Returns the number of hits written.
 * The scalar tail (positions not covered by the SIMD stride) is left to
 * the caller so it runs exactly once regardless of SIMD path. */

#if defined(__aarch64__)
static int find_anchor_neon(const uint32_t *cps, int len, uint32_t anchor,
                            uint8_t *hits, int cap) {
    uint32x4_t needle = vdupq_n_u32(anchor);
    int n = 0;
    int i = 0;
    for (; i + 4 <= len; i += 4) {
        /* cps is cast from Cell *; .ch is the first uint32 per cell.
         * Cell is 16 bytes = 4 uint32_t words, so consecutive .ch fields
         * sit 4 words apart.  Gather them manually into a lane vector. */
        uint32_t cv[4];
        cv[0] = cps[(i + 0) * 4];
        cv[1] = cps[(i + 1) * 4];
        cv[2] = cps[(i + 2) * 4];
        cv[3] = cps[(i + 3) * 4];
        uint32x4_t v = vld1q_u32(cv);
        uint32x4_t cmp = vceqq_u32(v, needle);
        if (vmaxvq_u32(cmp)) {
            uint32_t lanes[4];
            vst1q_u32(lanes, cmp);
            for (int k = 0; k < 4; k++) {
                if (lanes[k] && n < cap)
                    hits[n++] = (uint8_t)(i + k);
            }
        }
    }
    return n;
}
#elif defined(__x86_64__) && PHANTOM_HAVE_AVX2
static int find_anchor_avx2(const uint32_t *cps, int len, uint32_t anchor,
                            uint8_t *hits, int cap) {
    __m256i needle = _mm256_set1_epi32((int32_t)anchor);
    /* Each Cell is 16 bytes; .ch is the first int32.  The gather index
     * steps by 4 int32 words (= 16 bytes = 1 Cell). */
    __m256i idx = _mm256_set_epi32(28, 24, 20, 16, 12, 8, 4, 0);
    int n = 0;
    int i = 0;
    for (; i + 8 <= len; i += 8) {
        __m256i gathered =
            _mm256_i32gather_epi32((const int *)(cps + i * 4), idx, 4);
        __m256i cmp = _mm256_cmpeq_epi32(gathered, needle);
        int mask = _mm256_movemask_ps(_mm256_castsi256_ps(cmp));
        if (mask) {
            for (int k = 0; k < 8; k++) {
                if (((mask >> k) & 1) && n < cap)
                    hits[n++] = (uint8_t)(i + k);
            }
        }
    }
    return n;
}
#endif

static int find_anchor_scalar(const uint32_t *cps, int len, uint32_t anchor,
                              uint8_t *hits, int cap) __attribute__((unused));
static int find_anchor_scalar(const uint32_t *cps, int len, uint32_t anchor,
                              uint8_t *hits, int cap) {
    int n = 0;
    for (int i = 0; i < len; i++) {
        if (cps[i * 4] == anchor && n < cap)
            hits[n++] = (uint8_t)i;
    }
    return n;
}

/* Scan one row for the query.  Returns 1 if found. */
static int row_contains_query(const Cell *row, int cols, const uint32_t *qcps,
                              int qlen, int pivot_off) {
    if (0 == qlen)
        return 0;

    int offset = pivot_off;
    uint32_t pivot = qcps[offset];
    /* anchor_row[j].ch corresponds to (row + offset + j)->ch.
     * The .ch field is the first uint32 in Cell. */
    const Cell *anchor_row = row + offset;
    int anchor_end = cols - qlen + 1; /* exclusive upper bound */

    /* SIMD anchor-scan covers [0, anchor_end) in strides; the scalar tail
     * below covers the remainder unconditionally. */
#if defined(__aarch64__)
    {
        uint8_t hits[256];
        int nhits = find_anchor_neon((const uint32_t *)anchor_row,
                                     (anchor_end > 0) ? (anchor_end & ~3) : 0,
                                     pivot, hits, 256);
        for (int h = 0; h < nhits; h++) {
            int pos = (int)hits[h];
            if (cells_match_query(&row[pos], cols - pos, qcps, qlen))
                return 1;
        }
        /* Scalar tail for positions not covered by the SIMD stride. */
        for (int i = anchor_end & ~3; i < anchor_end; i++) {
            if (anchor_row[i].ch == pivot &&
                cells_match_query(&row[i], cols - i, qcps, qlen))
                return 1;
        }
        return 0;
    }
#elif defined(__x86_64__) && PHANTOM_HAVE_AVX2
    {
        uint8_t hits[256];
        int nhits = find_anchor_avx2((const uint32_t *)anchor_row,
                                     (anchor_end > 0) ? (anchor_end & ~7) : 0,
                                     pivot, hits, 256);
        for (int h = 0; h < nhits; h++) {
            int pos = (int)hits[h];
            if (cells_match_query(&row[pos], cols - pos, qcps, qlen))
                return 1;
        }
        /* Scalar tail for positions not covered by the SIMD stride. */
        for (int i = anchor_end & ~7; i < anchor_end; i++) {
            if (anchor_row[i].ch == pivot &&
                cells_match_query(&row[i], cols - i, qcps, qlen))
                return 1;
        }
        return 0;
    }
#else
    return row_swar_scan_impl(row, cols, qcps, qlen, pivot_off);
#endif
}

#if defined(__APPLE__)
typedef struct {
    SbChunk *snap;
    int n_chunks;
    Cell *tail_snap;
    int tail_count;
    int tail_base;
    int cols;
    uint64_t tail_bloom;
    uint64_t qbloom;
    Cell *live_snap;
    int live_rows;
    int live_cols;
    int live_base;
    SbRawChunk *rq_snap;
    int rq_snap_count;
    SbStore *sb;
    uint32_t *qcps;
    int qlen;
    int pivot_off;
    terminal_search_result_fn result_cb;
    terminal_search_done_fn done_cb;
    void *arg;
} SearchAsyncCtx;

static void search_chunk_worker(void *ctx, size_t ci) {
    SearchAsyncCtx *s = (SearchAsyncCtx *)ctx;
    if (s->sb->search_cancel)
        return;
    SbChunk *ck = &s->snap[ci];
    if ((ck->bloom & s->qbloom) != s->qbloom)
        return;
    size_t raw_sz = (size_t)(ck->row_count * ck->cols) * sizeof(Cell);
    static _Thread_local Cell *thread_scratch = NULL;
    static _Thread_local size_t thread_scratch_sz = 0;
    if (raw_sz > thread_scratch_sz) {
        Cell *p = realloc(thread_scratch, raw_sz > 0 ? raw_sz : 1);
        if (!p)
            return;
        thread_scratch = p;
        thread_scratch_sz = raw_sz;
    }
    if (ck->is_raw) {
        memcpy(thread_scratch, ck->buf->data, raw_sz);
    } else {
        LZ4_decompress_safe((const char *)ck->buf->data, (char *)thread_scratch,
                            (int)ck->buf->data_sz, (int)raw_sz);
    }
    for (int r = 0; r < ck->row_count && !s->sb->search_cancel; r++) {
        if (row_contains_query(thread_scratch + (size_t)r * ck->cols, ck->cols,
                               s->qcps, s->qlen, s->pivot_off))
            s->result_cb(ck->row_base + r, s->arg);
    }
}

static void search_async_worker(void *ctx) {
    SearchAsyncCtx *s = (SearchAsyncCtx *)ctx;
    for (int r = 0; r < s->live_rows && !s->sb->search_cancel; r++) {
        if (s->live_snap &&
            row_contains_query(&s->live_snap[(size_t)r * s->live_cols],
                               s->live_cols, s->qcps, s->qlen, s->pivot_off))
            s->result_cb(s->live_base + r, s->arg);
    }
    if ((s->tail_bloom & s->qbloom) == s->qbloom) {
        for (int r = 0; r < s->tail_count && !s->sb->search_cancel; r++) {
            if (row_contains_query(&s->tail_snap[(size_t)r * s->cols], s->cols,
                                   s->qcps, s->qlen, s->pivot_off))
                s->result_cb(s->tail_base + r, s->arg);
        }
    }
    for (int qi = 0; qi < s->rq_snap_count && !s->sb->search_cancel; qi++) {
        SbRawChunk *rq = &s->rq_snap[qi];
        if (!rq->cells || (rq->bloom & s->qbloom) != s->qbloom)
            continue;
        for (int r = 0; r < rq->row_count && !s->sb->search_cancel; r++) {
            if (row_contains_query(&rq->cells[(size_t)r * rq->cols], rq->cols,
                                   s->qcps, s->qlen, s->pivot_off))
                s->result_cb(rq->row_base + r, s->arg);
        }
    }
    dispatch_apply_f((size_t)s->n_chunks,
                     dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), s,
                     search_chunk_worker);
    for (int i = 0; i < s->n_chunks; i++)
        sb_chunk_buf_release(s->snap[i].buf);
    free(s->snap);
    free(s->tail_snap);
    free(s->live_snap);
    for (int i = 0; i < s->rq_snap_count; i++)
        free(s->rq_snap[i].cells);
    free(s->rq_snap);
    free(s->qcps);
    if (s->done_cb)
        s->done_cb(s->arg);
    dispatch_group_leave(s->sb->search_group);
    free(s);
}
#endif /* defined(__APPLE__) */

void terminal_search(Terminal *t, const char *query,
                     terminal_search_result_fn cb,
                     terminal_search_done_fn done_cb, void *arg) {
    if (!query || '\0' == query[0] || !cb)
        return;

    uint32_t qcps[256]; /* stack-local; 256 codepoints max */
    int qlen = 0;
    uint64_t qbloom = 0;
    const char *p = query;
    uint32_t cp;
    while (qlen < 256 && (cp = utf8_decode_next(&p)) != 0) {
        qcps[qlen++] = cp;
        qbloom |= sb_bloom_bits(cp);
    }
    if (0 == qlen)
        return;

    terminal_search_cancel(t);

    pthread_mutex_lock(&t->lock);
    t->sb->search_cancel = 0;
    int cols = t->sb->cols;

    /* chunk_mu guards chunks[] (comp thread appends without t->lock).
     * Addref each SbChunkBuf instead of deep-copying to avoid an
     * O(scrollback_bytes) memcpy on the main thread. */
    pthread_rwlock_rdlock(&t->sb->chunk_mu);
    int n_chunks = t->sb->chunk_count;
    SbChunk *snap = NULL;
    if (n_chunks > 0) {
        snap = malloc((size_t)n_chunks * sizeof(SbChunk));
        if (snap) {
            memcpy(snap, t->sb->chunks, (size_t)n_chunks * sizeof(SbChunk));
            for (int i = 0; i < n_chunks; i++) {
                if (snap[i].buf)
                    atomic_fetch_add_explicit(&snap[i].buf->refs, 1,
                                              memory_order_relaxed);
            }
        }
    }
    pthread_rwlock_unlock(&t->sb->chunk_mu);

    /* Snapshot raw queue entries (flushed from tail, not yet compressed). */
    int rq_snap_count = 0;
    SbRawChunk *rq_snap = NULL;
    pthread_mutex_lock(&t->sb->comp_mu);
    rq_snap_count = t->sb->rq_count;
    if (rq_snap_count > 0) {
        rq_snap = malloc((size_t)rq_snap_count * sizeof(SbRawChunk));
        if (rq_snap) {
            for (int i = 0; i < rq_snap_count; i++) {
                int idx = (t->sb->rq_head + i) % SB_RAW_QUEUE_CAP;
                SbRawChunk *rq = &t->sb->raw_queue[idx];
                size_t sz = (size_t)(rq->row_count * rq->cols) * sizeof(Cell);
                rq_snap[i] = *rq;
                rq_snap[i].cells = malloc(sz);
                if (rq_snap[i].cells)
                    memcpy(rq_snap[i].cells, rq->cells, sz);
            }
        }
    }
    pthread_mutex_unlock(&t->sb->comp_mu);

    pthread_mutex_unlock(&t->lock);

    if (n_chunks > 0 && !snap) {
        for (int i = 0; i < rq_snap_count; i++)
            free(rq_snap[i].cells);
        free(rq_snap);
        return;
    }

    int tail_count = 0;
    uint64_t tail_bloom = 0;
    Cell *tail_snap = NULL;
    Cell *live_snap = NULL;
    int live_rows = 0;
    int live_cols = 0;
    int live_base = 0;
    pthread_mutex_lock(&t->lock);
    tail_count = t->sb->tail_count;
    tail_bloom = t->sb->tail_bloom;
    if (tail_count > 0) {
        size_t tail_sz = (size_t)(tail_count * cols) * sizeof(Cell);
        tail_snap = malloc(tail_sz);
        if (tail_snap)
            memcpy(tail_snap, t->sb->tail_raw, tail_sz);
    }
    int tail_base = sb_tail_base(t->sb);
    live_rows = t->rows;
    live_cols = t->cols;
    live_base = t->sb->total_rows;
    if (live_rows > 0 && live_cols > 0) {
        size_t live_sz = (size_t)(live_rows * live_cols) * sizeof(Cell);
        live_snap = malloc(live_sz);
        if (live_snap) {
            /* Snapshot in logical row order (via row_perm) so abs_row =
             * live_base + logical_row matches how the renderer maps rows. */
            for (int r = 0; r < live_rows; r++)
                memcpy(&live_snap[(size_t)r * live_cols],
                       &t->cells[(size_t)t->row_perm[r] * live_cols],
                       (size_t)live_cols * sizeof(Cell));
        }
    }
    pthread_mutex_unlock(&t->lock);

    VT_LOG("search: start query=\"%.48s\" chunks=%d tail=%d live=%d", query,
           n_chunks, tail_count, live_rows);

#if defined(__APPLE__)
    SearchAsyncCtx *s = malloc(sizeof *s);
    uint32_t *hqcps = s ? malloc((size_t)qlen * sizeof(uint32_t)) : NULL;
    if (!s || !hqcps) {
        free(s);
        free(hqcps);
        free(snap);
        free(tail_snap);
        free(live_snap);
        for (int i = 0; i < rq_snap_count; i++)
            free(rq_snap[i].cells);
        free(rq_snap);
        return;
    }
    memcpy(hqcps, qcps, (size_t)qlen * sizeof(uint32_t));
    *s = (SearchAsyncCtx){
        .snap = snap,
        .n_chunks = n_chunks,
        .tail_snap = tail_snap,
        .tail_count = tail_count,
        .tail_base = tail_base,
        .cols = cols,
        .tail_bloom = tail_bloom,
        .qbloom = qbloom,
        .live_snap = live_snap,
        .live_rows = live_rows,
        .live_cols = live_cols,
        .live_base = live_base,
        .rq_snap = rq_snap,
        .rq_snap_count = rq_snap_count,
        .sb = t->sb,
        .qcps = hqcps,
        .qlen = qlen,
        .pivot_off = get_rarest_idx(hqcps, qlen),
        .result_cb = cb,
        .done_cb = done_cb,
        .arg = arg,
    };
    dispatch_group_enter(t->sb->search_group);
    dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), s,
                     search_async_worker);
    return;
#else
    uint32_t *bqcps = malloc((size_t)qlen * sizeof(uint32_t));
    if (!bqcps) {
        free(tail_snap);
        free(live_snap);
        for (int i = 0; i < rq_snap_count; i++)
            free(rq_snap[i].cells);
        free(rq_snap);
        if (snap) {
            for (int i = 0; i < n_chunks; i++)
                sb_chunk_buf_release(snap[i].buf);
            free(snap);
        }
        return;
    }
    memcpy(bqcps, qcps, (size_t)qlen * sizeof(uint32_t));

    CoordArgs *ca = malloc(sizeof *ca);
    if (!ca) {
        free(bqcps);
        free(tail_snap);
        free(live_snap);
        for (int i = 0; i < rq_snap_count; i++)
            free(rq_snap[i].cells);
        free(rq_snap);
        if (snap) {
            for (int i = 0; i < n_chunks; i++)
                sb_chunk_buf_release(snap[i].buf);
            free(snap);
        }
        return;
    }
    ca->sb = t->sb;
    ca->snap = snap;
    ca->n_chunks = n_chunks;
    ca->tail_snap = tail_snap;
    ca->tail_count = tail_count;
    ca->tail_bloom = tail_bloom;
    ca->tail_base = tail_base;
    ca->live_snap = live_snap;
    ca->live_rows = live_rows;
    ca->live_cols = live_cols;
    ca->live_base = live_base;
    ca->qcps = bqcps;
    ca->qlen = qlen;
    ca->pivot_off = get_rarest_idx(bqcps, qlen);
    ca->qbloom = qbloom;
    ca->cb = cb;
    ca->done_cb = done_cb;
    ca->arg = arg;
    ca->cols = cols;
    ca->rq_snap = rq_snap;
    ca->rq_snap_count = rq_snap_count;

    pthread_mutex_lock(&t->sb->search_mu);
    t->sb->search_active++;
    pthread_mutex_unlock(&t->sb->search_mu);

    pthread_t coord;
    if (0 != pthread_create(&coord, NULL, search_coord_fn, ca)) {
        pthread_mutex_lock(&t->sb->search_mu);
        t->sb->search_active--;
        pthread_mutex_unlock(&t->sb->search_mu);
        for (int r = 0; r < live_rows && live_snap; r++) {
            if (row_contains_query(&live_snap[r * live_cols], live_cols, qcps,
                                   qlen, ca->pivot_off))
                cb(live_base + r, arg);
        }
        free(bqcps);
        free(ca);
        free(tail_snap);
        free(live_snap);
        for (int i = 0; i < rq_snap_count; i++)
            free(rq_snap[i].cells);
        free(rq_snap);
        if (snap) {
            for (int i = 0; i < n_chunks; i++)
                sb_chunk_buf_release(snap[i].buf);
            free(snap);
        }
        return;
    }
    pthread_detach(coord);
#endif
}
