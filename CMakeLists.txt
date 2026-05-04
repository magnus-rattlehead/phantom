cmake_minimum_required(VERSION 3.25)
project(phantom C OBJC)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Dependencies ──────────────────────────────────────────────────────────────

# SDL3 (installed via: brew install sdl3)
find_package(SDL3 REQUIRED)

# OpenGL
find_package(OpenGL REQUIRED)

# LZ4 (installed via: brew install lz4)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LZ4 REQUIRED IMPORTED_TARGET liblz4)

# macOS system frameworks required by the CoreText font engine
if(APPLE)
    find_library(CORE_TEXT_LIB       CoreText       REQUIRED)
    find_library(CORE_GRAPHICS_LIB   CoreGraphics   REQUIRED)
    find_library(CORE_FOUNDATION_LIB CoreFoundation REQUIRED)
    find_library(APP_KIT_LIB         AppKit         REQUIRED)
else()
    find_package(Freetype REQUIRED)
    include(CheckCCompilerFlag)
    check_c_compiler_flag(-mavx2 COMPILER_HAS_AVX2)
    if(COMPILER_HAS_AVX2)
        set(PHANTOM_AVX2_CFLAGS -mavx2)
        set(PHANTOM_AVX2_DEFS   PHANTOM_HAVE_AVX2=1)
    endif()
endif()

# llama.cpp (Metal-accelerated on macOS; opt-in CUDA/Vulkan on Linux)
if(APPLE)
    set(GGML_METAL ON CACHE BOOL "" FORCE)
else()
    option(PHANTOM_CUDA   "Enable CUDA-accelerated llama.cpp inference" OFF)
    option(PHANTOM_VULKAN "Enable Vulkan-accelerated llama.cpp inference" OFF)
    if(PHANTOM_CUDA)
        set(GGML_CUDA   ON CACHE BOOL "" FORCE)
    endif()
    if(PHANTOM_VULKAN)
        set(GGML_VULKAN ON CACHE BOOL "" FORCE)
    endif()
endif()
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/llama.cpp EXCLUDE_FROM_ALL)

