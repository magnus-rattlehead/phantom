#include "ccg.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ml_log.h"
#define CCG_LOG(...) ml_log("ccg", __VA_ARGS__)

struct CCG {
    CcgNode *table[CCG_TABLE_CAP];
    size_t n_nodes;
    pthread_mutex_t lock;
};

#define FNV1A_SEED UINT64_C(0xcbf29ce484222325)
#define FNV1A_PRIME UINT64_C(0x100000001b3)

static uint64_t fnv1a_update(uint64_t h, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ p[i]) * FNV1A_PRIME;
    }
    return h;
}

uint64_t ccg_hash_state(const char *cwd, const char *last_cmd, int exit_code) {
    if (NULL == cwd)
        cwd = "";
    if (NULL == last_cmd)
        last_cmd = "";

    uint64_t h = FNV1A_SEED;
    h = fnv1a_update(h, cwd, strlen(cwd) + 1);
    h = fnv1a_update(h, last_cmd, strlen(last_cmd) + 1);
    uint32_t ec = (uint32_t)exit_code;
    h = fnv1a_update(h, &ec, sizeof ec);
    return h;
}

static CcgNode *find_node(const CCG *g, uint64_t state_hash) {
    size_t idx = (size_t)(state_hash & (CCG_TABLE_CAP - 1));
    CcgNode *n = g->table[idx];
    while (NULL != n) {
        if (n->state_hash == state_hash)
            return n;
        n = n->next;
    }
    return NULL;
}

static int find_edge(const CcgNode *n, const char *cmd) {
    for (int i = 0; i < n->n_edges; i++) {
        if (0 == strcmp(n->edges[i].cmd, cmd))
            return i;
    }
    return -1;
}

static int max_edge(const CcgNode *n) {
    if (0 == n->n_edges)
        return -1;
    int best = 0;
    for (int i = 1; i < n->n_edges; i++) {
        if (n->edges[i].freq > n->edges[best].freq)
            best = i;
    }
    return best;
}

CCG *ccg_create(void) {
    CCG *g = calloc(1, sizeof *g);
    if (NULL == g)
        return NULL;
    if (0 != pthread_mutex_init(&g->lock, NULL)) {
        free(g);
        return NULL;
    }
    return g;
}

void ccg_destroy(CCG *g) {
    if (NULL == g)
        return;
    for (size_t i = 0; i < CCG_TABLE_CAP; i++) {
        CcgNode *n = g->table[i];
        while (NULL != n) {
            CcgNode *next = n->next;
            free(n);
            n = next;
        }
    }
    pthread_mutex_destroy(&g->lock);
    free(g);
}

const char *ccg_lookup(const CCG *g, uint64_t state_hash, uint32_t *out_freq) {
    if (NULL != out_freq)
        *out_freq = 0;
    if (NULL == g)
        return NULL;

    pthread_mutex_lock((pthread_mutex_t *)&g->lock);

    const CcgNode *n = find_node(g, state_hash);
    if (NULL == n) {
        pthread_mutex_unlock((pthread_mutex_t *)&g->lock);
        return NULL;
    }

    int best = max_edge(n);
    if (best < 0) {
        pthread_mutex_unlock((pthread_mutex_t *)&g->lock);
        return NULL;
    }

    uint32_t freq = n->edges[best].freq;
    if (NULL != out_freq)
        *out_freq = freq;

    const char *result = NULL;
    if (freq >= (uint32_t)CCG_INSTANT_THRESHOLD) {
        result = n->edges[best].cmd;
        CCG_LOG("lookup hit: \"%s\" freq=%u", result, freq);
    }

    pthread_mutex_unlock((pthread_mutex_t *)&g->lock);
    return result;
}

int ccg_top_k(const CCG *g, uint64_t state_hash, const char **out, int k) {
    if (NULL == g || NULL == out || k <= 0)
        return 0;

    pthread_mutex_lock((pthread_mutex_t *)&g->lock);

    const CcgNode *n = find_node(g, state_hash);
    if (NULL == n) {
        pthread_mutex_unlock((pthread_mutex_t *)&g->lock);
        return 0;
    }

    int sorted[CCG_MAX_EDGES];
    int nsorted = 0;

    for (int i = 0; i < n->n_edges; i++) {
        int pos = nsorted;
        for (int j = 0; j < nsorted; j++) {
            if (n->edges[i].freq > n->edges[sorted[j]].freq) {
                pos = j;
                break;
            }
        }
        if (nsorted < k || pos < k) {
            int bound = (nsorted < k) ? nsorted : k - 1;
            for (int j = bound; j > pos; j--)
                sorted[j] = sorted[j - 1];
            sorted[pos] = i;
            if (nsorted < k)
                nsorted++;
        }
    }

    int result = nsorted < k ? nsorted : k;
    for (int i = 0; i < result; i++) {
        out[i] = n->edges[sorted[i]].cmd;
    }

    pthread_mutex_unlock((pthread_mutex_t *)&g->lock);
    return result;
}

int ccg_train(CCG *g, uint64_t state_hash, const char *cmd) {
    if (NULL == g || NULL == cmd)
        return -1;

    pthread_mutex_lock(&g->lock);

    size_t idx = (size_t)(state_hash & (CCG_TABLE_CAP - 1));
    CcgNode *n = find_node(g, state_hash);

    int new_node = 0;
    if (NULL == n) {
        n = calloc(1, sizeof *n);
        if (NULL == n) {
            pthread_mutex_unlock(&g->lock);
            return -1;
        }
        n->state_hash = state_hash;
        n->next = g->table[idx];
        g->table[idx] = n;
        g->n_nodes++;
        new_node = 1;
    }

    int ei = find_edge(n, cmd);
    if (-1 == ei) {
        if (n->n_edges >= CCG_MAX_EDGES) {
            CCG_LOG("train: edge table full for \"%s\", dropping", cmd);
            pthread_mutex_unlock(&g->lock);
            return 0;
        }
        ei = n->n_edges++;
        snprintf(n->edges[ei].cmd, sizeof n->edges[ei].cmd, "%s", cmd);
        n->edges[ei].freq = 0;
    }

    n->edges[ei].freq++;
    CCG_LOG("train: \"%s\" freq=%u%s", cmd, n->edges[ei].freq,
            new_node ? " (new node)" : "");

    pthread_mutex_unlock(&g->lock);
    return 0;
}

void ccg_prune(CCG *g) {
    if (NULL == g)
        return;

    pthread_mutex_lock(&g->lock);
    size_t nodes_before = g->n_nodes;

    for (size_t i = 0; i < CCG_TABLE_CAP; i++) {
        CcgNode **pp = &g->table[i];
        while (NULL != *pp) {
            CcgNode *n = *pp;

            int wi = 0;
            for (int ei = 0; ei < n->n_edges; ei++) {
                if (n->edges[ei].freq > 1) {
                    if (wi != ei)
                        n->edges[wi] = n->edges[ei];
                    wi++;
                }
            }
            n->n_edges = wi;

            if (0 == n->n_edges) {
                *pp = n->next;
                free(n);
                g->n_nodes--;
            } else {
                pp = &n->next;
            }
        }
    }

    CCG_LOG("prune: %zu -> %zu nodes", nodes_before, g->n_nodes);
    pthread_mutex_unlock(&g->lock);
}
