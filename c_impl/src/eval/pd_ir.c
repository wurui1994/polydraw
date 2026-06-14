/* pd_ir.c — IR builder implementation. */
#include "pd_ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void pd_builder_init(pd_Builder *b) {
    memset(b, 0, sizeof(*b));
    b->ok = 1;
}

void pd_builder_free(pd_Builder *b) {
    free(b->instr);   b->instr = NULL;
    free(b->consts);  b->consts = NULL;
    free(b->strings); b->strings = NULL;
    free(b->extra);   b->extra = NULL;
    memset(b, 0, sizeof(*b));
}

void pd_builder_clear(pd_Builder *b) {
    b->nInstr = b->nConst = b->nStrings = b->nExtra = b->nLocals = 0;
    b->ok = 1;
    b->err[0] = 0;
}

static int grow(void **p, size_t *cap, size_t need, size_t sz) {
    if (need <= *cap) return 1;
    size_t nc = *cap ? *cap * 2 : 16;
    while (nc < need) nc *= 2;
    void *np = realloc(*p, nc * sz);
    if (!np) return 0;
    *p = np; *cap = nc;
    return 1;
}

size_t pd_emit(pd_Builder *b, pd_Op op, pd_Reg out, const pd_Reg *ins, int nIn) {
    if (!grow((void**)&b->instr, &b->capInstr, b->nInstr + 1, sizeof(pd_Instr))) {
        b->ok = 0; return (size_t)-1;
    }
    pd_Instr *insp = &b->instr[b->nInstr];
    memset(insp, 0, sizeof(*insp));
    insp->op = op;
    insp->out = out;
    insp->nIn = (uint8_t)nIn;
    if (nIn > 0) insp->in[0] = ins[0];
    if (nIn > 1) insp->in[1] = ins[1];
    insp->aux = -1;
    insp->extraIdx = -1;
    /* args beyond in[1] go into the extra[] table */
    if (nIn > 2) {
        if (!grow((void**)&b->extra, &b->capExtra, b->nExtra + (nIn - 2), sizeof(pd_Reg))) {
            b->ok = 0; return (size_t)-1;
        }
        insp->extraIdx = (int32_t)b->nExtra;
        for (int k = 2; k < nIn; k++) b->extra[b->nExtra++] = ins[k];
    }
    return b->nInstr++;
}

size_t pd_emit0(pd_Builder *b, pd_Op op, pd_Reg out) {
    return pd_emit(b, op, out, NULL, 0);
}
size_t pd_emit1(pd_Builder *b, pd_Op op, pd_Reg out, pd_Reg in0) {
    return pd_emit(b, op, out, &in0, 1);
}
size_t pd_emit2(pd_Builder *b, pd_Op op, pd_Reg out, pd_Reg in0, pd_Reg in1) {
    pd_Reg ins[2] = {in0, in1};
    return pd_emit(b, op, out, ins, 2);
}

pd_Reg pd_new_local(pd_Builder *b) {
    /* each local occupies one 8-byte slot */
    pd_Reg r = pdR(PD_FAM_LOCAL, (uint32_t)(b->nLocals * 8));
    b->nLocals++;
    return r;
}

pd_Reg pd_new_const(pd_Builder *b, double v) {
    /* dedupe simple case to match original behaviour */
    for (size_t i = 0; i < b->nConst; i++)
        if (b->consts[i] == v)
            return pdR(PD_FAM_CONST, (uint32_t)(i * 8));
    if (!grow((void**)&b->consts, &b->capConst, b->nConst + 1, sizeof(double))) {
        b->ok = 0; return pdR(PD_FAM_VOID, 0);
    }
    b->consts[b->nConst] = v;
    return pdR(PD_FAM_CONST, (uint32_t)(b->nConst++ * 8));
}

pd_Reg pd_new_string(pd_Builder *b, const char *s, size_t len) {
    /* store: bytes + NUL terminator */
    if (!grow((void**)&b->strings, &b->capStrings, b->nStrings + len + 1, 1)) {
        b->ok = 0; return pdR(PD_FAM_VOID, 0);
    }
    pd_Reg r = pdR(PD_FAM_STR, (uint32_t)b->nStrings);
    memcpy(b->strings + b->nStrings, s, len);
    b->nStrings += len;
    b->strings[b->nStrings++] = 0;
    return r;
}

size_t pd_label_here(pd_Builder *b) {
    /* a label is just an instruction-index position; we emit a NOP so it has
     * a concrete instruction to land on. */
    return pd_emit0(b, PD_NOP, pdR(PD_FAM_VOID, 0));
}

