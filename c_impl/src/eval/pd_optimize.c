/* pd_optimize.c — lightweight IR optimization pass.
 *
 * Runs once at link time (after parsing, before execution). Reduces the
 * interpreter's per-pixel dispatch cost by eliminating the redundant MOV
 * chains and constant re-computations that the naive codegen emits.
 *
 * Three cooperative passes, iterated to a fixpoint:
 *   1. Constant folding  — pure ops whose inputs are all CONST get evaluated
 *      at compile time and replaced by a CONST.
 *   2. Copy propagation  — `MOV d, s` where s is a leaf (CONST/LOCAL/GLOBAL/
 *      PARAM/EXT) and d is never re-written afterwards lets every read of d
 *      use s instead; the MOV is then dead and removed.
 *   3. Dead-code elim    — an instruction with no side effects whose output
 *      is never read by a later instruction is deleted.
 *
 * Control flow (GOTO/IF targets) and side-effecting ops (PEEK/POKE/ADDR/
 * ADDRSLOT/CALL/RND/NRND) are left untouched.
 */
#include "pd_ir.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

/* Pure, side-effect-free arithmetic ops that can be constant-folded and
 * safely copy-propagated through. */
static int is_pure(pd_Op op) {
    switch (op) {
        case PD_NOP:
        case PD_MOV: case PD_NEGMOV: case PD_NEQU0:
        case PD_FABS: case PD_SGN: case PD_UNIT: case PD_FLOOR: case PD_CEIL:
        case PD_ROUND0: case PD_SIN: case PD_COS: case PD_TAN: case PD_ASIN:
        case PD_ACOS: case PD_ATAN: case PD_SQRT: case PD_EXP: case PD_FACT:
        case PD_LOG:
        case PD_TIMES: case PD_SLASH: case PD_PERC: case PD_PLUS: case PD_MINUS:
        case PD_POW: case PD_MIN: case PD_MAX: case PD_FMOD: case PD_ATAN2:
        case PD_LOGB:
        case PD_LES: case PD_LESEQ: case PD_MOR: case PD_MOREQ: case PD_EQU:
        case PD_NEQU: case PD_LAND: case PD_LOR:
            return 1;
        default:
            return 0; /* GOTO/IF/RETURN/PEEK/POKE/ADDR/ADDRSLOT/CALL/RND/NRND */
    }
}

/* Is this operand a "leaf" we may substitute by value without breaking
 * aliasing (arrays/pointers must not be copied through). */
static int is_leaf(pd_Reg r) {
    return r.fam == PD_FAM_CONST || r.fam == PD_FAM_LOCAL ||
           r.fam == PD_FAM_GLOBAL || r.fam == PD_FAM_PARAM ||
           r.fam == PD_FAM_EXT;
}

static int is_const(pd_Reg r) { return r.fam == PD_FAM_CONST; }

/* Evaluate a pure op on constant inputs. Writes result to *out, returns 1. */
static int fold(pd_Op op, double a, double b, double *out) {
    switch (op) {
        case PD_MOV: *out = a; return 1;
        case PD_NEGMOV: *out = -a; return 1;
        case PD_NEQU0: *out = (a != 0.0); return 1;
        case PD_FABS: *out = fabs(a); return 1;
        case PD_SGN: *out = (a > 0) - (a < 0); return 1;
        case PD_UNIT: *out = (a == 0.0) * 0.5 + (a > 0); return 1;
        case PD_FLOOR: *out = floor(a); return 1;
        case PD_CEIL: *out = ceil(a); return 1;
        case PD_ROUND0: *out = (a >= 0) ? floor(a) : -floor(-a); return 1;
        case PD_SIN: *out = sin(a); return 1;
        case PD_COS: *out = cos(a); return 1;
        case PD_TAN: *out = tan(a); return 1;
        case PD_ASIN: *out = asin(a); return 1;
        case PD_ACOS: *out = acos(a); return 1;
        case PD_ATAN: *out = atan(a); return 1;
        case PD_SQRT: *out = sqrt(a); return 1;
        case PD_EXP: *out = exp(a); return 1;
        case PD_LOG: *out = log(a); return 1;
        case PD_TIMES: *out = a * b; return 1;
        case PD_SLASH: *out = a / b; return 1;
        case PD_PERC: *out = a - floor(a / fabs(b)) * fabs(b); return 1;
        case PD_PLUS: case PD_FADD: *out = a + b; return 1;
        case PD_MINUS: *out = a - b; return 1;
        case PD_POW: return 0; /* integer/exact-power semantics: pow() is not
                                  bitwise exact, so never constant-fold POW. */
        case PD_MIN: *out = (b < a) ? b : a; return 1;
        case PD_MAX: *out = (b > a) ? b : a; return 1;
        case PD_FMOD: *out = fmod(a, b); return 1;
        case PD_ATAN2: *out = atan2(a, b); return 1;
        case PD_LOGB: *out = log(a) / log(b); return 1;
        case PD_LES: *out = (a <  b); return 1;
        case PD_LESEQ: *out = (a <= b); return 1;
        case PD_MOR: *out = (a >  b); return 1;
        case PD_MOREQ: *out = (a >= b); return 1;
        case PD_EQU: *out = (a == b); return 1;
        case PD_NEQU: *out = (a != b); return 1;
        case PD_LAND: *out = (a != 0.0) && (b != 0.0); return 1;
        case PD_LOR: *out = (a != 0.0) || (b != 0.0); return 1;
        default: return 0;
    }
}

