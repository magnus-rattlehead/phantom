#include "context.h"
#include "../config.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#endif

#include "../terminal.h"

#include "ml_log.h"
#define CTX_LOG(...) ml_log("ctx", __VA_ARGS__)

struct TerminalState {
    pthread_mutex_t lock;
    pid_t shell_pid;
    char cwd[PATH_MAX];
    char cached_cwd[PATH_MAX];
    char git_branch[128];
    char *sb_buf;
    size_t sb_len;
    char *fs_buf;
    size_t fs_len;
    char *cwd_listing_buf;
    size_t cwd_listing_len;
    char *make_targets_buf;
    char *pkg_scripts_buf;
};

TerminalState *terminal_state_create(Terminal *term) {
    TerminalState *ts = calloc(1, sizeof *ts);
    if (NULL == ts)
        return NULL;
    pthread_mutex_init(&ts->lock, NULL);
    terminal_state_probe(ts, term);
    return ts;
}

void terminal_state_destroy(TerminalState *ts) {
    if (NULL == ts)
        return;
    pthread_mutex_destroy(&ts->lock);
    free(ts->sb_buf);
    free(ts->fs_buf);
    free(ts->cwd_listing_buf);
    free(ts->make_targets_buf);
    free(ts->pkg_scripts_buf);
    free(ts);
}

/* Returns an inclusion priority for a directory entry.
 * Higher = emitted first when the byte cap is approached.
 * Returns -1 for entries that should be omitted entirely. */
static int fs_ext_priority(const char *name, int is_dir) {
    if (is_dir)
        return 2;

    const char *dot = strrchr(name, '.');
    if (!dot || '\0' == dot[1])
        return 1; /* no/trailing dot: executable etc. */

    /* Build artifacts: pure noise for the LLM. */
    static const char *const lo[] = {".o",   ".a",    ".so",   ".dylib", ".dll",
                                     ".exe", ".bin",  ".lock", ".class", ".pyc",
                                     ".pyo", ".wasm", ".map",  ".d",     NULL};
    for (int i = 0; lo[i]; i++) {
        if (0 == strcmp(dot, lo[i]))
            return -1;
    }

    /* Source files the LLM can reason about. */
    static const char *const hi[] = {
        ".c",     ".h",  ".cpp", ".cc",  ".cxx", ".hpp", ".hh",   ".go",
        ".py",    ".js", ".ts",  ".jsx", ".tsx", ".rs",  ".java", ".rb",
        ".swift", ".kt", ".cs",  ".zig", ".nim", NULL};
    for (int i = 0; hi[i]; i++) {
        if (0 == strcmp(dot, hi[i]))
            return 4;
    }

    /* Config, docs, scripts. */
    static const char *const md[] = {
        ".json", ".yaml", ".yml",  ".toml", ".ini",  ".cfg",   ".txt", ".md",
        ".rst",  ".sh",   ".bash", ".zsh",  ".fish", ".cmake", ".mk",  NULL};
    for (int i = 0; md[i]; i++) {
        if (0 == strcmp(dot, md[i]))
            return 3;
    }

    return 0; /* unknown extension */
}

#define FS_MAX_ENTRIES 64

typedef struct {
    char name[NAME_MAX + 1];
    int is_dir;
    int priority;
} FsEnt;

static int fs_ent_cmp(const void *a, const void *b) {
    return ((const FsEnt *)b)->priority - ((const FsEnt *)a)->priority;
}

#define CTX_LINE_BUF 1024

static int collect_dir_entries(DIR *d, FsEnt *entries, int cap) {
    int n = 0;
    struct dirent *ent;
    while (NULL != (ent = readdir(d))) {
        if ('.' == ent->d_name[0])
            continue;
        if (n >= cap)
            continue;
        int is_dir = (DT_DIR == ent->d_type);
        int pri = fs_ext_priority(ent->d_name, is_dir);
        if (pri < 0)
            continue;
        memcpy(entries[n].name, ent->d_name, strlen(ent->d_name) + 1);
        entries[n].is_dir = is_dir;
        entries[n].priority = pri;
        n++;
    }
    return n;
}

