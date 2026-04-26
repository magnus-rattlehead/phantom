#include "autocomplete.h"
#include "ansi.h"
#include "ccg.h"
#include "context.h"
#include "fsprobe.h"
#include "history.h"
#include "htrie.h"
#include "llm.h"
#include "schema.h"
#include "../terminal.h"
#include "../config.h"

#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <SDL3/SDL.h>

#include "ml_log.h"
#define AC_LOG(...) ml_log("ac", __VA_ARGS__)

#if PHANTOM_DEBUG
static double ac_ms_since(struct timespec t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0.tv_sec) * 1e3
           + (t1.tv_nsec - t0.tv_nsec) / 1e6;
}
#  define AC_TICK(t0) clock_gettime(CLOCK_MONOTONIC, &(t0))
#  define AC_TOCK(label, t0) \
       AC_LOG("%-20s %6.1f ms", (label), ac_ms_since(t0))
#else
#  define AC_TICK(t0) ((void)0)
#  define AC_TOCK(label, t0) ((void)0)
#endif

static Uint32 g_event_ac_ready = 0;

void autocomplete_set_event_type(Uint32 event_type)
{
    g_event_ac_ready = event_type;
}

#define QTYPE_COMPLETION 0
#define QTYPE_NL         1

struct Autocomplete {
    History       *history;
    LLM           *llm_base;      /* base model for FIM completions */
    LLM           *llm_instruct;  /* instruct model for # NL queries */
    TerminalState *ts;       /* environmental context; may be NULL */
    Terminal      *term;     /* used to read exit codes; may be NULL */
    CCG           *ccg;      /* command-context graph; may be NULL */

    pthread_mutex_t  lock;
    pthread_cond_t   cond;
    pthread_cond_t   done_cond;  /* signaled when worker goes idle */
    pthread_t        thread;
    int              running;
    int              working;    /* 1 while a query is in flight */

    FsProbe         *fsprobe;    /* background kqueue watcher; may be NULL */
    HTrie           *htrie;      /* frequency-ranked prefix history lookup */

    char            *suggestion; /* protected by lock; read by main thread */
    char            *pending;    /* next query; protected by lock */
    volatile int     cancel;
    int              prepin;    /* 1 = re-pin base model on next idle tick */

    /* cached prefix text per model so we only re-pin when prefix changes */
    char            *last_prefix_base;
    char            *last_prefix_instruct;

    /* CCG training state (main-thread only) */
    char            *last_cmd;   /* previous command for state hash */
    size_t           cmd_count;  /* total commands recorded; triggers prune */
};

/* Appends printf-formatted text to buf[pos..cap); returns bytes written.
 * Silently truncates if cap is exhausted; buf is always NUL-terminated. */
static size_t apnd(char *buf, size_t cap, size_t pos,
                   const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static size_t apnd(char *buf, size_t cap, size_t pos, const char *fmt, ...)
{
    if (pos + 1 >= cap) return 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + pos, cap - pos, fmt, ap);
    va_end(ap);
    if (n <= 0) return 0;
    return ((size_t)n < cap - pos) ? (size_t)n : cap - pos - 1;
}

#define AC_WORD_BUF 64

static void extract_first_word(const char *s, char *out, size_t cap)
{
    while (' ' == *s || '\t' == *s) s++;
    size_t i = 0;
    while (*s && ' ' != *s && '\t' != *s && i < cap - 1)
        out[i++] = *s++;
    out[i] = '\0';
}

/* Returns 1 if the query likely involves file arguments (so the filesystem
 * listing is worth injecting); 0 to skip it and reduce context noise. */
static int query_needs_fs(const char *q)
{
    if (!q || '\0' == q[0]) return 0;
    if (strchr(q, '/'))  return 1;
    if ('.' == q[0] && '\0' != q[1]) return 1;

    static const char *const file_cmds[] = {
        "vim", "vi", "nvim", "nano", "emacs",
        "cat", "less", "more", "head", "tail",
        "cp", "mv", "rm", "ln", "chmod", "chown",
        "open", "code", "subl",
        "gcc", "clang", "cc", "g++",
        "python", "python3", "ruby", "node", "go",
        "diff", "patch", "wc", "grep", "awk", "sed",
        "source", ".", "sh", "bash", "zsh",
        NULL
    };
    char word[AC_WORD_BUF];
    extract_first_word(q, word, sizeof word);
    for (int k = 0; file_cmds[k]; k++)
        if (0 == strcmp(word, file_cmds[k])) return 1;
    return 0;
}

