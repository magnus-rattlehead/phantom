#include "fsprobe.h"
#include "context.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/event.h>
#include <sys/qos.h>
#include <sys/time.h>
#else
#include <poll.h>
#include <sys/inotify.h>
#endif

#include "ml_log.h"
#define FP_LOG(...) ml_log("fsprobe", __VA_ARGS__)

struct FsProbe {
    TerminalState *ts;
    Terminal *term;
    pthread_t thread;
    volatile int running;
    volatile int probe_requested; /* set by fsprobe_request_env_probe */
    int wake_pipe[2]; /* write one byte to [1] to interrupt the loop */
#if defined(__APPLE__)
    int kq;
    int dir_fd; /* O_EVTONLY fd on the watched directory */
#else
    int inotify_fd;
    int inotify_wd; /* -1 = no directory watched yet */
#endif
};

#if defined(__APPLE__)

/* Register dir_fd with kqueue; EV_CLEAR prevents event batching. */
static void kq_watch_dir(int kq, int dir_fd) {
    struct kevent change;
    EV_SET(&change, (uintptr_t)dir_fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_DELETE | NOTE_RENAME, 0, NULL);
    kevent(kq, &change, 1, NULL, 0, NULL);
}

/* O_EVTONLY watches path without preventing unmount. */
static void fsprobe_watch(FsProbe *fp, const char *path) {
    if (fp->dir_fd >= 0) {
        struct kevent change;
        EV_SET(&change, (uintptr_t)fp->dir_fd, EVFILT_VNODE, EV_DELETE, 0, 0,
               NULL);
        kevent(fp->kq, &change, 1, NULL, 0, NULL);
        close(fp->dir_fd);
    }
    fp->dir_fd = open(path, O_RDONLY | O_EVTONLY);
    if (fp->dir_fd < 0) {
        FP_LOG("open O_EVTONLY failed for %s", path);
        return;
    }
    kq_watch_dir(fp->kq, fp->dir_fd);
    FP_LOG("watching %s (fd=%d)", path, fp->dir_fd);
}

static void *fsprobe_thread(void *arg) {
    FsProbe *fp = arg;

#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif

    terminal_state_probe_env(fp->ts);
    terminal_state_lock(fp->ts);
    char watched[PATH_MAX];
    memcpy(watched, terminal_state_cwd(fp->ts), PATH_MAX);
    terminal_state_unlock(fp->ts);
    fsprobe_watch(fp, watched);

    while (fp->running) {
        struct kevent events[8];
        struct timespec timeout = {2, 0};

        int n = kevent(fp->kq, NULL, 0, events, 8, &timeout);
        if (!fp->running)
            break;

        int need_fs = 0;

        if (0 == n) {
            /* kevent timed out, periodic CWD poll (fallback for shells that
             * don't emit OSC 7, and for test environments using chdir()). */
            fp->probe_requested = 1;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].filter == EVFILT_READ) {
                /* Drain the wake pipe so kevent doesn't fire again. */
                char drain[64];
                while (read(fp->wake_pipe[0], drain, sizeof drain) > 0) {
                }
                continue;
            }
            if (events[i].filter != EVFILT_VNODE)
                continue;
            uint32_t flags = (uint32_t)events[i].fflags;
            if (flags & (NOTE_DELETE | NOTE_RENAME)) {
                FP_LOG("watched dir deleted/renamed, requesting env probe");
                fp->probe_requested = 1;
            } else if (flags & NOTE_WRITE) {
                FP_LOG("NOTE_WRITE, rescanning fs");
                need_fs = 1;
            }
        }

        if (fp->probe_requested) {
            fp->probe_requested = 0;
            terminal_state_probe_env(fp->ts);
            terminal_state_lock(fp->ts);
            char cur_cwd[PATH_MAX];
            memcpy(cur_cwd, terminal_state_cwd(fp->ts), PATH_MAX);
            terminal_state_unlock(fp->ts);
            if (0 != strcmp(cur_cwd, watched)) {
                FP_LOG("CWD changed: %s -> %s", watched, cur_cwd);
                memcpy(watched, cur_cwd, sizeof watched);
                fsprobe_watch(fp, watched);
            }
            need_fs = 0;
        }

        if (need_fs) {
            terminal_state_probe_fs(fp->ts);
        }
    }

    return NULL;
}

#else

static void fsprobe_watch(FsProbe *fp, const char *path) {
    if (fp->inotify_wd >= 0) {
        inotify_rm_watch(fp->inotify_fd, fp->inotify_wd);
        fp->inotify_wd = -1;
    }
    fp->inotify_wd =
        inotify_add_watch(fp->inotify_fd, path,
                          IN_CREATE | IN_DELETE | IN_MOVE | IN_CLOSE_WRITE |
                              IN_DELETE_SELF | IN_MOVE_SELF);
    if (fp->inotify_wd < 0) {
        FP_LOG("inotify_add_watch failed for %s: %s", path, strerror(errno));
        return;
    }
    FP_LOG("watching %s (wd=%d)", path, fp->inotify_wd);
}

