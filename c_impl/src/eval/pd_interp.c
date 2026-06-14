/* pd_interp.c — interpreter.
 *
 * Evaluates a pd_Program by walking instr[]. This mirrors original
 * kasm87c_run (eval.c:5579) but with our cleaner IR types.
 *
 * Array model: arrays are represented by a LOCAL slot holding a
 * pd_Array descriptor pointer (stored as raw bits in the double).
 * PEEK/POKE operands:
 *   out  = LOCAL holding array descriptor (the array variable)
 *   in[0]= value (for POKE family) ; for PEEK unused
 *   in[1]= index expression result
 *   aux  = array size (compile-time constant) for bounds check
 * The descriptor's first double slot is the base pointer.
 */
#include "pd_interp.h"
#include "pd_host.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* RNG — exact copy of original krand/nrnd (eval.c:497, 503) so that
 * srand()/RND/NRND produce identical sequences to the original. */
static unsigned long g_holdrand = 1;
static int g_normstat = 0;
static double g_srand2;

void pd_srand(unsigned long s) { g_holdrand = s; g_normstat = 0; }

static unsigned long pd_krand(void) {
    g_holdrand = (unsigned long)((g_holdrand * (214013u * 2u) + 2531011u * 2u) >> 1);
    return g_holdrand;
}

static double pd_nrnd(void) {
    double x, y, r;
    static const double oneover2_31 = 1.0 / 2147483648.0;
    if (g_normstat) { g_normstat = 0; return g_srand2; }
    do {
        x = ((double)(pd_krand() - 1073741824u)) * (oneover2_31 * 2.0);
        y = ((double)(pd_krand() - 1073741824u)) * (oneover2_31 * 2.0);
        r = x*x + y*y;
    } while (r >= 1);
    g_normstat = 1;
    r = sqrt(-2.0 * log(r) / r);
    g_srand2 = x * r;
    return y * r;
}

/* original fact() from eval.c:526 — gamma-based factorial */
static double pd_fact(double num) {
    static const long pinf = 0x7f800000;
    if ((num <= -0.99999999999999996) || (num >= 170.6243769562767))
        return *(float*)&pinf;
    num++;
    return pow(num + 5.5, num + 0.5) * exp(-5.5 - num) *
        ((((((num * 2.506628275107298 + 83.8676043423952) * num + 1168.926494792211) * num +
             8687.245297053594) * num + 36308.29514770109) * num +
           80916.62789524846) * num + 75122.63315304522) /
        (((((((num + 21) * num + 175) * num + 735) * num + 1624) * num + 1764) * num + 720) * num);
}

/* bounds check helper: returns adjusted index per eval.txt rules */
static long pd_bounds(long j, long size) {
    if (size == 0) return j;
    if (!((size - 1) & size)) {           /* power of 2 -> mask */
        return j & (size - 1);
    }
    if ((unsigned long)j >= (unsigned long)size) j = 0;
    return j;
}

/* Resolve an array base address.
 * GLOBAL arrays: the reg points directly at the storage (no indirection).
 * LOCAL arrays (passed as pointer params): the slot holds a bit-cast double*.
 */
static double *pd_array_base(pd_Ctx *c, pd_Reg r) {
    double *slot = pd_slot(c, r);
    if (!slot) return NULL;
    if (r.fam == PD_FAM_GLOBAL || r.fam == PD_FAM_CONST) {
        /* storage IS the array */
        return slot;
    }
    /* LOCAL: slot holds a bit-cast pointer */
    void *pp;
    memcpy(&pp, slot, sizeof(void*));
    return (double*)pp;
}