/* Writes a FIM prefix into out[0..out_cap). Returns bytes written (excl. NUL). */
size_t build_fim_prefix(History *history, Terminal *term,
                        TerminalState *ts,
                        CCG *ccg, const char *last_cmd,
                        const char *query,
                        char *out, size_t out_cap)
{
    if (NULL == out || 0 == out_cap) return 0;

    size_t pos = 0;

    pos += apnd(out, out_cap, pos, "<|fim_prefix|>");

    if (NULL != ts) {
        terminal_state_lock(ts);
        const char *cwd    = terminal_state_cwd(ts);
        const char *branch = terminal_state_git_branch(ts);
        if ('\0' != cwd[0])
            pos += apnd(out, out_cap, pos, "[CWD]: %s\n", cwd);
        if (branch && '\0' != branch[0])
            pos += apnd(out, out_cap, pos, "[GIT_BRANCH]: %s\n", branch);
        terminal_state_unlock(ts);
    }

    /* [LAST_CMD] with exit code; [STDERR] on non-zero exit */
    int exit_code = (NULL != term) ? terminal_exit_code(term) : -1;
    int have_cmd  = (NULL != last_cmd && '\0' != last_cmd[0]);
    if (have_cmd || exit_code > 0) {
        if (have_cmd && exit_code > 0)
            pos += apnd(out, out_cap, pos,
                        "[LAST_CMD]: %s (exit %d)\n", last_cmd, exit_code);
        else if (have_cmd)
            pos += apnd(out, out_cap, pos, "[LAST_CMD]: %s\n", last_cmd);
        else
            pos += apnd(out, out_cap, pos, "[LAST_CMD]: (exit %d)\n", exit_code);
    }
    if (exit_code > 0 && NULL != ts) {
        char raw[1024];
        size_t raw_len = terminal_state_recent_bytes(ts, raw, sizeof raw);
        if (raw_len > 0) {
            char stripped[1024];
            strip_ansi(raw, raw_len, stripped, sizeof stripped);
            if ('\0' != stripped[0])
                pos += apnd(out, out_cap, pos, "[STDERR]: %s\n", stripped);
        }
    }

    if (NULL != ccg) {
        const char *cwd = (NULL != ts) ? terminal_state_cwd(ts) : "";
        uint64_t h = ccg_hash_state(cwd, last_cmd ? last_cmd : "", exit_code);
        const char *top[3];
        int nk = ccg_top_k(ccg, h, top, 3);
        if (nk > 0) {
            pos += apnd(out, out_cap, pos, "[LIKELY]:");
            for (int i = 0; i < nk; i++)
                pos += apnd(out, out_cap, pos,
                            "%s%s", (0 == i) ? " " : ", ", top[i]);
            pos += apnd(out, out_cap, pos, "\n");
        }
    }

    /* [FILES]: filesystem map, only when query likely involves paths */
    if (NULL != ts && query_needs_fs(query)) {
        terminal_state_lock(ts);
        const char *fs = terminal_state_fs(ts);
        if ('\0' != fs[0])
            pos += apnd(out, out_cap, pos,
                        "[FILES]: %s"
                        "[/FILES]\n", fs);
        terminal_state_unlock(ts);
    }

    if (NULL != ts) {
        terminal_state_lock(ts);
        const char *mk = terminal_state_make_targets(ts);
        if ('\0' != mk[0])
            pos += apnd(out, out_cap, pos, "[MAKE_TARGETS]: %s\n", mk);
        const char *npm = terminal_state_pkg_scripts(ts);
        if ('\0' != npm[0])
            pos += apnd(out, out_cap, pos, "[NPM_SCRIPTS]: %s\n", npm);
        terminal_state_unlock(ts);
    }

    /* [HISTORY]: prefix-matching entries first, then remaining recent ones */
    char *cmds[AC_HISTORY_ENTRIES];
    size_t n = (NULL != history)
               ? history_recent(history, cmds, AC_HISTORY_ENTRIES)
               : 0;
    if (n > 0) {
        pos += apnd(out, out_cap, pos, "[HISTORY]:\n");
        for (size_t i = n; i > 0; i--)
            pos += apnd(out, out_cap, pos, "$ %s\n", cmds[i - 1]);
        for (size_t i = 0; i < n; i++) free(cmds[i]);
    }

    /* [DIR]: flat current-dir listing, highest priority for LLM */
    if (NULL != ts && query_needs_fs(query)) {
        terminal_state_lock(ts);
        const char *listing = terminal_state_cwd_listing(ts);
        if ('\0' != listing[0])
            pos += apnd(out, out_cap, pos, "[DIR]: %s\n", listing);
        terminal_state_unlock(ts);
    }

    pos += apnd(out, out_cap, pos, "[INPUT]: ");

    if (pos < out_cap) out[pos] = '\0';
    return pos;
}

/* Commands where ALL non-flag args are paths (e.g. cd, ls, cat).
 * Bare names without . or / are still checked for these. */
