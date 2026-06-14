/* pd_compile.h — top-level: source string -> compiled program -> run.
 *
 * Convenience API used by tests and the CLI. Wires lexer + parser +
 * interpreter. JIT path added in M2.
 */
#ifndef PD_COMPILE_H
#define PD_COMPILE_H

#include "pd_ir.h"
#include "pd_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compile an EVAL source string into a program.
 * Returns 1 on success (prog filled), 0 on error (err filled). */
int pd_compile(pd_Program *prog, const char *src, char *err, size_t errLen);

/* Compile with a host function/variable table attached. */
int pd_compile_host(pd_Program *prog, const char *src, const pd_Host *host, char *err, size_t errLen);

/* Compile and run in one call (no params, no globals). Returns the
 * result, or NaN on compile error (err filled). */
double pd_eval(const char *src, char *err, size_t errLen);

#ifdef __cplusplus
}
#endif
#endif
