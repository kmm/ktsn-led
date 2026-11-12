#include "arena.h"
#include <string.h>

void arena_init(arena_t *a, void *mem, size_t size)
{
    uintptr_t p = (uintptr_t)mem, al = (p + 7) & ~(uintptr_t)7;
    size_t adj = (size_t)(al - p);
    a->base = (uint8_t *)al;
    a->mem = mem;
    a->size = size > adj ? size - adj : 0;
    a->used = a->high = 0;
}

void *arena_alloc(arena_t *a, size_t n)
{
    size_t u = (a->used + 7) & ~(size_t)7;
    if (n > a->size || u > a->size - n) return NULL;
    void *p = a->base + u;
    a->used = u + n;
    if (a->used > a->high) a->high = a->used;
    return p;
}

char *arena_strdup(arena_t *a, const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = arena_alloc(a, n);
    if (d) memcpy(d, s, n);
    return d;
}
