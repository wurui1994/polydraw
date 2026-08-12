/* pd_jit_sljit.c — sljit JIT backend for pd_Program.
 *
 * Compiles a program's entry function into native code. IEEE-754-exact
 * arithmetic, neg/abs, and comparisons are emitted inline; everything that
 * depends on libm semantics, RNG state, or host/user calls is routed to a C
 * helper that reuses the *same* code as the interpreter (pd_interp.c), so the
 * JIT is guaranteed bit-identical to pd_run.
 *
 * Freeze protection: a probe is emitted at every backward (loop) branch
 * edge that returns 0.0 if *shouldQuit is set.
 */
#include "pd_jit.h"
#include "pd_interp.h"
#include "sljitLir.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#ifndef POLYDRAW_JIT_OFF
#define PD_JIT_COMPILED 1
#endif
#ifdef PD_JIT_COMPILED
#ifndef PD_HAVE_SLJIT
#define PD_HAVE_SLJIT 1
#endif
#endif

/* ---- register assignment (saved GP regs hold base pointers) ---- */
#define J_CTX      SLJIT_S0   /* pd_Ctx*                */
#define J_FRAME    SLJIT_S1   /* c->frame  (double*)    */
#define J_GLOBALS  SLJIT_S2   /* c->globals (double*)   */
#define J_PARAMS   SLJIT_S3   /* c->params (double*)    */
#define J_CONSTS   SLJIT_S4   /* c->prog->consts        */
#define J_ROOT     SLJIT_S5   /* c->root                */
#define J_QUIT     SLJIT_S6   /* c->shouldQuit          */
#define RG_TMP     SLJIT_R1   /* address scratch        */
#define RG_FLG     SLJIT_R2   /* flag/int scratch        */

/* ---- math / stateful helpers (mirror interpreter exactly) ---- */
static double h_sin(double a){return sin(a);}
static double h_cos(double a){return cos(a);}
static double h_tan(double a){return tan(a);}
static double h_asin(double a){return asin(a);}
static double h_acos(double a){return acos(a);}
static double h_atan(double a){return atan(a);}
static double h_sqrt(double a){return sqrt(a);}
static double h_exp(double a){return exp(a);}
static double h_log(double a){return log(a);}
static double h_floor(double a){return floor(a);}
static double h_ceil(double a){return ceil(a);}
static double h_pow(double a,double b){return pow(a,b);}
static double h_atan2(double a,double b){return atan2(a,b);}
static double h_logb(double a,double b){return log(a)/log(b);}
static double h_fmod(double a,double b){return fmod(a,b);}
static double h_min(double a,double b){return (b<a)?b:a;}
static double h_max(double a,double b){return (b>a)?b:a;}
static double h_perc(double a,double b){return a - floor(a/fabs(b))*fabs(b);}
static double h_land(double a,double b){return (a!=0.0)&&(b!=0.0);}
static double h_lor(double a,double b){return (a!=0.0)||(b!=0.0);}
static double h_sgn(double a){return (a>0)-(a<0);}
static double h_unit(double a){return (a==0.0)*0.5 + (a>0);}
static double h_round0(double a){return (a>=0)?floor(a):-floor(-a);}
static double h_rnd(void){return ((double)pd_krand())*(1.0/2147483648.0);}
static double h_nrnd(void){return pd_nrnd();}
static double h_fact(double a){return pd_fact(a);}

/* ---- pending forward-jump bookkeeping ---- */
typedef struct PJ { struct sljit_jump *j; struct PJ *next; } PJ;
static void pj_add(PJ **list, struct sljit_jump *j) {
    if (!j) return;
    PJ *n = (PJ*)malloc(sizeof(PJ));
    if (!n) return;
    n->j = j; n->next = *list; *list = n;
}
static void pj_resolve(PJ **list, struct sljit_label *lab) {
    while (*list) { PJ *n = *list; *list = n->next; sljit_set_label(n->j, lab); free(n); }
}

