#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../src/ml/context.h"
#include "../../src/ml/fsprobe.h"

/* Poll ts->fs_buf every 20 ms for up to timeout_ms; returns 1 if needle found. */
static int wait_for_fs(TerminalState *ts, const char *needle, int timeout_ms)
{
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        struct timespec sl = {0, 20 * 1000000L};
        nanosleep(&sl, NULL);
        elapsed += 20;
        terminal_state_lock(ts);
        const char *fs = terminal_state_fs(ts);
        int found = (NULL != fs) && (NULL != strstr(fs, needle));
        terminal_state_unlock(ts);
        if (found) return 1;
    }
    return 0;
}

/* Poll ts->cwd every 20 ms for up to timeout_ms; returns 1 if needle found. */
static int wait_for_cwd(TerminalState *ts, const char *needle, int timeout_ms)
{
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        struct timespec sl = {0, 20 * 1000000L};
        nanosleep(&sl, NULL);
        elapsed += 20;
        terminal_state_lock(ts);
        const char *cwd = terminal_state_cwd(ts);
        int found = (NULL != cwd) && (NULL != strstr(cwd, needle));
        terminal_state_unlock(ts);
        if (found) return 1;
    }
    return 0;
}

static void test_detects_new_file(void)
{
    char tmpdir[] = "/tmp/phantom_fsprobe_XXXXXX";
    assert(NULL != mkdtemp(tmpdir));

    char orig[PATH_MAX];
    assert(NULL != getcwd(orig, sizeof orig));
    assert(0 == chdir(tmpdir));

    TerminalState *ts = terminal_state_create(NULL);
    assert(NULL != ts);
    FsProbe *fp = fsprobe_create(ts, NULL);
    assert(NULL != fp);

    /* Give the probe thread time to do its initial scan. */
    struct timespec sl = {0, 100 * 1000000L};
    nanosleep(&sl, NULL);

    /* Create a file and wait for NOTE_WRITE to trigger a rescan. */
    char fpath[PATH_MAX];
    snprintf(fpath, sizeof fpath, "%s/probe_sentinel.c", tmpdir);
    int fd = open(fpath, O_WRONLY | O_CREAT, 0644);
    assert(fd >= 0);
    close(fd);

    assert(wait_for_fs(ts, "probe_sentinel.c", 500)
           && "new file not detected within 500 ms");

    fsprobe_destroy(fp);
    terminal_state_destroy(ts);

    unlink(fpath);
    assert(0 == chdir(orig));
    rmdir(tmpdir);
    printf("PASS test_detects_new_file\n");
}

static void test_detects_cwd_change(void)
{
    char tmpdir[] = "/tmp/phantom_fsprobe2_XXXXXX";
    assert(NULL != mkdtemp(tmpdir));

    char orig[PATH_MAX];
    assert(NULL != getcwd(orig, sizeof orig));

    TerminalState *ts = terminal_state_create(NULL);
    assert(NULL != ts);
    FsProbe *fp = fsprobe_create(ts, NULL);
    assert(NULL != fp);

    /* Change into the new directory and wait for the probe to pick it up.
     * The 2-second polling fallback means we need to wait up to ~3 s. */
    assert(0 == chdir(tmpdir));
    assert(wait_for_cwd(ts, "phantom_fsprobe2", 3500)
           && "CWD change not detected within 3.5 s");

    fsprobe_destroy(fp);
    terminal_state_destroy(ts);

    assert(0 == chdir(orig));
    rmdir(tmpdir);
    printf("PASS test_detects_cwd_change\n");
}

int main(void)
{
    test_detects_new_file();
    test_detects_cwd_change();
    printf("test_fsprobe: all passed\n");
    return 0;
}
