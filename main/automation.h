#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The automation program: runs every g_set.auto_every seconds (aligned to wall-clock seconds once the time is set,
 * otherwise to uptime), at boot after the autoexec, and whenever it changes. It may use G0..G7, env and the
 * automation natives (intensity, set-intensity, play, stop), which reach the system through the dsl_host_t here.
 * Source: g_set.automation (progs partition, key "automat"); period: g_set.auto_every (settings, "autoevery"). */
#define AUTO_EVERY_DEFAULT 10
#define AUTO_EVERY_MAX     3600

typedef struct {
    bool        active;          /* a program is set */
    uint32_t    runs, faults;
    int64_t     last_run;        /* epoch seconds (or uptime seconds before the time is set); -1 = never */
    bool        last_run_is_time;
    const char *last_err;        /* last fault, or NULL */
} automation_status_t;

void        automation_init(void);                     /* after render_start: compiles the stored program, starts the task */
/* Compile only (cmd_lock held): NULL if fine, else err. */
const char *automation_check(const char *src, char *err, size_t errlen);
/* Set (NULL or "" clears) and run at once (cmd_lock held). NULL if fine, else err (nothing changed). */
const char *automation_set(const char *src, char *err, size_t errlen);
void        automation_run_now(void);
/* Compile the stored program again, keeping it if that fails (cmd_lock held): a library it uses has changed. */
void        automation_reload(void);
/* Connect the core to the system (library lookups for `use`): before anything is compiled at boot. */
void        automation_set_host(void);
void        automation_get_status(automation_status_t *st);   /* cmd_lock held */
