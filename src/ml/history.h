#pragma once
#include <stddef.h>

typedef struct History History;

/* Opens (or creates) the append-only history file at path. Returns NULL on failure. */
History *history_open(const char *path);

void     history_close(History *h);

/* Appends command to the history file. No-op if h is NULL. */
void     history_append(History *h, const char *command);

/* Fills out[] with up to max_entries recent commands. Returns the count written.
 * @return number of entries written; caller frees each out[i]. */
size_t   history_recent(const History *h, char **out, size_t max_entries);

/* Calls cb(cmd, arg) for every line in history, oldest first.
 * Used to seed the HTrie on startup. */
void     history_each(const History *h,
                      void (*cb)(const char *cmd, void *arg), void *arg);
