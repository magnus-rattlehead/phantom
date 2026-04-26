#include "pty.h"
#include "terminal.h"
#include "config.h"

#define PTY_READ_BUF_LEN 4096  /* bytes per read() call from the master fd */

#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static void *pty_reader_thread(void *arg);

/* SIGTERM then SIGKILL after 200 ms if the process hasn't exited. */
static void reap_child(pid_t pid)
{
    kill(pid, SIGTERM);
    struct timespec ts = {0, 20 * 1000000L};
    for (int i = 0; i < 10; i++) {
        if (waitpid(pid, NULL, WNOHANG) != 0) return;
        nanosleep(&ts, NULL);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

int pty_open(Pty *pty, int cols, int rows)
{
    int master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (-1 == master_fd) { perror("posix_openpt"); return -1; }
    if (-1 == grantpt(master_fd))  { perror("grantpt");  close(master_fd); return -1; }
    if (-1 == unlockpt(master_fd)) { perror("unlockpt"); close(master_fd); return -1; }

    const char *slave_name = ptsname(master_fd);
    if (!slave_name) { perror("ptsname"); close(master_fd); return -1; }

    int slave_fd = open(slave_name, O_RDWR | O_NOCTTY);
    if (-1 == slave_fd) { perror("open slave"); close(master_fd); return -1; }

    struct winsize ws = { .ws_col = (unsigned short)cols,
                          .ws_row = (unsigned short)rows };
    (void)ioctl(master_fd, TIOCSWINSZ, &ws);

    if (-1 == pipe(pty->wakeup_pipe)) {
        perror("pipe"); close(slave_fd); close(master_fd); return -1;
    }

    pid_t pid = fork();
    if (-1 == pid) {
        perror("fork");
        close(pty->wakeup_pipe[0]); close(pty->wakeup_pipe[1]);
        close(slave_fd); close(master_fd);
        return -1;
    }

    if (0 == pid) {
        /* Child: become session leader, attach PTY as controlling terminal. */
        setsid();
        if (-1 == ioctl(slave_fd, TIOCSCTTY, 0)) _exit(1);
        if (-1 == dup2(slave_fd, STDIN_FILENO))  _exit(1);
        if (-1 == dup2(slave_fd, STDOUT_FILENO)) _exit(1);
        if (-1 == dup2(slave_fd, STDERR_FILENO)) _exit(1);
        close(slave_fd);
        close(master_fd);
        close(pty->wakeup_pipe[0]);
        close(pty->wakeup_pipe[1]);

        setenv("TERM",         PTY_TERM,      1);
        setenv("COLORTERM",    PTY_COLORTERM, 1);
        setenv("TERM_PROGRAM", "phantom",        1);
        unsetenv("COLUMNS");
        unsetenv("LINES");
        unsetenv("TERMCAP");

        const char *shell = getenv("SHELL");
        if (!shell) shell = "/bin/sh";
        execl(shell, shell, (char *)NULL);
        _exit(1);
    }

    close(slave_fd);
    pty->master_fd      = master_fd;
    pty->child_pid      = pid;
    pty->terminal       = NULL;
    pty->stop_flag      = 0;
    pty->reader_thread  = 0;
    pty->last_child_exit = -1;
    return 0;
}

void pty_start_reader(Pty *pty, Terminal *term)
{
    pty->terminal = term;
#if defined(__APPLE__)
    {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
        pthread_create(&pty->reader_thread, &attr, pty_reader_thread, pty);
        pthread_attr_destroy(&attr);
    }
#else
    pthread_create(&pty->reader_thread, NULL, pty_reader_thread, pty);
#endif
}

static void *pty_reader_thread(void *arg)
{
    Pty *pty = (Pty *)arg;
    char buf[PTY_READ_BUF_LEN];
    int first_read = 1;

    while (!pty->stop_flag) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pty->master_fd, &rfds);
        FD_SET(pty->wakeup_pipe[0], &rfds);
        int nfds = (pty->master_fd > pty->wakeup_pipe[0]
                    ? pty->master_fd : pty->wakeup_pipe[0]) + 1;

        if (select(nfds, &rfds, NULL, NULL, NULL) < 0) break;
        if (FD_ISSET(pty->wakeup_pipe[0], &rfds)) break;

        if (FD_ISSET(pty->master_fd, &rfds)) {
            ssize_t n = read(pty->master_fd, buf, sizeof buf);
            if (n <= 0) {
                int status = 0;
                if (waitpid(pty->child_pid, &status, 0) > 0 &&
                    WIFEXITED(status)) {
                    pty->last_child_exit = WEXITSTATUS(status);
                }
                if (pty->on_exit) pty->on_exit(pty->callback_arg);
                break;
            }
            if (first_read) {
                first_read = 0;
                /* CR + erase to end of line clears the loading message. */
                terminal_feed(pty->terminal, "\r\x1b[K", 4);
            }
            terminal_feed(pty->terminal, buf, (size_t)n);
            if (pty->on_data) pty->on_data(pty->callback_arg);
        }
    }
    return NULL;
}

void pty_resize(const Pty *pty, int cols, int rows)
{
    struct winsize ws = { .ws_col = (unsigned short)cols,
                          .ws_row = (unsigned short)rows };
    (void)ioctl(pty->master_fd, TIOCSWINSZ, &ws);
}

void pty_write(const Pty *pty, const char *buf, size_t len)
{
    (void)write(pty->master_fd, buf, len);
}

void pty_close(Pty *pty)
{
    pty->stop_flag = 1;
    (void)write(pty->wakeup_pipe[1], "\0", 1);

    if (pty->reader_thread) {
        pthread_join(pty->reader_thread, NULL);
        pty->reader_thread = 0;
    }

    if (pty->master_fd >= 0) close(pty->master_fd);
    pty->master_fd = -1;
    close(pty->wakeup_pipe[0]);
    close(pty->wakeup_pipe[1]);

    if (pty->child_pid > 0) {
        reap_child(pty->child_pid);
        pty->child_pid = -1;
    }
}
