#include "history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HISTORY_LINE_BUF_LEN 4096

struct History {
    FILE *write_fp;  /* ~/.phantom_history  -  append-only, CCG training  */
    FILE *read_fp;   /* ~/.zsh_history      -  read-only, completions      */
};

/* Strips the zsh extended-history prefix ": <timestamp>:<elapsed>;" if present.
 * Returns a pointer into line (not a new allocation). */
static const char *strip_zsh_prefix(const char *line)
{
    if (':' != line[0] || ' ' != line[1]) return line;
    const char *p = line + 2;
    while (*p >= '0' && *p <= '9') p++;
    if (':' != *p) return line;
    p++;
    while (*p >= '0' && *p <= '9') p++;
    if (';' != *p) return line;
    return p + 1;
}

/* Returns the path to ~/.zsh_history, or NULL if HOME is unset. */
static const char *zsh_history_path(void)
{
    static char buf[1024];
    const char *home = getenv("HOME");
    if (!home) return NULL;
    snprintf(buf, sizeof buf, "%s/.zsh_history", home);
    return buf;
}

History *history_open(const char *path)
{
    History *h = calloc(1, sizeof *h);
    if (!h) return NULL;

    if (!path) return h;

    h->write_fp = fopen(path, "a+");

    const char *zsh = zsh_history_path();
    if (zsh) h->read_fp = fopen(zsh, "r");

    return h;
}

void history_close(History *h)
{
    if (!h) return;
    if (h->write_fp) fclose(h->write_fp);
    if (h->read_fp)  fclose(h->read_fp);
    free(h);
}

void history_append(History *h, const char *command)
{
    if (!h || !h->write_fp || !command) return;
    fprintf(h->write_fp, "%s\n", command);
    fflush(h->write_fp);
}

size_t history_recent(const History *h, char **out, size_t max_entries)
{
    FILE *fp = (h && h->read_fp) ? h->read_fp : (h ? h->write_fp : NULL);
    if (!fp || 0 == max_entries) return 0;

    /* Seek near end to avoid reading a potentially huge history file.
     * Budget: max_entries lines × generous bytes/line estimate. */
    long budget = (long)max_entries * HISTORY_LINE_BUF_LEN;
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    if (fsize > budget) {
        fseek(fp, -budget, SEEK_END);
        /* Discard the first (potentially partial) line. */
        char skip[HISTORY_LINE_BUF_LEN];
        (void)fgets(skip, sizeof skip, fp);
    } else {
        rewind(fp);
    }

    char **ring = calloc(max_entries, sizeof *ring);
    if (!ring) return 0;

    size_t idx = 0, count = 0;
    char line[HISTORY_LINE_BUF_LEN];

    while (fgets(line, sizeof line, fp)) {
        size_t len = strlen(line);
        if (len > 0 && '\n' == line[len - 1]) line[--len] = '\0';
        if (0 == len) continue;

        const char *cmd = strip_zsh_prefix(line);
        if ('\0' == *cmd) continue;

        free(ring[idx % max_entries]);
        ring[idx % max_entries] = strdup(cmd);
        idx++;
        if (count < max_entries) count++;
    }

    /* Return most-recent first. */
    for (size_t i = 0; i < count; i++) {
        size_t src = (idx - 1 - i + max_entries) % max_entries;
        out[i] = ring[src];
        ring[src] = NULL;
    }

    for (size_t i = 0; i < max_entries; i++) free(ring[i]);
    free(ring);
    return count;
}

void history_each(const History *h,
                  void (*cb)(const char *cmd, void *arg), void *arg)
{
    FILE *fp = (h && h->read_fp) ? h->read_fp : (h ? h->write_fp : NULL);
    if (!fp || !cb) return;
    rewind(fp);
    char line[HISTORY_LINE_BUF_LEN];
    while (fgets(line, sizeof line, fp)) {
        size_t len = strlen(line);
        if (len > 0 && '\n' == line[len - 1]) line[--len] = '\0';
        if (0 == len) continue;
        const char *cmd = strip_zsh_prefix(line);
        if ('\0' != *cmd) cb(cmd, arg);
    }
}