static void fs_list_cwd(const char *dir, char *buf, size_t cap, size_t *pos) {
    DIR *d = opendir(dir);
    if (NULL == d)
        return;

    FsEnt entries[FS_MAX_ENTRIES];
    int nentries = collect_dir_entries(d, entries, FS_MAX_ENTRIES);
    closedir(d);
    qsort(entries, (size_t)nentries, sizeof *entries, fs_ent_cmp);

    int separator_needed = 0;
    for (int i = 0; i < nentries; i++) {
        if (!entries[i].is_dir && !separator_needed) {
            separator_needed = 1;
            if (*pos > 0 && *pos + 2 < cap) {
                buf[(*pos)++] = ' ';
                buf[(*pos)++] = '|';
            }
        }
        size_t nlen = strlen(entries[i].name);
        size_t need = nlen + (entries[i].is_dir ? 2 : 1);
        if (*pos + need + 1 >= cap)
            break;
        if (*pos > 0 && ' ' != buf[*pos - 1])
            buf[(*pos)++] = ' ';
        memcpy(buf + *pos, entries[i].name, nlen);
        *pos += nlen;
        if (entries[i].is_dir)
            buf[(*pos)++] = '/';
    }
    if (*pos < cap)
        buf[*pos] = '\0';
}

#define FS_QUEUE_CAP 256

typedef struct {
    char path[PATH_MAX];
    int depth;
} FsDirJob;

static void fs_scan(const char *root, int max_depth, char *buf, size_t cap,
                    size_t *pos) {
    static const char *const skip_dirs[] = {"build",  "target", "node_modules",
                                            "vendor", ".git",   NULL};

    FsDirJob *queue = malloc((size_t)FS_QUEUE_CAP * sizeof(FsDirJob));
    if (!queue)
        return;
    int head = 0, tail = 0;

    snprintf(queue[tail].path, PATH_MAX, "%s", root);
    queue[tail].depth = 0;
    tail++;

    while (head < tail && *pos + 4 < cap) {
        FsDirJob *job = &queue[head++];

        DIR *d = opendir(job->path);
        if (!d)
            continue;

        /* Collect before writing so we can sort by priority. */
        FsEnt entries[FS_MAX_ENTRIES];
        int nentries = collect_dir_entries(d, entries, FS_MAX_ENTRIES);
        closedir(d);
        qsort(entries, (size_t)nentries, sizeof *entries, fs_ent_cmp);

        /* Suffix after root; empty at root itself -> display as ".". */
        const char *rel = job->path + strlen(root);
        if ('\0' == rel[0])
            rel = ".";

        int n = snprintf(buf + *pos, cap - *pos, "%s/", rel);
        if (n > 0)
            *pos += (size_t)n;

        /* dirs first, files after "|"; gives the LLM an unambiguous split
         * between navigable dirs and referenceable files. */
        int emitted_dirs = 0;
        int emitted_files = 0;

        for (int i = 0; i < nentries && *pos + 2 < cap; i++) {
            if (!entries[i].is_dir)
                continue;
            buf[(*pos)++] = ' ';
            size_t nlen = strlen(entries[i].name);
            if (*pos + nlen + 2 >= cap)
                break;
            memcpy(buf + *pos, entries[i].name, nlen);
            *pos += nlen;
            buf[(*pos)++] = '/';
            emitted_dirs++;

            if (job->depth < max_depth && tail < FS_QUEUE_CAP) {
                int skip = 0;
                for (int si = 0; skip_dirs[si]; si++) {
                    if (0 == strcmp(entries[i].name, skip_dirs[si])) {
                        skip = 1;
                        break;
                    }
                }
                if (!skip) {
                    snprintf(queue[tail].path, PATH_MAX, "%s/%s", job->path,
                             entries[i].name);
                    queue[tail].depth = job->depth + 1;
                    tail++;
                }
            }
        }

        if (emitted_dirs > 0 && *pos + 3 < cap) {
            buf[(*pos)++] = ' ';
            buf[(*pos)++] = '|';
        }

        for (int i = 0; i < nentries && *pos + 2 < cap; i++) {
            if (entries[i].is_dir)
                continue;
            buf[(*pos)++] = ' ';
            size_t nlen = strlen(entries[i].name);
            if (*pos + nlen + 1 >= cap)
                break;
            memcpy(buf + *pos, entries[i].name, nlen);
            *pos += nlen;
            emitted_files++;
        }

        /* Skip newline if all entries were filtered. */
        if ((emitted_dirs > 0 || emitted_files > 0) && *pos < cap)
            buf[(*pos)++] = '\n';
    }

    if (*pos < cap)
        buf[*pos] = '\0';
    free(queue);
}

