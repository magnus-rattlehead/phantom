#include "history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HISTORY_LINE_BUF_LEN 4096

typedef enum { SHELL_ZSH, SHELL_BASH, SHELL_FISH, SHELL_OTHER } ShellType;

struct History {
    FILE *write_fp; /* ~/.phantom_history: append-only, CCG training */
    FILE *read_fp;  /* shell history file: read-only, completions    */
    ShellType shell;
};

static ShellType detect_shell(void) {
    const char *shell = getenv("SHELL");
    if (!shell)
        return SHELL_OTHER;
    const char *base = strrchr(shell, '/');
    base = base ? base + 1 : shell;
    if (0 == strcmp(base, "zsh"))
        return SHELL_ZSH;
    if (0 == strcmp(base, "bash"))
        return SHELL_BASH;
    if (0 == strcmp(base, "fish"))
        return SHELL_FISH;
    return SHELL_OTHER;
}

static const char *sh_history_path(ShellType shell) {
    static char buf[1024];
    const char *custom = getenv("HISTFILE");
    if (custom) {
        snprintf(buf, sizeof buf, "%s", custom);
        return buf;
    }
    const char *home = getenv("HOME");
    if (!home)
        return NULL;

    const char *rel;
    switch (shell) {
    case SHELL_ZSH:
        rel = "/.zsh_history";
        break;
    case SHELL_BASH:
        rel = "/.bash_history";
        break;
    case SHELL_FISH:
        rel = "/.local/share/fish/fish_history";
        break;
    default:
        return NULL;
    }
    snprintf(buf, sizeof buf, "%s%s", home, rel);
    return buf;
}

/* Strips zsh extended-history prefix ": <timestamp>:<elapsed>;" if present.
 * Returns pointer into line, not a new allocation. */
static const char *strip_zsh_prefix(const char *line) {
    if (':' != line[0] || ' ' != line[1])
        return line;
    const char *p = line + 2;
    while (*p >= '0' && *p <= '9')
        p++;
    if (':' != *p)
        return line;
    p++;
    while (*p >= '0' && *p <= '9')
        p++;
    if (';' != *p)
        return line;
    return p + 1;
}

/* Fish history format: "- cmd: <command>\n  when: <timestamp>".
 * Returns pointer past "- cmd: ", or NULL for non-cmd lines. */
static const char *extract_fish_cmd(const char *line) {
    if (0 == strncmp(line, "- cmd: ", 7))
        return line + 7;
    return NULL;
}

static FILE *resolve_fp(const History *h, ShellType *shell_out) {
    *shell_out = h ? h->shell : SHELL_OTHER;
    return (h && h->read_fp) ? h->read_fp : (h ? h->write_fp : NULL);
}

static void strip_newline(char *line, size_t *len) {
    if (*len > 0 && '\n' == line[*len - 1])
        line[--(*len)] = '\0';
}

static const char *normalise_line(const char *line, ShellType shell) {
    switch (shell) {
    case SHELL_ZSH: {
        const char *cmd = strip_zsh_prefix(line);
        return ('\0' != *cmd) ? cmd : NULL;
    }
    case SHELL_BASH:
        /* Bash may prefix timestamps with '#<epoch>' when HISTTIMEFORMAT is
         * set.  Skip those lines so only the command text passes through. */
        if ('#' == line[0])
            return NULL;
        return ('\0' != *line) ? line : NULL;
    case SHELL_FISH:
        return extract_fish_cmd(line);
    default:
        return NULL;
    }
}

History *history_open(const char *path) {
    History *h = calloc(1, sizeof *h);
    if (!h)
        return NULL;

    h->shell = detect_shell();

    if (!path)
        return h;

    h->write_fp = fopen(path, "a+");

    const char *histfile = sh_history_path(h->shell);
    if (histfile)
        h->read_fp = fopen(histfile, "r");

    return h;
}

void history_close(History *h) {
    if (!h)
        return;
    if (h->write_fp)
        fclose(h->write_fp);
    if (h->read_fp)
        fclose(h->read_fp);
    free(h);
}

void history_append(History *h, const char *command) {
    if (!h || !h->write_fp || !command)
        return;
    fprintf(h->write_fp, "%s\n", command);
    fflush(h->write_fp);
}

size_t history_recent(const History *h, char **out, size_t max_entries) {
    ShellType shell;
    FILE *fp = resolve_fp(h, &shell);
    if (!fp || 0 == max_entries)
        return 0;

    /* Seek near end to avoid reading a huge history file in full. */
    long budget = (long)max_entries * HISTORY_LINE_BUF_LEN;
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    if (fsize > budget) {
        fseek(fp, -budget, SEEK_END);
        /* First line after seek may be partial. */
        char skip[HISTORY_LINE_BUF_LEN];
        (void)fgets(skip, sizeof skip, fp);
    } else {
        rewind(fp);
    }

    char **ring = calloc(max_entries, sizeof *ring);
    if (!ring)
        return 0;

    size_t idx = 0, count = 0;
    char line[HISTORY_LINE_BUF_LEN];

    while (fgets(line, sizeof line, fp)) {
        size_t len = strlen(line);
        strip_newline(line, &len);
        if (0 == len)
            continue;

        const char *cmd = normalise_line(line, shell);
        if (!cmd)
            continue;

        free(ring[idx % max_entries]);
        ring[idx % max_entries] = strdup(cmd);
        idx++;
        if (count < max_entries)
            count++;
    }

    for (size_t i = 0; i < count; i++) {
        size_t src = (idx - 1 - i + max_entries) % max_entries;
        out[i] = ring[src];
        ring[src] = NULL;
    }

    for (size_t i = 0; i < max_entries; i++)
        free(ring[i]);
    free(ring);
    return count;
}

void history_each(const History *h, void (*cb)(const char *cmd, void *arg),
                  void *arg) {
    ShellType shell;
    FILE *fp = resolve_fp(h, &shell);
    if (!fp || !cb)
        return;

    rewind(fp);
    char line[HISTORY_LINE_BUF_LEN];
    while (fgets(line, sizeof line, fp)) {
        size_t len = strlen(line);
        strip_newline(line, &len);
        if (0 == len)
            continue;
        const char *cmd = normalise_line(line, shell);
        if (cmd)
            cb(cmd, arg);
    }
}