void pd_patch_goto_target(pd_Builder *b, size_t gotoInstrIdx, size_t targetInstrIdx) {
    if (gotoInstrIdx < b->nInstr) {
        /* encode target in out.off (LABEL fam); keep aux as instr-index hint */
        b->instr[gotoInstrIdx].out = pdR(PD_FAM_LABEL, (uint32_t)targetInstrIdx);
    }
}

int pd_builder_finish(pd_Builder *b, pd_Program *out) {
    if (!b->ok) { snprintf(b->err, sizeof(b->err), "build failed (OOM?)"); return 0; }
    memset(out, 0, sizeof(*out));
    out->nInstr = b->nInstr;
    out->instr  = malloc((b->nInstr ? b->nInstr : 1) * sizeof(pd_Instr));
    if (!out->instr) return 0;
    if (b->nInstr) memcpy(out->instr, b->instr, b->nInstr * sizeof(pd_Instr));

    out->nConst  = b->nConst;
    out->consts  = malloc((b->nConst ? b->nConst : 1) * sizeof(double));
    if (!out->consts) return 0;
    if (b->nConst) memcpy(out->consts, b->consts, b->nConst * sizeof(double));

    out->nStrings = b->nStrings;
    out->strings  = malloc(b->nStrings ? b->nStrings : 1);
    if (!out->strings) return 0;
    if (b->nStrings) memcpy(out->strings, b->strings, b->nStrings);

    out->nExtra  = b->nExtra;
    out->extra   = malloc((b->nExtra ? b->nExtra : 1) * sizeof(pd_Reg));
    if (!out->extra) return 0;
    if (b->nExtra) memcpy(out->extra, b->extra, b->nExtra * sizeof(pd_Reg));

    out->nLocals  = b->nLocals;
    out->nParams  = 0;
    out->globals  = NULL;
    out->nGlobals = 0;
    out->funcs    = NULL;
    out->nFuncs   = 0;
    return 1;
}

void pd_program_free(pd_Program *p) {
    free(p->instr);   p->instr = NULL;
    free(p->consts);  p->consts = NULL;
    free(p->strings); p->strings = NULL;
    free(p->extra);   p->extra = NULL;
    for (size_t i = 0; i < p->nFuncs; i++) pd_program_free(&p->funcs[i]);
    free(p->funcs);   p->funcs = NULL;
    free(p->globals); p->globals = NULL;
}

/* ---- debug names ---- */
static const char *g_opNames[] = {
    "NOP","GOTO","RETURN","RND","NRND",
    "MOV","NEGMOV","NEQU0","IF0","IF1",
    "FABS","SGN","UNIT","FLOOR","CEIL","ROUND0",
    "SIN","COS","TAN","ASIN","ACOS","ATAN",
    "SQRT","EXP","FACT","LOG",
    "TIMES","SLASH","PERC","PLUS","MINUS",
    "LES","LESEQ","MOR","MOREQ","EQU","NEQU",
    "LAND","LOR","POW","MIN","MAX","FADD","FMOD",
    "ATAN2","LOGB",
    "PEEK","POKE","POKETIMES","POKESLASH","POKEPERC","POKEPLUS","POKEMINUS",
    "CALL","OP_END"
};
const char *pd_op_name(pd_Op op) {
    if (op < 0 || op >= PD_OP_END) return "?";
    return g_opNames[op];
}
const char *pd_fam_name(pd_Fam f) {
    static const char *n[] = {"VOID","LOCAL","CONST","PARAM","LABEL","PTR","EXT","GLOBAL","STR","ARR"};
    if (f < 0 || f > PD_FAM_ARR) return "?";
    return n[f];
}

void pd_dump_program(const pd_Program *p, FILE *f) {
    fprintf(f, ";; nInstr=%zu nConst=%zu nLocals=%zu nParams=%zu\n",
            p->nInstr, p->nConst, p->nLocals, p->nParams);
    for (size_t i = 0; i < p->nConst; i++)
        fprintf(f, ";; const[%zu] = %g\n", i, p->consts[i]);
    for (size_t i = 0; i < p->nInstr; i++) {
        const pd_Instr *in = &p->instr[i];
        fprintf(f, "%4zu: %-9s", i, pd_op_name(in->op));
        if (in->out.fam != PD_FAM_VOID || in->op==PD_GOTO || in->op==PD_IF0 || in->op==PD_IF1)
            fprintf(f, " %s:%u", pd_fam_name(in->out.fam), in->out.off);
        for (int k = 0; k < in->nIn; k++)
            fprintf(f, "  %s:%u", pd_fam_name(in->in[k].fam), in->in[k].off);
        if (in->aux >= 0) fprintf(f, "  aux=%d", in->aux);
        fprintf(f, "\n");
    }
}
