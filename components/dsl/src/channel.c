#include "dsl_internal.h"
#include <stdlib.h>
#include <string.h>

void stat_record(stat_us_t *s, uint32_t us)
{
    s->last = us;
    if (s->count == 0 || us < s->min) s->min = us;
    if (us > s->max) s->max = us;
    s->avg = s->count ? (uint32_t)((int32_t)s->avg + ((int32_t)us - (int32_t)s->avg) / 16) : us;   /* EMA, 1/16 */
    s->count++;
}

int globals_init(globals_t *g, uint16_t npix)
{
    memset(g, 0, sizeof *g);
    g->npix = npix;
    for (int i = 0; i < DSL_NUM_GLOBALS; i++) {
        g->buf[i] = calloc(npix, sizeof(pixel_t));
        if (!g->buf[i]) return -1;
    }
    return 0;
}

int channel_init(channel_t *ch, uint16_t npix, size_t scratch_bytes, const env_t *env, globals_t *g)
{
    memset(ch, 0, sizeof *ch);
    ch->npix = npix; ch->env = env; ch->globals = g;
    pixel_t *mem = calloc((size_t)BUF_COUNT * npix, sizeof(pixel_t));
    void *scratch = malloc(scratch_bytes);
    if (!mem || !scratch) { free(mem); free(scratch); return -1; }
    for (int i = 0; i < BUF_COUNT; i++) ch->buf[i] = mem + (size_t)i * npix;
    arena_init(&ch->scratch, scratch, scratch_bytes);
    atomic_init(&ch->pending, NULL);
    ch->rng = 0x2545F491u;                               /* fixed default; firmware reseeds from hardware RNG */
    return 0;
}

void channel_free(channel_t *ch)
{
    program_free(atomic_exchange(&ch->pending, NULL));
    program_free(ch->prog);
    free(ch->buf[0]); free(ch->scratch.mem); free(ch->source);     /* mem, not base: base may be rounded up */
    memset(ch, 0, sizeof *ch);
}

void channel_set_program(channel_t *ch, program_t *p, const char *src)
{
    program_free(atomic_exchange(&ch->pending, p));      /* drop a not-yet-picked-up predecessor */
    size_t n = strlen(src) + 1;
    char *copy = malloc(n);
    if (copy) { memcpy(copy, src, n); free(ch->source); ch->source = copy; }
}

int channel_set_source(channel_t *ch, const char *src, char *err, size_t errlen)
{
    program_t *p = dsl_compile(ch, src, err, errlen);
    if (!p) return -1;
    channel_set_program(ch, p, src);
    return 0;
}

value_t dsl_eval_arg(exec_ctx_t *c, const node_t *n, uint8_t i)
{
    const arg_t *a = &n->args[i];
    value_t nil = { .type = V_NIL }, out;
    if (!a->node) return a->val;
    value_t v = dsl_eval_node(c, a->node);
    if (c->err) return nil;
    if (!dsl_coerce((atype_t)n->def->args[i], v, &out)) { c->err = "type error at run time"; return nil; }
    return out;
}

value_t dsl_list_item(exec_ctx_t *c, const dsl_list_t *l, int i)
{
    const arg_t *a = &l->items[i];
    if (!a->node) return a->val;
    value_t v = dsl_eval_node(c, a->node);
    if (c->err) v.type = V_NIL;
    return v;
}

value_t dsl_eval_node(exec_ctx_t *c, const node_t *n)
{
    value_t nil = { .type = V_NIL };
    if (c->budget == 0) { c->err = "node budget exceeded"; return nil; }
    c->budget--;
    if (n->def->flags & NF_LAZY) { c->node = n; return n->def->fn(c, NULL, n->nargs); }
    value_t argv[n->nargs ? n->nargs : 1];               /* sized per call: deep nesting stays cheap on the stack */
    for (uint8_t i = 0; i < n->nargs; i++) {
        argv[i] = dsl_eval_arg(c, n, i);
        if (c->err) return nil;
    }
    return n->def->fn(c, argv, n->nargs);
}

static int64_t (*s_clock)(void);
void dsl_set_clock(int64_t (*now_us)(void)) { s_clock = now_us; }

/* Every function call and tail-call round passes here. Only loops can take long, so programs without functions never
 * see the time limit. */
static void time_check(exec_ctx_t *c)
{
    if (c->deadline && !(++c->ticks & 31) && s_clock() > c->deadline) c->err = "time limit exceeded (loops must fit in the frame)";
}

value_t dsl_call(exec_ctx_t *c, const ufn_t *f, const value_t *argv)
{
    value_t v = { .type = V_NIL };
    if (f->nparams) {
        if (!c->vars) { c->err = "function called without slots"; return v; }
        memcpy(&c->vars[f->slot0], argv, f->nparams * sizeof *argv);
    }
    for (;;) {
        time_check(c);
        if (c->err) { v.type = V_NIL; return v; }
        for (const node_t *b = f->body; b; b = b->next) {
            v = dsl_eval_node(c, b);
            if (c->err) { v.type = V_NIL; return v; }
        }
        if (c->tail != f) return v;
        c->tail = NULL;                                  /* self tail call: parameters are set, run the body again */
    }
}

/* A call of a user function (NF_LAZY: the function is the node's def). */
value_t dsl_ucall(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)a;
    const node_t *self = c->node;
    value_t argv[n ? n : 1];
    for (uint8_t i = 0; i < n; i++) {
        argv[i] = dsl_eval_arg(c, self, i);
        if (c->err) { argv[0].type = V_NIL; return argv[0]; }
    }
    return dsl_call(c, (const ufn_t *)self->def, argv);
}

/* A self tail call: all new values first (they may read the old ones), then the parameters, then dsl_call loops. */
value_t dsl_utail(exec_ctx_t *c, const value_t *a, uint8_t n)
{
    (void)a;
    const node_t *self = c->node;
    const ufn_t *f = (const ufn_t *)((const char *)self->def - offsetof(ufn_t, tdef));
    value_t argv[n ? n : 1], nil = { .type = V_NIL };
    for (uint8_t i = 0; i < n; i++) {
        argv[i] = dsl_eval_arg(c, self, i);
        if (c->err) return nil;
    }
    memcpy(&c->vars[f->slot0], argv, n * sizeof *argv);
    c->tail = f;
    return nil;
}

uint32_t channel_render(channel_t *ch)
{
    program_t *p = atomic_exchange(&ch->pending, NULL);
    if (p) { program_free(ch->prog); ch->prog = p; ch->perf.stack = p->stack; }   /* swap only at a frame boundary */
    arena_reset(&ch->scratch);
    if (!ch->prog) return 0;

    exec_ctx_t c = { .ch = ch, .env = ch->env, .scratch = &ch->scratch, .budget = DSL_NODE_BUDGET, .deadline = s_clock ? ch->deadline_us : 0 };
    if (ch->prog->nvars) {                                /* slots live in the program block: fresh every run */
        c.vars = ch->prog->slots;
        memset(c.vars, 0, ch->prog->nvars * sizeof(value_t));
    }
    for (const node_t *n = ch->prog->head; n && !c.err; n = n->next) dsl_eval_node(&c, n);
    if (c.err) { ch->perf.faults++; ch->perf.last_err = c.err; }
    ch->perf.nodes_last = DSL_NODE_BUDGET - c.budget;
    return ch->perf.nodes_last;
}