static const char *const g_all_path_cmds[] = {
    "cat", "less", "more", "head", "tail",
    "diff", "wc", "file", "du", "ls", "cd",
    "open",
    NULL
};

/* Commands where only tokens that look like paths (contain . or /) are
 * checked  -  the first arg may be a pattern or script name, not a path. */
static const char *const g_some_path_cmds[] = {
    "grep", "egrep", "fgrep", "rg", "ag",
    "python", "python3", "node", "ruby", "perl",
    "bash", "sh", "zsh", "fish", "source",
    NULL
};

/* Returns 0=skip check, 1=all tokens are paths, 2=path-looking only. */
static int path_cmd_mode(const char *cmd)
{
    for (int i = 0; g_all_path_cmds[i]; i++)
        if (0 == strcmp(cmd, g_all_path_cmds[i])) return 1;
    for (int i = 0; g_some_path_cmds[i]; i++)
        if (0 == strcmp(cmd, g_some_path_cmds[i])) return 2;
    return 0;
}

/* Returns 1 if the suggestion is safe to show, 0 if it references a
 * relative path that does not exist under cwd. Fast-path skips the check
 * entirely for commands not in either list. */
static int suggestion_valid(const char *query, const char *suggestion,
                             const char *cwd)
{
    if (!suggestion || !cwd || '\0' == cwd[0]) return 1;

    size_t qlen = strlen(query);
    size_t slen = strlen(suggestion);
    char  *full = malloc(qlen + slen + 1);
    if (!full) return 1;
    memcpy(full, query, qlen);
    memcpy(full + qlen, suggestion, slen + 1);

    /* Extract the command name */
    char *p = full;
    while (' ' == *p || '\t' == *p) p++;
    char cmd[AC_WORD_BUF] = {'\0'};
    extract_first_word(p, cmd, sizeof cmd);
    p += strlen(cmd);

    int mode = path_cmd_mode(cmd);
    if (!mode) { free(full); return 1; }

    int   valid   = 1;
    char *saveptr = NULL;
    char *tok     = strtok_r(p, " \t", &saveptr);
    while (tok && valid) {
        if ('-' == tok[0] || '\0' == tok[0]
            || '/' == tok[0] || '~' == tok[0]) {
            tok = strtok_r(NULL, " \t", &saveptr);
            continue;
        }

        /* Strip trailing slash before stat. */
        char clean[PATH_MAX];
        size_t tlen = strlen(tok);
        memcpy(clean, tok, tlen + 1);
        if (tlen > 1 && '/' == clean[tlen - 1])
            clean[--tlen] = '\0';

        int has_dot   = (NULL != strchr(clean, '.'));
        int has_slash = (NULL != strchr(clean, '/'));
        int should_check = (1 == mode) || (has_dot || has_slash);

        if (should_check) {
            char path[PATH_MAX];
            snprintf(path, sizeof path, "%s/%s", cwd, clean);
            struct stat st;
            if (0 != stat(path, &st)) {
                AC_LOG("filter: \"%s\" not found  -  discarding suggestion",
                       clean);
                valid = 0;
            }
        }
        tok = strtok_r(NULL, " \t", &saveptr);
    }

    free(full);
    return valid;
}

#if defined(__APPLE__) && PHANTOM_TESTING
#include <sys/qos.h>
volatile qos_class_t g_ac_worker_initial_qos = QOS_CLASS_UNSPECIFIED;
#endif

static void htrie_insert_cb(const char *cmd, void *t)
{
    htrie_insert((HTrie *)t, cmd);
}

static int is_nl_query(const char *q)
{
    while (' ' == *q || '\t' == *q) q++;
    return '#' == *q;
}

static int extract_path_arg(const char *query, char *out, size_t cap);

/* Strip a leading markdown code fence (```) from instruct output.
 * Returns cleaned heap string (original freed), or NULL on empty/error. */
static char *strip_md_fence(char *s)
{
    if (!s) return NULL;
    if (0 != strncmp(s, "```", 3)) return s;

    char *body = strchr(s, '\n');
    if (!body) { free(s); return NULL; }
    body++;

    char *close = strstr(body, "```");
    if (close) *close = '\0';

    size_t blen = strlen(body);
    while (blen > 0 && ('\n' == body[blen - 1] || '\r' == body[blen - 1]
                         || ' ' == body[blen - 1] || '\t' == body[blen - 1]))
        body[--blen] = '\0';

    char *result = (blen > 0) ? strdup(body) : NULL;
    free(s);
    return result;
}

/* Worker: Phase 1 (under lock: CCG/HTrie/Schema) -> Phase 1.5 (no lock:
 * make/npm + FS) -> instant delivery if hit -> Phase 2 (debounced LLM). */
