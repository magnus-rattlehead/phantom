#pragma once
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

typedef struct Terminal Terminal;

typedef struct Pty {
    int master_fd;
    pid_t child_pid;
    pthread_t reader_thread; /* drains master_fd -> terminal_feed() */
    Terminal *terminal;
    _Atomic int stop_flag; /* wakeup_pipe write provides the happens-before;
                              flag is a backup check */
    int wakeup_pipe[2];    /* [0]=read end, [1]=write end */
    int last_child_exit;   /* WEXITSTATUS; -1 until child exits */
    /* Both callbacks are invoked from the reader thread. */
    void (*on_data)(void *arg);
    void (*on_exit)(void *arg); /* EOF on master fd */
    void *callback_arg;
} Pty;

/* Returns 0 on success; fills pty->master_fd and pty->child_pid. */
int pty_open(Pty *pty, int cols, int rows);

void pty_start_reader(Pty *pty, Terminal *term);

/* TIOCSWINSZ on master; kernel delivers SIGWINCH to the child. */
void pty_resize(const Pty *pty, int cols, int rows);

void pty_write(const Pty *pty, const char *buf, size_t len);

/* Sets stop_flag, wakes reader via wakeup_pipe, joins thread, closes fds. */
void pty_close(Pty *pty);
