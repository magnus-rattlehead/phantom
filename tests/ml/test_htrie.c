#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/ml/htrie.h"

static void test_empty_returns_null(void)
{
    HTrie *t = htrie_create();
    assert(NULL == htrie_best(t, "git", 1));
    htrie_destroy(t);
}

static void test_single_insert_below_threshold(void)
{
    HTrie *t = htrie_create();
    htrie_insert(t, "git status");
    /* count = 1, min_count = 2 → no result */
    assert(NULL == htrie_best(t, "git", 2));
    htrie_destroy(t);
}

static void test_reaches_threshold(void)
{
    HTrie *t = htrie_create();
    htrie_insert(t, "git status");
    htrie_insert(t, "git status");
    const char *r = htrie_best(t, "git", 2);
    assert(NULL != r);
    assert(0 == strcmp(r, "git status"));
    htrie_destroy(t);
}

static void test_prefix_must_match(void)
{
    HTrie *t = htrie_create();
    htrie_insert(t, "npm install");
    htrie_insert(t, "npm install");
    /* "git" prefix should not match "npm install" */
    assert(NULL == htrie_best(t, "git", 2));
    htrie_destroy(t);
}

static void test_exact_match_not_returned(void)
{
    HTrie *t = htrie_create();
    htrie_insert(t, "git");
    htrie_insert(t, "git");
    /* prefix == cmd → no suffix to offer */
    assert(NULL == htrie_best(t, "git", 2));
    htrie_destroy(t);
}

static void test_higher_freq_wins(void)
{
    HTrie *t = htrie_create();
    htrie_insert(t, "git checkout");
    htrie_insert(t, "git checkout");
    htrie_insert(t, "git commit");
    htrie_insert(t, "git commit");
    htrie_insert(t, "git commit");
    /* "git commit" has freq=3, "git checkout" has freq=2 */
    const char *r = htrie_best(t, "git c", 2);
    assert(NULL != r);
    assert(0 == strcmp(r, "git commit"));
    htrie_destroy(t);
}

static void test_size(void)
{
    HTrie *t = htrie_create();
    assert(0 == htrie_size(t));
    htrie_insert(t, "ls -la");
    assert(1 == htrie_size(t));
    htrie_insert(t, "ls -la");   /* duplicate: no new entry */
    assert(1 == htrie_size(t));
    htrie_insert(t, "cd /tmp");
    assert(2 == htrie_size(t));
    htrie_destroy(t);
}

int main(void)
{
    test_empty_returns_null();
    test_single_insert_below_threshold();
    test_reaches_threshold();
    test_prefix_must_match();
    test_exact_match_not_returned();
    test_higher_freq_wins();
    test_size();
    printf("test_htrie: all passed\n");
    return 0;
}