#define MAKE_TARGETS_CAP 512

static char *parse_make_targets(const char *cwd) {
    static const char *const names[] = {"Makefile", "makefile", "GNUmakefile",
                                        NULL};
    FILE *f = NULL;
    for (int i = 0; names[i]; i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", cwd, names[i]);
        f = fopen(path, "r");
        if (NULL != f)
            break;
    }
    if (NULL == f)
        return NULL;
    CTX_LOG("parse_make_targets: found Makefile in %s", cwd);

    char *buf = malloc(MAKE_TARGETS_CAP + 1);
    if (NULL == buf) {
        fclose(f);
        return NULL;
    }
    size_t pos = 0;

    char line[CTX_LINE_BUF];
    while (NULL != fgets(line, (int)sizeof line, f)) {
        char *c = line;
        if ('\t' == *c || '.' == *c || '%' == *c || '#' == *c)
            continue;
        if (!isalnum((unsigned char)*c) && '_' != *c)
            continue;

        char *colon = strchr(c, ':');
        if (NULL == colon)
            continue;
        if ('=' == colon[1])
            continue;

        int valid = 1;
        for (char *p = c; p < colon; p++) {
            if ('#' == *p || '=' == *p || ' ' == *p || '\t' == *p) {
                valid = 0;
                break;
            }
        }
        if (!valid)
            continue;

        size_t name_len = (size_t)(colon - c);
        if (0 == name_len || pos + name_len + 2 > MAKE_TARGETS_CAP)
            continue;
        memcpy(buf + pos, c, name_len);
        pos += name_len;
        buf[pos++] = '\n';
    }
    fclose(f);

    if (0 == pos) {
        free(buf);
        return NULL;
    }
    buf[pos] = '\0';
    CTX_LOG("parse_make_targets: %zu bytes of targets", pos);
    return buf;
}

static char *parse_pkg_scripts(const char *cwd) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/package.json", cwd);
    FILE *f = fopen(path, "r");
    if (NULL == f)
        return NULL;
    CTX_LOG("parse_pkg_scripts: found package.json in %s", cwd);

    char *buf = malloc(MAKE_TARGETS_CAP + 1);
    if (NULL == buf) {
        fclose(f);
        return NULL;
    }
    size_t pos = 0;

    char line[CTX_LINE_BUF];
    int in_scripts = 0;
    int depth = 0;

    while (NULL != fgets(line, (int)sizeof line, f)) {
        if (!in_scripts) {
            if (NULL != strstr(line, "\"scripts\"")) {
                in_scripts = 1;
                for (char *p = line; *p; p++) {
                    if ('{' == *p)
                        depth++;
                    else if ('}' == *p)
                        depth--;
                }
            }
            continue;
        }

        for (char *p = line; *p; p++) {
            if ('{' == *p)
                depth++;
            else if ('}' == *p)
                depth--;
        }
        if (depth <= 0)
            break;

        char *q1 = strchr(line, '"');
        if (NULL == q1)
            continue;
        q1++;
        char *q2 = strchr(q1, '"');
        if (NULL == q2)
            continue;
        size_t name_len = (size_t)(q2 - q1);
        if (0 == name_len || name_len > 64)
            continue;

        char *after = q2 + 1;
        while (' ' == *after || '\t' == *after)
            after++;
        if (':' != *after)
            continue;

        if (pos + name_len + 2 > MAKE_TARGETS_CAP)
            break;
        memcpy(buf + pos, q1, name_len);
        pos += name_len;
        buf[pos++] = '\n';
    }
    fclose(f);

    if (0 == pos) {
        free(buf);
        return NULL;
    }
    buf[pos] = '\0';
    CTX_LOG("parse_pkg_scripts: %zu bytes of scripts", pos);
    return buf;
}

