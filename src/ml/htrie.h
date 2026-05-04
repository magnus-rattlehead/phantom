#pragma once
#ifndef PHANTOM_HTRIE_H
#define PHANTOM_HTRIE_H

#include <stddef.h>
#include <stdint.h>

typedef struct HTrie HTrie;

HTrie *htrie_create(void);
void htrie_destroy(HTrie *t);

/* Inserts or increments the frequency count for cmd.
 * Calls from multiple threads are safe. */
void htrie_insert(HTrie *t, const char *cmd);

/* Returns the highest-frequency command that starts with prefix (and is
 * longer than prefix).  Returns NULL if no match meets min_count.
 * The returned pointer is valid until the next htrie_insert call.
 * NOT thread-safe  -  call from a single thread or under external lock. */
const char *htrie_best(HTrie *t, const char *prefix, uint32_t min_count);

size_t htrie_size(const HTrie *t);

#endif /* PHANTOM_HTRIE_H */
