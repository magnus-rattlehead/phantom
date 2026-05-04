#pragma once
#ifndef PHANTOM_CCG_H
#define PHANTOM_CCG_H

#include <stddef.h>
#include <stdint.h>

#define CCG_MAX_EDGES 16
/* Hash table capacity  -  must be a power of 2. */
#define CCG_TABLE_CAP 65536
/* Minimum edge frequency for ccg_lookup() to return a suggestion. */
#define CCG_INSTANT_THRESHOLD 3

typedef struct CcgEdge {
    uint32_t freq;
    char cmd[512];
} CcgEdge;

typedef struct CcgNode {
    uint64_t state_hash;
    CcgEdge edges[CCG_MAX_EDGES];
    int n_edges;
    struct CcgNode *next;
} CcgNode;

typedef struct CCG CCG;

CCG *ccg_create(void);
void ccg_destroy(CCG *g);

/* FNV-1a 64-bit hash over (cwd + '\0' + last_cmd + '\0' + exit_code bytes).
 * Either argument may be NULL; treated as empty string. */
uint64_t ccg_hash_state(const char *cwd, const char *last_cmd, int exit_code);

/* Returns the highest-frequency command for state_hash, or NULL if no
 * edge meets CCG_INSTANT_THRESHOLD.  Sets *out_freq to the edge frequency
 * (0 if the node does not exist).  Thread-safe. */
const char *ccg_lookup(const CCG *g, uint64_t state_hash, uint32_t *out_freq);

/* Writes up to k highest-frequency commands for state_hash into
 * out[0..k).  Returns actual count written (0 if node not found).
 * Returned pointers are interior pointers into the graph; they remain
 * valid until the next ccg_train() or ccg_prune() call.  Thread-safe. */
int ccg_top_k(const CCG *g, uint64_t state_hash, const char **out, int k);

/* Records the transition (state_hash -> cmd) by incrementing the
 * edge frequency.  Creates the node and/or edge if absent.
 * Returns 0 on success, -1 on OOM.  Thread-safe. */
int ccg_train(CCG *g, uint64_t state_hash, const char *cmd);

/* Removes all edges with freq == 1 and frees any nodes that become
 * empty.  Should be called every CCG_PRUNE_INTERVAL commands to
 * bound memory growth.  Thread-safe. */
void ccg_prune(CCG *g);

#endif /* PHANTOM_CCG_H */
