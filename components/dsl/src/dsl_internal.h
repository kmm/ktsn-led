#pragma once
#include "dsl.h"
value_t dsl_eval_node(exec_ctx_t *c, const node_t *n);
/* Evaluate (if needed) and coerce arg i of n. On failure c->err is set and V_NIL returned. */
value_t dsl_eval_arg(exec_ctx_t *c, const node_t *n, uint8_t i);
bool    dsl_coerce(atype_t want, value_t in, value_t *out);
const native_def_t *dsl_find_native(const char *name);
const dsl_host_t *dsl_host(void);
/* Enumerate the native table (IDE autocomplete, reference coverage tests). */
int dsl_native_count(void);
const native_def_t *dsl_native_at(int i);
/* Variables: not in the native table (not callable by name). The compiler emits them for (def name value) and for
 * a variable name used as a value; arg 0 is the slot number. */
extern const native_def_t dsl_var_set, dsl_var_get;
/* A constant as a node (a function body form that is a literal or a folded call); node->ret = its type. */
extern const native_def_t dsl_const;

/* ---- user functions: (defn name (p:type ...) body...) and (fn (p:type ...) body...) ----
 * Parameters and locals are slots of the program (no call stack): functions never call each other in a circle, so a
 * function is never active twice. A self tail call reassigns the parameters and starts the body again (a loop).
 * Calls point at `def` (NF_LAZY, args typed by the parameters, ret = the body's type); self tail calls at `tdef`. */
typedef struct ufn {
    native_def_t def;            /* first member: a call node's def is the function */
    native_def_t tdef;
    node_t      *body;           /* forms in order; the value of the last is the result */
    uint8_t      slot0, nparams; /* parameters are slots slot0 .. slot0 + nparams - 1 */
    uint8_t      flags;          /* UF_* */
    uint16_t     stack;          /* estimated evaluator stack of the body in bytes */
} ufn_t;
#define UF_KEEP 1                /* writes a string into an enclosing variable: map-px / repeat must keep scratch */
/* Result type of a self tail call while its function's type is not known yet (only `if` branches see it). */
#define V_TAILCALL 0xFE
/* Run f with argv (its parameter values, already of the parameter types). V_NIL with c->err set on a fault. */
value_t dsl_call(exec_ctx_t *c, const ufn_t *f, const value_t *argv);
value_t dsl_ucall(exec_ctx_t *c, const value_t *a, uint8_t n);    /* def.fn */
value_t dsl_utail(exec_ctx_t *c, const value_t *a, uint8_t n);    /* tdef.fn */
