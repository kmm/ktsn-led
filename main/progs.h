#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Program text storage: the `progs` NVS partition (partitions_2mb.csv / partitions_4mb.csv, 256 KB), separate from
 * the 24 KB settings NVS, which would run out with a few long programs (a 3999-char program takes a whole 4 KB page).
 * Namespaces: PROGS_NS_SYS ("ledctl": prog<N>, auto, ...) and PROGS_NS_LIB ("lib": the program library, by name).
 * A board still on an older partition table has no `progs`: everything then stays in the settings NVS, as before. */
#define PROGS_NS_SYS "ledctl"     /* the project's old name: kept so saved programs and settings survive */
#define PROGS_NS_LIB "lib"

void        progs_init(void);                         /* after settings NVS init; moves programs over once */
bool        progs_separate(void);                     /* true if the progs partition is in use */
char       *progs_get(const char *ns, const char *key);                /* malloc'd, or NULL if not stored */
esp_err_t   progs_set(const char *ns, const char *key, const char *src); /* NULL or "" erases; commits */
/* Calls fn for every key in ns (alphabetical order not guaranteed). */
void        progs_list(const char *ns, void (*fn)(const char *key, size_t len, void *ctx), void *ctx);
void        progs_stats(size_t *used, size_t *total);  /* entries of the partition programs live in */
esp_err_t   progs_erase_ns(const char *ns);

/* Library names: 1..15 chars, a-z 0-9 - _, starting with a letter (so they also work as names in programs).
 * NULL if fine, else a reason. */
#define LIB_NAME_MAX 15
const char *lib_check_name(const char *name);
/* What (use name) reads: malloc'd source of a library program, or NULL. While lib_put / lib_del check a change, it
 * returns the changed text (or NULL for a deleted one). */
char       *lib_source(const char *name);
/* Store a library program after compiling it once (cmd_lock held). Running programs that use libraries (channels, the
 * autoexec, the automation program) are compiled against the new text first: if one would no longer compile, nothing
 * changes and the reason names it. Those that compile are then switched to the new text. NULL if fine, else a reason. */
const char *lib_put(const char *name, const char *src, char *err, size_t errlen);
/* Remove a library program, with the same check (cmd_lock held): ESP_ERR_NOT_FOUND if there is none,
 * ESP_ERR_INVALID_STATE if a running program uses it (err says which). */
esp_err_t   lib_del(const char *name, char *err, size_t errlen);
/* Bumped by every library change (automation re-checks programs it put on channels). */
extern volatile uint32_t g_lib_gen;
