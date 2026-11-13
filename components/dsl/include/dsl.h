#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "arena.h"
#include "pixel.h"

#define DSL_MAX_ARGS         10     /* set: buf index + 8 colors. Arg arrays are sized per call, so this costs no stack */
#define DSL_MAX_DEPTH        12     /* nesting depth limit of the source (parser recursion) */
#define DSL_MAX_EVAL_STACK   6400   /* estimated evaluator stack in bytes, function calls included, checked at compile time */
#define DSL_MAX_NODES        512    /* nodes per compiled program */
#define DSL_NODE_BUDGET      65536  /* nodes evaluated per run (backstop for endless loops; on the board see deadline_us) */
#define DSL_NUM_GLOBALS      8
#define DSL_PROG_ARENA_MAX   32768  /* compiled program size limit; the kept block is sized exactly (two-pass compile) */
#define DSL_TMP_ARENA_MAX    16384  /* compile-time AST scratch, per top-level form */
#define DSL_ERR_LEN          96
#define DSL_MAX_VARS         64     /* slots per program: (def name value), function parameters and locals */
#define DSL_MAX_FNS          64     /* (defn ...) per program, those of used libraries included */
#define DSL_MAX_USES         8      /* library programs one program pulls in with (use name), directly or not */
#define DSL_VAR_NAME_MAX     15
#define DSL_STR_MAX          255    /* characters in a string literal or a string made at run time */

/* ---- values ---- */
typedef enum { V_NIL = 0, V_INT, V_FIX, V_COLOR, V_BUF, V_SYM, V_BOOL, V_STR, V_FN } vtype_t;   /* V_BOOL: u.i is 0 (nil) or 1 (t) */
/* V_FIX is Q16.16. Argument kinds natives can declare: */
typedef enum { A_BUF = 0, A_COLOR, A_INT, A_UNIT, A_SYM, A_NUM, A_BOOL, A_ANY, A_NUMERIC, A_STR, A_FN } atype_t;
/* A_UNIT: 0.0..1.0 -> Q8 0..256 (clamped).  A_NUM: any int or decimal -> Q16.16 (V_FIX), ints saturate at +-32767.
 * A_NUMERIC: int or decimal, passed unchanged (V_INT or V_FIX).  A_BOOL: t/nil only, no numeric truthiness.
 * A_ANY: any value, including calls that return nothing (natives check types in their bind hook).  A_STR: a string.
 * A_FN: a function, given as a defn name or (fn (params) body); always a constant (V_FN), the bind hook checks its
 * signature (ufn_t in dsl_internal.h). Function values exist only as such arguments: they can't be stored or returned. */

/* View of `len` pixels: pixel i is px[i * stride]. Whole buffers have stride 1; slices may step or run backwards. */
typedef struct { pixel_t *px; uint16_t len; int32_t stride; } bufref_t;
static inline pixel_t *buf_at(const bufref_t *b, int32_t i) { return b->px + (ptrdiff_t)i * b->stride; }
/* A string is a view: len bytes at p, not NUL-terminated (str-sub returns part of another string). Literals live in
 * the compiled program; strings made while a program runs live in the per-run scratch, so they last one run. */
typedef struct { const char *p; uint16_t len; } strref_t;
typedef struct {
    uint8_t type;
    union { int32_t i; uint32_t color; bufref_t buf; const char *sym; strref_t str; const struct ufn *fn; } u;
} value_t;

/* ---- environment (written by the system, read by programs) ---- */
#define DSL_AUDIO_BANDS 8
#define DSL_BEATMS_NONE 2147483647   /* beatms before any beat: so "(lt (env beatms) 100)" is false */
typedef struct {
    uint32_t frame;
    uint16_t year;
    uint8_t  month, day;         /* 1..12, 1..31 */
    uint16_t doy;                /* ordinal day of the year, 1..366 */
    uint8_t  hour, minute, second;
    uint16_t ms;                 /* 0..999 within the second */
    uint8_t  weekday;            /* 0..6, 0 = Sunday */
    uint8_t  timeset;            /* 1 once the clock has been set (NTP or env time) */
    int8_t   daylight;           /* 1 sun above the sunrise altitude now, 0 below, -1 = no location or clock not set */
    uint16_t intensity;         /* global intensity 0..256, applied at output time */
    int16_t  sunrise, sunset;    /* minutes after local midnight; -1 = no location, clock not set, or none that day */
    int16_t  dawn, dusk;         /* civil twilight, same */
    int16_t  moon, moonlit;      /* phase angle 0..359 (0 new, 180 full), percent lit; -1 = clock not set */
    uint32_t band[DSL_AUDIO_BANDS];  /* audio input: energy per band, low to high, 0..65536 (= 1.0); 0 without one */
    uint32_t level;              /* audio input: overall, 0..65536 */
    uint32_t beats;              /* audio input: beats heard since it was set up */
    int32_t  beatms;             /* ms since the latest beat; DSL_BEATMS_NONE = no audio input or no beat yet */
} env_t;

