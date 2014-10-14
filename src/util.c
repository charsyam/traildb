
#include <sys/types.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include "util.h"

static int compare(const void *p1, const void *p2)
{
    const struct sortpair *x = (const struct sortpair*)p1;
    const struct sortpair *y = (const struct sortpair*)p2;

    if (x->value > y->value)
        return -1;
    else if (x->value < y->value)
        return 1;
    return 0;
}

struct sortpair *sort_judyl(const Pvoid_t judy, Word_t *num_items)
{
    uint32_t i;
    struct sortpair *pairs;
    Word_t key;
    Word_t *val;

    JLC(*num_items, judy, 0, -1);

    if (!(pairs = calloc(*num_items, sizeof(struct sortpair))))
        DIE("Couldn't allocate sortpairs (%llu pairs)\n",
            (unsigned long long)num_items);

    key = i = 0;
    JLF(val, judy, key);
    while (val){
        pairs[i].key = key;
        pairs[i++].value = *val;
        JLN(val, judy, key);
    }
    qsort(pairs, *num_items, sizeof(struct sortpair), compare);
    return pairs;
}

uint32_t bits_needed(uint32_t max)
{
    uint32_t x = max;
    uint32_t bits = x ? 0: 1;
    while (x){
        x >>= 1;
        ++bits;
    }
    return bits;
}

uint64_t parse_uint64(const char *str, const char *ctx)
{
    char *p;
    uint64_t n;

    errno = 0;
    n = strtoull(str, &p, 10);

    if (*p || errno)
        DIE("Invalid unsigned integer '%s' (%s)\n", str, ctx);

    return n;
}

void dsfmt_shuffle(uint64_t *arr, uint64_t len, uint32_t seed)
{
    uint64_t i;
    dsfmt_t state;

    if (len > 1){
        dsfmt_init_gen_rand(&state, seed);

        for (i = 0; i < len - 1; i++){
            uint32_t j = i + (len - i) * dsfmt_genrand_close_open(&state);
            uint64_t t = arr[j];
            arr[j] = arr[i];
            arr[i] = t;
        }
    }
}