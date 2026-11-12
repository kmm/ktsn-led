#pragma once
#include <stddef.h>
#include <stdint.h>

/* Bump allocator. Reset is O(1). `high` is the high-water mark across resets. */
typedef struct {
    uint8_t *base;               /* 8-aligned start: may be past `mem` */
    void    *mem;                /* the block as given to arena_init: free() this, never `base` */
    size_t size, used, high;
} arena_t;

void  arena_init(arena_t *a, void *mem, size_t size);
void *arena_alloc(arena_t *a, size_t n);          /* 8-byte aligned, NULL when full */
char *arena_strdup(arena_t *a, const char *s);
static inline void arena_reset(arena_t *a) { a->used = 0; }
