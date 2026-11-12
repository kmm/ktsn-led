/* DSL front end: tokenizer + parser -> temporary AST -> type-checked linked list of nodes.
 * All numeric literals are converted here: ints stay ints, decimals become Q16.16,
 * and A_UNIT arguments are folded to Q8 (0..256). No floating point anywhere. */
#include "dsl_internal.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { K_LIST, K_INT, K_FIX, K_COLOR, K_SYM, K_STR } kind_t;
typedef struct ast {
    kind_t kind;
    int32_t v;                    /* K_STR: length of s */
    const char *s;
    const char *at;               /* position in source, for error messages */
    struct ast *child, *next;
} ast_t;

/* src: the text being read. main / unit / use_at: a used library is reported at the (use ...) in the program: "line L
 * col C: in <unit> line l: message" (the IDE and the console take L and C for the program they show). */
typedef struct { const char *src, *p; arena_t *tmp; char *err; size_t errlen; const char *main, *unit, *use_at; } ps_t;
/* Names -> slots. A slot keeps its first type and belongs to the scope it was made in (slots are never reused). */
typedef struct { char name[DSL_VAR_NAME_MAX + 1]; uint8_t type; uint16_t scope; } var_t;
/* Open scopes, innermost last: the program's top level, a defn body (a barrier: nothing outside it is visible), and
 * (fn ...) bodies inside them (they see the scopes around them up to the barrier). */
typedef struct { uint16_t id; bool barrier; ufn_t *fn; } scope_t;
enum { FN_TODO = 0, FN_BUSY, FN_DONE };          /* ordering state (depth-first walk over the calls) */
typedef struct { ufn_t *f; const char *src; uint64_t calls; uint8_t state, unit; bool compiled; } fnent_t;
/* A text the program is made of: 0 is the program, the rest the libraries it uses (each loaded once). */
typedef struct { char name[DSL_VAR_NAME_MAX + 1]; char *src; const char *use_at; uint8_t state; } unit_t;
/* Tables too large for the compiling task's stack: one malloc per compile, vars and fns cleared per pass. */
typedef struct { var_t vars[DSL_MAX_VARS]; fnent_t fns[DSL_MAX_FNS]; unit_t units[DSL_MAX_USES + 1]; uint8_t nunits; } cc_tables_t;
typedef struct {
    channel_t *ch; arena_t *pa; ps_t *ps; uint16_t nnodes;
    var_t *vars; uint8_t nvars;
    fnent_t *fns; uint8_t nfns;                  /* (defn ...) of the program and its libraries, in source order */
    unit_t *units;
    scope_t scopes[DSL_MAX_DEPTH + 2]; uint8_t nscopes; uint16_t next_scope;
    ufn_t *cur;                                  /* defn whose body is being compiled; NULL at the top level */
    bool tail;                                   /* the call compiled next is in tail position of cur */
} cc_t;

static void perr(ps_t *ps, const char *at, const char *fmt, ...)
{
    if (ps->err[0]) return;                        /* first error wins */
    int line = 1, col = 1, mline = 1, mcol = 1, n;
    for (const char *c = ps->src; c < at && *c; c++) { if (*c == '\n') { line++; col = 1; } else col++; }
    if (ps->unit) {
        for (const char *c = ps->main; c < ps->use_at && *c; c++) { if (*c == '\n') { mline++; mcol = 1; } else mcol++; }
        n = snprintf(ps->err, ps->errlen, "line %d col %d: in %s line %d: ", mline, mcol, ps->unit, line);
    } else n = snprintf(ps->err, ps->errlen, "line %d col %d: ", line, col);
    if (n < 0 || (size_t)n >= ps->errlen) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(ps->err + n, ps->errlen - (size_t)n, fmt, ap);
    va_end(ap);
}

/* ---------------- lexer / parser ---------------- */
static void skip_ws(ps_t *ps)
{
    while (*ps->p) {
        if (isspace((unsigned char)*ps->p)) ps->p++;
        else if (*ps->p == ';') { while (*ps->p && *ps->p != '\n') ps->p++; }
        else break;
    }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_color(const char *t, uint32_t *out)       /* t is after '#': RRGGBB or RRGGBBWW */
{
    size_t n = strlen(t);
    if (n != 6 && n != 8) return false;
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) { int h = hexval(t[i]); if (h < 0) return false; v = (v << 4) | (uint32_t)h; }
    *out = (n == 6) ? v : (((v >> 8) & 0xFFFFFFu) | ((v & 0xFFu) << 24));
    return true;
}

static bool parse_number(const char *t, kind_t *kind, int32_t *out)
{
    const char *p = t;
    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    int64_t ip = 0; int nd = 0;
    while (isdigit((unsigned char)*p)) { ip = ip * 10 + (*p - '0'); if (ip > INT32_MAX) return false; p++; nd++; }
    if (*p == 0) { if (!nd) return false; *kind = K_INT; *out = (int32_t)(neg ? -ip : ip); return true; }
    if (*p != '.') return false;
    p++;
    uint64_t fr = 0, scale = 1; int used = 0, any = 0;
    while (isdigit((unsigned char)*p)) {
        if (used < 5) { fr = fr * 10 + (uint64_t)(*p - '0'); scale *= 10; used++; }
        any++; p++;
    }
    if (*p || (!nd && !any) || ip > 32767) return false;
    int64_t q = ip * 65536 + (int64_t)((fr * 65536 + scale / 2) / scale);   /* Q16.16, rounded */
    if (q > INT32_MAX) q = INT32_MAX;
    *kind = K_FIX; *out = (int32_t)(neg ? -q : q);
    return true;
}

static ast_t *new_ast(ps_t *ps, kind_t k, const char *at)
{
    ast_t *a = arena_alloc(ps->tmp, sizeof *a);
    if (!a) { perr(ps, at, "source too complex"); return NULL; }
    memset(a, 0, sizeof *a);
    a->kind = k; a->at = at;
    return a;
}