int fs_complete_path(const char *cwd, const char *partial_arg, char *out,
                     size_t cap) {
    if (NULL == cwd || '\0' == cwd[0] || NULL == partial_arg || cap < 2)
        return -1;

    /* Split partial_arg on the last '/' -> dir_part + file_prefix. */
    const char *last_slash = strrchr(partial_arg, '/');
    const char *file_prefix;
    char scan_path[PATH_MAX];

    if ('/' == partial_arg[0]) {
        size_t dir_len = (size_t)(last_slash - partial_arg);
        if (0 == dir_len) {
            snprintf(scan_path, sizeof scan_path, "/");
        } else {
            snprintf(scan_path, sizeof scan_path, "%.*s", (int)dir_len,
                     partial_arg);
        }
        file_prefix = last_slash + 1;
        CTX_LOG("fs_complete: absolute arg \"%s\" -> scan \"%s\"", partial_arg,
                scan_path);
    } else if (NULL != last_slash) {
        size_t dir_len = (size_t)(last_slash - partial_arg);
        char raw[PATH_MAX];
        snprintf(raw, sizeof raw, "%s/%.*s", cwd, (int)dir_len, partial_arg);
        if (NULL == realpath(raw, scan_path))
            return -1;
        file_prefix = last_slash + 1;
        CTX_LOG("fs_complete: realpath \"%s\" -> \"%s\"", raw, scan_path);
    } else {
        snprintf(scan_path, sizeof scan_path, "%s", cwd);
        file_prefix = partial_arg;
    }

    size_t prefix_len = strlen(file_prefix);

    DIR *d = opendir(scan_path);
    if (NULL == d)
        return -1;

    /* Track LCP across matches.
     * 1 match -> full suffix + dir slash; N>1 -> LCP suffix only. */
    char lcp[NAME_MAX + 1];
    size_t lcp_len = 0;
    int match_dtype = DT_UNKNOWN;
    int n_matches = 0;
    struct dirent *ent;

    while (NULL != (ent = readdir(d))) {
        if ('.' == ent->d_name[0])
            continue;
        if (0 != strncmp(ent->d_name, file_prefix, prefix_len))
            continue;
        size_t name_len = strlen(ent->d_name);
        if (0 == n_matches) {
            memcpy(lcp, ent->d_name, name_len + 1);
            lcp_len = name_len;
            match_dtype = ent->d_type;
        } else {
            size_t i = 0;
            while (i < lcp_len && i < name_len && lcp[i] == ent->d_name[i])
                i++;
            lcp_len = i;
        }
        n_matches++;
    }
    closedir(d);

    if (0 == n_matches)
        return -1;

    size_t suffix_len = lcp_len - prefix_len;

    int append_slash = 0;
    if (1 == n_matches) {
        if (DT_DIR == match_dtype) {
            append_slash = 1;
        } else if (DT_UNKNOWN == match_dtype) {
            char full[PATH_MAX];
            struct stat st;
            snprintf(full, sizeof full, "%s/%s", scan_path, lcp);
            append_slash = (0 == stat(full, &st) && S_ISDIR(st.st_mode));
        }
    }

    if (n_matches > 1 && 0 == suffix_len)
        return -1;

    size_t total = suffix_len + (size_t)append_slash;
    if (total + 1 > cap)
        return -1;

    memcpy(out, lcp + prefix_len, suffix_len);
    if (append_slash)
        out[suffix_len] = '/';
    out[total] = '\0';
    return (int)total;
}

void terminal_state_set_shell_pid(TerminalState *ts, pid_t pid) {
    if (NULL == ts)
        return;
    pthread_mutex_lock(&ts->lock);
    ts->shell_pid = pid;
    pthread_mutex_unlock(&ts->lock);
}

void terminal_state_update_cwd(TerminalState *ts, const char *cwd) {
    if (NULL == ts || NULL == cwd || '\0' == cwd[0])
        return;
    pthread_mutex_lock(&ts->lock);
    snprintf(ts->cwd, sizeof ts->cwd, "%s", cwd);
    pthread_mutex_unlock(&ts->lock);
    CTX_LOG("osc7 cwd: %s", cwd);
}

