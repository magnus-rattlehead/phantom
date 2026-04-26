#pragma once
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

typedef struct Terminal Terminal;

typedef struct Pty {
    int       master_fd;
    pid_t     child_pid;
    pthread_t reader_thread;
    Terminal *terminal;
    _Atomic int stop_flag;     /* wakeup_pipe write provides the happens-before; flag is a backup check */
    int       wakeup_pipe[2];  /* [0]=read end, [1]=write end */
    int       last_child_exit; /* WEXITSTATUS from waitpid; -1 until child exits */
    /* Optional callbacks, both invoked from the reader thread. */
    void    (*on_data)(void *arg);  /* called after each successful read */
    void    (*on_exit)(void *arg);  /* called when child process exits (EOF on master) */
    void     *callback_arg;
} Pty;

/* Opens a PTY and forks $SHELL at the given size. Returns 0 on success. */
int  pty_open(Pty *pty, int cols, int rows);

/* Starts a background thread that reads PTY output into term. */
void pty_start_reader(Pty *pty, Terminal *term);

/* Updates the PTY window size and delivers SIGWINCH to the child. */
void pty_resize(const Pty *pty, int cols, int rows);

/* Writes bytes to the PTY master fd (keyboard/mouse input to the shell). */
void pty_write(const Pty *pty, const char *buf, size_t len);

/* Signals the child, joins the reader thread, and closes all fds. */
void pty_close(Pty *pty);
