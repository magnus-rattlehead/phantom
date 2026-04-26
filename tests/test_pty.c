#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../src/pty.h"

static void test_open_close(void)
{
    Pty pty = {0};
    int rc = pty_open(&pty, 80, 24);
    assert(0 == rc);
    assert(pty.master_fd >= 0);
    assert(pty.child_pid > 0);
    pty_close(&pty);
    assert(-1 == pty.master_fd);
    assert(-1 == pty.child_pid);
}

static void test_write_does_not_crash(void)
{
    Pty pty = {0};
    pty_open(&pty, 80, 24);
    pty_write(&pty, "echo hi\n", 8);
    pty_close(&pty);
}

static void test_resize(void)
{
    Pty pty = {0};
    pty_open(&pty, 80, 24);
    pty_resize(&pty, 132, 50);
    pty_close(&pty);
}

int main(void)
{
    test_open_close();
    test_write_does_not_crash();
    test_resize();
    printf("test_pty: all passed\n");
    return 0;
}