static ast_t *parse_expr(ps_t *ps, int depth)
{
    skip_ws(ps);
    const char *at = ps->p;
    if (!*at) { perr(ps, at, "unexpected end of input"); return NULL; }
    if (*at == ')') { perr(ps, at, "unexpected ')'"); return NULL; }

    if (*at == '(') {
        if (depth >= DSL_MAX_DEPTH) { perr(ps, at, "nesting too deep (max %d)", DSL_MAX_DEPTH); return NULL; }
        ps->p++;
        ast_t *lst = new_ast(ps, K_LIST, at);
        if (!lst) return NULL;
        ast_t **tail = &lst->child;
        for (;;) {
            skip_ws(ps);
            if (!*ps->p) { perr(ps, at, "unclosed '('"); return NULL; }
            if (*ps->p == ')') { ps->p++; break; }
            ast_t *c = parse_expr(ps, depth + 1);
            if (!c) return NULL;
            *tail = c; tail = &c->next;
        }
        return lst;
    }

    if (*at == '"') {                                    /* "text": \" and \\ are the only escapes; one line */
        size_t n = 0;                                    /* first pass: find the end, count, check escapes */
        const char *q = at + 1;
        for (;; q++) {
            if (!*q || *q == '\n') { perr(ps, at, "unclosed string (strings end with \" on the same line)"); return NULL; }
            if (*q == '"') break;
            if (*q == '\\' && *++q != '"' && *q != '\\') { perr(ps, q - 1, "unknown escape in a string (only \\\" and \\\\)"); return NULL; }
            if (++n > DSL_STR_MAX) { perr(ps, at, "string too long (max %d characters)", DSL_STR_MAX); return NULL; }
        }
        ps->p = q + 1;
        if (*ps->p && !isspace((unsigned char)*ps->p) && *ps->p != '(' && *ps->p != ')' && *ps->p != ';') {
            perr(ps, ps->p, "expected a space or ')' after the string"); return NULL;
        }
        ast_t *a = new_ast(ps, K_STR, at);
        char *copy = a ? arena_alloc(ps->tmp, n ? n : 1) : NULL;
        if (!copy) { perr(ps, at, "source too complex"); return NULL; }
        size_t k = 0;
        for (q = at + 1; *q != '"'; q++) copy[k++] = *q == '\\' ? *++q : *q;
        a->s = copy; a->v = (int32_t)n;
        return a;
    }

    char tok[40];
    const char *s = ps->p;
    while (*ps->p && !isspace((unsigned char)*ps->p) && *ps->p != '(' && *ps->p != ')' && *ps->p != ';') ps->p++;
    size_t len = (size_t)(ps->p - s);
    if (len >= sizeof tok) { perr(ps, at, "token too long"); return NULL; }
    memcpy(tok, s, len); tok[len] = 0;

    if (tok[0] == '#') {
        uint32_t c;
        if (!parse_color(tok + 1, &c)) { perr(ps, at, "bad color '%s' (want #RRGGBB or #RRGGBBWW)", tok); return NULL; }
        ast_t *a = new_ast(ps, K_COLOR, at); if (a) a->v = (int32_t)c;
        return a;
    }
    bool numlike = isdigit((unsigned char)tok[0]) || tok[0] == '.' ||
                   (tok[0] == '-' && (isdigit((unsigned char)tok[1]) || tok[1] == '.'));
    if (numlike) {
        kind_t k; int32_t v;
        if (!parse_number(tok, &k, &v)) { perr(ps, at, "bad number '%s'", tok); return NULL; }
        ast_t *a = new_ast(ps, k, at); if (a) a->v = v;
        return a;
    }
    ast_t *a = new_ast(ps, K_SYM, at);
    if (!a) return NULL;
    a->s = arena_strdup(ps->tmp, tok);
    if (!a->s) { perr(ps, at, "source too complex"); return NULL; }
    return a;
}

/* ---------------- type checking / compile ---------------- */
static const char *const k_atype_names[] = { "buffer", "color", "int", "number", "symbol", "number", "boolean", "value", "number", "string", "function" };
static const char *const k_vtype_names[] = { "nothing", "int", "number", "color", "buffer", "symbol", "boolean", "string", "function" };

bool dsl_coerce(atype_t want, value_t in, value_t *out)
{
    *out = in;
    switch (want) {
    case A_BUF:   return in.type == V_BUF;
    case A_COLOR: return in.type == V_COLOR;
    case A_INT:   return in.type == V_INT;
    case A_SYM:   return in.type == V_SYM;
    case A_BOOL:  return in.type == V_BOOL;
    case A_STR:   return in.type == V_STR;
    case A_FN:    return in.type == V_FN;
    case A_ANY:   return true;
    case A_NUMERIC: return in.type == V_INT || in.type == V_FIX;
    case A_NUM:
        if (in.type == V_FIX) return true;
        if (in.type != V_INT) return false;
        out->type = V_FIX;
        out->u.i = in.u.i > 32767 ? INT32_MAX : in.u.i < -32767 ? -INT32_MAX : in.u.i * 65536;
        return true;
    case A_UNIT: {
        int32_t q;
        if (in.type == V_INT)      q = in.u.i < 0 ? 0 : (in.u.i > 1 ? (int32_t)UNIT_ONE : in.u.i * (int32_t)UNIT_ONE);
        else if (in.type == V_FIX) { q = (int32_t)(((int64_t)in.u.i + 128) >> 8); if (q < 0) q = 0; if (q > (int32_t)UNIT_ONE) q = UNIT_ONE; }
        else return false;
        out->type = V_INT; out->u.i = q;
        return true;
    }
    }
    return false;
}

static bool ret_ok(atype_t want, uint8_t ret)
{
    switch (want) {
    case A_BUF:   return ret == V_BUF;
    case A_COLOR: return ret == V_COLOR;
    case A_INT:   return ret == V_INT;
    case A_SYM:   return ret == V_SYM;
    case A_UNIT:  return ret == V_INT || ret == V_FIX;
    case A_NUM:   return ret == V_INT || ret == V_FIX;
    case A_NUMERIC: return ret == V_INT || ret == V_FIX;
    case A_BOOL:  return ret == V_BOOL;
    case A_STR:   return ret == V_STR;
    case A_FN:    return ret == V_FN;
    case A_ANY:   return true;
    }
    return false;
}

static node_t *compile_call(cc_t *cc, const ast_t *lst);
static bool compile_fn_arg(cc_t *cc, const ast_t *a, arg_t *out);
static int find_fn(const cc_t *cc, const char *name);