# ── Font resolution ────────────────────────────────────────────────────────────
# Searches system font directories for <stem>.ttf and sets <out_var> to the
# absolute path, or leaves it empty if not found.
# Usage: phantom_find_font("FiraCodeNerdFontMono-Regular" FOUND_PATH)
function(phantom_find_font stem out_var)
    if(APPLE)
        set(_font_dirs
            "$ENV{HOME}/Library/Fonts"
            "/Library/Fonts"
            "/System/Library/Fonts"
        )
        foreach(_dir ${_font_dirs})
            if(EXISTS "${_dir}/${stem}.ttf")
                set(${out_var} "${_dir}/${stem}.ttf" PARENT_SCOPE)
                return()
            endif()
        endforeach()
    else()
        # Use fc-match for reliable font lookup on Linux.
        find_program(FC_MATCH_EXE fc-match)
        if(FC_MATCH_EXE)
            execute_process(
                COMMAND ${FC_MATCH_EXE} "${stem}" --format=%{file}
                OUTPUT_VARIABLE _fc_result
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            if(_fc_result AND EXISTS "${_fc_result}")
                set(${out_var} "${_fc_result}" PARENT_SCOPE)
                return()
            endif()
        endif()
        # fc-match unavailable — search common dirs recursively.
        set(_font_dirs
            "$ENV{HOME}/.local/share/fonts"
            "$ENV{HOME}/.fonts"
            "/usr/local/share/fonts"
            "/usr/share/fonts"
        )
        foreach(_dir ${_font_dirs})
            file(GLOB_RECURSE _found "${_dir}/${stem}.ttf")
            if(_found)
                list(GET _found 0 _first)
                set(${out_var} "${_first}" PARENT_SCOPE)
                return()
            endif()
        endforeach()
    endif()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

# Font face paths — override with -DPHANTOM_FONT_<FACE>_PATH=/path.ttf
# Empty = auto-detect from system fonts via phantom_find_font.
set(PHANTOM_FONT_PATH             "" CACHE FILEPATH "Regular font TTF.")
set(PHANTOM_FONT_BOLD_PATH        "" CACHE FILEPATH "Bold font TTF.")
set(PHANTOM_FONT_ITALIC_PATH      "" CACHE FILEPATH "Italic font TTF.")
set(PHANTOM_FONT_BOLD_ITALIC_PATH "" CACHE FILEPATH "Bold-italic font TTF.")

# Auto-detect each face from system fonts.
# Fonts without a native italic (e.g. Fira Code) leave italic paths empty;
# the renderer skips that atlas and falls back to the regular face.
macro(phantom_resolve_font var stem label)
    if(NOT ${var})
        phantom_find_font("${stem}" ${var})
        if(${var})
            message(STATUS "Font (${label}): ${${var}}")
        else()
            message(STATUS "Font (${label}): not found — set manually in config.h")
        endif()
    endif()
endmacro()

phantom_resolve_font(PHANTOM_FONT_PATH
    "VictorMonoNerdFontMono-Regular"    "regular")
phantom_resolve_font(PHANTOM_FONT_BOLD_PATH
    "VictorMonoNerdFontMono-Bold"       "bold")
phantom_resolve_font(PHANTOM_FONT_ITALIC_PATH
    "VictorMonoNerdFontMono-Italic"     "italic")
phantom_resolve_font(PHANTOM_FONT_BOLD_ITALIC_PATH
    "VictorMonoNerdFontMono-BoldItalic" "bold-italic")

# ── User config ───────────────────────────────────────────────────────────────
# Bootstrap config.h from the template if the user hasn't created one yet.
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/src/config.h")
    message(STATUS "Creating src/config.h from config.def.h")
    configure_file(
        "${CMAKE_SOURCE_DIR}/src/config.def.h"
        "${CMAKE_SOURCE_DIR}/src/config.h"
        COPYONLY
    )
endif()

# ── Sources ───────────────────────────────────────────────────────────────────

set(ANSI_SRCS    src/ml/ansi.c)
set(CONTEXT_SRCS src/ml/context.c src/ml/fsprobe.c)
set(HTRIE_SRCS   src/ml/htrie.c)
set(SCHEMA_SRCS  src/ml/schema.c)
set(CCG_SRCS     src/ml/ccg.c)

set(PHANTOM_SOURCES
    src/main.c
    src/pty.c
    src/terminal.c
    src/render.c
    src/font.c
    src/input.c
    src/search.c
    src/ml/history.c
    src/ml/llm.c
    src/ml/autocomplete.c
    ${ANSI_SRCS}
    ${CONTEXT_SRCS}
    ${CCG_SRCS}
    ${HTRIE_SRCS}
    ${SCHEMA_SRCS}
)

if(APPLE)
    list(APPEND PHANTOM_SOURCES src/platform_macos.m)
endif()

add_executable(phantom ${PHANTOM_SOURCES})

target_include_directories(phantom PRIVATE
    src/
    third_party/llama.cpp/include
)

target_compile_definitions(phantom PRIVATE
    $<$<CONFIG:Debug>:PHANTOM_DEBUG=1>
)
macro(phantom_inject_font_def cmake_var c_define)
    if(${cmake_var})
        target_compile_definitions(phantom PRIVATE
            ${c_define}="${${cmake_var}}")
    endif()
endmacro()
phantom_inject_font_def(PHANTOM_FONT_PATH            PHANTOM_DEFAULT_FONT)
phantom_inject_font_def(PHANTOM_FONT_BOLD_PATH       PHANTOM_DEFAULT_FONT_BOLD)
phantom_inject_font_def(PHANTOM_FONT_ITALIC_PATH     PHANTOM_DEFAULT_FONT_ITALIC)
phantom_inject_font_def(PHANTOM_FONT_BOLD_ITALIC_PATH PHANTOM_DEFAULT_FONT_BOLD_ITALIC)

set(PHANTOM_LIBS SDL3::SDL3 OpenGL::GL llama PkgConfig::LZ4)
if(APPLE)
    list(APPEND PHANTOM_LIBS
        ${CORE_TEXT_LIB} ${CORE_GRAPHICS_LIB} ${CORE_FOUNDATION_LIB}
        ${APP_KIT_LIB})
else()
    list(APPEND PHANTOM_LIBS Freetype::Freetype)
endif()
target_link_libraries(phantom PRIVATE ${PHANTOM_LIBS})

target_compile_options(phantom PRIVATE -Wall -Wextra -Wpedantic)
if(PHANTOM_AVX2_CFLAGS)
    target_compile_options(phantom PRIVATE ${PHANTOM_AVX2_CFLAGS})
    target_compile_definitions(phantom PRIVATE ${PHANTOM_AVX2_DEFS})
endif()

# ── macOS app bundle ──────────────────────────────────────────────────────────

if(APPLE)
    option(PHANTOM_MACOS_BUNDLE "Build phantom as a .app bundle" ON)
    if(PHANTOM_MACOS_BUNDLE)
        set(PHANTOM_ICNS "${CMAKE_SOURCE_DIR}/assets/phantom.icns")
        target_sources(phantom PRIVATE "${PHANTOM_ICNS}")
        set_source_files_properties("${PHANTOM_ICNS}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources")
        set_target_properties(phantom PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_INFO_PLIST
                "${CMAKE_SOURCE_DIR}/cmake/Info.plist.in"
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.magnusrattlehead.phantom"
            MACOSX_BUNDLE_BUNDLE_VERSION "1"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
            MACOSX_BUNDLE_ICON_FILE "phantom"
        )
        # Copy any *.gguf models from source models/ to
        # ~/Library/Application Support/phantom/models/ on every build.
        # Uses -n so existing files are not overwritten.
        add_custom_command(TARGET phantom POST_BUILD
            COMMAND "${CMAKE_SOURCE_DIR}/cmake/sync_models.sh"
                "${CMAKE_SOURCE_DIR}/models"
                "$ENV{HOME}/Library/Application Support/phantom/models"
            COMMENT "Syncing models to ~/Library/Application Support/phantom/models/"
        )

        # Install to /Applications (run: cmake --build build --target install-app)
        add_custom_target(install-app
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:phantom>"
                "/Applications/phantom.app"
            DEPENDS phantom
            COMMENT "Installing phantom.app to /Applications"
        )
    endif()
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────

enable_testing()

# Sources shared by tests that don't need SDL/OpenGL
set(TERMINAL_SRCS src/terminal.c)
set(HISTORY_SRCS  src/ml/history.c)
set(PTY_SRCS      src/pty.c src/terminal.c)

function(add_phantom_test name sources)
    add_executable(${name} ${sources})
    target_include_directories(${name} PRIVATE src/)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(${name} PRIVATE
        $<$<CONFIG:Debug>:PHANTOM_DEBUG=1>)
    if(PHANTOM_AVX2_CFLAGS)
        target_compile_options(${name} PRIVATE ${PHANTOM_AVX2_CFLAGS})
        target_compile_definitions(${name} PRIVATE ${PHANTOM_AVX2_DEFS})
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_phantom_test(test_terminal
    "tests/test_terminal.c;${TERMINAL_SRCS}")
target_link_libraries(test_terminal PRIVATE PkgConfig::LZ4)

add_phantom_test(test_history
    "tests/ml/test_history.c;${HISTORY_SRCS}")

add_phantom_test(test_pty
    "tests/test_pty.c;${PTY_SRCS}")
target_link_libraries(test_pty PRIVATE pthread)
target_link_libraries(test_pty PRIVATE PkgConfig::LZ4)

add_phantom_test(test_search
    "tests/test_search.c;src/search.c;${TERMINAL_SRCS}")
target_link_libraries(test_search PRIVATE SDL3::SDL3 pthread)
target_link_libraries(test_search PRIVATE PkgConfig::LZ4)

add_phantom_test(test_ansi
    "tests/ml/test_ansi.c;${ANSI_SRCS}")

add_phantom_test(test_context
    "tests/ml/test_context.c;${CONTEXT_SRCS};${TERMINAL_SRCS}")
target_link_libraries(test_context PRIVATE pthread)
target_link_libraries(test_context PRIVATE PkgConfig::LZ4)

add_phantom_test(test_fsprobe
    "tests/ml/test_fsprobe.c;${CONTEXT_SRCS};${TERMINAL_SRCS}")
target_link_libraries(test_fsprobe PRIVATE pthread)
target_link_libraries(test_fsprobe PRIVATE PkgConfig::LZ4)

add_phantom_test(test_fs_complete
    "tests/ml/test_fs_complete.c;${CONTEXT_SRCS};${TERMINAL_SRCS}")
target_link_libraries(test_fs_complete PRIVATE pthread)
target_link_libraries(test_fs_complete PRIVATE PkgConfig::LZ4)

add_phantom_test(test_htrie
    "tests/ml/test_htrie.c;${HTRIE_SRCS}")
target_link_libraries(test_htrie PRIVATE pthread)

add_phantom_test(test_schema
    "tests/ml/test_schema.c;${SCHEMA_SRCS}")

add_phantom_test(test_ccg
    "tests/ml/test_ccg.c;${CCG_SRCS}")
target_link_libraries(test_ccg PRIVATE pthread)

add_phantom_test(test_exit_code
    "tests/test_exit_code.c;${TERMINAL_SRCS}")
target_link_libraries(test_exit_code PRIVATE PkgConfig::LZ4)

set(CCG_TRAINING_SRCS
    src/ml/autocomplete.c
    src/ml/ccg.c
    src/ml/ansi.c
    src/ml/context.c
    src/ml/fsprobe.c
    src/ml/history.c
    src/ml/htrie.c
    src/ml/llm.c
    src/ml/schema.c
    src/terminal.c)
add_phantom_test(test_ccg_training
    "tests/ml/test_ccg_training.c;${CCG_TRAINING_SRCS}")
target_link_libraries(test_ccg_training
    PRIVATE SDL3::SDL3 llama pthread)
target_link_libraries(test_ccg_training PRIVATE PkgConfig::LZ4)

set(FIM_TEST_SRCS
    src/ml/autocomplete.c
    src/ml/ansi.c
    src/ml/context.c
    src/ml/fsprobe.c
    src/ml/history.c
    src/ml/htrie.c
    src/ml/llm.c
    src/ml/schema.c
    src/terminal.c)
add_phantom_test(test_autocomplete_fim
    "tests/ml/test_autocomplete_fim.c;${FIM_TEST_SRCS};${CCG_SRCS}")
target_link_libraries(test_autocomplete_fim
    PRIVATE SDL3::SDL3 llama pthread)
target_link_libraries(test_autocomplete_fim PRIVATE PkgConfig::LZ4)

add_phantom_test(test_kv_cache
    "tests/ml/test_kv_cache.c;src/ml/llm.c;tests/ml/llama_stub.c")
target_include_directories(test_kv_cache PRIVATE
    third_party/llama.cpp/include
    third_party/llama.cpp/ggml/include)
target_link_libraries(test_kv_cache PRIVATE pthread)
# Stub replaces llama; do NOT link the real llama library here.

set(QOS_TEST_SRCS
    src/ml/autocomplete.c
    src/ml/ansi.c
    src/ml/context.c
    src/ml/fsprobe.c
    src/ml/history.c
    src/ml/htrie.c
    src/ml/llm.c
    src/ml/schema.c
    src/terminal.c)
add_phantom_test(test_thread_qos
    "tests/ml/test_thread_qos.c;${QOS_TEST_SRCS};${CCG_SRCS};tests/ml/llama_stub.c")
target_include_directories(test_thread_qos PRIVATE
    third_party/llama.cpp/include
    third_party/llama.cpp/ggml/include)
target_compile_definitions(test_thread_qos PRIVATE PHANTOM_TESTING=1)
target_link_libraries(test_thread_qos PRIVATE SDL3::SDL3 pthread)
target_link_libraries(test_thread_qos PRIVATE PkgConfig::LZ4)

set(HYBRID_FLOW_SRCS
    src/ml/autocomplete.c
    src/ml/ansi.c
    src/ml/context.c
    src/ml/fsprobe.c
    src/ml/history.c
    src/ml/htrie.c
    src/ml/llm.c
    src/ml/schema.c
    src/terminal.c)
add_phantom_test(test_hybrid_flow
    "tests/ml/test_hybrid_flow.c;${HYBRID_FLOW_SRCS};${CCG_SRCS}")
target_compile_definitions(test_hybrid_flow PRIVATE PHANTOM_TESTING=1)
target_link_libraries(test_hybrid_flow
    PRIVATE SDL3::SDL3 llama pthread)
target_link_libraries(test_hybrid_flow PRIVATE PkgConfig::LZ4)