/* ---- perf stats ---- */
typedef struct { uint32_t last, min, max, avg, count; } stat_us_t;
void stat_record(stat_us_t *s, uint32_t us);

/* ---- execution context handed to native functions ---- */
struct channel;
typedef struct {
    struct channel *ch;
    const env_t    *env;
    arena_t        *scratch;     /* per-run temp memory, reset before every run (NULL during compile-time folding) */
    uint32_t        budget;
    const char     *err;         /* natives set this to abort the run */
    const struct node *node;     /* call being evaluated; NF_LAZY natives read it (on entry) to evaluate their own args */
    value_t        *vars;        /* the program's slots (variables, parameters, locals); NULL during compile-time folding */
    const struct ufn *tail;      /* set by a self tail call: the function's body starts again with the new parameters */
    int64_t         deadline;    /* time limit for loops (channel_t.deadline_us), 0 = none */
    uint8_t         ticks;       /* the clock is read on every 32nd function call */
} exec_ctx_t;

typedef value_t (*native_fn)(exec_ctx_t *c, const value_t *args, uint8_t nargs);
#define NF_FOLD 1   /* pure: evaluated at compile time when all args are constant */
#define NF_LAZY 2   /* gets args == NULL and evaluates them itself via dsl_eval_arg(c, c->node, i) (if/and/or) */
#define NF_AUTOMATION 4   /* only in the automation program (DSL_CTX_AUTOMATION); a compile error elsewhere */
#define NF_OPT0 8   /* the first argument may be `_` although it is required (e.g. set-intensity: _ = global) */
#define NF_IDARG 16 /* the first argument (A_INT) may also be a bare name, passed as V_SYM for the bind hook to resolve */

/* Optional compile-time hook, run once after a call's args are compiled. It may validate and rewrite constant args
 * (e.g. turn a name into an index) and set the call's result type n->ret (e.g. `if` takes its branches' type).
 * Returns NULL, or an error message with *bad set to the offending arg index. */
typedef struct { struct node *node; value_t val; } arg_t;     /* node != NULL -> evaluated at run time */
typedef const char *(*native_bind_fn)(struct node *n, uint8_t *bad);

typedef struct {
    const char *name;
    native_fn   fn;
    uint8_t     ret;                       /* vtype_t */
    uint8_t     min_args, max_args;
    uint8_t     args[DSL_MAX_ARGS];        /* atype_t per position */
    uint8_t     flags;
    native_bind_fn bind;                   /* NULL if none */
} native_def_t;

/* ---- compiled program: linked list of call nodes, all in one malloc block ---- */
typedef struct node node_t;
struct node {
    const native_def_t *def;
    arg_t              *args;
    uint8_t             nargs;
    uint8_t             ret;          /* result type (vtype_t): def->ret unless the bind hook refines it */
    uint16_t            stack;        /* estimated evaluator stack of this call in bytes, function bodies included */
    node_t             *next;
};
/* slots: nvars values in the program block, cleared at the start of every run (one runner per program). */
/* stack: estimated evaluator stack of the program in bytes (its deepest top-level form, function bodies included). */
typedef struct { node_t *head; uint16_t nnodes, stack; uint8_t nvars; value_t *slots; arena_t arena; } program_t;
void program_free(program_t *p);

/* ---- buffers ---- */
typedef struct { pixel_t *buf[DSL_NUM_GLOBALS]; uint16_t npix; } globals_t;
int  globals_init(globals_t *g, uint16_t npix);

enum { BUF_X = 0, BUF_Y, BUF_Z, BUF_O, BUF_COUNT };

/* What a channel's programs are for. Render programs (the default) draw into X Y Z O; the automation program runs every
 * few seconds, may only use G0..G7 and env, and is the only one allowed the NF_AUTOMATION natives. */
enum { DSL_CTX_RENDER = 0, DSL_CTX_AUTOMATION };