/* Numbers are not booleans, decimals are not ints: point at the fix. */
static const char *bool_hint(atype_t want, uint8_t got)
{
    if (want == A_BOOL && (got == V_INT || got == V_FIX)) return " (compare it, e.g. (gt x 0))";
    if (want == A_INT && got == V_FIX) return " (use floor or round)";
    return "";
}

/* Index of the innermost open scope with this id at or above the nearest barrier, or -1 (not visible). */
static int scope_index(const cc_t *cc, uint16_t id)
{
    for (int k = cc->nscopes - 1; k >= 0; k--) {
        if (cc->scopes[k].id == id) return k;
        if (cc->scopes[k].barrier) break;
    }
    return -1;
}

/* Slot of a visible variable (the newest of that name), or -1. */
static int find_var(const cc_t *cc, const char *name)
{
    for (int i = cc->nvars - 1; i >= 0; i--)
        if (!strcmp(cc->vars[i].name, name) && scope_index(cc, cc->vars[i].scope) >= 0) return i;
    return -1;
}

static int find_fn(const cc_t *cc, const char *name)
{
    for (int i = 0; i < cc->nfns; i++) if (!strcmp(cc->fns[i].f->def.name, name)) return i;
    return -1;
}

/* What one call costs the evaluator, in bytes of stack: dsl_eval_arg + dsl_eval_node + the 16-byte argument values of
 * the call (a fixed 16, so the host, the IDE and the board accept the same programs), plus n_if / n_or for lazy natives,
 * dsl_ucall + dsl_call for user functions, n_map_px + dsl_call for a function argument. Frame sizes of the -O2 firmware,
 * the larger of ESP32 and C6 (disassembly, 2026-11-09); check them when the evaluator changes. DSL_MAX_EVAL_STACK leaves
 * room in the 8 KB task stacks for the tasks' own frames, the deepest native (seg_draw ~400 B) and interrupts. */
enum { STK_CALL = 176, STK_LAZY = 288, STK_USER = 304, STK_TAIL = 256, STK_FN_ARG = 208, STK_VALUE = 16 };

/* A node with constant args only (e.g. a variable slot number). */
static node_t *new_node(cc_t *cc, const native_def_t *def, uint8_t ret, uint8_t nargs, const char *at)
{
    if (++cc->nnodes > DSL_MAX_NODES) { perr(cc->ps, at, "program too large (max %d calls)", DSL_MAX_NODES); return NULL; }
    node_t *nd = arena_alloc(cc->pa, sizeof *nd);
    arg_t *args = arena_alloc(cc->pa, nargs * sizeof *args);
    if (!nd || !args) { perr(cc->ps, at, "program too large"); return NULL; }
    memset(args, 0, nargs * sizeof *args);
    nd->def = def; nd->args = args; nd->nargs = nargs; nd->ret = ret; nd->stack = STK_CALL + nargs * STK_VALUE; nd->next = NULL;
    return nd;
}

/* A variable name used as a value: fetch its slot at run time. */
static bool compile_var_ref(cc_t *cc, const ast_t *a, atype_t want, int slot, arg_t *out)
{
    uint8_t type = cc->vars[slot].type;
    if (!ret_ok(want, type)) {
        perr(cc->ps, a->at, "expected %s, but variable '%s' is %s%s", k_atype_names[want], a->s, k_vtype_names[type], bool_hint(want, type));
        return false;
    }
    node_t *nd = new_node(cc, &dsl_var_get, type, 1, a->at);
    if (!nd) return false;
    nd->args[0].val.type = V_INT; nd->args[0].val.u.i = slot;
    out->node = nd;
    return true;
}

/* `optional`: the native has a default for this position, so `_` may stand in for it (passed as V_NIL). */
static bool compile_arg(cc_t *cc, const ast_t *a, atype_t want, bool optional, arg_t *out)
{
    ps_t *ps = cc->ps;
    out->node = NULL;
    memset(&out->val, 0, sizeof out->val);

    if (a->kind == K_SYM && !strcmp(a->s, "_")) {
        if (!optional) { perr(ps, a->at, "'_' can only stand in for an optional argument"); return false; }
        return true;
    }
    if (want == A_FN) return compile_fn_arg(cc, a, out);

    if (a->kind == K_LIST) {
        node_t *sub = compile_call(cc, a);
        if (!sub) return false;
        if (sub->ret == V_NIL && want != A_ANY) { perr(ps, a->at, "'%s' returns nothing and can't be used as a value", sub->def->name); return false; }
        bool allconst = true;
        for (uint8_t i = 0; i < sub->nargs; i++) if (sub->args[i].node) allconst = false;
        if ((sub->def->flags & NF_FOLD) && allconst) {
            exec_ctx_t fc = { .ch = cc->ch, .env = cc->ch->env, .budget = 8 };
            value_t av[sub->nargs ? sub->nargs : 1];      /* sized per call: keeps nested compiles cheap */
            for (uint8_t i = 0; i < sub->nargs; i++) av[i] = sub->args[i].val;
            value_t v = sub->def->fn(&fc, av, sub->nargs);
            if (fc.err) { perr(ps, a->at, "%s", fc.err); return false; }
            if (!dsl_coerce(want, v, &out->val)) {
                perr(ps, a->at, "expected %s, got %s%s", k_atype_names[want], k_vtype_names[v.type], bool_hint(want, v.type)); return false;
            }
            return true;
        }
        if (!ret_ok(want, sub->ret)) {
            perr(ps, a->at, "expected %s, but '%s' returns %s%s", k_atype_names[want], sub->def->name, k_vtype_names[sub->ret],
                 bool_hint(want, sub->ret));
            return false;
        }
        out->node = sub;
        return true;
    }

    value_t v; memset(&v, 0, sizeof v);
    switch (a->kind) {
    case K_INT:   v.type = V_INT;   v.u.i = a->v; break;
    case K_FIX:   v.type = V_FIX;   v.u.i = a->v; break;
    case K_COLOR: v.type = V_COLOR; v.u.color = (uint32_t)a->v; break;
    case K_STR: {                                         /* copied into the program: the AST goes at the end of the form */
        char *p = a->v ? arena_alloc(cc->pa, (size_t)a->v) : NULL;
        if (a->v && !p) { perr(ps, a->at, "program too large"); return false; }
        if (a->v) memcpy(p, a->s, (size_t)a->v);
        v.type = V_STR; v.u.str.p = p ? p : ""; v.u.str.len = (uint16_t)a->v;
        break;
    }
    case K_SYM:
        if (want != A_SYM && (!strcmp(a->s, "t") || !strcmp(a->s, "nil"))) { v.type = V_BOOL; v.u.i = a->s[0] == 't'; break; }
        if (want != A_SYM) {                              /* a name where a value goes: must be a variable */
            int slot = find_var(cc, a->s);
            if (slot >= 0) return compile_var_ref(cc, a, want, slot, out);
            if (find_fn(cc, a->s) >= 0) { perr(ps, a->at, "'%s' is a function: call it, e.g. (%s ...)", a->s, a->s); return false; }
            if (want == A_BUF) perr(ps, a->at, "unknown name '%s' (did you mean (buf %s)? or define it: (def %s ...))", a->s, a->s, a->s);
            else perr(ps, a->at, "unknown name '%s' (define it first: (def %s ...))", a->s, a->s);
            return false;
        }
        v.type = V_SYM; v.u.sym = arena_strdup(cc->pa, a->s);
        if (!v.u.sym) { perr(ps, a->at, "program too large"); return false; }
        break;
    default: return false;
    }
    if (!dsl_coerce(want, v, &out->val)) {
        if (a->kind == K_SYM && want == A_BUF)
            perr(ps, a->at, "expected buffer, got symbol '%s' (did you mean (buf %s)?)", a->s, a->s);
        else
            perr(ps, a->at, "expected %s, got %s%s", k_atype_names[want], k_vtype_names[v.type], bool_hint(want, v.type));
        return false;
    }
    return true;
}