static void *fsprobe_thread(void *arg) {
    FsProbe *fp = arg;

    terminal_state_probe_env(fp->ts);
    terminal_state_lock(fp->ts);
    char watched[PATH_MAX];
    memcpy(watched, terminal_state_cwd(fp->ts), PATH_MAX);
    terminal_state_unlock(fp->ts);
    fsprobe_watch(fp, watched);

    while (fp->running) {
        struct pollfd fds[2];
        fds[0].fd = fp->inotify_fd;
        fds[0].events = POLLIN;
        fds[1].fd = fp->wake_pipe[0];
        fds[1].events = POLLIN;

        int n = poll(fds, 2, 2000);
        if (!fp->running)
            break;

        if (0 == n) {
            /* timeout  -  periodic env probe for shells without OSC 7 */
            fp->probe_requested = 1;
        }

        if (fds[1].revents & POLLIN) {
            char drain[64];
            while (read(fp->wake_pipe[0], drain, sizeof drain) > 0) {
            }
        }

        int need_fs = 0;
        if (fds[0].revents & POLLIN) {
            char buf[4096];
            ssize_t len;
            while ((len = read(fp->inotify_fd, buf, sizeof buf)) > 0) {
                char *p = buf;
                while (p < buf + len) {
                    struct inotify_event *ev = (struct inotify_event *)p;
                    if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
                        FP_LOG("watched dir deleted/moved  -  requesting env "
                               "probe");
                        fp->probe_requested = 1;
                    } else {
                        need_fs = 1;
                    }
                    p += sizeof(struct inotify_event) + ev->len;
                }
            }
        }

        if (fp->probe_requested) {
            fp->probe_requested = 0;
            terminal_state_probe_env(fp->ts);
            terminal_state_lock(fp->ts);
            char cur_cwd[PATH_MAX];
            memcpy(cur_cwd, terminal_state_cwd(fp->ts), PATH_MAX);
            terminal_state_unlock(fp->ts);
            if (0 != strcmp(cur_cwd, watched)) {
                FP_LOG("CWD changed: %s -> %s", watched, cur_cwd);
                memcpy(watched, cur_cwd, sizeof watched);
                fsprobe_watch(fp, watched);
            }
            need_fs = 0;
        }

        if (need_fs) {
            terminal_state_probe_fs(fp->ts);
        }
    }
    return NULL;
}

#endif

FsProbe *fsprobe_create(TerminalState *ts, Terminal *term) {
    FsProbe *fp = calloc(1, sizeof *fp);
    if (NULL == fp)
        return NULL;
    fp->ts = ts;
    fp->term = term;
    fp->running = 1;

#if defined(__APPLE__)
    fp->kq = kqueue();
    fp->dir_fd = -1;
    if (fp->kq < 0) {
        free(fp);
        return NULL;
    }

    /* Wake pipe interrupts blocking kevent(); read-end is O_NONBLOCK
     * so draining it never blocks the thread. */
    if (0 != pipe(fp->wake_pipe)) {
        close(fp->kq);
        free(fp);
        return NULL;
    }
    fcntl(fp->wake_pipe[0], F_SETFL, O_NONBLOCK);
    {
        struct kevent change;
        EV_SET(&change, (uintptr_t)fp->wake_pipe[0], EVFILT_READ, EV_ADD, 0, 0,
               NULL);
        kevent(fp->kq, &change, 1, NULL, 0, NULL);
    }
#else
    fp->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fp->inotify_fd < 0) {
        free(fp);
        return NULL;
    }
    fp->inotify_wd = -1;
    if (0 != pipe(fp->wake_pipe)) {
        close(fp->inotify_fd);
        free(fp);
        return NULL;
    }
    fcntl(fp->wake_pipe[0], F_SETFL, O_NONBLOCK);
#endif

    pthread_attr_t attr;
    pthread_attr_init(&attr);
#if defined(__APPLE__)
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_UTILITY, 0);
#endif
    int rc = pthread_create(&fp->thread, &attr, fsprobe_thread, fp);
    pthread_attr_destroy(&attr);

    if (0 != rc) {
        close(fp->wake_pipe[0]);
        close(fp->wake_pipe[1]);
#if defined(__APPLE__)
        if (fp->dir_fd >= 0)
            close(fp->dir_fd);
        close(fp->kq);
#endif
        free(fp);
        return NULL;
    }
    return fp;
}

void fsprobe_request_env_probe(FsProbe *fp) {
    if (NULL == fp)
        return;
    fp->probe_requested = 1;
    char b = 0;
    (void)write(fp->wake_pipe[1], &b, 1);
}

void fsprobe_destroy(FsProbe *fp) {
    if (NULL == fp)
        return;
    fp->running = 0;
    /* Wake the blocked kevent()/poll() so the thread exits promptly. */
    char b = 1;
    (void)write(fp->wake_pipe[1], &b, 1);
    pthread_join(fp->thread, NULL);
    close(fp->wake_pipe[0]);
    close(fp->wake_pipe[1]);
#if defined(__APPLE__)
    if (fp->dir_fd >= 0)
        close(fp->dir_fd);
    close(fp->kq);
#else
    close(fp->inotify_fd);
#endif
    free(fp);
}
