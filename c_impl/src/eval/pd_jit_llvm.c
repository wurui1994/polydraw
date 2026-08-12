/* pd_jit_llvm.c — LLVM JIT backend for pd_Program (core target, M5).
 *
 * Compiles a program's entry function to LLVM IR and JITs it via ORC/LLJIT.
 * IEEE-754-exact arithmetic and comparisons are emitted as LLVM float ops
 * (no fast-math flags, so semantics stay strict); every op that depends on
 * libm semantics, RNG state, or host/user calls is routed to the *same* C
 * helper used by the interpreter (pd_interp.c), so the JIT is guaranteed
 * bit-identical to pd_run.
 *
 * Freeze protection: a volatile load of *shouldQuit is emitted before every
 * backward (loop) branch; when set, the program returns 0.0 immediately.
 *
 * Build/run: requires system LLVM (llvm-c headers + libLLVM). The Makefile
 * detects it via llvm-config / brew --prefix llvm and links the C API.
 */
#include "pd_jit.h"
#include "pd_interp.h"

#include <llvm-c/Core.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Orc.h>
#include <llvm-c/Target.h>
#include <llvm-c/Error.h>
#include <llvm-c/Transforms/PassBuilder.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#ifndef PD_HAVE_LLVM
#define PD_HAVE_LLVM 1
#endif

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
static double h_fabs(double a){return fabs(a);}
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

/* ---- runtime: one shared LLJIT instance ---- */
static LLVMOrcLLJITRef g_jit = NULL;
static unsigned long g_module_counter = 0;

static int llvm_init(void) {
    if (g_jit) return 1;
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();
    LLVMOrcLLJITBuilderRef b = LLVMOrcCreateLLJITBuilder();
    LLVMErrorRef err = LLVMOrcCreateLLJIT(&g_jit, b);
    if (err) {
        char *msg = LLVMGetErrorMessage(err);
        fprintf(stderr, "[pd_jit_llvm] LLJIT init failed: %s\n", msg ? msg : "?");
        if (msg) LLVMDisposeErrorMessage(msg);
        g_jit = NULL;
        return 0;
    }
    return 1;
}

/* ---- IR generation state ---- */
typedef struct {
    LLVMContextRef C;
    LLVMModuleRef  M;
    LLVMBuilderRef B;
    LLVMTypeRef P, D, I32, I64, I8;
    LLVMValueRef F;            /* entry function */
    LLVMValueRef prog, frame, globals, params, consts, quit, root;
    LLVMValueRef retval;       /* alloca for explicit RETURN value */
    LLVMBasicBlockRef *bbs;    /* n+2: [0..n-1] instr, n=exit0, n+1=exit_val */
    LLVMValueRef fabs;         /* (unused; fabs routed to h_fabs helper) */
} Gen;

/* ---- tiny helpers ---- */
static LLVMValueRef gep8(LLVMContextRef C, LLVMTypeRef I8, LLVMBuilderRef B,
                         LLVMValueRef base, size_t off) {
    LLVMValueRef idx = LLVMConstInt(LLVMInt64TypeInContext(C), (uint64_t)off, 0);
    LLVMValueRef geps[] = { idx };
    return LLVMBuildInBoundsGEP2(B, I8, base, geps, 1, "gep");
}

static LLVMValueRef helper_global(LLVMContextRef C, LLVMModuleRef M, const char *prefix,
                                  void *addr) {
    char name[96];
    snprintf(name, sizeof(name), "%s_%llx", prefix, (unsigned long long)(uintptr_t)addr);
    LLVMValueRef g = LLVMAddGlobal(M, LLVMPointerTypeInContext(C, 0), name);
    LLVMValueRef a = LLVMConstInt(LLVMInt64TypeInContext(C), (uint64_t)(uintptr_t)addr, 0);
    LLVMSetInitializer(g, LLVMConstIntToPtr(a, LLVMPointerTypeInContext(C, 0)));
    LLVMSetGlobalConstant(g, 1);
    LLVMSetLinkage(g, LLVMInternalLinkage);
    return g;
}

