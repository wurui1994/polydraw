/* pd_interp.h — bytecode interpreter for pd_Program.
 *
 * Equivalent of original kasm87c_run() (eval.c:5579).
 * Walks instr[], resolves operand registers to double storage,
 * dispatches on opcode. This is the always-available fallback
 * when JIT is unavailable.
 */
#ifndef PD_INTERP_H
#define PD_INTERP_H

#include "pd_ir.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Execution context. Holds the per-call frame (locals) and parameter
 * pointer. Recursion uses the C stack (interp calls itself for PD_CALL). */
typedef struct pd_Ctx {
    const pd_Program *prog;
    double   *frame;       /* locals, size = prog->nLocals */
    const double *params;  /* function parameters, size = prog->nParams */
    double   *globals;     /* shared global static block */
    volatile int *shouldQuit; /* freeze-probe; checked in loops */
    int        quitCounter;
    struct pd_Ctx *parent; /* caller's ctx, for accessing host externs */
    const pd_Program *root; /* the entry program (holds funcs[], host, extra[]) */
} pd_Ctx;

/* Resolve a register to a double storage pointer (for value operands).
 * For LABEL, returns &instr-index (the out.off is the target).
 * STR regs have no double storage (they are string-table offsets resolved
 * only by pd_call_arg); reading one elsewhere yields a safe zero slot. */
static inline double *pd_slot(pd_Ctx *c, pd_Reg r) {
    static double zero = 0.0;
    switch (r.fam) {
        case PD_FAM_LOCAL:  return &c->frame[r.off / 8];
        case PD_FAM_CONST:  return &c->prog->consts[r.off / 8];
        case PD_FAM_PARAM:  return (double*)&c->params[r.off / 8];
        case PD_FAM_GLOBAL: return &c->globals[r.off / 8];
        case PD_FAM_STR:    return &zero;
        case PD_FAM_EXT: {
            /* the const slot holds a bit-cast double* to the host variable */
            double *slot = &c->prog->consts[r.off / 8];
            void *pp; memcpy(&pp, slot, sizeof(void*));
            return (double*)pp;
        }
        default:            return NULL;
    }
}

/* Run a program. params must point at prog->nParams doubles.
 * globals must point at the shared global static block.
 * shouldQuit may be NULL (no freeze protection). */
double pd_run(const pd_Program *prog, const double *params,
              double *globals, volatile int *shouldQuit);

/* Run with an explicit ctx (used for recursive CALL). */
double pd_run_ctx(pd_Ctx *c);

/* Seed the EVAL RNG (affects RND/NRND ops). Matches original ksrand. */
void pd_srand(unsigned long s);

/* ---- Shared helpers used by both the interpreter and the sljit JIT ----
 * These expose the exact CALL/PEEK/POKE/ADDR semantics and the RNG state so
 * the JIT backend produces bit-identical results. */
unsigned long pd_krand(void);
double pd_nrnd(void);
double pd_fact(double num);
double pd_jit_addr(pd_Ctx *c, const pd_Instr *in);
double pd_jit_addrslot(pd_Ctx *c, const pd_Instr *in);
double pd_jit_peek(pd_Ctx *c, const pd_Instr *in);
void   pd_jit_poke(pd_Ctx *c, const pd_Instr *in, pd_Op op);
double pd_jit_call(pd_Ctx *c, const pd_Instr *in);

#ifdef __cplusplus
}
#endif
#endif
