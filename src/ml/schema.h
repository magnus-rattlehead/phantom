#pragma once
#ifndef PHANTOM_SCHEMA_H
#define PHANTOM_SCHEMA_H

#include <stddef.h>

/* Completes `query` against static CLI schemas.
 *
 * Handles three patterns:
 *   cmd partial_sub             -  subcommand prefix completion
 *   cmd subcmd --partial_flag   -  per-subcommand flag completion
 *   cmd --partial_flag          -  global flag completion
 *
 * Writes the suffix to append into `out` (NUL-terminated).
 * Returns the number of bytes written (>0 = useful suffix),
 * 0 for exact match, -1 for no match or ambiguous with no LCP advance. */
int schema_complete(const char *query, char *out, size_t cap);

#endif /* PHANTOM_SCHEMA_H */