static LLVMValueRef build_call(Gen *g, LLVMTypeRef fnType, LLVMValueRef gfn,
                               LLVMValueRef *args, int n) {
    LLVMValueRef callee = LLVMBuildLoad2(g->B, g->P, gfn, "fn");
    return LLVMBuildCall2(g->B, fnType, callee, args, n, "");
}

/* pointer to a register's double storage (NULL if none) */
static LLVMValueRef reg_ptr(Gen *g, pd_Reg r) {
    switch (r.fam) {
        case PD_FAM_LOCAL:  return gep8(g->C, g->I8, g->B, g->frame, (size_t)r.off);
        case PD_FAM_GLOBAL: return gep8(g->C, g->I8, g->B, g->globals, (size_t)r.off);
        case PD_FAM_PARAM:  return gep8(g->C, g->I8, g->B, g->params, (size_t)r.off);
        case PD_FAM_CONST:  return gep8(g->C, g->I8, g->B, g->consts, (size_t)r.off);
        case PD_FAM_EXT: {
            LLVMValueRef slot = gep8(g->C, g->I8, g->B, g->consts, (size_t)r.off);
            return LLVMBuildLoad2(g->B, g->P, slot, "extp");
        }
        default: return NULL;
    }
}

static LLVMValueRef load_reg(Gen *g, pd_Reg r) {
    LLVMValueRef p = reg_ptr(g, r);
    if (!p) return LLVMConstReal(g->D, 0.0);
    return LLVMBuildLoad2(g->B, g->D, p, "v");
}

static void store_reg(Gen *g, pd_Reg r, LLVMValueRef v) {
    LLVMValueRef p = reg_ptr(g, r);
    if (p) LLVMBuildStore(g->B, v, p);
}

/* freeze probe: if *shouldQuit != 0 → exit0 */
static void emit_probe(Gen *g, LLVMBasicBlockRef exit0) {
    LLVMValueRef q = LLVMBuildLoad2(g->B, g->I32, g->quit, "quit");
    LLVMSetVolatile(q, 1);
    LLVMValueRef c = LLVMBuildICmp(g->B, LLVMIntNE, q, LLVMConstInt(g->I32, 0, 0), "q");
    LLVMBasicBlockRef cont = LLVMAppendBasicBlockInContext(g->C, g->F, "probe_cont");
    LLVMBuildCondBr(g->B, c, exit0, cont);
    LLVMPositionBuilderAtEnd(g->B, cont);
}

