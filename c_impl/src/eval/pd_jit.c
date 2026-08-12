/* pd_jit.c — backend dispatcher.
 *
 * Implements the pd_jit_* API used by the rest of the codebase by selecting
 * the preferred compiled backend: LLVM (core target) first, then sljit
 * (transitional), else the interpreter. The Makefile defines PD_HAVE_LLVM /
 * PD_HAVE_SLJIT for the backends actually compiled in.
 */
#include "pd_jit.h"

int pd_jit_available(void) {
#if defined(PD_HAVE_LLVM)
    return pd_llvm_available();
#elif defined(PD_HAVE_SLJIT)
    return pd_sljit_available();
#else
    return 0;
#endif
}

const char *pd_jit_backend_name(void) {
#if defined(PD_HAVE_LLVM)
    if (pd_llvm_available()) return "llvm";
#elif defined(PD_HAVE_SLJIT)
    if (pd_sljit_available()) return "sljit";
#else
    (void)0;
#endif
    return "none";
}

void pd_jit_set_enabled(int on) {
#if defined(PD_HAVE_LLVM)
    pd_llvm_set_enabled(on);
#endif
#if defined(PD_HAVE_SLJIT)
    pd_sljit_set_enabled(on);
#endif
}

int pd_jit_enabled(void) {
#if defined(PD_HAVE_LLVM)
    if (pd_llvm_enabled()) return 1;
#endif
#if defined(PD_HAVE_SLJIT)
    if (pd_sljit_enabled()) return 1;
#endif
    return 0;
}

pd_jit_func_t pd_jit_get(const pd_Program *prog) {
#if defined(PD_HAVE_LLVM)
    return pd_llvm_get(prog);
#elif defined(PD_HAVE_SLJIT)
    return pd_sljit_get(prog);
#else
    (void)prog;
    return NULL;
#endif
}

double pd_run_jit(const pd_Program *prog, const double *params,
                  double *globals, volatile int *shouldQuit) {
#if defined(PD_HAVE_LLVM)
    return pd_llvm_run_jit(prog, params, globals, shouldQuit);
#elif defined(PD_HAVE_SLJIT)
    return pd_sljit_run_jit(prog, params, globals, shouldQuit);
#else
    return pd_run(prog, params, globals, shouldQuit);
#endif
}
