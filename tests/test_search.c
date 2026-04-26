#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/search.h"
#include "../src/terminal.h"

static void feed_lines(Terminal *t, const char *text, int count)
{
    size_t len = strlen(text);
    for (int i = 0; i < count; i++)
        terminal_feed(t, text, len);
}

static int wait_for_results(SearchState *s, int want)
{
    for (int i = 0; i < 200; i++) {
        if (search_result_count(s) >= want) return 1;
        struct timespec ts = {0, 5000000};
        nanosleep(&ts, NULL);
    }
    return 0;
}

static SDL_TextInputEvent make_text_event(char *buf)
{
    SDL_TextInputEvent e;
    memset(&e, 0, sizeof e);
    e.text = buf;
    return e;
}

static SDL_KeyboardEvent make_key_event(SDL_Keycode key, SDL_Keymod mod)
{
    SDL_KeyboardEvent e;
    memset(&e, 0, sizeof e);
    e.key = key;
    e.mod = mod;
    return e;
}


static void test_search_lifecycle(void)
{
    Terminal    *term = terminal_create(80, 24);
    SearchState *s    = search_create();

    assert(NULL != s);
    assert(!search_is_active(s));
    assert(0 == search_query_len(s));
    assert('\0' == search_query(s)[0]);

    search_open(s, term);
    assert(search_is_active(s));
    assert(0 == search_query_len(s));
    assert(0 == search_result_count(s));

    search_close(s, term);
    assert(!search_is_active(s));

    search_destroy(s);
    terminal_destroy(term);
    printf("test_search_lifecycle: PASS\n");
}

static void test_search_query_append(void)
{
    Terminal    *term = terminal_create(80, 24);
    SearchState *s    = search_create();
    search_open(s, term);

    char buf1[] = "hel";
    SDL_TextInputEvent te = make_text_event(buf1);
    search_handle_text(s, term, &te);
    assert(3 == search_query_len(s));
    assert(0 == strcmp(search_query(s), "hel"));

    char buf2[] = "lo";
    te = make_text_event(buf2);
    search_handle_text(s, term, &te);
    assert(5 == search_query_len(s));
    assert(0 == strcmp(search_query(s), "hello"));

    search_destroy(s);
    terminal_destroy(term);
    printf("test_search_query_append: PASS\n");
}

static void test_search_backspace_ascii(void)
{
    Terminal    *term = terminal_create(80, 24);
    SearchState *s    = search_create();
    search_open(s, term);

    char buf[] = "hello";
    SDL_TextInputEvent te = make_text_event(buf);
    search_handle_text(s, term, &te);

    SDL_KeyboardEvent ke = make_key_event(SDLK_BACKSPACE, 0);
    int consumed = search_handle_key(s, term, &ke);

    assert(1 == consumed);
    assert(4 == search_query_len(s));
    assert(0 == strcmp(search_query(s), "hell"));

    search_handle_key(s, term, &ke);
    search_handle_key(s, term, &ke);
    search_handle_key(s, term, &ke);
    assert(1 == search_query_len(s));
    assert(0 == strcmp(search_query(s), "h"));

    search_handle_key(s, term, &ke);
    assert(0 == search_query_len(s));
    assert('\0' == search_query(s)[0]);

    search_handle_key(s, term, &ke);
    assert(0 == search_query_len(s));

    search_destroy(s);
    terminal_destroy(term);
    printf("test_search_backspace_ascii: PASS\n");
}

static void test_search_backspace_utf8(void)
{
    Terminal    *term = terminal_create(80, 24);
    SearchState *s    = search_create();
    search_open(s, term);

    char buf[] = "caf\xC3\xA9";
    SDL_TextInputEvent te = make_text_event(buf);
    search_handle_text(s, term, &te);
    assert(5 == search_query_len(s));

    SDL_KeyboardEvent ke = make_key_event(SDLK_BACKSPACE, 0);
    search_handle_key(s, term, &ke);
    assert(3 == search_query_len(s));
    assert(0 == strcmp(search_query(s), "caf"));

    search_handle_key(s, term, &ke);
    assert(2 == search_query_len(s));
    assert(0 == strcmp(search_query(s), "ca"));

    search_destroy(s);
    terminal_destroy(term);
    printf("test_search_backspace_utf8: PASS\n");
}