double pd_run_ctx(pd_Ctx *c) {
    const pd_Program *p = c->prog;
    size_t i = 0;
    c->quitCounter = 0;

    while (i < p->nInstr) {
        /* freeze protection: check every 4096 dynamic instructions */
        if (++c->quitCounter >= 4096) {
            c->quitCounter = 0;
            if (c->shouldQuit && *c->shouldQuit) return 0.0;
        }
        const pd_Instr *in = &p->instr[i];
        double *out = (in->op == PD_GOTO || in->op == PD_IF0 || in->op == PD_IF1)
                      ? NULL : pd_slot(c, in->out);
        double a = (in->nIn >= 1) ? *pd_slot(c, in->in[0]) : 0.0;
        double b = (in->nIn >= 2) ? *pd_slot(c, in->in[1]) : 0.0;
        switch (in->op) {
            case PD_NOP: break;
            case PD_GOTO: i = in->out.off; continue;
            case PD_RETURN: return a;
            case PD_RND:    *out = ((double)pd_krand()) * (1.0 / 2147483648.0); break;
            case PD_NRND:   *out = pd_nrnd(); break;
            case PD_MOV:    *out = a; break;
            case PD_NEGMOV: *out = -a; break;
            case PD_NEQU0:  *out = (a != 0.0); break;
            case PD_IF0: if (a == 0.0) { i = in->out.off; continue; } break;
            case PD_IF1: if (a != 0.0) { i = in->out.off; continue; } break;
            /* 1-input math */
            case PD_FABS:   *out = fabs(a); break;
            case PD_SGN:    *out = (a > 0) - (a < 0); break;
            case PD_UNIT:   *out = (a == 0.0) * 0.5 + (a > 0); break;
            case PD_FLOOR:  *out = floor(a); break;
            case PD_CEIL:   *out = ceil(a); break;
            case PD_ROUND0: *out = (a >= 0) ? floor(a) : -floor(-a); break;
            case PD_SIN:    *out = sin(a); break;
            case PD_COS:    *out = cos(a); break;
            case PD_TAN:    *out = tan(a); break;
            case PD_ASIN:   *out = asin(a); break;
            case PD_ACOS:   *out = acos(a); break;
            case PD_ATAN:   *out = atan(a); break;
            case PD_SQRT:   *out = sqrt(a); break;
            case PD_EXP:    *out = exp(a); break;
            case PD_FACT:   *out = pd_fact(a); break;
            case PD_LOG:    *out = log(a); break;
            /* 2-input */
            case PD_TIMES:  *out = a * b; break;
            case PD_SLASH:  *out = a / b; break;
            case PD_PERC:   *out = a - floor(a / fabs(b)) * fabs(b); break;
            case PD_PLUS:
            case PD_FADD:   *out = a + b; break;
            case PD_MINUS:  *out = a - b; break;
            case PD_POW:    *out = pow(a, b); break;
            case PD_MIN:    *out = (b < a) ? b : a; break;
            case PD_MAX:    *out = (b > a) ? b : a; break;
            case PD_FMOD:   *out = fmod(a, b); break;
            case PD_ATAN2:  *out = atan2(a, b); break;
            case PD_LOGB:   *out = log(a) / log(b); break;
            case PD_LES:    *out = (a <  b); break;
            case PD_LESEQ:  *out = (a <= b); break;
            case PD_MOR:    *out = (a >  b); break;
            case PD_MOREQ:  *out = (a >= b); break;
            case PD_EQU:    *out = (a == b); break;
            case PD_NEQU:   *out = (a != b); break;
            case PD_LAND:   *out = (a != 0.0) && (b != 0.0); break;
            case PD_LOR:    *out = (a != 0.0) || (b != 0.0); break;
            /* arrays. Convention:
             *   out    = result local (PEEK) / array-base local (POKE family)
             *   in[0]  = array-base local (PEEK) / value (POKE)
             *   in[1]  = index
             *   aux    = array element count (compile-time constant) for bounds check
             */
            case PD_PEEK: {
                double *base = pd_array_base(c, in->in[0]);
                long j = pd_bounds((long)b, in->aux);
                if (out && base) *out = base[j];
                break;
            }
            case PD_POKE: {
                double *base = pd_array_base(c, in->out);
                long j = pd_bounds((long)b, in->aux);
                if (base) base[j] = a;
                break;
            }
            case PD_POKETIMES: { double *base=pd_array_base(c,in->out); long j=pd_bounds((long)b,in->aux); base[j]*=a; break; }
            case PD_POKESLASH: { double *base=pd_array_base(c,in->out); long j=pd_bounds((long)b,in->aux); base[j]/=a; break; }
            case PD_POKEPERC:  { double *base=pd_array_base(c,in->out); long j=pd_bounds((long)b,in->aux); base[j]-=floor(base[j]/fabs(a))*fabs(a); break; }
            case PD_POKEPLUS:  { double *base=pd_array_base(c,in->out); long j=pd_bounds((long)b,in->aux); base[j]+=a; break; }
            case PD_POKEMINUS: { double *base=pd_array_base(c,in->out); long j=pd_bounds((long)b,in->aux); base[j]-=a; break; }
            case PD_CALL: {
                /* aux >= 0: user function (index into root->funcs[]).
                 * aux <= -1000: external host function (host idx = -1000 - aux).
                 * aux == -1: unresolved; return 0. */
                const pd_Program *root = c->root ? c->root : p;
                int na = in->nIn;
                double argbuf[16];
                if (na > 0) argbuf[0] = *pd_slot(c, in->in[0]);
                if (na > 1) argbuf[1] = *pd_slot(c, in->in[1]);
                for (int k = 2; k < na && k < 16; k++)
                    argbuf[k] = *pd_slot(c, root->extra[in->extraIdx + k - 2]);
                if (in->aux <= -1000) {
                    int hidx = -1000 - in->aux;
                    if (root->host && hidx >= 0 && hidx < root->host->nFns) {
                        double rv = root->host->fns[hidx].fn(na, argbuf);
                        if (out) *out = rv;
                    } else { if (out) *out = 0; }
                    break;
                }
                pd_Program *fn = (in->aux >= 0 && (size_t)in->aux < root->nFuncs)
                                 ? &root->funcs[in->aux] : NULL;
                if (!fn) { if (out) *out = 0; break; }
                pd_Ctx child;
                child.prog = fn;
                child.frame = calloc(fn->nLocals ? fn->nLocals : 1, sizeof(double));
                child.params = argbuf;
                child.globals = c->globals;
                child.shouldQuit = c->shouldQuit;
                child.parent = c;
                child.root = root;
                double r = pd_run_ctx(&child);
                free(child.frame);
                if (out) *out = r;
                break;
            }
            default: break;
        }
        i++;
    }
    return 0.0;
}

double pd_run(const pd_Program *prog, const double *params,
              double *globals, volatile int *shouldQuit) {
    pd_Ctx c;
    c.prog = prog;
    c.frame = calloc(prog->nLocals ? prog->nLocals : 1, sizeof(double));
    c.params = params ? params : (const double*)"\0\0\0\0\0\0\0\0";
    c.globals = globals;
    c.shouldQuit = shouldQuit;
    c.parent = NULL;
    c.root = prog;
    double r = pd_run_ctx(&c);
    free(c.frame);
    return r;
}
