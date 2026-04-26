#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

#include "../../src/ml/context.h"

static void test_probe_cwd(void)
{
    TerminalState *ts = terminal_state_create(NULL);
    assert(NULL != ts);

    terminal_state_probe(ts, NULL);

    char expected[PATH_MAX];
    assert(NULL != getcwd(expected, sizeof expected));
    assert(0 == strcmp(terminal_state_cwd(ts), expected));

    terminal_state_destroy(ts);
}

static void test_git_no_repo(void)
{
    char orig[PATH_MAX];
    assert(NULL != getcwd(orig, sizeof orig));

    assert(0 == chdir("/tmp"));

    TerminalState *ts = terminal_state_create(NULL);
    assert(NULL != ts);
    terminal_state_probe(ts, NULL);

    assert(0 == strlen(terminal_state_git_branch(ts)));

    terminal_state_destroy(ts);
    assert(0 == chdir(orig));
}

static void test_recent_bytes_null_term(void)
{
    TerminalState *ts = terminal_state_create(NULL);
    assert(NULL != ts);
    terminal_state_probe(ts, NULL);

    char buf[64];
    size_t n = terminal_state_recent_bytes(ts, buf, sizeof buf);
    assert(0 == n);

    terminal_state_destroy(ts);
}

static void test_cwd_non_null_after_create(void)
{
    TerminalState *ts = terminal_state_create(NULL);
    assert(NULL != ts);
    assert(NULL != terminal_state_cwd(ts));
    assert(NULL != terminal_state_git_branch(ts));
    terminal_state_destroy(ts);
}

static void test_make_targets_parsing(void)
{
    char tmpdir[PATH_MAX];
    snprintf(tmpdir, sizeof tmpdir, "/tmp/phantom_ctx_XXXXXX");
    assert(NULL != mkdtemp(tmpdir));

    char mkpath[PATH_MAX];
    snprintf(mkpath, sizeof mkpath, "%s/Makefile", tmpdir);
    FILE *f = fopen(mkpath, "w");
    assert(NULL != f);
    fprintf(f, "build:\n\tgcc main.c -o phantom\n");
    fprintf(f, "clean:\n\trm -f phantom *.o\n");
    fprintf(f, "test:\n\tctest --test-dir build\n");
    fprintf(f, ".PHONY: build clean test\n");
    fclose(f);

    TerminalState *ts = terminal_state_create(NULL);
    assert(NULL != ts);
    terminal_state_update_cwd(ts, tmpdir);
    terminal_state_probe_fs(ts);

    terminal_state_lock(ts);
    const char *targets = terminal_state_make_targets(ts);
    assert(NULL != targets && '\0' != targets[0]);
    assert(NULL != strstr(targets, "build"));
    assert(NULL != strstr(targets, "clean"));
    assert(NULL != strstr(targets, "test"));
    /* .PHONY declaration must not be included */
    assert(NULL == strstr(targets, ".PHONY"));
    terminal_state_unlock(ts);

    terminal_state_destroy(ts);
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof cmd, "rm -rf %s", tmpdir);
    (void)system(cmd);
}

static void test_make_targets_absent(void)
{
    char tmpdir[PATH_MAX];
    snprintf(tmpdir, sizeof tmpdir, "/tmp/phantom_ctx_XXXXXX");
    assert(NULL != mkdtemp(tmpdir));

    TerminalState *ts = terminal_state_create(NULL);
    assert(NULL != ts);
    terminal_state_update_cwd(ts, tmpdir);
    terminal_state_probe_fs(ts);

    terminal_state_lock(ts);
    /* No Makefile → accessor returns "" not NULL. */
    assert(NULL != terminal_state_make_targets(ts));
    terminal_state_unlock(ts);

    terminal_state_destroy(ts);
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof cmd, "rm -rf %s", tmpdir);
    (void)system(cmd);
}

static void test_pkg_scripts_parsing(void)
{
    char tmpdir[PATH_MAX];
    snprintf(tmpdir, sizeof tmpdir, "/tmp/phantom_ctx_XXXXXX");
    assert(NULL != mkdtemp(tmpdir));

    char pkgpath[PATH_MAX];
    snprintf(pkgpath, sizeof pkgpath, "%s/package.json", tmpdir);
    FILE *f = fopen(pkgpath, "w");
    assert(NULL != f);
    fprintf(f, "{\n");
    fprintf(f, "  \"name\": \"myapp\",\n");
    fprintf(f, "  \"scripts\": {\n");
    fprintf(f, "    \"start\": \"node server.js\",\n");
    fprintf(f, "    \"test\": \"jest\",\n");
    fprintf(f, "    \"build\": \"webpack\"\n");
    fprintf(f, "  }\n");
    fprintf(f, "}\n");
    fclose(f);

    TerminalState *ts = terminal_state_create(NULL);
    assert(NULL != ts);
    terminal_state_update_cwd(ts, tmpdir);
    terminal_state_probe_fs(ts);

    terminal_state_lock(ts);
    const char *scripts = terminal_state_pkg_scripts(ts);
    assert(NULL != scripts && '\0' != scripts[0]);
    assert(NULL != strstr(scripts, "start"));
    assert(NULL != strstr(scripts, "test"));
    assert(NULL != strstr(scripts, "build"));
    terminal_state_unlock(ts);

    terminal_state_destroy(ts);
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof cmd, "rm -rf %s", tmpdir);
    (void)system(cmd);
}

int main(void)
{
    test_probe_cwd();
    test_git_no_repo();
    test_recent_bytes_null_term();
    test_cwd_non_null_after_create();
    test_make_targets_parsing();
    test_make_targets_absent();
    test_pkg_scripts_parsing();
    printf("test_context: all passed\n");
    return 0;
}