static node_t *compile_call(cc_t *cc, const ast_t *lst)
{
    ps_t *ps = cc->ps;
    const ast_t *h = lst->child;
    if (!h) { perr(ps, lst->at, "empty list"); return NULL; }
    if (h->kind != K_SYM) { perr(ps, h->at, "expected a function name after '('"); return NULL; }
    bool tail = cc->tail;                                /* only this call, not its arguments */
    cc->tail = false;
    if (!strcmp(h->s, "def")) {
        perr(ps, lst->at, "def only works at the top level of a program or of a function body, e.g. (def p (slice (buf G0) 0 3))");
        return NULL;
    }
    if (!strcmp(h->s, "defn")) { perr(ps, lst->at, "defn only works at the top level of a program"); return NULL; }
    if (!strcmp(h->s, "use")) { perr(ps, lst->at, "use only works at the top level of a program"); return NULL; }
    if (!strcmp(h->s, "fn")) { perr(ps, lst->at, "fn makes a function to pass to map-px or repeat; it can't be used here"); return NULL; }
    const native_def_t *def = dsl_find_native(h->s);
    const ufn_t *uf = NULL;                              /* a user function call (not a self tail call) */
    if (!def) {
        int fi = find_fn(cc, h->s);
        if (fi < 0) { perr(ps, h->at, "unknown function '%s'", h->s); return NULL; }
        ufn_t *f = cc->fns[fi].f;
        if (f == cc->cur) {
            if (!tail) { perr(ps, h->at, "'%s' calls itself here: that only works as its very last step (a tail call)", h->s); return NULL; }
            def = &f->tdef;
        } else if (!cc->fns[fi].compiled) {
            perr(ps, h->at, "'%s' is used before it is compiled", h->s); return NULL;   /* not reachable: callees go first */
        } else {
            def = &f->def; uf = f;
        }
    }
    if ((def->flags & NF_AUTOMATION) && cc->ch->ctx != DSL_CTX_AUTOMATION) {
        perr(ps, h->at, "'%s' only works in the automation program", def->name);
        return NULL;
    }

    int n = 0;
    for (const ast_t *a = h->next; a; a = a->next) n++;
    if (n < def->min_args || n > def->max_args) {
        if (def->min_args == def->max_args) perr(ps, lst->at, "'%s' takes %d argument(s), got %d", def->name, def->min_args, n);
        else perr(ps, lst->at, "'%s' takes %d to %d arguments, got %d", def->name, def->min_args, def->max_args, n);
        return NULL;
    }
    if (++cc->nnodes > DSL_MAX_NODES) { perr(ps, lst->at, "program too large (max %d calls)", DSL_MAX_NODES); return NULL; }

    node_t *node = arena_alloc(cc->pa, sizeof *node);
    arg_t *args = n ? arena_alloc(cc->pa, (size_t)n * sizeof *args) : NULL;
    if (!node || (n && !args)) { perr(ps, lst->at, "program too large"); return NULL; }
    node->def = def; node->args = args; node->nargs = (uint8_t)n; node->ret = def->ret; node->stack = 0; node->next = NULL;

    int i = 0;
    uint16_t deepest = uf ? uf->stack : 0;               /* the deepest argument, or the body the call runs */
    bool is_if = !strcmp(def->name, "if");
    for (const ast_t *a = h->next; a; a = a->next, i++) {
        atype_t want = (atype_t)def->args[i];
        /* NF_IDARG: a bare name (not a variable, t or nil) in the first position goes to the bind hook as a symbol */
        if (i == 0 && (def->flags & NF_IDARG) && a->kind == K_SYM && strcmp(a->s, "_") && strcmp(a->s, "t") && strcmp(a->s, "nil")
            && find_var(cc, a->s) < 0) want = A_SYM;
        cc->tail = tail && is_if && i > 0;               /* an if's branches are in tail position if the if is */
        bool ok = compile_arg(cc, a, want, i >= def->min_args || (i == 0 && (def->flags & NF_OPT0)), &args[i]);
        cc->tail = false;
        if (!ok) return NULL;
        uint16_t st = args[i].node ? args[i].node->stack : args[i].val.type == V_FN ? STK_FN_ARG + args[i].val.u.fn->stack : 0;
        if (st > deepest) deepest = st;
    }
    node->stack = deepest + (uf ? STK_USER + n * STK_VALUE : def->fn == dsl_utail ? STK_TAIL + n * STK_VALUE
                             : (def->flags & NF_LAZY) ? STK_LAZY : STK_CALL + n * STK_VALUE);
    if (node->stack > DSL_MAX_EVAL_STACK) {
        perr(ps, lst->at, "calls nested too deep (about %d bytes of stack, counting the functions they call; max %d)", node->stack, DSL_MAX_EVAL_STACK);
        return NULL;
    }
    if (def->bind) {
        uint8_t bad = 0;
        const char *msg = def->bind(node, &bad);
        if (msg) {
            const ast_t *at = h->next;
            for (uint8_t k = 0; k < bad && at && at->next; k++) at = at->next;
            perr(ps, at ? at->at : lst->at, "%s", msg);
            return NULL;
        }
    }
    return node;
}

