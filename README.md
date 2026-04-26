# phantom

A GPU-accelerated terminal emulator for macOS+Linux with LLM-powered shell command autocomplete.

Built on SDL3 + OpenGL 3.3.
Inference runs on Metal via llama.cpp.

I started this project about a year ago to learn graphics programming and never intended to go
beyond that. After trying the Warp terminal, I really grew to love its autocomplete suggestions,
but hated that it wasn't Open Source (and its text buffer search implementation is extremely slow).
So I came back early this year to try and expand my initial project into something more. To be honest 
the LLM integration was definitely over my head, I went back and forth with Gemini to help me with it.
The ARM64 SIMD search was also written with the help of Gemini, as I am not familiar with ARM64 instructions.

---

## Features

- VT100/ANSI/xterm-256color with truecolor
- LZFSE/zlib-compressed infinite scrollback
- Incremental search (AVX2 on x86_64, NEON on ARM64, SWAR fallback; rarest-character pivot)
- Autocomplete: CCG, history trie, schema expert, FS completion, Makefile/npm targets, LLM FIM

---

## Requirements

macOS or Linux, CMake ≥ 3.25, SDL3.

```sh
xcode-select --install   # macOS
brew install cmake sdl3
```

### Models (optional)

Two GGUF models in `models/`. Defaults in `config.h`:

| Key | Default |
|---|---|
| `LLM_BASE_MODEL_PATH` | `models/qwen2.5-coder-7b-q8_0.gguf` |
| `LLM_INSTRUCT_MODEL_PATH` | `models/qwen2.5-coder-7b-instruct-q8_0.gguf` |

Set to `""` to disable. Works without models; falls back to history and schema experts.

---

## Build

```sh
git clone --recurse-submodules https://github.com/magnus-rattlehead/phantom
cd phantom
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)
./build/phantom
```

Debug (enables stderr logging):

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(sysctl -n hw.ncpu)
```

### macOS app bundle

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DPHANTOM_MACOS_BUNDLE=ON
cmake --build build -j$(sysctl -n hw.ncpu)
open build/phantom.app
```

Font paths are baked in at compile time from system font detection, so the bundle works on the build machine. For distribution to other machines, embed dylibs with `dylibbundler` and ensure model paths in `config.h` are absolute.

### GPU inference on Linux

CUDA (NVIDIA) or Vulkan (cross-vendor) can be enabled at configure time:

```sh
cmake -B build -DPHANTOM_CUDA=ON    # NVIDIA  -  requires CUDA toolkit
cmake -B build -DPHANTOM_VULKAN=ON  # AMD/Intel/NVIDIA  -  requires Vulkan SDK
```

---

## Configuration

Copy `src/config.def.h` → `src/config.h` and edit. CMake does this on first configure.

| Constant | Default | Description |
|---|---|---|
| `FONT` | `NULL` | Regular font TTF; `NULL` auto-detects, falls back to bundled JetBrains Mono |
| `FONT_SIZE` | `16.0` | Font size in points |
| `WIN_INITIAL_W/H` | `1320 × 800` | Initial window size |
| `SCROLL_LINES_PER_TICK` | `3` | Mouse wheel speed |
| `SB_CHUNK_ROWS` | `64` | Rows per compressed scrollback chunk |
| `LLM_GPU_LAYERS` | `-1` | Metal layers to offload (`-1` = all, `0` = CPU only) |
| `AC_MAX_TOKENS` | `12` | Max tokens per inline completion |
| `AC_HISTORY_ENTRIES` | `10` | Recent commands in LLM prompt |
| `HTRIE_MIN_FREQ` | `2` | Min frequency for trie instant completion |

Colors are `COLOR_0`–`COLOR_15` (RGBA hex), standard ANSI layout. Defaults: Gruvbox Dark.

---

## Key bindings

| Key | Action |
|---|---|
| Tab | Accept autocomplete suggestion |
| Escape | Dismiss suggestion |
| Cmd+F | Open search |
| Enter / n | Next search result |
| Shift+N | Previous result |
| Escape | Close search |
| Mouse drag / release | Select + copy to clipboard |
| Mouse wheel | Scroll scrollback |
| Cmd+T | New tab |
| Cmd+W | Close tab |
| Cmd+1–9 | Switch tab |

---

## Architecture

```
phantom/
├── src/
│   ├── main.c            SDL3 init, event loop, teardown
│   ├── pty.c/.h          PTY: posix_openpt + fork/exec $SHELL, reader thread
│   ├── terminal.c/.h     VT100/ANSI parser, cell grid, scrollback, OSC
│   ├── render.c/.h       OpenGL 3.3: glyph atlas, cell quads, overlays
│   ├── font.c/.h         CoreText/FreeType rasterization → GL texture atlas
│   ├── input.c/.h        SDL3 events → VT sequences → PTY
│   ├── search.c/.h       Incremental search: bloom filter, SIMD workers
│   ├── config.def.h      Config template
│   └── ml/
│       ├── autocomplete.c/.h   Two-phase worker; zero-latency main-thread path
│       ├── context.c/.h        CWD, git branch, fs map, Makefile/npm targets
│       ├── fsprobe.c/.h        kqueue/inotify watcher; triggers env re-probe
│       ├── history.c/.h        Append-only ~/.phantom_history
│       ├── htrie.c/.h          Frequency-ranked prefix trie
│       ├── ccg.c/.h            Command-context graph: next-command prediction
│       ├── schema.c/.h         Static subcommand completions (git, docker, …)
│       ├── ansi.c/.h           ANSI strip for LLM context
│       └── llm.c/.h            llama.cpp wrapper: KV-cache prefix pinning
├── third_party/llama.cpp/  git submodule
└── tests/                  Unit tests mirroring src/
```

### Data flow

```
PTY read thread
  └─ terminal_feed() ──→ cell grid + scrollback

SDL main thread ◄── g_event_pty_data
  ├─ input_handle_event() → VT sequences → PTY write
  ├─ autocomplete_query() → cancel + signal (zero latency)
  │       └─ worker thread
  │            ├─ Phase 1 (lock): CCG → HTrie → Schema
  │            ├─ Phase 1.5:      make/npm targets, FS path
  │            └─ Phase 2:        LLM FIM → g_event_ac_ready
  └─ renderer_draw() → cell grid, search overlay, ghost text
```

### Autocomplete pipeline

Worker runs all phases; main thread only cancels and signals.

| Phase | Experts | Notes |
|---|---|---|
| 1  -  instant | CCG, HTrie, Schema | Under lock; microseconds |
| 1.5  -  fast I/O | make/npm targets, FS completion | No lock; hits skip LLM |
| 2  -  LLM | FIM inference | Cancelled on next keystroke |

---

## Tests

```sh
ctest --test-dir build
```

---

## Known issues / caveats

- The `compress2` / `uncompress` paths in the scrollback buffer are only exercised on non-Apple builds (macOS uses LZFSE). Run `ctest` on Linux after changes to the scrollback compression code.

---

## Planned improvements

- Vim-based navigation

## Third-party

| Dependency | License |
|---|---|
| [llama.cpp](https://github.com/ggml-org/llama.cpp) | MIT |
| [SDL3](https://libsdl.org) | zlib |
| [stb_truetype](https://github.com/nothings/stb) | MIT / Public Domain |