static void *worker(void *arg)
{
    Autocomplete *ac = arg;

#if defined(__APPLE__)
#if PHANTOM_TESTING
    g_ac_worker_initial_qos = qos_class_self();
#endif
#endif

    pthread_mutex_lock(&ac->lock);
    while (ac->running) {
        if (NULL == ac->pending) {
            ac->working = 0;
            pthread_cond_broadcast(&ac->done_cond);
        }
        while (ac->running && !ac->pending && !ac->prepin) {
#if defined(__APPLE__)
            /* Drop to BACKGROUND while idle so we don't compete with UI. */
            pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
#endif
            pthread_cond_wait(&ac->cond, &ac->lock);
        }

        if (!ac->running) break;

        /* Proactive re-pin: runs right after Enter while command output prints. */
        if (ac->prepin && !ac->pending) {
            ac->prepin = 0;
            char prepin_snap[PATH_MAX] = {'\0'};
            if (ac->last_cmd)
                snprintf(prepin_snap, sizeof prepin_snap,
                         "%s", ac->last_cmd);
#if defined(__APPLE__)
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
            pthread_mutex_unlock(&ac->lock);

            if (ac->ts) terminal_state_probe_screen(ac->ts, ac->term);

            if (ac->llm_base) {
                size_t pcap = 64 + 1100 + CONTEXT_FS_BYTES + PATH_MAX
                              + CONTEXT_CWD_LISTING_BYTES
                              + AC_HISTORY_ENTRIES * 520 + 1024;
                char *pbuf = malloc(pcap);
                if (pbuf) {
                    build_fim_prefix(ac->history, ac->term, ac->ts,
                                     ac->ccg, prepin_snap, "",
                                     pbuf, pcap);
                    if (!ac->last_prefix_base
                        || 0 != strcmp(ac->last_prefix_base, pbuf)) {
                        AC_LOG("prepin: base model");
                        llm_pin_history(ac->llm_base, pbuf);
                        free(ac->last_prefix_base);
                        ac->last_prefix_base = strdup(pbuf);
                    }
                    free(pbuf);
                }
            }

            pthread_mutex_lock(&ac->lock);
            continue;
        }

        /* Raise QOS before re-acquiring lock to avoid priority inversion. */
#if defined(__APPLE__)
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif

        char *query = ac->pending;
        ac->pending = NULL;
        ac->cancel  = 0;

        int instant_ready = 0;
        int skip_llm      = 0;

        /* Phase 1: instant in-memory experts (under lock). */
        if (!instant_ready && NULL != ac->ccg) {
            const char *cwd = (NULL != ac->ts)
                              ? terminal_state_cwd(ac->ts) : "";
            int exit_code = (NULL != ac->term)
                            ? terminal_exit_code(ac->term) : -1;
            uint64_t h = ccg_hash_state(cwd,
                                        ac->last_cmd ? ac->last_cmd : "",
                                        exit_code);
            uint32_t freq = 0;
            const char *instant = ccg_lookup(ac->ccg, h, &freq);
            if (NULL != instant) {
                size_t typed_len = strlen(query);
                if (typed_len == 0
                    || (strncmp(instant, query, typed_len) == 0
                        && '\0' != instant[typed_len])) {
                    AC_LOG("CCG instant hit: \"%s\" (freq=%u)",
                           instant, freq);
                    free(ac->suggestion);
                    ac->suggestion = strdup(instant + typed_len);
                    instant_ready  = 1;
                }
            }
        }

        if (!instant_ready && NULL != ac->htrie
            && strlen(query) >= HTRIE_MIN_PREFIX) {
            const char *best = htrie_best(ac->htrie, query,
                                          HTRIE_MIN_FREQ);
            if (NULL != best) {
                size_t typed_len = strlen(query);
                AC_LOG("HTrie hit: \"%s\"", best);
                free(ac->suggestion);
                ac->suggestion = strdup(best + typed_len);
                instant_ready  = 1;
            }
        }

        if (!instant_ready) {
            char schema_buf[256];
            if (schema_complete(query, schema_buf, sizeof schema_buf) > 0) {
                AC_LOG("schema hit: appending \"%s\"", schema_buf);
                free(ac->suggestion);
                ac->suggestion = strdup(schema_buf);
                instant_ready  = 1;
            }
        }

        /* Snapshot last_cmd before unlocking for Phase 2 use. */
        char last_cmd_snap[PATH_MAX] = {'\0'};
        if (ac->last_cmd)
            snprintf(last_cmd_snap, sizeof last_cmd_snap,
                     "%s", ac->last_cmd);

        pthread_mutex_unlock(&ac->lock);

        /* Phase 1.5: I/O experts (no ac->lock held). */
        if (!instant_ready && NULL != ac->ts) {
            const char *p = query;
            while (' ' == *p || '\t' == *p) p++;
            int is_make = (0 == strncmp(p, "make ", 5) ||
                           0 == strncmp(p, "make\t", 5));
            int is_npm  = (0 == strncmp(p, "npm run ", 8));
            if (is_make || is_npm) {
                const char *typed = p + (is_make ? 5 : 8);
                while ('-' == *typed) {
                    while (*typed && ' ' != *typed) typed++;
                    while (' ' == *typed) typed++;
                }
                size_t typed_len = strlen(typed);
                terminal_state_lock(ac->ts);
                const char *pool = is_make
                                   ? terminal_state_make_targets(ac->ts)
                                   : terminal_state_pkg_scripts(ac->ts);
                if (NULL != pool && '\0' != pool[0]) {
                    const char *match    = NULL;
                    int         n_matches = 0;
                    const char *lp       = pool;
                    while (*lp) {
                        const char *le = strchr(lp, '\n');
                        if (NULL == le) le = lp + strlen(lp);
                        size_t ll = (size_t)(le - lp);
                        if (ll >= typed_len &&
                            0 == strncmp(lp, typed, typed_len)) {
                            match = lp;
                            n_matches++;
                            if (n_matches > 1) break;
                        }
                        lp = ('\0' != *le) ? le + 1 : le;
                    }
                    if (1 == n_matches) {
                        const char *me = strchr(match, '\n');
                        size_t ml = me ? (size_t)(me - match)
                                       : strlen(match);
                        if (ml > typed_len) {
                            size_t sl  = ml - typed_len;
                            char  *suf = malloc(sl + 1);
                            if (NULL != suf) {
                                memcpy(suf, match + typed_len, sl);
                                suf[sl] = '\0';
                                AC_LOG("make-target hit: \"%s\"", suf);
                                pthread_mutex_lock(&ac->lock);
                                if (!ac->cancel) {
                                    free(ac->suggestion);
                                    ac->suggestion = suf;
                                    instant_ready  = 1;
                                } else {
                                    free(suf);
                                }
                                pthread_mutex_unlock(&ac->lock);
                            }
                        }
                    }
                }
                terminal_state_unlock(ac->ts);
            }
        }

        if (!instant_ready && NULL != ac->ts) {
            char cmd25[AC_WORD_BUF] = {'\0'};
            extract_first_word(query, cmd25, sizeof cmd25);
            int is_path_cmd = (1 == path_cmd_mode(cmd25));
            if (is_path_cmd) {
                skip_llm = 1;
                char partial[PATH_MAX];
                if (1 == extract_path_arg(query, partial,
                                          sizeof partial)) {
                    char cwd_snap[PATH_MAX];
                    terminal_state_lock(ac->ts);
                    snprintf(cwd_snap, sizeof cwd_snap, "%s",
                             terminal_state_cwd(ac->ts));
                    terminal_state_unlock(ac->ts);
                    if ('\0' != cwd_snap[0]) {
                        char suffix[NAME_MAX + 2];
                        int n = fs_complete_path(cwd_snap, partial,
                                                 suffix, sizeof suffix);
                        if (n >= 0) {
                            AC_LOG("fs_complete hit: \"%s\"", suffix);
                            pthread_mutex_lock(&ac->lock);
                            if (!ac->cancel) {
                                free(ac->suggestion);
                                ac->suggestion = strdup(suffix);
                                instant_ready  = 1;
                            }
                            pthread_mutex_unlock(&ac->lock);
                        }
                    }
                }
            } else {
                char partial[PATH_MAX];
                if (1 == extract_path_arg(query, partial,
                                          sizeof partial)
                    && NULL != strchr(partial, '/')) {
                    skip_llm = 1;
                    char cwd_snap[PATH_MAX];
                    terminal_state_lock(ac->ts);
                    snprintf(cwd_snap, sizeof cwd_snap, "%s",
                             terminal_state_cwd(ac->ts));
                    terminal_state_unlock(ac->ts);
                    if ('\0' != cwd_snap[0]) {
                        char suffix[NAME_MAX + 2];
                        int n = fs_complete_path(cwd_snap, partial,
                                                 suffix, sizeof suffix);
                        if (n >= 0) {
                            AC_LOG("fs path hit: \"%s\"", suffix);
                            pthread_mutex_lock(&ac->lock);
                            if (!ac->cancel) {
                                free(ac->suggestion);
                                ac->suggestion = strdup(suffix);
                                instant_ready  = 1;
                            }
                            pthread_mutex_unlock(&ac->lock);
                        }
                    }
                }
            }
        }

        if (instant_ready) {
            if (g_event_ac_ready) {
                SDL_Event e;
                SDL_zero(e);
                e.type = g_event_ac_ready;
                SDL_PushEvent(&e);
            }
            free(query);
            pthread_mutex_lock(&ac->lock);
            continue;
        }

        if (skip_llm) {
            free(query);
            pthread_mutex_lock(&ac->lock);
            continue;
        }

        /* Phase 2: LLM inference; cancel if superseded. */
        pthread_mutex_lock(&ac->lock);
        if (ac->cancel) {
            AC_LOG("cancelled before LLM: \"%s\"", query);
            free(query);
            continue;
        }
        pthread_mutex_unlock(&ac->lock);

        {
            struct timespec _t0;
            AC_TICK(_t0);
            if (ac->ts) terminal_state_probe_screen(ac->ts, ac->term);
            AC_TOCK("probe_screen", _t0);
        }

        int  qtype   = is_nl_query(query) ? QTYPE_NL : QTYPE_COMPLETION;
        LLM *llm     = (QTYPE_NL == qtype) ? ac->llm_instruct : ac->llm_base;
        int  max_tok = (QTYPE_NL == qtype) ? AC_NL_MAX_TOKENS : AC_MAX_TOKENS;

        if (!llm) {
            AC_LOG("no model for qtype=%d, skipping", qtype);
            free(query);
            pthread_mutex_lock(&ac->lock);
            continue;
        }

        if (QTYPE_COMPLETION == qtype) {
            /* FIM prefix for base model: <|fim_prefix|> + context */
            size_t prefix_cap = 64 + 1100 + CONTEXT_FS_BYTES + PATH_MAX
                                + CONTEXT_CWD_LISTING_BYTES
                                + AC_HISTORY_ENTRIES * 520
                                + 1024; /* make targets + npm scripts */
            char  *prefix_buf = malloc(prefix_cap);
            if (!prefix_buf) {
                free(query); pthread_mutex_lock(&ac->lock); continue;
            }
            {
                struct timespec _t0;
                AC_TICK(_t0);
                build_fim_prefix(ac->history, ac->term, ac->ts,
                                 ac->ccg, last_cmd_snap,
                                 query, prefix_buf, prefix_cap);
                AC_TOCK("build_prefix", _t0);
            }

            int prefix_changed = !ac->last_prefix_base
                                 || 0 != strcmp(ac->last_prefix_base,
                                                prefix_buf);
            if (prefix_changed) {
                AC_LOG("prefix changed: re-pinning base");
                struct timespec _t0;
                AC_TICK(_t0);
                llm_pin_history(llm, prefix_buf);
                AC_TOCK("pin_history", _t0);
                free(ac->last_prefix_base);
                ac->last_prefix_base = strdup(prefix_buf);
            }
            free(prefix_buf);
        } else {
            /* Chat template prefix for instruct model: system + user header */
            char nl_prefix[PATH_MAX + 320];
            size_t p = 0;
            p += (size_t)snprintf(nl_prefix + p, sizeof nl_prefix - p,
                "<|im_start|>system\n"
                "You are a shell assistant. Output ONLY raw shell "
                "code  -  commands or scripts. No markdown, no code "
                "fences (no ``` or ~~~), no explanation, no prose. "
                "Multi-line scripts are allowed.\n"
                "<|im_end|>\n"
                "<|im_start|>user\n");
            if (ac->ts) {
                terminal_state_lock(ac->ts);
                const char *cwd = terminal_state_cwd(ac->ts);
                if (cwd && '\0' != cwd[0])
                    p += (size_t)snprintf(nl_prefix + p,
                                          sizeof nl_prefix - p,
                                          "CWD: %s\n", cwd);
                terminal_state_unlock(ac->ts);
            }

            int prefix_changed = !ac->last_prefix_instruct
                                 || 0 != strcmp(ac->last_prefix_instruct,
                                                nl_prefix);
            if (prefix_changed) {
                AC_LOG("nl prefix changed: re-pinning instruct");
                llm_pin_history(llm, nl_prefix);
                free(ac->last_prefix_instruct);
                ac->last_prefix_instruct = strdup(nl_prefix);
            }
        }

        /* Decode suffix into ephemeral seq 1. */
        size_t qlen    = strlen(query);
        size_t sfx_cap = qlen + 64;
        char  *sfx_buf = malloc(sfx_cap);
        if (!sfx_buf) {
            free(query); pthread_mutex_lock(&ac->lock); continue;
        }
        if (QTYPE_NL == qtype) {
            /* Instruct suffix: user text + turn boundaries + assistant start */
            snprintf(sfx_buf, sfx_cap,
                     "%s\n<|im_end|>\n<|im_start|>assistant\n", query);
        } else {
            /* FIM suffix for base model */
            snprintf(sfx_buf, sfx_cap,
                     "%s<|fim_suffix|>\n<|fim_middle|>", query);
        }

        /* USER_INITIATED keeps decode on P-cores without competing at
         * the same priority as the render/PTY main thread. */
#if defined(__APPLE__)
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
        {
            struct timespec _t0;
            AC_TICK(_t0);
            llm_decode_suffix(llm, sfx_buf, &ac->cancel);
            AC_TOCK("decode_suffix", _t0);
        }
        free(sfx_buf);

        /* Generate completion from combined seq-0+seq-1 KV state.
         * For NL queries: allow newlines and continue in blocks up to
         * AC_NL_MAX_TOTAL_TOKENS to handle multi-sentence / script output. */
        int   allow_nl   = (QTYPE_NL == qtype);
        char *result     = NULL;
        {
            struct timespec _t0;
            AC_TICK(_t0);
            result = llm_complete_from_seq(llm, max_tok, allow_nl,
                                           &ac->cancel);
            if (allow_nl && result) {
                size_t total_bytes = strlen(result);
                size_t cap         = total_bytes + 1;
                while (!ac->cancel) {
                    char *more = llm_continue(llm, max_tok, allow_nl,
                                              &ac->cancel);
                    if (!more) break;
                    size_t mlen = strlen(more);
                    if (total_bytes + mlen + 1 > cap) {
                        cap = (total_bytes + mlen + 1) * 2;
                        char *grown = realloc(result, cap);
                        if (!grown) { free(more); break; }
                        result = grown;
                    }
                    memcpy(result + total_bytes, more, mlen + 1);
                    total_bytes += mlen;
                    free(more);
                }
            }
            AC_TOCK("complete", _t0);
        }
#if defined(__APPLE__)
        pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
#endif

        /* Strip leading, trailing whitespace, newlines */
        if (result) {
            char *p = result;
            while (' ' == *p || '\t' == *p) p++;
            if (p != result) {
                char *trimmed = strdup(p);
                free(result);
                result = trimmed;
            }
        }

        if (result) {
            size_t rlen = strlen(result);
            while (rlen > 0 && ('\n' == result[rlen - 1]
                                 || '\r' == result[rlen - 1]
                                 || ' '  == result[rlen - 1]
                                 || '\t' == result[rlen - 1]))
                result[--rlen] = '\0';
            if (0 == rlen) { free(result); result = NULL; }
        }

        if (QTYPE_NL == qtype)
            result = strip_md_fence(result);

        /* Discard suggestions that reference non-existent local paths.
         * Skip for NL queries and multiline output. */
        if (result && ac->ts && QTYPE_COMPLETION == qtype
            && NULL == strchr(result, '\n')) {
            char cwd_snap[PATH_MAX] = {'\0'};
            terminal_state_lock(ac->ts);
            const char *cwd_ptr = terminal_state_cwd(ac->ts);
            if (cwd_ptr) memcpy(cwd_snap, cwd_ptr, PATH_MAX);
            terminal_state_unlock(ac->ts);
            if (!suggestion_valid(query, result, cwd_snap)) {
                free(result);
                result = NULL;
            }
        }
        free(query);

        pthread_mutex_lock(&ac->lock);
        if (result && !ac->cancel) {
            AC_LOG("suggestion ready: \"%s\"", result);
            free(ac->suggestion);
            ac->suggestion = result;
            pthread_mutex_unlock(&ac->lock);

            /* Wake the main SDL event loop. */
            if (g_event_ac_ready) {
                SDL_Event e;
                SDL_zero(e);
                e.type = g_event_ac_ready;
                SDL_PushEvent(&e);
            }
            pthread_mutex_lock(&ac->lock);
        } else {
            if (ac->cancel) AC_LOG("inference cancelled");
            free(result);
        }
    }
    pthread_mutex_unlock(&ac->lock);
    return NULL;
}

