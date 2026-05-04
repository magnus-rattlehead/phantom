#pragma once
#ifndef PHANTOM_CONTEXT_H
#define PHANTOM_CONTEXT_H

#include <stddef.h>
#include <sys/types.h>

typedef struct TerminalState TerminalState;
typedef struct Terminal Terminal;

/* Allocates a TerminalState.  term may be NULL; the scrollback snapshot
 * is skipped. Caller must call terminal_state_destroy(). */
TerminalState *terminal_state_create(Terminal *term);

void terminal_state_destroy(TerminalState *ts);

/* Combined probe: refreshes CWD, git branch, fs map, and screen snapshot.
 * Equivalent to probe_env + probe_screen. term may be NULL. */
void terminal_state_probe(TerminalState *ts, Terminal *term);

/* Refreshes CWD, git branch, and filesystem map only (no screen snapshot).
 * Acquires the internal lock while writing; safe to call from any thread. */
void terminal_state_probe_env(TerminalState *ts);

/* Completes a filesystem path relative to cwd.
 * partial_arg is the last non-flag token from the query (e.g. "fi",
 * "src/ml/a"). If exactly one directory entry in the implied parent dir matches
 * the prefix: writes the name-remainder (plus "/" for directories) into
 * out[0..cap), NUL-terminates, and returns byte count (0 for exact match).
 * Returns -1 if zero or two-or-more entries match, or on any I/O error.
 * Thread-safe */
int fs_complete_path(const char *cwd, const char *partial_arg, char *out,
                     size_t cap);

/* Set the PID of the child shell process so probe_env can read its CWD
 * instead of the phantom process's own CWD.  Call after pty_open(). */
void terminal_state_set_shell_pid(TerminalState *ts, pid_t pid);

/* Directly set the CWD from an OSC 7 notification.  Thread-safe.
 * Bypasses probe_env; fs map and git branch are NOT updated here, call
 * autocomplete_request_env_probe() afterwards to schedule that work. */
void terminal_state_update_cwd(TerminalState *ts, const char *cwd);

/* Rebuilds the filesystem map from the current CWD without re-probing git.
 * Called on NOTE_WRITE when only directory contents changed. */
void terminal_state_probe_fs(TerminalState *ts);

/* Refreshes the visible-screen snapshot only.
 * Called per-query from the autocomplete worker. term may be NULL. */
void terminal_state_probe_screen(TerminalState *ts, Terminal *term);

/* Lock / unlock the internal mutex that protects cwd, git_branch, and
 * fs_buf.  Hold the lock across any multi-field read sequence to prevent
 * a concurrent probe_env from swapping fs_buf mid-read. */
void terminal_state_lock(TerminalState *ts);
void terminal_state_unlock(TerminalState *ts);

/* Accessors  -  return interior pointers.  Callers must hold the lock when
 * reading more than one field, or when the pointer is used after a potential
 * probe_env call on another thread. */
const char *terminal_state_cwd(const TerminalState *ts);
const char *terminal_state_git_branch(const TerminalState *ts);
const char *terminal_state_fs(const TerminalState *ts);
const char *terminal_state_cwd_listing(const TerminalState *ts);
const char *terminal_state_make_targets(const TerminalState *ts);
const char *terminal_state_pkg_scripts(const TerminalState *ts);

/* Copies up to max_bytes of the most recent visible-screen text (UTF-8,
 * ANSI-stripped) into out_buf.  Returns bytes written; no NUL guarantee.
 * Returns 0 if the terminal snapshot is empty or ts has no snapshot. */
size_t terminal_state_recent_bytes(const TerminalState *ts, char *out_buf,
                                   size_t max_bytes);

#endif /* PHANTOM_CONTEXT_H */
