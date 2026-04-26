#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../src/ml/context.h"


static char g_tmpdir[PATH_MAX];

static void make_tmpdir(void)
{
    snprintf(g_tmpdir, sizeof g_tmpdir, "/tmp/phantom_fsc_XXXXXX");
    assert(NULL != mkdtemp(g_tmpdir));
}

static void cleanup_tmpdir(void)
{
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_tmpdir);
    (void)system(cmd);
}

static void touch(const char *dir, const char *name)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    assert(NULL != f);
    fclose(f);
}

static void mkdir_child(const char *dir, const char *name)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    assert(0 == mkdir(path, 0755));
}

static void test_unique_file_match(void)
{
    make_tmpdir();
    touch(g_tmpdir, "foobar.c");

    char out[PATH_MAX];
    int n = fs_complete_path(g_tmpdir, "foo", out, sizeof out);
    assert(n > 0);
    assert(0 == strcmp(out, "bar.c"));

    cleanup_tmpdir();
}

static void test_unique_dir_match_appends_slash(void)
{
    make_tmpdir();
    mkdir_child(g_tmpdir, "phantom");

    char out[PATH_MAX];
    int n = fs_complete_path(g_tmpdir, "phan", out, sizeof out);
    assert(n > 0);
    assert(0 == strcmp(out, "tom/"));

    cleanup_tmpdir();
}

static void test_no_match_returns_neg1(void)
{
    make_tmpdir();
    touch(g_tmpdir, "readme.md");

    char out[PATH_MAX];
    int n = fs_complete_path(g_tmpdir, "xyz", out, sizeof out);
    assert(-1 == n);

    cleanup_tmpdir();
}

static void test_multiple_matches_returns_lcp(void)
{
    make_tmpdir();
    touch(g_tmpdir, "foobar.c");
    touch(g_tmpdir, "foobaz.h");

    /* LCP of "foobar.c" and "foobaz.h" past prefix "foo" is "ba". */
    char out[PATH_MAX];
    int n = fs_complete_path(g_tmpdir, "foo", out, sizeof out);
    assert(n > 0);
    assert(0 == strcmp(out, "ba"));

    cleanup_tmpdir();
}

static void test_multiple_matches_no_lcp_returns_neg1(void)
{
    make_tmpdir();
    touch(g_tmpdir, "foobar.c");
    touch(g_tmpdir, "fooqux.h");

    /* LCP of "foobar" and "fooqux" past prefix "foo" is "" → no advance. */
    char out[PATH_MAX];
    int n = fs_complete_path(g_tmpdir, "foo", out, sizeof out);
    assert(-1 == n);

    cleanup_tmpdir();
}

static void test_exact_name_match_returns_empty_string(void)
{
    make_tmpdir();
    touch(g_tmpdir, "main.c");

    char out[PATH_MAX];
    int n = fs_complete_path(g_tmpdir, "main.c", out, sizeof out);
    assert(0 == n);
    assert('\0' == out[0]);

    cleanup_tmpdir();
}

static void test_subdir_partial(void)
{
    make_tmpdir();
    mkdir_child(g_tmpdir, "src");
    char subdir[PATH_MAX];
    snprintf(subdir, sizeof subdir, "%s/src", g_tmpdir);
    touch(subdir, "autocomplete.c");

    /* partial_arg = "src/auto" → scan g_tmpdir/src for "auto" prefix */
    char out[PATH_MAX];
    int n = fs_complete_path(g_tmpdir, "src/auto", out, sizeof out);
    assert(n > 0);
    assert(0 == strcmp(out, "complete.c"));

    cleanup_tmpdir();
}

static void test_empty_prefix_multiple_entries_returns_neg1(void)
{
    make_tmpdir();
    touch(g_tmpdir, "a.c");
    touch(g_tmpdir, "b.c");

    char out[PATH_MAX];
    int n = fs_complete_path(g_tmpdir, "", out, sizeof out);
    assert(-1 == n);

    cleanup_tmpdir();
}

static void test_empty_prefix_single_entry_returns_full_name(void)
{
    make_tmpdir();
    touch(g_tmpdir, "only.txt");

    char out[PATH_MAX];
    int n = fs_complete_path(g_tmpdir, "", out, sizeof out);
    assert(n > 0);
    assert(0 == strcmp(out, "only.txt"));

    cleanup_tmpdir();
}

static void test_hidden_files_excluded(void)
{
    make_tmpdir();
    touch(g_tmpdir, ".hidden");
    touch(g_tmpdir, "visible.c");

    /* ".h" prefix should not match ".hidden"  -  dot-files are skipped */
    char out[PATH_MAX];
    int n = fs_complete_path(g_tmpdir, ".h", out, sizeof out);
    /* .hidden is skipped, so no match */
    assert(-1 == n);

    cleanup_tmpdir();
}

/* parent dir via ".."  -  scan_path must resolve to the parent of cwd. */
static void test_dotdot_resolution(void)
{
    make_tmpdir();
    touch(g_tmpdir, "foobar.c");
    mkdir_child(g_tmpdir, "sub");

    char subcwd[PATH_MAX];
    snprintf(subcwd, sizeof subcwd, "%s/sub", g_tmpdir);

    char out[PATH_MAX];
    int n = fs_complete_path(subcwd, "../foo", out, sizeof out);
    assert(n > 0);
    assert(0 == strcmp(out, "bar.c"));

    cleanup_tmpdir();
}

/* Absolute partial_arg (tilde already expanded by extract_path_arg). */
static void test_absolute_partial_arg(void)
{
    make_tmpdir();
    touch(g_tmpdir, "barfile.txt");

    /* Build an absolute partial_arg: g_tmpdir + "/bar" */
    char partial[PATH_MAX];
    snprintf(partial, sizeof partial, "%s/bar", g_tmpdir);

    char out[PATH_MAX];
    int n = fs_complete_path("/ignored_cwd", partial, out, sizeof out);
    assert(n > 0);
    assert(0 == strcmp(out, "file.txt"));

    cleanup_tmpdir();
}

/* Absolute partial_arg  -  directory with trailing slash already typed. */
static void test_absolute_partial_dir_slash(void)
{
    make_tmpdir();
    mkdir_child(g_tmpdir, "mydir");
    char subdir[PATH_MAX];
    snprintf(subdir, sizeof subdir, "%s/mydir", g_tmpdir);
    touch(subdir, "notes.txt");

    /* partial = "/tmp/phantom.../mydir/not"  -  scan mydir for "not" */
    char partial[PATH_MAX];
    snprintf(partial, sizeof partial, "%s/mydir/not", g_tmpdir);

    char out[PATH_MAX];
    int n = fs_complete_path("/ignored_cwd", partial, out, sizeof out);
    assert(n > 0);
    assert(0 == strcmp(out, "es.txt"));

    cleanup_tmpdir();
}

int main(void)
{
    test_unique_file_match();
    test_unique_dir_match_appends_slash();
    test_no_match_returns_neg1();
    test_multiple_matches_returns_lcp();
    test_multiple_matches_no_lcp_returns_neg1();
    test_exact_name_match_returns_empty_string();
    test_subdir_partial();
    test_empty_prefix_multiple_entries_returns_neg1();
    test_empty_prefix_single_entry_returns_full_name();
    test_hidden_files_excluded();
    test_dotdot_resolution();
    test_absolute_partial_arg();
    test_absolute_partial_dir_slash();
    printf("test_fs_complete: all passed\n");
    return 0;
}