void program_free(program_t *p) { free(p); }

static bool var_name_ok(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > DSL_VAR_NAME_MAX || !isalpha((unsigned char)s[0])) return false;
    for (const char *p = s; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') return false;
    return strcmp(s, "t") && strcmp(s, "nil");
}

static bool is_form(const ast_t *e, const char *head)
{
    return e->kind == K_LIST && e->child && e->child->kind == K_SYM && !strcmp(e->child->s, head);
}

/* A name taken by a function can't also be a variable or parameter (keeps calls and values apart). */
static bool name_free(cc_t *cc, const ast_t *name)
{
    if (find_fn(cc, name->s) < 0) return true;
    perr(cc->ps, name->at, "'%s' is already a function name", name->s);
    return false;
}

static bool open_scope(cc_t *cc, bool barrier, ufn_t *fn, const char *at)
{
    if (cc->nscopes >= sizeof cc->scopes / sizeof cc->scopes[0]) { perr(cc->ps, at, "functions nested too deep"); return false; }
    cc->scopes[cc->nscopes++] = (scope_t){ .id = cc->next_scope++, .barrier = barrier, .fn = fn };
    return true;
}

static int new_var(cc_t *cc, const ast_t *name, uint8_t type)
{
    if (cc->nvars >= DSL_MAX_VARS) { perr(cc->ps, name->at, "too many variables and parameters (max %d)", DSL_MAX_VARS); return -1; }
    var_t *v = &cc->vars[cc->nvars];
    strcpy(v->name, name->s);
    v->type = type;
    v->scope = cc->scopes[cc->nscopes - 1].id;
    return cc->nvars++;
}

/* (def <name> <value>) at the top level of the program or of a function body: the value is computed when the def runs
 * and stored in the variable's slot. A visible variable of that name is assigned (same type); otherwise the def makes
 * a new one in the current scope. */
static node_t *compile_def(cc_t *cc, const ast_t *lst)
{
    ps_t *ps = cc->ps;
    const ast_t *name = lst->child->next, *expr = name ? name->next : NULL;
    if (!name || !expr || expr->next) { perr(ps, lst->at, "'def' takes a name and a value: (def name value)"); return NULL; }
    if (name->kind != K_SYM || !var_name_ok(name->s)) {
        perr(ps, name->at, "bad variable name (a letter, then letters, digits, - or _; max %d; not t or nil)", DSL_VAR_NAME_MAX);
        return NULL;
    }
    if (!name_free(cc, name)) return NULL;
    arg_t val;
    if (!compile_arg(cc, expr, A_ANY, false, &val)) return NULL;      /* compiled first: may read the old value */
    uint8_t type = val.node ? val.node->ret : val.val.type;
    if (type == V_NIL) { perr(ps, expr->at, "'def' needs a value, but this returns nothing"); return NULL; }
    if (type == V_SYM) { perr(ps, expr->at, "'def' can't store a bare name"); return NULL; }
    int slot = find_var(cc, name->s);
    if (slot >= 0 && cc->vars[slot].type != type) {
        perr(ps, expr->at, "'%s' is already %s, it can't become %s%s", name->s, k_vtype_names[cc->vars[slot].type], k_vtype_names[type],
             cc->vars[slot].type == V_INT && type == V_FIX ? " (define it as a decimal from the start, e.g. 1.0)" : "");
        return NULL;
    }
    if (slot >= 0 && type == V_STR) {                    /* a string kept outside the fns it is made in: */
        for (int k = scope_index(cc, cc->vars[slot].scope) + 1; k < cc->nscopes; k++)   /* map-px / repeat keep scratch */
            if (cc->scopes[k].fn) cc->scopes[k].fn->flags |= UF_KEEP;
    }
    if (slot < 0 && (slot = new_var(cc, name, type)) < 0) return NULL;
    node_t *nd = new_node(cc, &dsl_var_set, V_NIL, 2, lst->at);
    if (!nd) return NULL;
    nd->args[0].val.type = V_INT; nd->args[0].val.u.i = slot;
    nd->args[1] = val;
    if (val.node) nd->stack += val.node->stack;
    return nd;
}

/* ---- functions ---- */

static const struct { const char *name; uint8_t atype, vtype; } k_ptypes[] = {
    { "int", A_INT, V_INT }, { "decimal", A_NUM, V_FIX }, { "color", A_COLOR, V_COLOR },
    { "bool", A_BOOL, V_BOOL }, { "buf", A_BUF, V_BUF }, { "str", A_STR, V_STR },
};

static ufn_t *new_ufn(cc_t *cc, const char *name, const char *at)
{
    ufn_t *f = arena_alloc(cc->pa, sizeof *f);
    const char *nm = f ? arena_strdup(cc->pa, name) : NULL;
    if (!nm) { perr(cc->ps, at, "program too large"); return NULL; }
    memset(f, 0, sizeof *f);
    f->def.name = nm; f->def.fn = dsl_ucall; f->def.ret = V_NIL; f->def.flags = NF_LAZY;
    f->tdef = f->def;
    f->tdef.fn = dsl_utail; f->tdef.ret = V_TAILCALL;
    return f;
}

/* (p:type ...): the parameter types go into f's call signature; with `declare`, the names become slots in the current
 * scope (the body's). Both are done from the same list, so a defn's header can be read before its body. */
