/* Phantom configuration  -  copy to config.h and edit to customize. */
#pragma once

/* NULL -> cmake auto-detects a system font; absolute path overrides. */
#define FONT NULL
#define FONT_BOLD NULL
#define FONT_ITALIC NULL
#define FONT_BOLD_ITALIC NULL
#define FONT_SIZE 18.0f
#define LINE_HEIGHT 1.2f

/* Device points, not pixels (HiDPI-aware). */
#define WIN_INITIAL_W 1320
#define WIN_INITIAL_H 800

/* Fallback when font metrics are unavailable at startup. */
#define TERM_COLS_FALLBACK 80
#define TERM_ROWS_FALLBACK 24

#define SB_CHUNK_ROWS 64  /* rows per LZ4-compressed scrollback chunk */
#define SB_DECOMP_CACHE 4 /* decompressed chunks held in LRU cache */

#define SCROLL_LINES_PER_TICK 3

/* Exported to the child shell process. */
#define PTY_TERM "xterm-256color"
#define PTY_COLORTERM "truecolor"

/* --- Colorscheme --------------------------------------------------------- */

/* ANSI 16-color palette (RGBA).  COLOR_0..7 = normal, COLOR_8..15 = bright. */
#define COLOR_0 0x282828FF
#define COLOR_1 0xCC241DFF
#define COLOR_2 0x98971AFF
#define COLOR_3 0xD79921FF
#define COLOR_4 0x458588FF
#define COLOR_5 0xB16286FF
#define COLOR_6 0x689D6AFF
#define COLOR_7 0xA89984FF
#define COLOR_8 0x928374FF
#define COLOR_9 0xFB4934FF
#define COLOR_10 0xB8BB26FF
#define COLOR_11 0xFABD2FFF
#define COLOR_12 0x83A598FF
#define COLOR_13 0xD3869BFF
#define COLOR_14 0x8EC07CFF
#define COLOR_15 0xEBDBB2FF

/* Aliased to palette entries above. */
#define TERM_DEFAULT_BG COLOR_0
#define TERM_DEFAULT_FG COLOR_15

/* CURSOR_THICKNESS: beam/underline width in px. */
#define CURSOR_THICKNESS 2

/* --- LLM warmup (Metal pipeline pre-heating) ----------------------------- */
#define LLM_WARMUP_HIST_TOKS 192
#define LLM_WARMUP_SUF_MIN 3
#define LLM_WARMUP_SUF_MAX 8
#define LLM_WARMUP_DUMMY_TOKS 4

/* --- Autocomplete / LLM -------------------------------------------------- */

/* FIM (fill-in-middle) base model; set to "" to disable. */
#define LLM_BASE_MODEL_PATH "models/Qwen2.5-Coder-7B-Q8_0.gguf"

/* Instruct model for #-prefix natural-language queries; set to "" to disable.
 * Example: "# rename all .txt" */
#define LLM_INSTRUCT_MODEL_PATH "models/Qwen2.5-Coder-7B-Instruct-Q8_0.gguf"

/* -1 = all layers on Metal, 0 = CPU only. */
#define LLM_GPU_LAYERS (-1)

#define AC_MAX_TOKENS 12
#define AC_NL_MAX_TOKENS 128
#define AC_DEBOUNCE_MS 20 /* keystroke silence before inference fires */
#define AC_HISTORY_ENTRIES 10

/* --- Context / environmental probe --------------------------------------- */

#define CONTEXT_SB_BYTES                                                       \
    2048 /* visible-screen text in LLM context snapshot                        \
          */

#define CONTEXT_FS_DEPTH 3
#define CONTEXT_FS_BYTES                                                       \
    2048 /* filesystem map cap; happens to equal CONTEXT_SB_BYTES */

/* ls-style CWD listing injected immediately before [INPUT]: */
#define CONTEXT_CWD_LISTING_BYTES 256

/* --- History Trie (HTrie) ------------------------------------------------ */
#define HTRIE_MIN_FREQ 2
#define HTRIE_MIN_PREFIX 2

/* --- Command-Context Graph (CCG) ----------------------------------------- */

/* Every N commands: train into CCG and trigger graph prune. */
#define CCG_PRUNE_INTERVAL 1000

/* --- Debug logging -------------------------------------------------------- */

/* Log raw bytes fed to terminal_feed().  No effect without PHANTOM_DEBUG=1. */
#define LOG_BUFFER 0
