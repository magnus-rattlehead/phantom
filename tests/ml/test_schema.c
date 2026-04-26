#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "../../src/ml/schema.h"

#define BUF 256
static char g_buf[BUF];

/* helpers */
static int sc(const char *q) { return schema_complete(q, g_buf, BUF); }

/* ── subcommand completion ────────────────────────────────────────────────── */
static void test_git_partial_subcommand(void)
{
    /* "checkout" and "cherry-pick" both match "ch" → LCP = "che" */
    assert(sc("git ch") > 0);
    assert(0 == strcmp(g_buf, "e"));
}

static void test_git_exact_subcommand_no_suffix(void)
{
    /* "git status" exact match → suffix = "" → returns 0 */
    assert(sc("git status") == 0);
    assert('\0' == g_buf[0]);
}

static void test_git_empty_partial_ambiguous(void)
{
    /* "git "  -  all subcommands match empty prefix → LCP = "" → -1 */
    assert(sc("git ") == -1);
}

static void test_npm_partial(void)
{
    /* "install" and "init" both match "i" → LCP = "in" → advance one char */
    assert(sc("npm i") > 0);
    assert(0 == strcmp(g_buf, "n"));
}

static void test_swift_unique_match(void)
{
    assert(sc("swift b") > 0);
    assert(0 == strcmp(g_buf, "uild"));
}

static void test_brew_ambiguous(void)
{
    /* "brew up" → "update" and "upgrade" both match → LCP past "up" = "" → -1 */
    assert(sc("brew up") == -1);
}

static void test_unknown_command(void)
{
    assert(sc("zsh -c") == -1);
    assert(sc("unknown cmd") == -1);
}

static void test_no_space_returns_neg1(void)
{
    assert(sc("git") == -1);
    assert(sc("npm") == -1);
}

static void test_null_empty_return_neg1(void)
{
    char tmp[8];
    assert(schema_complete(NULL, tmp, sizeof tmp) == -1);
    assert(sc("") == -1);
}

/* ── flag completion ─────────────────────────────────────────────────────── */
static void test_git_commit_flag_unique(void)
{
    /* "--am" → only "--amend" matches */
    assert(sc("git commit --am") > 0);
    assert(0 == strcmp(g_buf, "end"));
}

static void test_git_commit_flag_lcp(void)
{
    /* "--a" → --all, --amend, --author → LCP of "ll","mend","uthor" = "" → -1 */
    assert(sc("git commit --a") == -1);
}

static void test_git_push_flag_lcp(void)
{
    /* "--for" → --force and --force-with-lease share LCP "ce" */
    int n = sc("git push --for");
    assert(n > 0);
    assert(0 == strcmp(g_buf, "ce"));
}

static void test_git_push_flag_unique(void)
{
    /* "--set" → only --set-upstream */
    assert(sc("git push --set") > 0);
    assert(0 == strcmp(g_buf, "-upstream"));
}

static void test_git_log_flag_unique(void)
{
    assert(sc("git log --oneli") > 0);
    assert(0 == strcmp(g_buf, "ne"));
}

static void test_git_global_flag(void)
{
    /* global flag, no active subcommand */
    assert(sc("git --vers") > 0);
    assert(0 == strcmp(g_buf, "ion"));
}

static void test_git_subcmd_flag_not_leaked(void)
{
    /* --amend is a commit flag; should not appear under git log */
    assert(sc("git log --amend") == -1);
}

/* ── sub-subcommand completion ───────────────────────────────────────────── */
static void test_git_stash_subsubcmd(void)
{
    /* "git stash p" → "push" and "pop" share LCP "p" → -1 */
    assert(sc("git stash p") == -1);
}

static void test_git_stash_subsubcmd_unique(void)
{
    assert(sc("git stash cl") > 0);
    assert(0 == strcmp(g_buf, "ear"));
}

static void test_cargo_build_flag(void)
{
    assert(sc("cargo build --rel") > 0);
    assert(0 == strcmp(g_buf, "ease"));
}

static void test_kubectl_logs_flag(void)
{
    assert(sc("kubectl logs --fo") > 0);
    assert(0 == strcmp(g_buf, "llow"));
}

static void test_docker_run_flag(void)
{
    assert(sc("docker run --int") > 0);
    assert(0 == strcmp(g_buf, "eractive"));
}

int main(void)
{
    test_git_partial_subcommand();
    test_git_exact_subcommand_no_suffix();
    test_git_empty_partial_ambiguous();
    test_npm_partial();
    test_swift_unique_match();
    test_brew_ambiguous();
    test_unknown_command();
    test_no_space_returns_neg1();
    test_null_empty_return_neg1();
    test_git_commit_flag_unique();
    test_git_commit_flag_lcp();
    test_git_push_flag_lcp();
    test_git_push_flag_unique();
    test_git_log_flag_unique();
    test_git_global_flag();
    test_git_subcmd_flag_not_leaked();
    test_git_stash_subsubcmd();
    test_git_stash_subsubcmd_unique();
    test_cargo_build_flag();
    test_kubectl_logs_flag();
    test_docker_run_flag();
    printf("test_schema: all passed\n");
    return 0;
}