static bool parse_params(cc_t *cc, const ast_t *list, ufn_t *f, bool declare)
{
    ps_t *ps = cc->ps;
    uint8_t n = 0;
    for (const ast_t *p = list->child; p; p = p->next, n++) {
        const char *colon = p->kind == K_SYM ? strchr(p->s, ':') : NULL;
        if (!colon) {
            perr(ps, p->at, "a parameter is name:type, e.g. c:color (types: int decimal color bool buf str)");
            return false;
        }
        char name[DSL_VAR_NAME_MAX + 2];
        size_t len = (size_t)(colon - p->s);
        if (len > DSL_VAR_NAME_MAX) len = DSL_VAR_NAME_MAX + 1;           /* too long: fails var_name_ok */
        memcpy(name, p->s, len); name[len] = 0;
        if (!var_name_ok(name)) {
            perr(ps, p->at, "bad parameter name (a letter, then letters, digits, - or _; max %d; not t or nil)", DSL_VAR_NAME_MAX);
            return false;
        }
        int t = -1;
        for (size_t k = 0; k < sizeof k_ptypes / sizeof k_ptypes[0]; k++) if (!strcmp(colon + 1, k_ptypes[k].name)) t = (int)k;
        if (t < 0) { perr(ps, p->at, "unknown parameter type '%s' (int decimal color bool buf str)", colon + 1); return false; }
        if (n >= DSL_MAX_ARGS) { perr(ps, p->at, "too many parameters (max %d)", DSL_MAX_ARGS); return false; }
        for (const ast_t *q = list->child; q != p; q = q->next)
            if (!strncmp(q->s, name, len) && q->s[len] == ':') { perr(ps, p->at, "parameter '%s' appears twice", name); return false; }
        f->def.args[n] = f->tdef.args[n] = k_ptypes[t].atype;
        if (declare) {
            ast_t nm = { .kind = K_SYM, .s = name, .at = p->at };
            if (!name_free(cc, &nm) || new_var(cc, &nm, k_ptypes[t].vtype) < 0) return false;
        }
    }
    f->nparams = f->def.min_args = f->def.max_args = f->tdef.min_args = f->tdef.max_args = n;
    return true;
}

/* Parameters, then the body forms in a new scope (`barrier`: a defn, which sees nothing outside itself). A form is a
 * def, a call or a value; the last one's value is the result. In a defn the last form is in tail position. */
static bool compile_body(cc_t *cc, ufn_t *f, const ast_t *params, const ast_t *body, bool barrier, const char *at)
{
    ps_t *ps = cc->ps;
    if (!body) { perr(ps, at, "'%s' needs a body after its parameters", f->def.name); return false; }
    if (!open_scope(cc, barrier, f, at)) return false;
    f->slot0 = cc->nvars;
    bool ok = parse_params(cc, params, f, true);
    node_t **tail = &f->body;
    uint8_t ret = V_NIL;
    uint16_t stack = 0;
    const ast_t *b;
    for (b = body; ok && b; b = b->next) {
        node_t *nd;
        if (is_form(b, "def")) nd = compile_def(cc, b);
        else {
            arg_t v;
            cc->tail = barrier && !b->next;
            ok = compile_arg(cc, b, A_ANY, false, &v);
            cc->tail = false;
            if (!ok) break;
            nd = v.node;
            if (!nd && (nd = new_node(cc, &dsl_const, v.val.type, 1, b->at))) nd->args[0].val = v.val;
        }
        if (!nd) { ok = false; break; }
        ret = nd->ret;
        if (nd->stack > stack) stack = nd->stack;
        *tail = nd; tail = &nd->next;
        if (!b->next && ret == V_TAILCALL) { perr(ps, b->at, "'%s' never returns: it always calls itself again", f->def.name); ok = false; }
    }
    cc->nscopes--;
    f->def.ret = f->tdef.ret = ret;
    f->stack = stack;
    return ok;
}

/* A function argument (A_FN): a defn name or (fn (p:type ...) body...). Always a constant. */
static bool compile_fn_arg(cc_t *cc, const ast_t *a, arg_t *out)
{
    ps_t *ps = cc->ps;
    ufn_t *f = NULL;
    if (a->kind == K_SYM) {
        int fi = find_fn(cc, a->s);
        if (fi < 0) { perr(ps, a->at, "unknown function '%s' (define it with defn, or write (fn (p:type ...) body))", a->s); return false; }
        f = cc->fns[fi].f;
        if (f == cc->cur) { perr(ps, a->at, "'%s' can't pass itself on: a function may only call itself as its very last step", a->s); return false; }
        if (!cc->fns[fi].compiled) { perr(ps, a->at, "'%s' is used before it is compiled", a->s); return false; }
    } else if (is_form(a, "fn")) {
        const ast_t *params = a->child->next;
        if (!params || params->kind != K_LIST) { perr(ps, a->at, "'fn' takes a parameter list and a body: (fn (p:type ...) body)"); return false; }
        if (!(f = new_ufn(cc, "fn", a->at)) || !compile_body(cc, f, params, params->next, false, a->at)) return false;
    } else {
        perr(ps, a->at, "expected a function: a defn name or (fn (p:type ...) body)");
        return false;
    }
    out->val.type = V_FN;
    out->val.u.fn = f;
    return true;
}

/* First look at a (defn name (p:type ...) body...): name and signature, so calls anywhere can be checked. */
static bool declare_fn(cc_t *cc, const ast_t *e, uint8_t unit)
{
    ps_t *ps = cc->ps;
    const ast_t *name = e->child->next, *params = name ? name->next : NULL;
    if (!name || name->kind != K_SYM || !params || params->kind != K_LIST) {
        perr(ps, e->at, "'defn' takes a name, a parameter list and a body: (defn name (p:type ...) body)");
        return false;
    }
    if (!var_name_ok(name->s) || !strcmp(name->s, "def") || !strcmp(name->s, "defn") || !strcmp(name->s, "fn") || !strcmp(name->s, "use")) {
        perr(ps, name->at, "bad function name (a letter, then letters, digits, - or _; max %d)", DSL_VAR_NAME_MAX);
        return false;
    }
    if (dsl_find_native(name->s)) { perr(ps, name->at, "'%s' is a built-in function", name->s); return false; }
    if (find_fn(cc, name->s) >= 0) { perr(ps, name->at, "'%s' is defined twice", name->s); return false; }
    if (cc->nfns >= DSL_MAX_FNS) { perr(ps, name->at, "too many functions (max %d)", DSL_MAX_FNS); return false; }
    ufn_t *f = new_ufn(cc, name->s, name->at);
    if (!f || !parse_params(cc, params, f, false)) return false;
    if (!params->next) { perr(ps, e->at, "'%s' needs a body after its parameters", name->s); return false; }
    cc->fns[cc->nfns++] = (fnent_t){ .f = f, .src = e->at, .unit = unit };
    return true;
}