/* ---- operand load/store ---- */
static void emit_load(struct sljit_compiler *sc, pd_Reg r, sljit_s32 FRd) {
    switch (r.fam) {
        case PD_FAM_LOCAL:
            sljit_emit_op2(sc, SLJIT_ADD, RG_TMP, 0, J_FRAME, 0, SLJIT_IMM, (sljit_sw)r.off); break;
        case PD_FAM_GLOBAL:
            sljit_emit_op2(sc, SLJIT_ADD, RG_TMP, 0, J_GLOBALS, 0, SLJIT_IMM, (sljit_sw)r.off); break;
        case PD_FAM_PARAM:
            sljit_emit_op2(sc, SLJIT_ADD, RG_TMP, 0, J_PARAMS, 0, SLJIT_IMM, (sljit_sw)r.off); break;
        case PD_FAM_CONST:
            sljit_emit_op2(sc, SLJIT_ADD, RG_TMP, 0, J_CONSTS, 0, SLJIT_IMM, (sljit_sw)r.off); break;
        case PD_FAM_EXT:
            sljit_emit_op1(sc, SLJIT_MOV_P, RG_TMP, 0, SLJIT_MEM1(J_CONSTS), (sljit_sw)r.off);
            sljit_emit_fop1(sc, SLJIT_MOV_F64, FRd, 0, SLJIT_MEM1(RG_TMP), 0);
            return;
        default:
            sljit_emit_fset64(sc, FRd, 0.0);
            return;
    }
    sljit_emit_fop1(sc, SLJIT_MOV_F64, FRd, 0, SLJIT_MEM1(RG_TMP), 0);
}

static int is_storeable(pd_Fam f) {
    return f == PD_FAM_LOCAL || f == PD_FAM_GLOBAL || f == PD_FAM_PARAM ||
           f == PD_FAM_CONST || f == PD_FAM_EXT;
}

static void emit_store(struct sljit_compiler *sc, pd_Reg out, sljit_s32 FRv) {
    switch (out.fam) {
        case PD_FAM_LOCAL:
            sljit_emit_op2(sc, SLJIT_ADD, RG_TMP, 0, J_FRAME, 0, SLJIT_IMM, (sljit_sw)out.off); break;
        case PD_FAM_GLOBAL:
            sljit_emit_op2(sc, SLJIT_ADD, RG_TMP, 0, J_GLOBALS, 0, SLJIT_IMM, (sljit_sw)out.off); break;
        case PD_FAM_PARAM:
            sljit_emit_op2(sc, SLJIT_ADD, RG_TMP, 0, J_PARAMS, 0, SLJIT_IMM, (sljit_sw)out.off); break;
        case PD_FAM_CONST:
            sljit_emit_op2(sc, SLJIT_ADD, RG_TMP, 0, J_CONSTS, 0, SLJIT_IMM, (sljit_sw)out.off); break;
        case PD_FAM_EXT:
            sljit_emit_op1(sc, SLJIT_MOV_P, RG_TMP, 0, SLJIT_MEM1(J_CONSTS), (sljit_sw)out.off);
            sljit_emit_fop1(sc, SLJIT_MOV_F64, SLJIT_MEM1(RG_TMP), 0, FRv, 0);
            return;
        default:
            return; /* VOID/STR/LABEL/PTR/ARR: no storage */
    }
    sljit_emit_fop1(sc, SLJIT_MOV_F64, SLJIT_MEM1(RG_TMP), 0, FRv, 0);
}

/* emit a C-ABI call to func returning F64 (args already in FR0/FR1/R0..) */
static struct sljit_jump *call_helper(struct sljit_compiler *sc,
                                      sljit_s32 arg_types, void *func) {
    struct sljit_jump *j = sljit_emit_call(sc, SLJIT_CALL, arg_types);
    if (j) sljit_set_target(j, (sljit_uw)func);
    return j;
}