Autocomplete *autocomplete_create(History *history,
                                  LLM *llm_base, LLM *llm_instruct,
                                  TerminalState *ts, Terminal *term,
                                  CCG *ccg)
{
    Autocomplete *ac = calloc(1, sizeof *ac);
    if (!ac) return NULL;
    ac->history      = history;
    ac->llm_base     = llm_base;
    ac->llm_instruct = llm_instruct;
    ac->ts      = ts;
    ac->term    = term;
    ac->ccg     = ccg;
    ac->running  = 1;
    ac->fsprobe  = fsprobe_create(ts, term);

    /* Build history trie from all stored commands. */
    ac->htrie = htrie_create();
    if (ac->htrie && history)
        history_each(history, htrie_insert_cb, ac->htrie);
    pthread_mutex_init(&ac->lock, NULL);
    pthread_cond_init(&ac->cond, NULL);
    pthread_cond_init(&ac->done_cond, NULL);
#if defined(__APPLE__)
    {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_set_qos_class_np(&attr, QOS_CLASS_BACKGROUND, 0);
        pthread_create(&ac->thread, &attr, worker, ac);
        pthread_attr_destroy(&attr);
    }
#else
    pthread_create(&ac->thread, NULL, worker, ac);
#endif
    return ac;
}

void autocomplete_destroy(Autocomplete *ac)
{
    if (!ac) return;
    fsprobe_destroy(ac->fsprobe);
    htrie_destroy(ac->htrie);
    pthread_mutex_lock(&ac->lock);
    ac->running = 0;
    ac->cancel  = 1;
    pthread_cond_signal(&ac->cond);
    pthread_mutex_unlock(&ac->lock);
    pthread_join(ac->thread, NULL);
    pthread_cond_destroy(&ac->done_cond);
    pthread_cond_destroy(&ac->cond);
    pthread_mutex_destroy(&ac->lock);
    free(ac->suggestion);
    free(ac->pending);
    free(ac->last_prefix_base);
    free(ac->last_prefix_instruct);
    free(ac->last_cmd);
    free(ac);
}

