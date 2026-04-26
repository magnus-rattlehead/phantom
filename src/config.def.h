/* Phantom configuration  -  copy to config.h and edit to customize.
 * Rebuild after any change: cmake --build build -j$(sysctl -n hw.ncpu)
 */
#pragma once

/* Font paths.  NULL -> cmake auto-detects a system font.
 * Set to an absolute path to override for that face. */
static const char *FONT = NULL;             /* regular              */
static const char *FONT_BOLD = NULL;        /* bold                 */
static const char *FONT_ITALIC = NULL;      /* italic */
static const char *FONT_BOLD_ITALIC = NULL; /* bold italic          */
static const float FONT_SIZE = 16.0f;

/* Initial window size in device points (not pixels). */
#define WIN_INITIAL_W 1320
#define WIN_INITIAL_H 800

/* Fallback terminal size when font metrics are unavailable at startup. */
#define TERM_COLS_FALLBACK 80
#define TERM_ROWS_FALLBACK 24

/* Scrollback chunk size and decompression cache depth. */
#define SB_CHUNK_ROWS 64  /* rows per LZFSE-compressed scrollback chunk */
#define SB_DECOMP_CACHE 4 /* decompressed chunks held in LRU cache */

/* Mouse wheel scroll speed (lines per tick). */
#define SCROLL_LINES_PER_TICK 3

/* $TERM and $COLORTERM exported to the child shell process. */
#define PTY_TERM "xterm-256color"
#define PTY_COLORTERM "truecolor"

/* ── Colorscheme ─────────────────────────────────────────────────────────── */

/* ANSI 16-color palette (RGBA).  COLOR_0..7 = normal, COLOR_8..15 = bright. */
#define COLOR_0 0x282828FF  /* black          */
#define COLOR_1 0xCC241DFF  /* red            */
#define COLOR_2 0x98971AFF  /* green          */
#define COLOR_3 0xD79921FF  /* yellow         */
#define COLOR_4 0x458588FF  /* blue           */
#define COLOR_5 0xB16286FF  /* magenta        */
#define COLOR_6 0x689D6AFF  /* cyan           */
#define COLOR_7 0xA89984FF  /* white          */
#define COLOR_8 0x928374FF  /* bright black   */
#define COLOR_9 0xFB4934FF  /* bright red     */
#define COLOR_10 0xB8BB26FF /* bright green   */
#define COLOR_11 0xFABD2FFF /* bright yellow  */
#define COLOR_12 0x83A598FF /* bright blue    */
#define COLOR_13 0xD3869BFF /* bright magenta */
#define COLOR_14 0x8EC07CFF /* bright cyan    */
#define COLOR_15 0xEBDBB2FF /* bright white   */

/* Terminal default fg/bg  -  aliased to ANSI palette entries. */
#define TERM_DEFAULT_BG COLOR_0
#define TERM_DEFAULT_FG COLOR_15

/* ── Autocomplete / LLM ──────────────────────────────────────────────────── */

/* Base model for FIM command completions (no -instruct suffix).
 * Set to "" to disable LLM command completion. */
#define LLM_BASE_MODEL_PATH "models/Qwen2.5-Coder-7B-Q8_0.gguf"

/* Instruct model for # natural-language queries (e.g. # rename all .txt).
 * Set to "" to disable NL query support. */
#define LLM_INSTRUCT_MODEL_PATH "models/Qwen2.5-Coder-7B-Instruct-Q8_0.gguf"

/* GPU layers to offload to Metal (-1 = all, 0 = CPU only). */
#define LLM_GPU_LAYERS (-1)

/* Max new tokens generated per completion request. */
#define AC_MAX_TOKENS 12

/* Max tokens for NL (#-prefix) query generation. */
#define AC_NL_MAX_TOKENS 128

/* Milliseconds of keystroke silence before inference fires. */
#define AC_DEBOUNCE_MS 20

/* Recent commands included in the prompt context. */
#define AC_HISTORY_ENTRIES 10

/* ── Context / environmental probe ──────────────────────────────────────── */

/* Max bytes of visible-screen text included in the LLM context snapshot. */
#define CONTEXT_SB_BYTES 2048

/* Filesystem map sent to the LLM: recursion depth and byte cap. */
#define CONTEXT_FS_DEPTH 3
#define CONTEXT_FS_BYTES 2048

/* Flat ls-style listing of CWD injected immediately before [INPUT]: */
#define CONTEXT_CWD_LISTING_BYTES 256

/* ── History Trie (HTrie) ──────────────────────────────────────────────── */
#define HTRIE_MIN_FREQ 2
#define HTRIE_MIN_PREFIX 2

/* ── Command-Context Graph (CCG) ─────────────────────────────────────── */

/* Train a command into the CCG every N commands; triggers graph prune. */
#define CCG_PRUNE_INTERVAL 1000

/* ── Debug logging ───────────────────────────────────────────────────── */

/* Log raw bytes fed to terminal_feed() (very verbose  -  debug only).
 * Has no effect unless PHANTOM_DEBUG=1 is also set (debug build). */
#define LOG_BUFFER 0