void terminal_state_probe_env(TerminalState *ts) {
    if (NULL == ts)
        return;

    pthread_mutex_lock(&ts->lock);
    pid_t shell_pid = ts->shell_pid;
    pthread_mutex_unlock(&ts->lock);

    char new_cwd[PATH_MAX];
    new_cwd[0] = '\0';

#if defined(__APPLE__)
    if (shell_pid > 0) {
        struct proc_vnodepathinfo vnpi;
        int n = proc_pidinfo((int)shell_pid, PROC_PIDVNODEPATHINFO, 0, &vnpi,
                             (int)sizeof vnpi);
        if (n >= (int)sizeof(struct proc_vnodepathinfo)) {
            snprintf(new_cwd, sizeof new_cwd, "%s", vnpi.pvi_cdir.vip_path);
            CTX_LOG("probe_env: proc_pidinfo -> \"%s\"", new_cwd);
        } else {
            CTX_LOG("probe_env: proc_pidinfo failed (pid=%d n=%d)",
                    (int)shell_pid, n);
        }
    } else {
        CTX_LOG("probe_env: shell_pid not set, falling back to getcwd");
    }
#else
    if (shell_pid > 0) {
        char proc_path[64];
        snprintf(proc_path, sizeof proc_path, "/proc/%d/cwd", (int)shell_pid);
        ssize_t len = readlink(proc_path, new_cwd, sizeof new_cwd - 1);
        if (len > 0) {
            new_cwd[len] = '\0';
            CTX_LOG("probe_env: readlink %s -> \"%s\"", proc_path, new_cwd);
        } else {
            CTX_LOG("probe_env: readlink %s failed", proc_path);
        }
    } else {
        CTX_LOG("probe_env: shell_pid not set, falling back to getcwd");
    }
#endif

    if ('\0' == new_cwd[0]) {
        if (NULL == getcwd(new_cwd, sizeof new_cwd)) {
            new_cwd[0] = '\0';
            CTX_LOG("getcwd failed");
        } else {
            CTX_LOG("probe_env: getcwd -> \"%s\"", new_cwd);
        }
    }

    pthread_mutex_lock(&ts->lock);
    int cwd_changed = (0 != strcmp(new_cwd, ts->cached_cwd));
    CTX_LOG("probe_env: new=\"%s\" cached=\"%s\" changed=%d", new_cwd,
            ts->cached_cwd, cwd_changed);
    if (cwd_changed) {
        memcpy(ts->cwd, new_cwd, sizeof ts->cwd);
        memcpy(ts->cached_cwd, new_cwd, sizeof ts->cached_cwd);
    }
    pthread_mutex_unlock(&ts->lock);

    if (!cwd_changed)
        return;

    char git_branch[128] = {'\0'};
    {
        FILE *fp = popen("git branch --show-current 2>/dev/null", "r");
        if (NULL != fp) {
            if (NULL != fgets(git_branch, (int)sizeof git_branch, fp)) {
                size_t n = strlen(git_branch);
                if (n > 0 && '\n' == git_branch[n - 1])
                    git_branch[n - 1] = '\0';
            }
            pclose(fp);
        }
        CTX_LOG("git branch: \"%s\"", git_branch);
    }

    char *new_fs = NULL;
    size_t new_len = 0;
    if ('\0' != new_cwd[0]) {
        new_fs = malloc(CONTEXT_FS_BYTES);
        if (NULL != new_fs) {
            fs_scan(new_cwd, CONTEXT_FS_DEPTH, new_fs, CONTEXT_FS_BYTES,
                    &new_len);
            CTX_LOG("fs map: %zu bytes", new_len);
        }
    }

    char *new_listing = NULL;
    size_t new_listing_len = 0;
    if ('\0' != new_cwd[0]) {
        new_listing = malloc(CONTEXT_CWD_LISTING_BYTES);
        if (NULL != new_listing) {
            fs_list_cwd(new_cwd, new_listing, CONTEXT_CWD_LISTING_BYTES,
                        &new_listing_len);
            CTX_LOG("cwd listing: %zu bytes", new_listing_len);
        }
    }

    CTX_LOG("probe_env: scanning special files in \"%s\"", new_cwd);
    char *new_make = ('\0' != new_cwd[0]) ? parse_make_targets(new_cwd) : NULL;
    char *new_pkg = ('\0' != new_cwd[0]) ? parse_pkg_scripts(new_cwd) : NULL;

    pthread_mutex_lock(&ts->lock);
    memcpy(ts->git_branch, git_branch, sizeof ts->git_branch);
    free(ts->fs_buf);
    ts->fs_buf = new_fs;
    ts->fs_len = new_len;
    free(ts->cwd_listing_buf);
    ts->cwd_listing_buf = new_listing;
    ts->cwd_listing_len = new_listing_len;
    free(ts->make_targets_buf);
    ts->make_targets_buf = new_make;
    free(ts->pkg_scripts_buf);
    ts->pkg_scripts_buf = new_pkg;
    pthread_mutex_unlock(&ts->lock);
}

