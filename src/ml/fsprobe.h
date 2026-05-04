#pragma once
#ifndef PHANTOM_FSPROBE_H
#define PHANTOM_FSPROBE_H

typedef struct FsProbe FsProbe;
typedef struct TerminalState TerminalState;
typedef struct Terminal Terminal;

/* Creates a background thread that watches the CWD for filesystem changes
 * and reactively updates ts.
 *
 * macOS: kqueue EVFILT_VNODE (NOTE_WRITE/DELETE/RENAME).  A 2-second
 * kevent timeout is the CWD-change fallback for shells that skip OSC 7.
 * Linux: inotify_add_watch + poll; same 2-second timeout for the fallback.
 *
 * ts and term must remain valid until fsprobe_destroy() returns.
 * term may be NULL; the screen snapshot is skipped in that case.
 */
FsProbe *fsprobe_create(TerminalState *ts, Terminal *term);
void fsprobe_destroy(FsProbe *fp);

/* Signal the fsprobe thread to re-probe CWD, git branch, and filesystem map.
 * Call after a cd-family command completes.  Thread-safe; non-blocking. */
void fsprobe_request_env_probe(FsProbe *fp);

#endif /* PHANTOM_FSPROBE_H */