static int extract_path_arg(const char *query, char *out, size_t cap)
{
    if (NULL == query || '\0' == query[0]) return -1;

    size_t len = strlen(query);

    if (' ' == query[len - 1]) return 0;

    const char *tok = query + len;
    while (tok > query && ' ' != tok[-1]) tok--;

    if (tok == query) {
        /* First token: only proceed for path-like patterns (contains /,
         * starts with ~ or /) to avoid completing command names as paths.
         * Relative paths with / (e.g. src/, ./build/) are also accepted. */
        if ('/' != tok[0] && '~' != tok[0]
            && NULL == strchr(tok, '/'))
            return -1;
    }
    if ('-' == tok[0]) return -1;

    if ('~' == tok[0]) {
        const char *home = getenv("HOME");
        if (NULL == home || '\0' == home[0]) return -1;
        size_t home_len = strlen(home);
        size_t rest_len = (size_t)(query + len - tok) - 1;
        if (home_len + rest_len >= cap) return -1;
        memcpy(out, home, home_len);
        memcpy(out + home_len, tok + 1, rest_len);
        out[home_len + rest_len] = '\0';
        AC_LOG("extract_path_arg: ~ expanded to \"%s\"", out);
        return 1;
    }

    size_t tlen = (size_t)(query + len - tok);
    if (0 == tlen || tlen >= cap) return -1;
    memcpy(out, tok, tlen);
    out[tlen] = '\0';
    return 1;
}