static void test_search_escape_closes(void)
{
    Terminal    *term = terminal_create(80, 24);
    SearchState *s    = search_create();
    search_open(s, term);
    assert(search_is_active(s));

    SDL_KeyboardEvent ke = make_key_event(SDLK_ESCAPE, 0);
    int consumed = search_handle_key(s, term, &ke);

    assert(1 == consumed);
    assert(!search_is_active(s));

    search_destroy(s);
    terminal_destroy(term);
    printf("test_search_escape_closes: PASS\n");
}

static void test_search_enter_noop_no_results(void)
{
    Terminal    *term = terminal_create(80, 24);
    SearchState *s    = search_create();
    search_open(s, term);

    SDL_KeyboardEvent ke = make_key_event(SDLK_RETURN, 0);
    int consumed = search_handle_key(s, term, &ke);

    assert(1 == consumed);
    assert(0 == search_current_idx(s));

    search_destroy(s);
    terminal_destroy(term);
    printf("test_search_enter_noop_no_results: PASS\n");
}

static void test_search_query_grows(void)
{
    Terminal    *term = terminal_create(80, 24);
    SearchState *s    = search_create();
    search_open(s, term);

    char chunk[31];
    memset(chunk, 'a', 30);
    chunk[30] = '\0';
    SDL_TextInputEvent te = make_text_event(chunk);
    for (int i = 0; i < 10; i++)
        search_handle_text(s, term, &te);

    assert(300 == search_query_len(s));
    const char *q = search_query(s);
    for (int i = 0; i < 300; i++)
        assert('a' == q[i]);
    assert('\0' == q[300]);

    search_destroy(s);
    terminal_destroy(term);
    printf("test_search_query_grows: PASS\n");
}

static void test_search_async_finds_result(void)
{
    Terminal *term = terminal_create(80, 10);

    feed_lines(term, "filler\r\n", 100);
    terminal_feed(term, "needle\r\n", 8);
    feed_lines(term, "extra\r\n", 15);

    SearchState *s = search_create();
    search_open(s, term);

    char qbuf[] = "needle";
    SDL_TextInputEvent te = make_text_event(qbuf);
    search_handle_text(s, term, &te);

    assert(wait_for_results(s, 1) && "search should find at least one result");
    assert(search_result_count(s) >= 1);

    search_destroy(s);
    terminal_destroy(term);
    printf("test_search_async_finds_result: PASS\n");
}

static void test_search_navigate(void)
{
    Terminal *term = terminal_create(80, 10);

    feed_lines(term, "filler\r\n", 100);
    terminal_feed(term, "needle\r\n", 8);
    feed_lines(term, "filler\r\n", 5);
    terminal_feed(term, "needle\r\n", 8);
    feed_lines(term, "extra\r\n", 15);

    SearchState *s = search_create();
    search_open(s, term);

    char qbuf[] = "needle";
    SDL_TextInputEvent te = make_text_event(qbuf);
    search_handle_text(s, term, &te);

    assert(wait_for_results(s, 2) && "search should find both needles");

    assert(0 == search_current_idx(s));

    SDL_KeyboardEvent ke = make_key_event(SDLK_RETURN, 0);
    search_handle_key(s, term, &ke);
    assert(1 == search_current_idx(s));

    ke.mod = SDL_KMOD_LSHIFT;
    search_handle_key(s, term, &ke);
    assert(0 == search_current_idx(s));

    search_destroy(s);
    terminal_destroy(term);
    printf("test_search_navigate: PASS\n");
}

static void test_search_open_clears_previous(void)
{
    Terminal    *term = terminal_create(80, 24);
    SearchState *s    = search_create();

    search_open(s, term);
    char buf[] = "something";
    SDL_TextInputEvent te = make_text_event(buf);
    search_handle_text(s, term, &te);
    assert(search_query_len(s) > 0);

    search_open(s, term);
    assert(0 == search_query_len(s));
    assert('\0' == search_query(s)[0]);

    search_destroy(s);
    terminal_destroy(term);
    printf("test_search_open_clears_previous: PASS\n");
}

int main(void)
{
    test_search_lifecycle();
    test_search_query_append();
    test_search_backspace_ascii();
    test_search_backspace_utf8();
    test_search_escape_closes();
    test_search_enter_noop_no_results();
    test_search_query_grows();
    test_search_async_finds_result();
    test_search_navigate();
    test_search_open_clears_previous();

    printf("test_search: all passed\n");
    return 0;
}