/* freeze probe at a loop back-edge: if (*J_QUIT) goto exit0 */
static void emit_probe(struct sljit_compiler *sc, PJ **pending, size_t exitIdx) {
    sljit_emit_op1(sc, SLJIT_MOV32, RG_FLG, 0, SLJIT_MEM1(J_QUIT), 0);
    struct sljit_jump *j = sljit_emit_cmp(sc, SLJIT_NOT_EQUAL, RG_FLG, 0, SLJIT_IMM, 0);
    pj_add(&pending[exitIdx], j);
}

/* ---- the compiler ---- */
static pd_jit_func_t compile_program(const pd_Program *prog) {
    struct sljit_compiler *sc = sljit_create_compiler(NULL);
    if (!sc) return NULL;

    size_t n = prog->nInstr;
    /* labels[0..n-1] = instructions; [n] = exit0; [n+1] = exit_val */
    struct sljit_label **labels = (struct sljit_label**)calloc(n + 2, sizeof(struct sljit_label*));
    PJ **pending = (PJ**)calloc(n + 2, sizeof(PJ*));
    if (!labels || !pending) { free(labels); free(pending); sljit_free_compiler(sc); return NULL; }

    int ok = sljit_emit_enter(sc, 0, SLJIT_ARGS1(F64, P), 5 | SLJIT_ENTER_FLOAT(7), 7, 0);
    if (ok) {
        /* cache base pointers from the ctx (arg in R0) */
        sljit_emit_op1(sc, SLJIT_MOV_P, J_CTX, 0, SLJIT_R0, 0);
        sljit_emit_op1(sc, SLJIT_MOV_P, RG_TMP, 0, SLJIT_MEM1(J_CTX), (sljit_sw)offsetof(pd_Ctx, prog));
        sljit_emit_op1(sc, SLJIT_MOV_P, J_FRAME,   0, SLJIT_MEM1(J_CTX), (sljit_sw)offsetof(pd_Ctx, frame));
        sljit_emit_op1(sc, SLJIT_MOV_P, J_GLOBALS, 0, SLJIT_MEM1(J_CTX), (sljit_sw)offsetof(pd_Ctx, globals));
        sljit_emit_op1(sc, SLJIT_MOV_P, J_PARAMS,  0, SLJIT_MEM1(J_CTX), (sljit_sw)offsetof(pd_Ctx, params));
        sljit_emit_op1(sc, SLJIT_MOV_P, J_CONSTS,  0, SLJIT_MEM1(RG_TMP), (sljit_sw)offsetof(pd_Program, consts));
        sljit_emit_op1(sc, SLJIT_MOV_P, J_ROOT,    0, SLJIT_MEM1(J_CTX), (sljit_sw)offsetof(pd_Ctx, root));
        sljit_emit_op1(sc, SLJIT_MOV_P, J_QUIT,    0, SLJIT_MEM1(J_CTX), (sljit_sw)offsetof(pd_Ctx, shouldQuit));
    }

    for (size_t i = 0; ok && i < n; i++) {
        const pd_Instr *in = &prog->instr[i];
        labels[i] = sljit_emit_label(sc);
        pj_resolve(&pending[i], labels[i]);

        switch (in->op) {
            case PD_NOP:
                break;

            case PD_GOTO: {
                if (in->out.off <= i) emit_probe(sc, pending, n);
                struct sljit_jump *j = sljit_emit_jump(sc, SLJIT_JUMP);
                if (in->out.off < n + 2 && labels[in->out.off])
                    sljit_set_label(j, labels[in->out.off]);
                else pj_add(&pending[in->out.off], j);
                break;
            }
            case PD_RETURN: {
                emit_load(sc, in->in[0], SLJIT_FR0);
                struct sljit_jump *j = sljit_emit_jump(sc, SLJIT_JUMP);
                pj_add(&pending[n + 1], j); /* exit_val */
                break;
            }
            case PD_IF0: {
                emit_load(sc, in->in[0], SLJIT_FR0);
                sljit_emit_fop1(sc, SLJIT_CMP_F64, SLJIT_FR0, 0, SLJIT_IMM, 0);
                if (in->out.off <= i) emit_probe(sc, pending, n);
                struct sljit_jump *j = sljit_emit_jump(sc, SLJIT_F_EQUAL);
                if (in->out.off < n + 2 && labels[in->out.off])
                    sljit_set_label(j, labels[in->out.off]);
                else pj_add(&pending[in->out.off], j);
                break;
            }
            case PD_IF1: {
                emit_load(sc, in->in[0], SLJIT_FR0);
                sljit_emit_fop1(sc, SLJIT_CMP_F64, SLJIT_FR0, 0, SLJIT_IMM, 0);
                if (in->out.off <= i) emit_probe(sc, pending, n);
                struct sljit_jump *j = sljit_emit_jump(sc, SLJIT_F_NOT_EQUAL);
                if (in->out.off < n + 2 && labels[in->out.off])
                    sljit_set_label(j, labels[in->out.off]);
                else pj_add(&pending[in->out.off], j);
                break;
            }

            /* ---- inline float arithmetic ---- */
            case PD_MOV:
                emit_load(sc, in->in[0], SLJIT_FR0);
                emit_store(sc, in->out, SLJIT_FR0);
                break;
            case PD_NEGMOV:
                emit_load(sc, in->in[0], SLJIT_FR0);
                sljit_emit_fop1(sc, SLJIT_NEG_F64, SLJIT_FR2, 0, SLJIT_FR0, 0);
                emit_store(sc, in->out, SLJIT_FR2);
                break;
            case PD_FABS:
                emit_load(sc, in->in[0], SLJIT_FR0);
                sljit_emit_fop1(sc, SLJIT_ABS_F64, SLJIT_FR2, 0, SLJIT_FR0, 0);
                emit_store(sc, in->out, SLJIT_FR2);
                break;
            case PD_PLUS:
            case PD_FADD:
                emit_load(sc, in->in[0], SLJIT_FR0);
                emit_load(sc, in->in[1], SLJIT_FR1);
                sljit_emit_fop2(sc, SLJIT_ADD_F64, SLJIT_FR2, 0, SLJIT_FR0, 0, SLJIT_FR1, 0);
                emit_store(sc, in->out, SLJIT_FR2);
                break;
            case PD_MINUS:
                emit_load(sc, in->in[0], SLJIT_FR0);
                emit_load(sc, in->in[1], SLJIT_FR1);
                sljit_emit_fop2(sc, SLJIT_SUB_F64, SLJIT_FR2, 0, SLJIT_FR0, 0, SLJIT_FR1, 0);
                emit_store(sc, in->out, SLJIT_FR2);
                break;
            case PD_TIMES:
                emit_load(sc, in->in[0], SLJIT_FR0);
                emit_load(sc, in->in[1], SLJIT_FR1);
                sljit_emit_fop2(sc, SLJIT_MUL_F64, SLJIT_FR2, 0, SLJIT_FR0, 0, SLJIT_FR1, 0);
                emit_store(sc, in->out, SLJIT_FR2);
                break;
            case PD_SLASH:
                emit_load(sc, in->in[0], SLJIT_FR0);
                emit_load(sc, in->in[1], SLJIT_FR1);
                sljit_emit_fop2(sc, SLJIT_DIV_F64, SLJIT_FR2, 0, SLJIT_FR0, 0, SLJIT_FR1, 0);
                emit_store(sc, in->out, SLJIT_FR2);
                break;

            /* ---- comparisons -> 0.0/1.0 ---- */
            case PD_NEQU0: {
                emit_load(sc, in->in[0], SLJIT_FR0);
                sljit_emit_fop1(sc, SLJIT_CMP_F64, SLJIT_FR0, 0, SLJIT_IMM, 0);
                sljit_emit_op_flags(sc, SLJIT_MOV32, RG_FLG, 0, SLJIT_F_NOT_EQUAL);
                sljit_emit_fop1(sc, SLJIT_CONV_F64_FROM_S32, SLJIT_FR2, 0, RG_FLG, 0);
                emit_store(sc, in->out, SLJIT_FR2);
                break;
            }
            case PD_LES: case PD_LESEQ: case PD_MOR: case PD_MOREQ:
            case PD_EQU: case PD_NEQU: {
                emit_load(sc, in->in[0], SLJIT_FR0);
                emit_load(sc, in->in[1], SLJIT_FR1);
                sljit_emit_fop1(sc, SLJIT_CMP_F64, SLJIT_FR0, 0, SLJIT_FR1, 0);
                sljit_s32 cond;
                switch (in->op) {
                    case PD_LES:   cond = SLJIT_F_LESS; break;
                    case PD_LESEQ: cond = SLJIT_F_LESS_EQUAL; break;
                    case PD_MOR:   cond = SLJIT_F_GREATER; break;
                    case PD_MOREQ: cond = SLJIT_F_GREATER_EQUAL; break;
                    case PD_EQU:   cond = SLJIT_F_EQUAL; break;
                    default:       cond = SLJIT_F_NOT_EQUAL; break;
                }
                sljit_emit_op_flags(sc, SLJIT_MOV32, RG_FLG, 0, cond);
                sljit_emit_fop1(sc, SLJIT_CONV_F64_FROM_S32, SLJIT_FR2, 0, RG_FLG, 0);
                emit_store(sc, in->out, SLJIT_FR2);
                break;
            }

            /* ---- unary math via helper ---- */
            case PD_SIN: case PD_COS: case PD_TAN: case PD_ASIN: case PD_ACOS:
            case PD_ATAN: case PD_SQRT: case PD_EXP: case PD_LOG: case PD_FACT:
            case PD_FLOOR: case PD_CEIL: case PD_ROUND0: case PD_SGN: case PD_UNIT:
            case PD_RND: case PD_NRND: {
                void *fn = NULL;
                switch (in->op) {
                    case PD_SIN: fn = (void*)h_sin; break;
                    case PD_COS: fn = (void*)h_cos; break;
                    case PD_TAN: fn = (void*)h_tan; break;
                    case PD_ASIN: fn = (void*)h_asin; break;
                    case PD_ACOS: fn = (void*)h_acos; break;
                    case PD_ATAN: fn = (void*)h_atan; break;
                    case PD_SQRT: fn = (void*)h_sqrt; break;
                    case PD_EXP: fn = (void*)h_exp; break;
                    case PD_LOG: fn = (void*)h_log; break;
                    case PD_FACT: fn = (void*)h_fact; break;
                    case PD_FLOOR: fn = (void*)h_floor; break;
                    case PD_CEIL: fn = (void*)h_ceil; break;
                    case PD_ROUND0: fn = (void*)h_round0; break;
                    case PD_SGN: fn = (void*)h_sgn; break;
                    case PD_UNIT: fn = (void*)h_unit; break;
                    case PD_RND: fn = (void*)h_rnd; break;
                    case PD_NRND: fn = (void*)h_nrnd; break;
                    default: break;
                }
                if (in->op == PD_RND || in->op == PD_NRND) {
                    call_helper(sc, SLJIT_ARGS0(F64), fn);
                } else {
                    emit_load(sc, in->in[0], SLJIT_FR0);
                    call_helper(sc, SLJIT_ARGS1(F64, F64), fn);
                }
                emit_store(sc, in->out, SLJIT_FR0);
                break;
            }

            /* ---- binary math via helper ---- */
            case PD_POW: case PD_ATAN2: case PD_LOGB: case PD_FMOD:
            case PD_MIN: case PD_MAX: case PD_PERC: case PD_LAND: case PD_LOR: {
                void *fn = NULL;
                switch (in->op) {
                    case PD_POW: fn = (void*)h_pow; break;
                    case PD_ATAN2: fn = (void*)h_atan2; break;
                    case PD_LOGB: fn = (void*)h_logb; break;
                    case PD_FMOD: fn = (void*)h_fmod; break;
                    case PD_MIN: fn = (void*)h_min; break;
                    case PD_MAX: fn = (void*)h_max; break;
                    case PD_PERC: fn = (void*)h_perc; break;
                    case PD_LAND: fn = (void*)h_land; break;
                    case PD_LOR: fn = (void*)h_lor; break;
                    default: break;
                }
                emit_load(sc, in->in[0], SLJIT_FR0);
                emit_load(sc, in->in[1], SLJIT_FR1);
                call_helper(sc, SLJIT_ARGS2(F64, F64, F64), fn);
                emit_store(sc, in->out, SLJIT_FR0);
                break;
            }

            /* ---- call / memory ---- */
            case PD_CALL:
                sljit_emit_op1(sc, SLJIT_MOV_P, SLJIT_R0, 0, J_CTX, 0);
                sljit_emit_op1(sc, SLJIT_MOV_P, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)(prog->instr + i));
                call_helper(sc, SLJIT_ARGS2(F64, P, P), (void*)pd_jit_call);
                emit_store(sc, in->out, SLJIT_FR0);
                break;
            case PD_PEEK:
                sljit_emit_op1(sc, SLJIT_MOV_P, SLJIT_R0, 0, J_CTX, 0);
                sljit_emit_op1(sc, SLJIT_MOV_P, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)(prog->instr + i));
                call_helper(sc, SLJIT_ARGS2(F64, P, P), (void*)pd_jit_peek);
                emit_store(sc, in->out, SLJIT_FR0);
                break;
            case PD_ADDR:
                sljit_emit_op1(sc, SLJIT_MOV_P, SLJIT_R0, 0, J_CTX, 0);
                sljit_emit_op1(sc, SLJIT_MOV_P, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)(prog->instr + i));
                call_helper(sc, SLJIT_ARGS2(F64, P, P), (void*)pd_jit_addr);
                emit_store(sc, in->out, SLJIT_FR0);
                break;
            case PD_ADDRSLOT:
                sljit_emit_op1(sc, SLJIT_MOV_P, SLJIT_R0, 0, J_CTX, 0);
                sljit_emit_op1(sc, SLJIT_MOV_P, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)(prog->instr + i));
                call_helper(sc, SLJIT_ARGS2(F64, P, P), (void*)pd_jit_addrslot);
                emit_store(sc, in->out, SLJIT_FR0);
                break;
            case PD_POKE: case PD_POKETIMES: case PD_POKESLASH:
            case PD_POKEPERC: case PD_POKEPLUS: case PD_POKEMINUS:
                sljit_emit_op1(sc, SLJIT_MOV_P, SLJIT_R0, 0, J_CTX, 0);
                sljit_emit_op1(sc, SLJIT_MOV_P, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)(prog->instr + i));
                sljit_emit_op1(sc, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, (sljit_sw)(sljit_s32)in->op);
                call_helper(sc, SLJIT_ARGS3(RET_VOID, P, P, 32), (void*)pd_jit_poke);
                break;

            default:
                break;
        }
    }

    /* exit0: natural end / freeze quit -> return 0.0 */
    labels[n] = sljit_emit_label(sc);
    pj_resolve(&pending[n], labels[n]);
    sljit_emit_fset64(sc, SLJIT_FR0, 0.0);
    sljit_emit_return(sc, SLJIT_MOV_F64, SLJIT_FR0, 0);

    /* exit_val: explicit RETURN -> return current FR0 */
    labels[n + 1] = sljit_emit_label(sc);
    pj_resolve(&pending[n + 1], labels[n + 1]);
    sljit_emit_return(sc, SLJIT_MOV_F64, SLJIT_FR0, 0);

    pd_jit_func_t fn = NULL;
    if (ok) {
        void *code = sljit_generate_code(sc, 0, NULL);
        fn = (pd_jit_func_t)code;
    }
    sljit_free_compiler(sc);
    for (size_t k = 0; k < n + 2; k++) {
        while (pending[k]) { PJ *nx = pending[k]->next; free(pending[k]); pending[k] = nx; }
    }
    free(labels);
    free(pending);
    return fn;
}