/* Does register r (by fam+off) appear as an input of instruction in?
 * `p` is needed to inspect a CALL's extra arguments (args beyond in[1]). */
static int reads(pd_Reg r, const pd_Instr *in, const pd_Program *p) {
    if (in->nIn >= 1 && in->in[0].fam == r.fam && in->in[0].off == r.off) return 1;
    if (in->nIn >= 2 && in->in[1].fam == r.fam && in->in[1].off == r.off) return 1;
    if (in->op == PD_CALL && in->extraIdx >= 0 && p) {
        /* A CALL with >2 args stores the remaining args in p->extra[].
         * Any of those counts as a read of r. */
        for (int k = 2; k < in->nIn; k++) {
            pd_Reg e = p->extra[in->extraIdx + k - 2];
            if (e.fam == r.fam && e.off == r.off) return 1;
        }
    }
    return 0;
}

/* Is register r ever written (as an output) by instruction at index j? */
static int writes(pd_Reg r, const pd_Instr *in) {
    if (in->out.fam == r.fam && in->out.off == r.off) return 1;
    return 0;
}

/* Liveness of a register written by instruction `i` in program `p`.
 *
 * A naive forward-only scan is UNSAFE in the presence of backward control
 * flow (loops): a loop-variable update `MOV d, x` at the end of the body is
 * read by the loop's back-edge jump target, which sits *before* `i` in linear
 * order. The forward scan misses that read and the store gets wrongly deleted,
 * freezing the loop variable and causing an infinite loop.
 *
 * We therefore also consider the region reachable via a single backward wrap:
 * if some GOTO/IF in (i, n) targets a label t <= i, then after `i` control can
 * jump back to t, so any read of d in [t, i) that is not preceded (in [t, i))
 * by a re-write of d is a genuine live read. */
static int reg_is_live(pd_Program *p, size_t i, pd_Reg d) {
    size_t n = p->nInstr;
    /* forward scan */
    for (size_t j = i + 1; j < n; j++) {
        if (reads(d, &p->instr[j], p)) return 1;
    }
    /* backward-wrap scan: find a backward edge from >= i+1 landing at <= i */
    for (size_t g = i + 1; g < n; g++) {
        pd_Op op = p->instr[g].op;
        if (op != PD_GOTO && op != PD_IF0 && op != PD_IF1) continue;
        size_t t = p->instr[g].out.off;
        if (t >= n || t > i) continue;            /* not a backward edge */
        /* region [t, i) is reachable after i; check for a live read of d
         * that isn't shadowed by an earlier re-write of d in [t, j). */
        for (size_t j = t; j < i; j++) {
            if (writes(d, &p->instr[j])) break;   /* d redefined before read */
            if (reads(d, &p->instr[j], p)) return 1;
        }
    }
    return 0;
}

static int g_opt_fold = -1, g_opt_copy = -1, g_opt_dce = -1;
static int opt_flag(const char *name, int *cache) {
    if (*cache < 0) {
        const char *v = getenv(name);
        if (!v) *cache = 1;                              /* default: ON */
        else *cache = (strcmp(v, "0") == 0) ? 0 : 1;    /* explicit "0" disables */
    }
    return *cache;
}
#define OPT_FOLD() opt_flag("PD_OPT_FOLD", &g_opt_fold)
#define OPT_COPY() opt_flag("PD_OPT_COPY", &g_opt_copy)
#define OPT_DCE()  opt_flag("PD_OPT_DCE", &g_opt_dce)