/* What the automation natives need from the system (the core itself stays portable). Set once with dsl_set_host;
 * without a host those natives fault at run time. Intensities are 0..256, ch -1 = global. play/stop return NULL or
 * an error message (counted as a fault). lib_exists and io_find check names at compile time; NULL accepts any library
 * name (and no output names). */
typedef struct {
    int         (*nchannels)(void);
    uint16_t    (*intensity_get)(int ch);
    void        (*intensity_set)(int ch, uint16_t v, uint32_t fade_ms);
    const char *(*play)(int ch, const char *name);
    const char *(*stop)(int ch);
    bool        (*lib_exists)(const char *name);
    /* GPIO outputs (io / io-set): io_find = slot of a name or -1; io_used = slot n is configured; values 0..256. */
    int         (*io_find)(const char *name);
    bool        (*io_used)(int n);
    uint16_t    (*io_get)(int n);
    const char *(*io_set)(int n, uint16_t v);
    /* (use name): a library program's source, malloc'd (the core frees it), NULL if there is none. Without this hook
     * programs can't use libraries. lib_why, optional: why a name isn't available (NULL: the usual message). */
    char       *(*lib_load)(const char *name);
    const char *(*lib_why)(const char *name);
    /* GPIO inputs (in / analog; channel programs too, so these must be cheap and safe from the render task).
     * io_mode: DSL_IO_* of a configured slot, -1 = not configured or unknown (then the compiler doesn't check the
     * kind). in_get: a digital input, debounced. analog_get: the average of the latest `samples` readings, 0..65536. */
    int         (*io_mode)(int n);
    bool        (*in_get)(int n);
    uint32_t    (*analog_get)(int n, int samples);
} dsl_host_t;
enum { DSL_IO_BINARY = 0, DSL_IO_PWM, DSL_IO_INPUT, DSL_IO_ANALOG, DSL_IO_AUDIO };
#define DSL_ANALOG_SAMPLES_DEFAULT 16
#define DSL_ANALOG_SAMPLES_MAX     256
void dsl_set_host(const dsl_host_t *h);

typedef struct channel {
    uint16_t   npix;
    pixel_t   *buf[BUF_COUNT];                 /* X Y Z O: persist between runs */
    arena_t    scratch;                        /* reset before each run */
    program_t *prog;                           /* owned by the render task */
    _Atomic(program_t *) pending;              /* freshly compiled, swapped in at frame start */
    char      *source;                         /* last accepted source text (for `list`) */
    const env_t *env;
    globals_t *globals;
    uint32_t   rng;                            /* xorshift32 state, never 0; seed it after channel_init */
    uint8_t    ctx;                            /* DSL_CTX_*: set before compiling (channel_init: render) */
    int64_t    deadline_us;                    /* function calls and loops fault past this time (dsl_set_clock), 0 = no limit */
    struct {
        stat_us_t render, output;
        uint32_t  nodes_last, faults;
        uint16_t  stack;                       /* the running program's estimated stack (program_t.stack) */
        const char *last_err;
    } perf;
} channel_t;

int      channel_init(channel_t *ch, uint16_t npix, size_t scratch_bytes, const env_t *env, globals_t *g);
void     channel_free(channel_t *ch);
/* Compile + queue. On failure nothing changes and `err` holds "line L col C: message". */
int      channel_set_source(channel_t *ch, const char *src, char *err, size_t errlen);
/* Publish a program already compiled for this channel by dsl_compile (takes ownership) and remember its source.
 * Lets callers compile several programs first and publish only if all of them compiled. */
void     channel_set_program(channel_t *ch, program_t *p, const char *src);
/* Swap in pending program, reset scratch, run. Returns nodes evaluated. */
uint32_t channel_render(channel_t *ch);
/* "X".."O" or "G0".."G7". Returns false if unknown. */
bool     dsl_resolve_buf(const channel_t *ch, const char *name, bufref_t *out);
/* Segment-display glyph of an ASCII character, 0 if it can't be shown. Bits are the segments in the display's legend
 * order (7: a..g p; 9: a..g j m p; 14: a b c d e f g1 g2 h j k l m n p); `kind` is 7, 9 or 14. */
uint16_t dsl_seg_glyph(int kind, char ch);

program_t *dsl_compile(channel_t *ch, const char *src, char *err, size_t errlen);
/* The clock channel_t.deadline_us is measured against (microseconds). Without one (NULL) there is no time limit. */
void dsl_set_clock(int64_t (*now_us)(void));