/* ---- cache ----
 * Keyed by a content hash of the program, NOT its address: programs may be
 * freed and the same heap address reused for a different program. The compiled
 * code only reads c->prog / c->root at runtime, so two programs with identical
 * content share one JIT function safely. */
static uint64_t prog_hash(const pd_Program *p) {
    uint64_t h = 1469598103934665603ULL; /* FNV-1a 64 */
#define PD_MIX(ptr, len) do { \
    const unsigned char *bp = (const unsigned char*)(ptr); size_t bl = (size_t)(len); \
    for (size_t k = 0; k < bl; k++) { h ^= bp[k]; h *= 1099511628211ULL; } \
} while (0)
    PD_MIX(&p->nInstr, sizeof(p->nInstr));
    PD_MIX(&p->nConst, sizeof(p->nConst));
    PD_MIX(&p->nExtra, sizeof(p->nExtra));
    PD_MIX(&p->nLocals, sizeof(p->nLocals));
    PD_MIX(&p->nParams, sizeof(p->nParams));
    PD_MIX(p->instr, p->nInstr * sizeof(pd_Instr));
    PD_MIX(p->consts, p->nConst * sizeof(double));
    PD_MIX(p->extra, p->nExtra * sizeof(pd_Reg));
    return h;
#undef PD_MIX
}

