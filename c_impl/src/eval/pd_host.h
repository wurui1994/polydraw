/* pd_host.h — external (host) function registration.
 *
 * Lets EVAL scripts call into C functions provided by the host (e.g. printf,
 * glBegin, glVertex, klock, xres, etc). This is how polydraw.c's myext[]
 * table is replicated.
 *
 * Two kinds of host symbols:
 *   - host variables (xres, yres, numframes, ...): a double* the host updates
 *       each frame; the EVAL script reads it like a normal variable.
 *   - host functions (glBegin, printf, ...): a C function pointer with a
 *       known parameter count; the interpreter calls it via PD_CALL.
 */
#ifndef PD_HOST_H
#define PD_HOST_H

#include "pd_ir.h"
#include "pd_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A host function: receives count + an array of double args (each arg is the
 * raw double value; for pointer/array params the host casts the bits).
 * Returns a double result (0.0 for void functions). */
typedef double (*pd_HostFn)(int nargs, const double *args);

typedef struct {
    char       name[40];   /* upper-case, includes prototype-less base name */
    int        nParams;    /* parameter count (variadic printf uses nargs as-is) */
    pd_HostFn  fn;         /* the C callback */
    int        variadic;   /* 1 if printf-style variadic (nParams is a hint) */
} pd_HostFunc;

/* A host variable: pointer to a double the host keeps updated. */
typedef struct {
    char    name[40];
    double *pVal;
} pd_HostVar;

#define PD_MAX_HOST_FNS 256
#define PD_MAX_HOST_VARS 64

typedef struct pd_Host {
    pd_HostFunc fns[PD_MAX_HOST_FNS];
    int         nFns;
    pd_HostVar  vars[PD_MAX_HOST_VARS];
    int         nVars;
} pd_Host;

void pd_host_init(pd_Host *h);

/* Register a host function. proto is like "GLBEGIN()" or "GLVERTEX(,,)".
 * Returns the function index or -1 on table full / bad proto. */
int pd_host_add_fn(pd_Host *h, const char *proto, pd_HostFn fn, int variadic);

/* Register a host variable (script reads it as a global). */
int pd_host_add_var(pd_Host *h, const char *name, double *pVal);

/* Look up a function by name + arity. Returns index or -1. */
int pd_host_find_fn(const pd_Host *h, const char *name, int nargs);

/* Look up a variable by name. Returns index or -1. */
int pd_host_find_var(const pd_Host *h, const char *name);

/* Register all symbols into a parser's symbol table so scripts can reference
 * them. The host table's lifetime must outlive the compiled program. */
void pd_host_install(const pd_Host *h, pd_Parser *p);

/* Attach a host table to a program (for interpreter dispatch). The program
 * does not own the table. */
void pd_host_attach(pd_Program *prog, const pd_Host *h);

#ifdef __cplusplus
}
#endif
#endif
