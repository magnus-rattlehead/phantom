#ifndef PHANTOM_UTIL_H
#define PHANTOM_UTIL_H

/* Branchless three-way comparator for use with qsort/bsearch on int arrays. */
static inline int int_cmp(const void *a, const void *b) {
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    return (ia > ib) - (ia < ib);
}

#endif /* PHANTOM_UTIL_H */