/* ---- the compiler ---- */
static pd_jit_func_t compile_program(const pd_Program *prog) {
    if (!llvm_init()) return NULL;

    size_t n = prog->nInstr;
    Gen g;
    memset(&g, 0, sizeof(g));

    LLVMContextRef ctx = LLVMContextCreate();
    LLVMModuleRef M = LLVMModuleCreateWithNameInContext("pd_jit", ctx);
    g.C = ctx;
    g.M = M;
    g.P = LLVMPointerTypeInContext(ctx, 0);
    g.D = LLVMDoubleTypeInContext(ctx);
    g.I32 = LLVMInt32TypeInContext(ctx);
    g.I64 = LLVMInt64TypeInContext(ctx);
    g.I8 = LLVMInt8TypeInContext(ctx);

    /* align module to the JIT host target */
    const char *triple = LLVMOrcLLJITGetTripleString(g_jit);
    LLVMSetTarget(M, triple);
    const char *dl = LLVMOrcLLJITGetDataLayoutStr(g_jit);
    if (dl && *dl) LLVMSetDataLayout(M, dl);

    /* entry: double (ptr) */
    char fname[64];
    snprintf(fname, sizeof(fname), "pd_jit_%lu", ++g_module_counter);
    LLVMTypeRef Parg = g.P;
    LLVMTypeRef F1 = LLVMFunctionType(g.D, &Parg, 1, 0);
    g.F = LLVMAddFunction(M, fname, F1);
    LLVMSetFunctionCallConv(g.F, LLVMCCallConv);

    g.B = LLVMCreateBuilderInContext(ctx);

    /* ----- entry block: load base pointers ----- */
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ctx, g.F, "entry");
    LLVMPositionBuilderAtEnd(g.B, entry);
    LLVMValueRef c = LLVMGetParam(g.F, 0);
    g.prog    = LLVMBuildLoad2(g.B, g.P, gep8(ctx, g.I8, g.B, c, offsetof(pd_Ctx, prog)), "prog");
    g.frame   = LLVMBuildLoad2(g.B, g.P, gep8(ctx, g.I8, g.B, c, offsetof(pd_Ctx, frame)), "frame");
    g.globals = LLVMBuildLoad2(g.B, g.P, gep8(ctx, g.I8, g.B, c, offsetof(pd_Ctx, globals)), "globals");
    g.params  = LLVMBuildLoad2(g.B, g.P, gep8(ctx, g.I8, g.B, c, offsetof(pd_Ctx, params)), "params");
    g.quit    = LLVMBuildLoad2(g.B, g.P, gep8(ctx, g.I8, g.B, c, offsetof(pd_Ctx, shouldQuit)), "quitp");
    g.root    = LLVMBuildLoad2(g.B, g.P, gep8(ctx, g.I8, g.B, c, offsetof(pd_Ctx, root)), "root");
    g.consts  = LLVMBuildLoad2(g.B, g.P, gep8(ctx, g.I8, g.B, g.prog, offsetof(pd_Program, consts)), "consts");

    g.retval = LLVMBuildAlloca(g.B, g.D, "retval");
    LLVMBuildStore(g.B, LLVMConstReal(g.D, 0.0), g.retval);

    /* ----- blocks for every instruction + exit0/exit_val ----- */
    g.bbs = (LLVMBasicBlockRef*)calloc(n + 2, sizeof(LLVMBasicBlockRef));
    if (!g.bbs) { LLVMDisposeBuilder(g.B); LLVMDisposeModule(M); LLVMContextDispose(ctx); return NULL; }
    for (size_t i = 0; i < n; i++) {
        char nm[32]; snprintf(nm, sizeof(nm), "i%zu", i);
        g.bbs[i] = LLVMAppendBasicBlockInContext(ctx, g.F, nm);
    }
    g.bbs[n] = LLVMAppendBasicBlockInContext(ctx, g.F, "exit0");
    g.bbs[n + 1] = LLVMAppendBasicBlockInContext(ctx, g.F, "exit_val");

    /* entry block must terminate: fall into the first instruction */
    LLVMBuildBr(g.B, g.bbs[0]);

    /* ----- instruction emission ----- */
    int ok = 1;
    for (size_t i = 0; ok && i < n; i++) {
        const pd_Instr *in = &prog->instr[i];
        LLVMPositionBuilderAtEnd(g.B, g.bbs[i]);

        int terminator = 1;
        switch (in->op) {
            case PD_NOP: terminator = 0; break;

            case PD_GOTO: {
                if ((size_t)in->out.off <= i) emit_probe(&g, g.bbs[n]);
                LLVMBasicBlockRef t = (in->out.off >= 0 && (size_t)in->out.off < n + 2)
                                      ? g.bbs[in->out.off] : g.bbs[n];
                LLVMBuildBr(g.B, t);
                break;
            }
            case PD_RETURN: {
                LLVMValueRef v = load_reg(&g, in->in[0]);
                LLVMBuildStore(g.B, v, g.retval);
                LLVMBuildBr(g.B, g.bbs[n + 1]);
                break;
            }
            case PD_IF0: {
                LLVMValueRef v = load_reg(&g, in->in[0]);
                LLVMValueRef cnd = LLVMBuildFCmp(g.B, LLVMRealOEQ, v, LLVMConstReal(g.D, 0.0), "if0");
                if ((size_t)in->out.off <= i) emit_probe(&g, g.bbs[n]);
                LLVMBasicBlockRef t = (in->out.off >= 0 && (size_t)in->out.off < n + 2)
                                      ? g.bbs[in->out.off] : g.bbs[n];
                LLVMBuildCondBr(g.B, cnd, t, g.bbs[i + 1]);
                break;
            }
            case PD_IF1: {
                LLVMValueRef v = load_reg(&g, in->in[0]);
                LLVMValueRef cnd = LLVMBuildFCmp(g.B, LLVMRealUNE, v, LLVMConstReal(g.D, 0.0), "if1");
                if ((size_t)in->out.off <= i) emit_probe(&g, g.bbs[n]);
                LLVMBasicBlockRef t = (in->out.off >= 0 && (size_t)in->out.off < n + 2)
                                      ? g.bbs[in->out.off] : g.bbs[n];
                LLVMBuildCondBr(g.B, cnd, t, g.bbs[i + 1]);
                break;
            }

            /* ---- inline float arithmetic ---- */
            case PD_MOV:
                store_reg(&g, in->out, load_reg(&g, in->in[0]));
                terminator = 0; break;
            case PD_NEGMOV:
                store_reg(&g, in->out, LLVMBuildFNeg(g.B, load_reg(&g, in->in[0]), "neg"));
                terminator = 0; break;
            case PD_FABS: {
                LLVMTypeRef F1d = LLVMFunctionType(g.D, &g.D, 1, 0);
                LLVMValueRef gf = helper_global(ctx, M, "h_fabs", (void*)h_fabs);
                LLVMValueRef a = load_reg(&g, in->in[0]);
                store_reg(&g, in->out, build_call(&g, F1d, gf, &a, 1));
                terminator = 0; break;
            }
            case PD_PLUS: case PD_FADD:
                store_reg(&g, in->out, LLVMBuildFAdd(g.B, load_reg(&g, in->in[0]),
                              load_reg(&g, in->in[1]), "add"));
                terminator = 0; break;
            case PD_MINUS:
                store_reg(&g, in->out, LLVMBuildFSub(g.B, load_reg(&g, in->in[0]),
                              load_reg(&g, in->in[1]), "sub"));
                terminator = 0; break;
            case PD_TIMES:
                store_reg(&g, in->out, LLVMBuildFMul(g.B, load_reg(&g, in->in[0]),
                              load_reg(&g, in->in[1]), "mul"));
                terminator = 0; break;
            case PD_SLASH:
                store_reg(&g, in->out, LLVMBuildFDiv(g.B, load_reg(&g, in->in[0]),
                              load_reg(&g, in->in[1]), "div"));
                terminator = 0; break;

            /* ---- comparisons -> 0.0/1.0 ---- */
            case PD_NEQU0: {
                LLVMValueRef v = load_reg(&g, in->in[0]);
                LLVMValueRef cnd = LLVMBuildFCmp(g.B, LLVMRealUNE, v, LLVMConstReal(g.D, 0.0), "c");
                LLVMValueRef r = LLVMBuildSelect(g.B, cnd, LLVMConstReal(g.D, 1.0), LLVMConstReal(g.D, 0.0), "ne0");
                store_reg(&g, in->out, r);
                terminator = 0; break;
            }
            case PD_LES: case PD_LESEQ: case PD_MOR: case PD_MOREQ:
            case PD_EQU: case PD_NEQU: {
                LLVMValueRef a = load_reg(&g, in->in[0]);
                LLVMValueRef b = load_reg(&g, in->in[1]);
                LLVMRealPredicate pr;
                switch (in->op) {
                    case PD_LES:   pr = LLVMRealOLT; break;
                    case PD_LESEQ: pr = LLVMRealOLE; break;
                    case PD_MOR:   pr = LLVMRealOGT; break;
                    case PD_MOREQ: pr = LLVMRealOGE; break;
                    case PD_EQU:   pr = LLVMRealOEQ; break;
                    default:       pr = LLVMRealUNE; break;
                }
                LLVMValueRef cnd = LLVMBuildFCmp(g.B, pr, a, b, "c");
                LLVMValueRef r = LLVMBuildSelect(g.B, cnd, LLVMConstReal(g.D, 1.0), LLVMConstReal(g.D, 0.0), "cmp");
                store_reg(&g, in->out, r);
                terminator = 0; break;
            }

            /* ---- unary math via helper ---- */
            case PD_SIN: case PD_COS: case PD_TAN: case PD_ASIN: case PD_ACOS:
            case PD_ATAN: case PD_SQRT: case PD_EXP: case PD_LOG: case PD_FACT:
            case PD_FLOOR: case PD_CEIL: case PD_ROUND0: case PD_SGN: case PD_UNIT:
            case PD_RND: case PD_NRND: {
                LLVMTypeRef F1d = LLVMFunctionType(g.D, &g.D, 1, 0);
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
                    LLVMTypeRef F0 = LLVMFunctionType(g.D, NULL, 0, 0);
                    LLVMValueRef gf = helper_global(ctx, M, "h_rng", fn);
                    store_reg(&g, in->out, build_call(&g, F0, gf, NULL, 0));
                } else {
                    LLVMValueRef gf = helper_global(ctx, M, "h_u", fn);
                    LLVMValueRef a = load_reg(&g, in->in[0]);
                    store_reg(&g, in->out, build_call(&g, F1d, gf, &a, 1));
                }
                terminator = 0; break;
            }

            /* ---- binary math via helper ---- */
            case PD_POW: case PD_ATAN2: case PD_LOGB: case PD_FMOD:
            case PD_MIN: case PD_MAX: case PD_PERC: case PD_LAND: case PD_LOR: {
                LLVMTypeRef D2[2] = { g.D, g.D };
                LLVMTypeRef F2d = LLVMFunctionType(g.D, D2, 2, 0);
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
                LLVMValueRef gf = helper_global(ctx, M, "h_b", fn);
                LLVMValueRef args[2];
                args[0] = load_reg(&g, in->in[0]);
                args[1] = load_reg(&g, in->in[1]);
                store_reg(&g, in->out, build_call(&g, F2d, gf, args, 2));
                terminator = 0; break;
            }

            /* ---- call / memory ---- */
            case PD_CALL:
            case PD_PEEK:
            case PD_ADDR:
            case PD_ADDRSLOT: {
                LLVMTypeRef P2[2] = { g.P, g.P };
                LLVMTypeRef F2p = LLVMFunctionType(g.D, P2, 2, 0);
                void *fn = in->op == PD_CALL ? (void*)pd_jit_call
                         : in->op == PD_PEEK ? (void*)pd_jit_peek
                         : in->op == PD_ADDR ? (void*)pd_jit_addr
                                             : (void*)pd_jit_addrslot;
                LLVMValueRef instrPtr = LLVMBuildLoad2(g.B, g.P,
                    gep8(ctx, g.I8, g.B, g.prog, offsetof(pd_Program, instr)), "instrp");
                LLVMValueRef cur = gep8(ctx, g.I8, g.B, instrPtr, i * sizeof(pd_Instr));
                LLVMValueRef args[2] = { LLVMGetParam(g.F, 0), cur };
                LLVMValueRef gf = helper_global(ctx, M, "h_jit", fn);
                store_reg(&g, in->out, build_call(&g, F2p, gf, args, 2));
                terminator = 0; break;
            }
            case PD_POKE: case PD_POKETIMES: case PD_POKESLASH:
            case PD_POKEPERC: case PD_POKEPLUS: case PD_POKEMINUS: {
                LLVMTypeRef P2[3] = { g.P, g.P, g.I32 };
                LLVMTypeRef F3p = LLVMFunctionType(LLVMVoidTypeInContext(ctx), P2, 3, 0);
                LLVMValueRef gf = helper_global(ctx, M, "h_poke", (void*)pd_jit_poke);
                LLVMValueRef instrPtr = LLVMBuildLoad2(g.B, g.P,
                    gep8(ctx, g.I8, g.B, g.prog, offsetof(pd_Program, instr)), "instrp");
                LLVMValueRef cur = gep8(ctx, g.I8, g.B, instrPtr, i * sizeof(pd_Instr));
                LLVMValueRef args[3] = { LLVMGetParam(g.F, 0), cur,
                                         LLVMConstInt(g.I32, (uint64_t)in->op, 0) };
                build_call(&g, F3p, gf, args, 3);
                terminator = 0; break;
            }

            default:
                terminator = 0; break;
        }

        if (terminator) continue;
        LLVMBasicBlockRef next = (i + 1 < n) ? g.bbs[i + 1] : g.bbs[n];
        LLVMBuildBr(g.B, next);
    }

    /* ----- exit0: natural end / freeze quit → 0.0 ----- */
    LLVMPositionBuilderAtEnd(g.B, g.bbs[n]);
    LLVMBuildRet(g.B, LLVMConstReal(g.D, 0.0));
    /* ----- exit_val: explicit RETURN ----- */
    LLVMPositionBuilderAtEnd(g.B, g.bbs[n + 1]);
    LLVMBuildRet(g.B, LLVMBuildLoad2(g.B, g.D, g.retval, "rv"));

    if (getenv("PD_LLVM_DUMP")) {
        char *ir = LLVMPrintModuleToString(M);
        if (ir) { fputs(ir, stderr); LLVMDisposeMessage(ir); }
    }

    /* optimize the module (strict IEEE: no fast-math flags) */
    if (getenv("PD_LLVM_NOOPT") == NULL) {
        LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();
        LLVMErrorRef perr = LLVMRunPasses(M, "default<O2>", NULL, opts);
        if (perr) {
            char *msg = LLVMGetErrorMessage(perr);
            fprintf(stderr, "[pd_jit_llvm] pass run failed: %s\n", msg ? msg : "?");
            if (msg) LLVMDisposeErrorMessage(msg);
            ok = 0;
        }
        LLVMDisposePassBuilderOptions(opts);
    }

    pd_jit_func_t fn = NULL;
    int transferred = 0;
    if (ok) {
        /* hand the module to the JIT (TSM takes ownership of module+context) */
        LLVMOrcThreadSafeContextRef tsc =
            LLVMOrcCreateNewThreadSafeContextFromLLVMContext(ctx);
        LLVMOrcThreadSafeModuleRef tsm = LLVMOrcCreateNewThreadSafeModule(M, tsc);
        if (tsm) {
            transferred = 1;
            LLVMOrcJITDylibRef jd = LLVMOrcLLJITGetMainJITDylib(g_jit);
            LLVMErrorRef jerr = LLVMOrcLLJITAddLLVMIRModule(g_jit, jd, tsm);
            if (jerr) {
                char *msg = LLVMGetErrorMessage(jerr);
                fprintf(stderr, "[pd_jit_llvm] add module failed: %s\n", msg ? msg : "?");
                if (msg) LLVMDisposeErrorMessage(msg);
                ok = 0;
            } else {
                LLVMOrcExecutorAddress addr = 0;
                LLVMErrorRef lerr = LLVMOrcLLJITLookup(g_jit, &addr, fname);
                if (lerr) {
                    char *msg = LLVMGetErrorMessage(lerr);
                    fprintf(stderr, "[pd_jit_llvm] lookup %s failed: %s\n", fname, msg ? msg : "?");
                    if (msg) LLVMDisposeErrorMessage(msg);
                    ok = 0;
                } else {
                    fn = (pd_jit_func_t)(uintptr_t)addr;
                }
            }
        } else {
            ok = 0;
        }
    }

    LLVMDisposeBuilder(g.B);
    if (!ok && !transferred) { LLVMDisposeModule(M); LLVMContextDispose(ctx); } /* reclaimed on failure */
    free(g.bbs);
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

int pd_llvm_available(void) {
#ifdef PD_HAVE_LLVM
    return 1;
#else
    return 0;
#endif
}
void pd_llvm_set_enabled(int on) { g_enabled = on ? 1 : 0; }
int  pd_llvm_enabled(void) { return pd_llvm_available() && g_enabled; }

pd_jit_func_t pd_llvm_get(const pd_Program *prog) {
    if (!pd_llvm_available() || !prog) return NULL;
    pd_jit_func_t fn = cache_find(prog);
    if (fn) return fn;
    fn = compile_program(prog);
    if (fn) cache_add(prog, fn);
    return fn;
}

double pd_llvm_run_jit(const pd_Program *prog, const double *params,
                       double *globals, volatile int *shouldQuit) {
    if (!pd_llvm_enabled()) return pd_run(prog, params, globals, shouldQuit);
    pd_jit_func_t fn = pd_llvm_get(prog);
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