/* The functions a form uses: calls by name, and names passed where a native takes a function (A_FN). With `want` >= 0,
 * *at is set to the first use of that function. */
static void fn_refs(const cc_t *cc, const ast_t *a, uint64_t *mask, int want, const char **at)
{
    if (a->kind != K_LIST || !a->child) return;
    const ast_t *h = a->child;
    const native_def_t *nd = NULL;
    int j;
    if (h->kind == K_SYM) {
        if ((j = find_fn(cc, h->s)) >= 0) { *mask |= 1ull << j; if (j == want && !*at) *at = h->at; }
        nd = dsl_find_native(h->s);
    }
    int i = 0;
    for (const ast_t *x = h->next; x; x = x->next, i++) {
        if (nd && i < nd->max_args && nd->args[i] == A_FN && x->kind == K_SYM && (j = find_fn(cc, x->s)) >= 0) {
            *mask |= 1ull << j;
            if (j == want && !*at) *at = x->at;
        }
        fn_refs(cc, x, mask, want, at);
    }
}

/* Error positions from here on are in unit u. */
static void enter_unit(cc_t *cc, int u)
{
    cc->ps->src = cc->units[u].src;
    cc->ps->unit = u ? cc->units[u].name : NULL;
    cc->ps->use_at = cc->units[u].use_at;
}

static ast_t *parse_at(cc_t *cc, const char *at)
{
    cc->ps->p = at;
    return parse_expr(cc->ps, 0);
}

/* Depth-first over the calls: callees are appended to `order` before their callers. A function reached again while
 * it is still being walked closes a circle (a function calling itself is left to compile_call: tail calls only). */
static bool order_fns(cc_t *cc, int i, uint8_t *order, uint8_t *n)
{
    fnent_t *e = &cc->fns[i];
    if (e->state == FN_DONE) return true;
    e->state = FN_BUSY;
    for (int j = 0; j < cc->nfns; j++) {
        if (j == i || !(e->calls & (1ull << j))) continue;
        if (cc->fns[j].state == FN_BUSY) {
            uint64_t m = 0;
            const char *at = NULL;
            enter_unit(cc, e->unit);
            fn_refs(cc, parse_at(cc, e->src), &m, j, &at);
            perr(cc->ps, at ? at : e->src, "'%s' and '%s' call each other in a circle (only a function calling itself, as its "
                 "very last step, is allowed)", e->f->def.name, cc->fns[j].f->def.name);
            return false;
        }
        if (!order_fns(cc, j, order, n)) return false;
    }
    e->state = FN_DONE;
    order[(*n)++] = (uint8_t)i;
    return true;
}

/* Load the libraries unit u uses, depth first (each once, so a library reached twice is fine; a circle is an error).
 * Also the first look at every form's syntax. tmem grows to fit the largest text. */
static bool scan_unit(cc_tables_t *tb, int u, const char *top_at, uint8_t **tmem, size_t *tsz, char *ebuf, size_t elen)
{
    unit_t *un = &tb->units[u];
    size_t need = 1024 + 32 * strlen(un->src);           /* temp AST memory, reused per top-level form */
    if (need > DSL_TMP_ARENA_MAX) need = DSL_TMP_ARENA_MAX;
    if (need > *tsz) {
        free(*tmem);
        if (!(*tmem = malloc(need))) { *tsz = 0; snprintf(ebuf, elen, "out of memory"); return false; }
        *tsz = need;
    }
    arena_t tmp;
    arena_init(&tmp, *tmem, *tsz);
    ps_t ps = { .src = un->src, .p = un->src, .tmp = &tmp, .err = ebuf, .errlen = elen, .main = tb->units[0].src,
                .unit = u ? un->name : NULL, .use_at = top_at };
    un->state = FN_BUSY;
    for (;;) {
        skip_ws(&ps);
        if (!*ps.p) break;
        ast_t *e = parse_expr(&ps, 0);
        if (!e) return false;
        if (e->kind != K_LIST) { perr(&ps, e->at, "top-level forms must be calls, e.g. (fill (buf O) #FF0000)"); return false; }
        if (is_form(e, "use")) {
            const ast_t *nm = e->child->next;
            if (!nm || nm->kind != K_SYM || nm->next) { perr(&ps, e->at, "'use' takes a library program's name: (use name)"); return false; }
            int k = 0;
            while (k < tb->nunits && (k == 0 || strcmp(tb->units[k].name, nm->s))) k++;
            if (k < tb->nunits) {
                if (tb->units[k].state == FN_BUSY) { perr(&ps, nm->at, "'%s' uses itself (directly or through other libraries)", nm->s); return false; }
            } else {
                const dsl_host_t *h = dsl_host();
                if (!h || !h->lib_load) { perr(&ps, e->at, "programs can't use libraries here"); return false; }
                if (tb->nunits > DSL_MAX_USES) { perr(&ps, e->at, "too many libraries (max %d)", DSL_MAX_USES); return false; }
                if (strlen(nm->s) > DSL_VAR_NAME_MAX || !(tb->units[k].src = h->lib_load(nm->s))) {
                    const char *why = h->lib_why ? h->lib_why(nm->s) : NULL;
                    if (why) perr(&ps, nm->at, "%s", why); else perr(&ps, nm->at, "no library program '%s'", nm->s);
                    return false;
                }
                strcpy(tb->units[k].name, nm->s);
                tb->units[k].state = FN_TODO;
                tb->units[k].use_at = u ? top_at : e->at;
                tb->nunits++;
                const char *next_top = tb->units[k].use_at;
                if (!scan_unit(tb, k, next_top, tmem, tsz, ebuf, elen)) return false;
                arena_init(&tmp, *tmem, *tsz);                   /* tmem may have moved */
            }
        } else if (u && !is_form(e, "defn")) {
            perr(&ps, e->at, "a library for 'use' may contain only defn and use");
            return false;
        }
        arena_reset(&tmp);
    }
    un->state = FN_DONE;
    return true;
}

