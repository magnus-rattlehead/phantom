#include "htrie.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *cmd;
    uint32_t count;
} HTEntry;

struct HTrie {
    HTEntry *entries;
    size_t n;
    size_t cap;
    int dirty; /* 1 = needs re-sort before next lookup */
    pthread_mutex_t lock;
};

static int ht_cmp_desc(const void *a, const void *b) {
    const HTEntry *ea = a;
    const HTEntry *eb = b;
    if (eb->count > ea->count)
        return 1;
    if (eb->count < ea->count)
        return -1;
    return 0;
}

HTrie *htrie_create(void) {
    HTrie *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->cap = 256;
    t->entries = malloc(t->cap * sizeof *t->entries);
    if (!t->entries) {
        free(t);
        return NULL;
    }
    pthread_mutex_init(&t->lock, NULL);
    return t;
}

void htrie_destroy(HTrie *t) {
    if (!t)
        return;
    pthread_mutex_lock(&t->lock);
    for (size_t i = 0; i < t->n; i++)
        free(t->entries[i].cmd);
    free(t->entries);
    pthread_mutex_unlock(&t->lock);
    pthread_mutex_destroy(&t->lock);
    free(t);
}

void htrie_insert(HTrie *t, const char *cmd) {
    if (!t || !cmd || '\0' == cmd[0])
        return;
    pthread_mutex_lock(&t->lock);

    for (size_t i = 0; i < t->n; i++) {
        if (0 == strcmp(t->entries[i].cmd, cmd)) {
            t->entries[i].count++;
            t->dirty = 1;
            pthread_mutex_unlock(&t->lock);
            return;
        }
    }

    if (t->n >= t->cap) {
        size_t ncap = t->cap * 2;
        HTEntry *ne = realloc(t->entries, ncap * sizeof *t->entries);
        if (!ne) {
            pthread_mutex_unlock(&t->lock);
            return;
        }
        t->entries = ne;
        t->cap = ncap;
    }
    char *copy = strdup(cmd);
    if (!copy) {
        pthread_mutex_unlock(&t->lock);
        return;
    }
    t->entries[t->n].cmd = copy;
    t->entries[t->n].count = 1;
    t->n++;
    t->dirty = 1;

    pthread_mutex_unlock(&t->lock);
}

const char *htrie_best(HTrie *t, const char *prefix, uint32_t min_count) {
    if (!t || !prefix || '\0' == prefix[0])
        return NULL;
    size_t plen = strlen(prefix);

    pthread_mutex_lock(&t->lock);

    if (t->dirty) {
        qsort(t->entries, t->n, sizeof *t->entries, ht_cmp_desc);
        t->dirty = 0;
    }

    const char *result = NULL;
    for (size_t i = 0; i < t->n; i++) {
        /* Sorted descending: below threshold, no further matches exist. */
        if (t->entries[i].count < min_count)
            break;
        if (0 == strncmp(t->entries[i].cmd, prefix, plen) &&
            '\0' != t->entries[i].cmd[plen]) {
            result = t->entries[i].cmd;
            break;
        }
    }

    pthread_mutex_unlock(&t->lock);
    return result;
}

size_t htrie_size(const HTrie *t) { return t ? t->n : 0; }
