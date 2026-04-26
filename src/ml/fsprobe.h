#pragma once
#ifndef PHANTOM_FSPROBE_H
#define PHANTOM_FSPROBE_H

typedef struct FsProbe       FsProbe;
typedef struct TerminalState TerminalState;
typedef struct Terminal      Terminal;

/* Creates a background thread that watches the current working directory
 * for filesystem changes and reactively updates ts.
 *
 * On Apple platforms: uses kqueue EVFILT_VNODE to detect file additions /
 * removals (NOTE_WRITE) and directory renames/deletions (NOTE_DELETE,
 * NOTE_RENAME).  A 2-second kevent timeout serves as the CWD-change polling
 * fallback for detecting shell `cd` commands.
 *
 * On other platforms: falls back to a plain 2-second poll thread.
 *
 * ts and term must remain valid until fsprobe_destroy() returns.
 * term may be NULL; the screen snapshot is skipped in that case.
 */
FsProbe *fsprobe_create(TerminalState *ts, Terminal *term);
void     fsprobe_destroy(FsProbe *fp);

/* Signal the fsprobe thread to re-probe CWD, git branch, and filesystem map.
 * Call after a cd-family command completes.  Thread-safe; non-blocking. */
void     fsprobe_request_env_probe(FsProbe *fp);

#endif /* PHANTOM_FSPROBE_H */