/* One compile pass into a program block with `cap` arena bytes. NULL on error (ebuf set).
 * defn headers first (calls anywhere can be checked), then the bodies with callees first, then the other top-level
 * forms. All of the program's own functions are compiled; of the libraries only what the program reaches. */
static program_t *compile_pass(channel_t *ch, const char *src, size_t cap, uint8_t *tmem, size_t tsz, cc_tables_t *tb,
                               char *ebuf, size_t elen)
{
    program_t *prog = malloc(sizeof *prog + cap);
    if (!prog) { snprintf(ebuf, elen, "out of memory"); return NULL; }
    arena_t tmp;
    arena_init(&tmp, tmem, tsz);
    prog->head = NULL; prog->nnodes = 0; prog->nvars = 0; prog->stack = 0; prog->slots = NULL;
    arena_init(&prog->arena, prog + 1, cap);
    memset(tb->vars, 0, sizeof tb->vars);
    memset(tb->fns, 0, sizeof tb->fns);

    ps_t ps = { .src = src, .p = src, .tmp = &tmp, .err = ebuf, .errlen = elen, .main = src };
    cc_t cc = { .ch = ch, .pa = &prog->arena, .ps = &ps, .vars = tb->vars, .fns = tb->fns, .units = tb->units, .nscopes = 1, .next_scope = 1 };
    cc.scopes[0] = (scope_t){ .id = 0, .barrier = true, .fn = NULL };
    for (int u = 0; u < tb->nunits; u++) {               /* 1: defn headers */
        enter_unit(&cc, u);
        ps.p = tb->units[u].src;
        for (;;) {
            skip_ws(&ps);
            if (!*ps.p) break;
            ast_t *e = parse_expr(&ps, 0);
            if (!e) goto fail;
            if (is_form(e, "defn") && !declare_fn(&cc, e, (uint8_t)u)) goto fail;
            arena_reset(&tmp);
        }
    }
    uint64_t roots = 0;                                  /* 2: who calls whom, and what the program itself needs */
    enter_unit(&cc, 0);
    ps.p = src;
    for (;;) {
        skip_ws(&ps);
        if (!*ps.p) break;
        ast_t *e = parse_expr(&ps, 0);
        if (!e) goto fail;
        if (!is_form(e, "defn") && !is_form(e, "use")) fn_refs(&cc, e, &roots, -1, NULL);
        arena_reset(&tmp);
    }
    for (int i = 0; i < cc.nfns; i++) {
        if (cc.fns[i].unit == 0) roots |= 1ull << i;
        fn_refs(&cc, parse_at(&cc, cc.fns[i].src), &cc.fns[i].calls, -1, NULL);
        arena_reset(&tmp);
    }
    uint8_t order[DSL_MAX_FNS], nord = 0;
    for (int i = 0; i < cc.nfns; i++) {
        if (!(roots & (1ull << i))) continue;
        bool ok = order_fns(&cc, i, order, &nord);
        arena_reset(&tmp);
        if (!ok) goto fail;
    }
    for (int k = 0; k < nord; k++) {                     /* 3: bodies, callees first */
        fnent_t *fe = &cc.fns[order[k]];
        enter_unit(&cc, fe->unit);
        const ast_t *e = parse_at(&cc, fe->src), *params = e->child->next->next;
        cc.cur = fe->f;
        bool ok = compile_body(&cc, fe->f, params, params->next, true, e->at);
        cc.cur = NULL;
        arena_reset(&tmp);
        if (!ok) goto fail;
        fe->compiled = true;
    }
    node_t **tail = &prog->head;                         /* 4: the program */
    enter_unit(&cc, 0);
    ps.p = src;
    for (;;) {
        skip_ws(&ps);
        if (!*ps.p) break;
        ast_t *e = parse_expr(&ps, 0);
        if (!e) goto fail;
        if (!is_form(e, "defn") && !is_form(e, "use")) {
            node_t *n = is_form(e, "def") ? compile_def(&cc, e) : compile_call(&cc, e);
            if (!n) goto fail;
            if (n->stack > prog->stack) prog->stack = n->stack;
            *tail = n; tail = &n->next;
        }
        arena_reset(&tmp);                               /* compiled nodes copied what they need */
    }
    if (cc.nvars && !(prog->slots = arena_alloc(&prog->arena, cc.nvars * sizeof(value_t)))) {
        snprintf(ebuf, elen, "program too large");
        goto fail;
    }
    prog->nnodes = cc.nnodes;
    prog->nvars = cc.nvars;
    return prog;
fail:
    free(prog);
    return NULL;
}

/* Two passes: the first into a generous block finds errors and measures; the second compiles into a block of exactly
 * that size (same layout: arena offsets are relative to an 8-aligned base, +7 covers the base adjustment). */
program_t *dsl_compile(channel_t *ch, const char *src, char *err, size_t errlen)
{
    char ebuf[DSL_ERR_LEN] = "";
    uint8_t *tmem = NULL;
    size_t tsz = 0;
    cc_tables_t *tb = malloc(sizeof *tb);
    program_t *result = NULL;
    if (!tb) { snprintf(ebuf, sizeof ebuf, "out of memory"); goto done; }
    memset(tb, 0, sizeof *tb);
    tb->units[0].src = (char *)src;                      /* never freed: unit 0 belongs to the caller */
    tb->nunits = 1;
    if (!scan_unit(tb, 0, NULL, &tmem, &tsz, ebuf, sizeof ebuf)) goto done;

    program_t *probe = compile_pass(ch, src, DSL_PROG_ARENA_MAX, tmem, tsz, tb, ebuf, sizeof ebuf);
    if (!probe) goto done;
    size_t need = probe->arena.used + 7;
    free(probe);
    result = compile_pass(ch, src, need, tmem, tsz, tb, ebuf, sizeof ebuf);
done:
    if (!result && err && errlen) snprintf(err, errlen, "%s", ebuf[0] ? ebuf : "compile failed");
    if (tb) for (int u = 1; u < tb->nunits; u++) free(tb->units[u].src);
    free(tb);
    free(tmem);
    return result;
}