void terminal_state_probe_fs(TerminalState *ts) {
    if (NULL == ts)
        return;

    char cwd[PATH_MAX];
    pthread_mutex_lock(&ts->lock);
    memcpy(cwd, ts->cwd, sizeof cwd);
    pthread_mutex_unlock(&ts->lock);

    if ('\0' == cwd[0])
        return;

    char *new_fs = malloc(CONTEXT_FS_BYTES);
    size_t new_len = 0;
    if (NULL != new_fs) {
        fs_scan(cwd, CONTEXT_FS_DEPTH, new_fs, CONTEXT_FS_BYTES, &new_len);
        CTX_LOG("fs map (rescan): %zu bytes", new_len);
    }

    char *new_listing = malloc(CONTEXT_CWD_LISTING_BYTES);
    size_t new_listing_len = 0;
    if (NULL != new_listing) {
        fs_list_cwd(cwd, new_listing, CONTEXT_CWD_LISTING_BYTES,
                    &new_listing_len);
        CTX_LOG("cwd listing (rescan): %zu bytes", new_listing_len);
    }

    CTX_LOG("probe_fs: scanning special files in \"%s\"", cwd);
    char *new_make = parse_make_targets(cwd);
    char *new_pkg = parse_pkg_scripts(cwd);

    pthread_mutex_lock(&ts->lock);
    free(ts->fs_buf);
    ts->fs_buf = new_fs;
    ts->fs_len = new_len;
    free(ts->cwd_listing_buf);
    ts->cwd_listing_buf = new_listing;
    ts->cwd_listing_len = new_listing_len;
    free(ts->make_targets_buf);
    ts->make_targets_buf = new_make;
    free(ts->pkg_scripts_buf);
    ts->pkg_scripts_buf = new_pkg;
    pthread_mutex_unlock(&ts->lock);
}

void terminal_state_probe_screen(TerminalState *ts, Terminal *term) {
    if (NULL == ts)
        return;

    free(ts->sb_buf);
    ts->sb_buf = NULL;
    ts->sb_len = 0;

    if (NULL == term)
        return;

    int cur_col = 0, cur_row = 0;
    terminal_cursor_pos(term, &cur_col, &cur_row);
    CTX_LOG("screen probe: cursor (%d, %d)", cur_col, cur_row);
}

void terminal_state_probe(TerminalState *ts, Terminal *term) {
    terminal_state_probe_env(ts);
    terminal_state_probe_screen(ts, term);
}

void terminal_state_lock(TerminalState *ts) {
    if (NULL != ts)
        pthread_mutex_lock(&ts->lock);
}

void terminal_state_unlock(TerminalState *ts) {
    if (NULL != ts)
        pthread_mutex_unlock(&ts->lock);
}

const char *terminal_state_cwd(const TerminalState *ts) {
    return ts ? ts->cwd : "";
}

const char *terminal_state_fs(const TerminalState *ts) {
    return (ts && ts->fs_buf) ? ts->fs_buf : "";
}

const char *terminal_state_git_branch(const TerminalState *ts) {
    return ts ? ts->git_branch : "";
}

const char *terminal_state_cwd_listing(const TerminalState *ts) {
    return (ts && ts->cwd_listing_buf) ? ts->cwd_listing_buf : "";
}

const char *terminal_state_make_targets(const TerminalState *ts) {
    return (ts && ts->make_targets_buf) ? ts->make_targets_buf : "";
}

const char *terminal_state_pkg_scripts(const TerminalState *ts) {
    return (ts && ts->pkg_scripts_buf) ? ts->pkg_scripts_buf : "";
}

size_t terminal_state_recent_bytes(const TerminalState *ts, char *out_buf,
                                   size_t max_bytes) {
    if (NULL == ts || NULL == ts->sb_buf || 0 == max_bytes)
        return 0;
    size_t n = ts->sb_len < max_bytes ? ts->sb_len : max_bytes;
    memcpy(out_buf, ts->sb_buf, n);
    return n;
}