static void optimize_one(pd_Program *p) {
    /* Build a value map for copy propagation: reg -> replacement reg. */
    for (int pass = 0; pass < 12; pass++) {
        int changed = 0;

        /* ---- constant folding ---- */
        if (!OPT_FOLD()) goto skip_fold;
        for (size_t i = 0; i < p->nInstr; i++) {
            pd_Instr *in = &p->instr[i];
            if (!is_pure(in->op)) continue;
            if (in->op == PD_MOV || in->op == PD_NEGMOV) {
                if (in->nIn < 1 || !is_const(in->in[0])) continue;
            } else if (in->nIn == 1) {
                if (!is_const(in->in[0])) continue;
            } else if (in->nIn == 2) {
                if (!is_const(in->in[0]) || !is_const(in->in[1])) continue;
            } else continue;
            double a = is_const(in->in[0]) ? p->consts[in->in[0].off / 8] : 0.0;
            double b = (in->nIn >= 2 && is_const(in->in[1]))
                       ? p->consts[in->in[1].off / 8] : 0.0;
            double res;
            if (!fold(in->op, a, b, &res)) continue;
            /* Intern the folded constant, but KEEP the original destination
             * register `d` intact: the result must still be written into d, so
             * the instruction collapses to `MOV d <- new_const` rather than
             * clobbering d's identity (which would corrupt every later reader).
             * A plain MOV of a const is already in this form, so skip it. */
            if (in->op == PD_MOV || in->op == PD_NEGMOV) continue;
            size_t ci = p->nConst;
            p->consts = (double*)realloc(p->consts, (p->nConst + 1) * sizeof(double));
            p->consts[ci] = res;
            p->nConst++;
            pd_Reg d = in->out;
            in->op   = PD_MOV;
            in->out  = d;
            in->in[0] = pdR(PD_FAM_CONST, (uint32_t)(ci * 8));
            in->nIn  = 1;
            changed = 1;
        }
        skip_fold:

        /* ---- copy propagation + dead code + nop elimination ---- */
        if (!OPT_COPY()) goto skip_copy;
        /* A MOV d<-s (s leaf) that is never re-written afterwards can have its
         * readers redirected to s and itself removed. */
        for (size_t i = 0; i < p->nInstr; i++) {
            pd_Instr *in = &p->instr[i];
            if (in->op != PD_MOV) continue;
            if (in->nIn < 1 || !is_leaf(in->in[0])) continue;
            pd_Reg d = in->out, s = in->in[0];
            if (d.fam == PD_FAM_VOID) continue;
            /* d must not be written again after i, s must stay constant after i
             * (otherwise a reader past s's reassignment would read the wrong
             * value), and d must be read somewhere. */
            int rewritten_later = 0, s_rewritten = 0, read_anywhere = 0;
            for (size_t j = i + 1; j < p->nInstr; j++) {
                pd_Instr:;
                pd_Instr *jn = &p->instr[j];
                if ((jn->out.fam == d.fam && jn->out.off == d.off) &&
                    jn->op != PD_GOTO && jn->op != PD_IF0 && jn->op != PD_IF1)
                    rewritten_later = 1;
                if ((jn->out.fam == s.fam && jn->out.off == s.off) &&
                    jn->op != PD_GOTO && jn->op != PD_IF0 && jn->op != PD_IF1)
                    s_rewritten = 1;
                if (reads(d, jn, p)) read_anywhere = 1;
            }
            if (rewritten_later || s_rewritten) continue;
            /* A backward-loop reader (loop-variable update reached via a
             * back-edge) keeps d live through the wrap. This must be checked
             * even when d is ALSO read forward (read_anywhere): redirecting
             * the forward readers and NOP-ing the MOV would still strand the
             * back-edge reader, freezing the loop. So compute it unconditionally. */
            int live_backward = reg_is_live(p, i, d);
            if (!read_anywhere && !live_backward) {
                /* dead MOV (and its destination is never used) -> delete */
                in->op = PD_NOP;
                changed = 1;
                continue;
            }
            /* redirect all later (forward) readers of d to s */
            for (size_t j = i + 1; j < p->nInstr; j++) {
                pd_Instr *jn = &p->instr[j];
                if (jn->nIn >= 1 && jn->in[0].fam == d.fam && jn->in[0].off == d.off)
                    jn->in[0] = s;
                if (jn->nIn >= 2 && jn->in[1].fam == d.fam && jn->in[1].off == d.off)
                    jn->in[1] = s;
                /* A CALL may carry extra args (args beyond in[1]) in p->extra[].
                 * Redirect those too, otherwise the MOV we are about to NOP would
                 * strand the CALL's extra argument at a dead/zeroed slot. */
                if (jn->op == PD_CALL && jn->extraIdx >= 0) {
                    for (int k = 2; k < jn->nIn; k++) {
                        if (p->extra[jn->extraIdx + k - 2].fam == d.fam &&
                            p->extra[jn->extraIdx + k - 2].off == d.off)
                            p->extra[jn->extraIdx + k - 2] = s;
                    }
                }
            }
            /* If d is live via a backward edge (loop variable), the back-edge
             * readers are NOT in the forward range above and still need d to
             * hold s. So KEEP the MOV (don't NOP it) in that case; otherwise
             * the loop variable update is lost and the loop never advances.
             * When d is only read forward (no backward reader), redirecting +
             * NOP is safe because every forward reader now uses s directly. */
            if (read_anywhere && !live_backward) {
                in->op = PD_NOP;
                changed = 1;
            }
        }
        skip_copy:

        /* ---- dead code elimination for pure ops whose output is unused ---- */
        if (!OPT_DCE()) goto skip_dce;
        for (size_t i = 0; i < p->nInstr; i++) {
            pd_Instr *in = &p->instr[i];
            if (in->op == PD_NOP) continue;
            if (!is_pure(in->op)) continue;
            if (in->op == PD_RETURN) continue;
            pd_Reg d = in->out;
            if (d.fam == PD_FAM_VOID) { in->op = PD_NOP; changed = 1; continue; }
            /* Never eliminate writes to GLOBAL/EXT/PARAM: their value may be
             * consumed by a later frame (cross-pixel state) or by the host,
             * which linear intra-function analysis cannot see. */
            if (d.fam == PD_FAM_GLOBAL || d.fam == PD_FAM_EXT || d.fam == PD_FAM_PARAM)
                continue;
            int read_anywhere = reg_is_live(p, i, d);
            if (!read_anywhere) { in->op = PD_NOP; changed = 1; }
        }
        skip_dce:

        if (!changed) break;
    }

    /* Compact: remove NOPs, fixing GOTO/IF label targets.
     * A NOP that is the target of a GOTO/IF must be KEPT (it is a jump
     * landing pad) even though it does nothing. */
    int *keep = (int*)calloc(p->nInstr, sizeof(int));
    for (size_t i = 0; i < p->nInstr; i++) {
        pd_Op op = p->instr[i].op;
        if (op == PD_GOTO || op == PD_IF0 || op == PD_IF1) {
            size_t tgt = p->instr[i].out.off;
            if (tgt < p->nInstr) keep[tgt] = 1;
        }
    }
    size_t *remap = (size_t*)malloc(p->nInstr * sizeof(size_t));
    size_t w = 0;
    for (size_t i = 0; i < p->nInstr; i++) {
        if (p->instr[i].op == PD_NOP && !keep[i]) { remap[i] = (size_t)-1; continue; }
        remap[i] = w++;
    }
    if (w != p->nInstr) {
        pd_Instr *ni = (pd_Instr*)malloc(w * sizeof(pd_Instr));
        w = 0;
        for (size_t i = 0; i < p->nInstr; i++) {
            if (p->instr[i].op == PD_NOP && !keep[i]) continue;
            ni[w++] = p->instr[i];
        }
        /* fix jump/branch targets */
        for (size_t i = 0; i < w; i++) {
            pd_Op op = ni[i].op;
            if (op == PD_GOTO || op == PD_IF0 || op == PD_IF1) {
                size_t old = ni[i].out.off;
                if (old < p->nInstr && remap[old] != (size_t)-1)
                    ni[i].out.off = (uint32_t)remap[old];
                else if (old < p->nInstr)
                    ni[i].out.off = (uint32_t)remap[old]; /* remap[old] valid (-1 only if dropped, which we prevented) */
            }
        }
        free(p->instr);
        p->instr = ni;
        p->nInstr = w;
    }
    free(remap);
    free(keep);
}

void pd_optimize_program(pd_Program *p) {
    optimize_one(p);
    for (size_t f = 0; f < p->nFuncs; f++)
        optimize_one(&p->funcs[f]);
}
