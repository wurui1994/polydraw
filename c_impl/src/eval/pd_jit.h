/* pd_jit.h — JIT backends for pd_Program.
 *
 * Two backends share one contract:
 *   - pd_jit_sljit.c: sljit (transitional backend).
 *   - pd_jit_llvm.c:  LLVM via ORC/LLJIT (core target, non-optional).
 *
 * Each compiles a pd_Program entry function into native code and runs it.
 * All arithmetic/control-flow that is bit-exact when lowered to IEEE-754
 * machine instructions is emitted inline; every op that depends on libm
 * semantics, RNG state, or host/user calls is routed to a C helper that
 * reuses the *same* code as the interpreter (see pd_interp.c), so a JIT is
 * guaranteed bit-identical to pd_run.
 *
 * pd_jit.c is a thin dispatcher that selects the preferred backend (LLVM if
 * compiled in, else sljit) and implements the pd_jit_* API used by the rest
 * of the codebase. The per-backend pd_sljit_* / pd_llvm_* functions exist so
 * differential tests can exercise both backends in one binary.
 */
#ifndef PD_JIT_H
#define PD_JIT_H

#include "pd_ir.h"
#include "pd_interp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if any JIT backend was compiled in (POLYDRAW_JIT != off). */
int pd_jit_available(void);

/* Human-readable name of the preferred JIT backend ("llvm", "sljit", or
 * "none"), for diagnostics. Always safe to call. */
const char *pd_jit_backend_name(void);

/* Runtime on/off toggle (only meaningful when pd_jit_available()). */
void pd_jit_set_enabled(int on);
int  pd_jit_enabled(void);

/* Typedef for a JIT-compiled entry function. Takes the run context (frame,
 * params, globals, etc. already populated) and returns the program value. */
typedef double (*pd_jit_func_t)(pd_Ctx *c);

/* Compile (or fetch a cached copy of) JIT code for prog's entry function.
 * Returns NULL if the JIT is unavailable or compilation failed. */
pd_jit_func_t pd_jit_get(const pd_Program *prog);

/* Run a program via the preferred JIT backend. Falls back to the interpreter
 * if JIT is disabled or unavailable. Freeze protection uses shouldQuit. */
double pd_run_jit(const pd_Program *prog, const double *params,
                  double *globals, volatile int *shouldQuit);

/* ---- sljit backend (present when PD_HAVE_SLJIT) ---- */
int  pd_sljit_available(void);
void pd_sljit_set_enabled(int on);
int  pd_sljit_enabled(void);
pd_jit_func_t pd_sljit_get(const pd_Program *prog);
double pd_sljit_run_jit(const pd_Program *prog, const double *params,
                        double *globals, volatile int *shouldQuit);

/* ---- LLVM backend (present when PD_HAVE_LLVM) ---- */
int  pd_llvm_available(void);
void pd_llvm_set_enabled(int on);
int  pd_llvm_enabled(void);
pd_jit_func_t pd_llvm_get(const pd_Program *prog);
double pd_llvm_run_jit(const pd_Program *prog, const double *params,
                       double *globals, volatile int *shouldQuit);

#ifdef __cplusplus
}
#endif
#endif
