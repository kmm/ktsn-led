#pragma once
#include <stdbool.h>
#include <stdint.h>

/* GPIO slots: outputs (relays, PWM) and inputs (digital, analog), up to IO_MAX, configured in g_set.io. Outputs are
 * controlled from the console (`io`), the JSON API and the automation program (io / io-set), always off at boot.
 * Inputs are sampled in the background (inputs task): programs read them with (in x) / (analog x [samples]).
 * Configuration changes apply at once. */
#define IO_MAX        8
#define IO_NAME_MAX   15
#define IO_FREQ_MIN   1
#define IO_FREQ_MAX   20000
#define IO_PWM_DEFAULT_HZ 1000

#define IO_DEBOUNCE_DEFAULT 20           /* ms a digital input must stay put before it counts */
#define IO_DEBOUNCE_MAX   1000
#define IO_SAMPLES_MAX    256              /* analog readings kept per input (one per ms) */
#define IO_ANALOG_FULL    65536            /* io_analog_get scale: 1.0 (Q16.16) */

enum { IO_BINARY = 0, IO_PWM, IO_INPUT, IO_ANALOG, IO_MODE_COUNT };
enum { IO_PULL_NONE = 0, IO_PULL_UP, IO_PULL_DOWN };
static inline bool io_mode_is_output(int m) { return m == IO_BINARY || m == IO_PWM; }

/* Stored as the NVS blob "io": the fields before `pull` are the layout before inputs existed (0.19.0 and older,
 * IO_CFG_V1_SIZE bytes per slot); settings.c reads either size. */
typedef struct {
    int8_t   gpio;                       /* -1 = unused slot */
    uint8_t  mode;                       /* IO_BINARY / IO_PWM / IO_INPUT / IO_ANALOG */
    uint8_t  invert;                     /* output: active low (on = pin low); input: pressed = low reads as t */
    uint16_t freq;                       /* PWM Hz */
    char     name[IO_NAME_MAX + 1];      /* "" = none; a-z 0-9 - _, starting with a letter */
    uint8_t  pull;                       /* input: IO_PULL_* */
    uint8_t  trigger;                    /* input: 1 = run the automation program after each (debounced) change */
    uint16_t debounce;                   /* input: ms, 0..IO_DEBOUNCE_MAX */
} io_cfg_t;
#define IO_CFG_V1_SIZE 22                /* bytes per slot before 0.20.0 (gpio .. name) */

/* Check a whole slot table against the LED channels' pins and itself. NULL if fine, else a reason (with `bad` =
 * the slot). PWM needs a free LEDC timer per distinct frequency and a channel per output; analog inputs need an
 * ADC1 pin (ADC2 can't run next to WiFi). */
const char *io_check(const io_cfg_t *io, int *bad);
const char *io_check_name(const char *name);  /* NULL if fine */

/* Set the pins up from g_set.io (cmd_lock held). Outputs whose pin, mode, polarity and frequency are unchanged keep
 * running untouched (renaming doesn't glitch a relay); changed or new ones start off. */
void        io_apply(void);
int         io_on_gpio(int gpio);            /* slot configured on that gpio, or -1 */
int         io_mode(int n);                  /* IO_* of a configured slot, or -1 */
const char *io_mode_name(int m);             /* "binary", "pwm", "input", "analog" */
int         io_mode_parse(const char *s);    /* the reverse, -1 if unknown */
const char *io_pull_name(int p);             /* "none", "up", "down" */
int         io_pull_parse(const char *s);
const char *io_analog_pins(void);            /* the pins analog inputs may use, for messages, e.g. "32..39" */
bool        io_working(int n);               /* set up without errors */
int         io_find(const char *name);       /* slot with that name, or -1 */
bool        io_used(int n);                  /* slot n is configured */
/* Set output n: 0..256 (binary: 0 = off, anything else = on). NULL if fine, else a reason. */
const char *io_set(int n, uint16_t v);
uint16_t    io_get(int n);                   /* outputs: 0..256 (binary: 0 or 256); inputs: as io_in_get / io_analog_get, scaled */
/* Inputs (any task; values come from the inputs task): */
bool        io_in_get(int n);                /* digital input, debounced, after invert; false if not an input */
/* Analog input: the average of the latest `samples` readings (one per ms, 1..IO_SAMPLES_MAX), 0..IO_ANALOG_FULL. */
uint32_t    io_analog_get(int n, int samples);
