#pragma once
#ifndef PHANTOM_ANSI_H
#define PHANTOM_ANSI_H

#include <stddef.h>

/* Strips ANSI/VT100 escape sequences from src[0..src_len).
 * Writes plain UTF-8 text into dst[0..dst_cap).
 * dst is always NUL-terminated (dst[dst_cap-1]='\0' on truncation).
 * Returns bytes written to dst, excluding the NUL terminator.
 *
 * Sequences handled:
 *   ESC [ ... <final 0x40-0x7E>   CSI (SGR, cursor movement, etc.)
 *   ESC ] ... BEL / ESC \         OSC (title, shell integration, etc.)
 *   ESC <single byte>             2-byte ESC sequences
 *   C0 controls except LF/CR      dropped
 *   UTF-8 multi-byte sequences    passed through unchanged
 */
size_t strip_ansi(const char *src, size_t src_len,
                  char *dst, size_t dst_cap);

#endif /* PHANTOM_ANSI_H */
