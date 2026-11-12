#pragma once
#include <stdbool.h>
#include <stdint.h>

/* GPIO outputs (relays, PWM): up to IO_MAX, configured in g_set.io, controlled from the console (`io`), the JSON API
 * and the automation program (io / io-set). Always off at boot; configuration changes apply at once. */
#define IO_MAX        8
#define IO_NAME_MAX   15
#define IO_FREQ_MIN   1
#define IO_FREQ_MAX   20000
#define IO_PWM_DEFAULT_HZ 1000

enum { IO_BINARY = 0, IO_PWM };

typedef struct {
    int8_t   gpio;                       /* -1 = unused slot */
    uint8_t  mode;                       /* IO_BINARY / IO_PWM */
    uint8_t  invert;                     /* 1: active low (on = pin low) */
    uint16_t freq;                       /* PWM Hz */
    char     name[IO_NAME_MAX + 1];      /* "" = none; a-z 0-9 - _, starting with a letter */
} io_cfg_t;

/* Check a whole output table against the LED channels' pins and itself. NULL if fine, else a reason (with `bad` =
 * the slot). PWM needs a free LEDC timer per distinct frequency and a channel per output. */
const char *io_check(const io_cfg_t *io, int *bad);
const char *io_check_name(const char *name);  /* NULL if fine */

/* Set the pins up from g_set.io (cmd_lock held). Outputs whose pin, mode, polarity and frequency are unchanged keep
 * running untouched (renaming doesn't glitch a relay); changed or new ones start off. */
void        io_apply(void);
int         io_on_gpio(int gpio);            /* output slot configured on that gpio, or -1 */
bool        io_working(int n);               /* set up without errors */
int         io_find(const char *name);       /* slot with that name, or -1 */
bool        io_used(int n);                  /* slot n is configured */
/* Set output n: 0..256 (binary: 0 = off, anything else = on). NULL if fine, else a reason. */
const char *io_set(int n, uint16_t v);
uint16_t    io_get(int n);                   /* 0..256 (binary: 0 or 256) */