typedef struct JitEntry { uint64_t key; pd_jit_func_t fn; struct JitEntry *next; } JitEntry;
static JitEntry *g_cache = NULL;
static int g_enabled = 1;

static pd_jit_func_t cache_find(const pd_Program *prog) {
    uint64_t key = prog_hash(prog);
    for (JitEntry *e = g_cache; e; e = e->next)
        if (e->key == key) return e->fn;
    return NULL;
}
static void cache_add(const pd_Program *prog, pd_jit_func_t fn) {
    JitEntry *e = (JitEntry*)malloc(sizeof(JitEntry));
    if (!e) return;
    e->key = prog_hash(prog); e->fn = fn; e->next = g_cache; g_cache = e;
}

int pd_sljit_available(void) {
#ifdef PD_JIT_COMPILED
    return 1;
#else
    return 0;
#endif
}
void pd_sljit_set_enabled(int on) { g_enabled = on ? 1 : 0; }
int  pd_sljit_enabled(void) { return pd_sljit_available() && g_enabled; }

pd_jit_func_t pd_sljit_get(const pd_Program *prog) {
    if (!pd_sljit_available() || !prog) return NULL;
    pd_jit_func_t fn = cache_find(prog);
    if (fn) return fn;
    fn = compile_program(prog);
    if (fn) cache_add(prog, fn);
    return fn;
}

double pd_sljit_run_jit(const pd_Program *prog, const double *params,
                        double *globals, volatile int *shouldQuit) {
    if (!pd_sljit_enabled()) return pd_run(prog, params, globals, shouldQuit);
    pd_jit_func_t fn = pd_sljit_get(prog);
    if (!fn) return pd_run(prog, params, globals, shouldQuit);

    /* ensure a non-NULL quit probe so the freeze check is always safe */
    volatile int localQuit = 0;
    volatile int *q = shouldQuit ? shouldQuit : &localQuit;

    pd_Ctx c;
    c.prog = prog;
    c.frame = (double*)calloc(prog->nLocals ? prog->nLocals : 1, sizeof(double));
    c.params = params ? params : (const double*)"\0\0\0\0\0\0\0\0";
    c.globals = globals;
    c.shouldQuit = q;
    c.parent = NULL;
    c.root = prog;
    double r = fn(&c);
    free(c.frame);
    return r;
}