void autocomplete_query(Autocomplete *ac, const char *current_line)
{
    if (!ac || !current_line) return;
    pthread_mutex_lock(&ac->lock);
    free(ac->suggestion);
    ac->suggestion = NULL;
    ac->cancel     = 1;
    ac->working    = 1;
    free(ac->pending);
    ac->pending = strdup(current_line);
    pthread_cond_signal(&ac->cond);
    pthread_mutex_unlock(&ac->lock);
}

const char *autocomplete_get_suggestion(const Autocomplete *ac)
{
    return ac ? ac->suggestion : NULL;
}

void autocomplete_clear(Autocomplete *ac)
{
    if (!ac) return;
    pthread_mutex_lock(&ac->lock);
    free(ac->suggestion);
    ac->suggestion = NULL;
    pthread_mutex_unlock(&ac->lock);
}

void autocomplete_record_command(Autocomplete *ac,
                                 const char   *cmd,
                                 int           exit_code)
{
    if (NULL == ac || NULL == ac->ccg || NULL == cmd) return;

    pthread_mutex_lock(&ac->lock);
    const char *cwd = (NULL != ac->ts)
                      ? terminal_state_cwd(ac->ts)
                      : "";
    uint64_t h = ccg_hash_state(cwd,
                                ac->last_cmd ? ac->last_cmd : "",
                                exit_code);
    ccg_train(ac->ccg, h, cmd);
    htrie_insert(ac->htrie, cmd);
    free(ac->last_cmd);
    ac->last_cmd = strdup(cmd);
    if (0 == ++ac->cmd_count % CCG_PRUNE_INTERVAL) {
        AC_LOG("CCG prune triggered at cmd_count=%zu", ac->cmd_count);
        ccg_prune(ac->ccg);
    }
    if (ac->llm_base) {
        ac->prepin = 1;
        pthread_cond_signal(&ac->cond);
    }
    pthread_mutex_unlock(&ac->lock);
}

#if PHANTOM_TESTING
void autocomplete_drain(Autocomplete *ac)
{
    if (!ac) return;
    pthread_mutex_lock(&ac->lock);
    while (ac->working)
        pthread_cond_wait(&ac->done_cond, &ac->lock);
    pthread_mutex_unlock(&ac->lock);
}
#endif

void autocomplete_request_env_probe(Autocomplete *ac)
{
    if (NULL != ac) fsprobe_request_env_probe(ac->fsprobe);
}
