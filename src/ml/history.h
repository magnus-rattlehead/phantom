#pragma once
#include <stddef.h>

typedef struct History History;

/* Opens or creates the phantom history file at path. NULL on failure. */
History *history_open(const char *path);

void history_close(History *h);

/* No-op if h is NULL or write_fp is closed. */
void history_append(History *h, const char *command);

/* Fills out[] with up to max_entries recent commands, most-recent first.
 * Caller must free each out[i]. Returns count written. */
size_t history_recent(const History *h, char **out, size_t max_entries);

/* Calls cb(cmd, arg) for every line in history, oldest first.
 * Used to seed the HTrie on startup. */
void history_each(const History *h, void (*cb)(const char *cmd, void *arg),
                  void *arg);
