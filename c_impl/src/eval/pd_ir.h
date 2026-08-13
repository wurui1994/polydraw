/* pd_ir.h — PolyDraw EVAL intermediate representation.
 *
 * Defines the Instr/Reg/Op types shared by the parser, optimizer,
 * interpreter, and JIT backend. Mirrors the design of the original
 * gasmtyp (eval.c:410) but with a cleaner, strongly-typed layout.
 *
 * See Plan/02_IR_and_Optimizer.md for the full design.
 */
#ifndef PD_IR_H
#define PD_IR_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* forward decl so pd_Program can hold a host pointer without including
 * pd_host.h (avoids circular include). */
struct pd_Host;

/* ---- Register families (high nibble of original rtyp.r) ---- */
/* Order matters only for debug printing. */
typedef enum {
    PD_FAM_VOID = 0,   /* KUNUSED  - operand not used */
    PD_FAM_LOCAL,      /* KECX     - local temp slot, off = byte offset */
    PD_FAM_CONST,      /* KEDX     - constant in const table, off = index*8 */
    PD_FAM_PARAM,      /* KESP     - function parameter, off = param slot */
    PD_FAM_LABEL,      /* KEIP     - jump label, off = instr index */
    PD_FAM_PTR,        /* KPTR     - pointer-to-parameter */
    PD_FAM_EXT,        /* KIMM     - external (host) symbol, off = ext index */
    PD_FAM_GLOBAL,     /* KGLB     - global static, off = byte offset */
    PD_FAM_STR,        /* KSTR     - string table (folded into CONST late) */
    PD_FAM_ARR         /* KARR     - array table (folded into CONST late) */
} pd_Fam;

/* ---- Opcodes. 1:1 with eval.c:242 enum, renamed for clarity. ---- */
typedef enum {
    PD_NOP = 0,
    /* control flow */
    PD_GOTO,
    PD_RETURN,
    PD_RND,
    PD_NRND,
    /* 1-input (nIn==1): out = f(in[0]); in[1] unused */
    PD_MOV,       /* MOV: but note out/in[0] may alias for some ops */
    PD_NEGMOV,
    PD_NEQU0,     /* out = (in[0] != 0) ? 1.0 : 0.0 */
    PD_IF0,       /* if (in[1]==0) goto out(label). in[0] = instr-index hint */
    PD_IF1,       /* if (in[1]!=0) goto out(label). */
    PD_FABS, PD_SGN, PD_UNIT, PD_FLOOR, PD_CEIL, PD_ROUND0,
    PD_SIN, PD_COS, PD_TAN, PD_ASIN, PD_ACOS, PD_ATAN,
    PD_SQRT, PD_EXP, PD_FACT, PD_LOG,
    /* 2-input (nIn==2): out = in[0] OP in[1]
     * Note: original stores operands as r[1],r[2] with r[0] output;
     * here we use in[0],in[1] for the two sources. */
    PD_TIMES, PD_SLASH, PD_PERC, PD_PLUS, PD_MINUS,
    PD_LES, PD_LESEQ, PD_MOR, PD_MOREQ, PD_EQU, PD_NEQU,
    PD_LAND, PD_LOR, PD_POW, PD_MIN, PD_MAX, PD_FADD, PD_FMOD,
    PD_ATAN2, PD_LOGB,
    /* memory: arrays.
     *   PEEK:    out=result, in[0]=array-base local, in[1]=index, aux=size
     *   POKE family: out=array-base local, in[0]=value, in[1]=index, aux=size
     *   ADDR:    out=bit-cast array base pointer (host & array args) */
    PD_PEEK,
    PD_ADDR,
    PD_ADDRSLOT,  /* address of a variable's storage slot (scalar by-ref) */
    PD_POKE,
    PD_POKETIMES, PD_POKESLASH, PD_POKEPERC,
    PD_POKEPLUS, PD_POKEMINUS,
    /* function call */
    PD_CALL,      /* call user function; aux = function id; ins are args */
    PD_OP_END
} pd_Op;

/* ---- Operand register ---- */
typedef struct {
    pd_Fam   fam;
    uint32_t off;   /* meaning depends on fam */
    int32_t  aux;   /* array index adjust / func-ptr offset */
} pd_Reg;

/* static inline constructor helpers */
static inline pd_Reg pdR(pd_Fam f, uint32_t off) {
    pd_Reg r; r.fam = f; r.off = off; r.aux = 0; return r;
}

/* ---- Instruction ---- */
typedef struct {
    pd_Op  op;
    int32_t aux;       /* function id for PD_CALL; array descriptor idx; etc */
    uint8_t nIn;       /* number of inputs (0,1,2) ; CALL may use extra[] */
    pd_Reg out;        /* r[0]: destination or label-target */
    pd_Reg in[2];      /* r[1], r[2] */
    int32_t extraIdx;  /* index into program->extra[] when nIn>2 (CALL) */
} pd_Instr;

/* ---- Compiled program ----
 * Self-contained block produced by the parser/optimizer and consumed by
 * the interpreter and JIT backend. Equivalent to original kcd_t.
 */
typedef struct pd_Program pd_Program;
struct pd_Program {
    pd_Instr *instr;
    size_t    nInstr;

    /* constant pool (doubles) */
    double   *consts;
    size_t    nConst;

    /* string table (raw bytes, concatenated; null-terminated entries) */
    char     *strings;
    size_t    nStrings;

    /* extra call arguments (when a CALL has >2 inputs) */
    pd_Reg   *extra;
    size_t    nExtra;

    /* number of local 8-byte slots needed (stack frame size) */
    size_t    nLocals;

    /* number of function parameters (for the entry function) */
    size_t    nParams;

    /* global static storage (shared, grows as statics are declared).
     * For the entry program this points at the host's static block. */
    double   *globals;
    size_t    nGlobals;

    /* sub-functions (user-defined).  Index matches PD_CALL .aux. */
    pd_Program *funcs;
    size_t      nFuncs;

    /* external (host) function table — non-owning, set by pd_host_attach.
     * PD_CALL with aux==-2 means "external": out.off is the host fn index.
     * Non-const: host functions mutate per-ctx state (state/glbuf/attribs). */
    struct pd_Host *host;

    /* error reporting */
    char      err[256];
};

/* ---- IR builder: used by the parser to construct programs ---- */
typedef struct {
    pd_Instr *instr;   size_t nInstr, capInstr;
    double   *consts;  size_t nConst, capConst;
    char     *strings; size_t nStrings, capStrings;
    pd_Reg   *extra;   size_t nExtra, capExtra;
    size_t    nLocals;
    char      err[256];
    int       ok;
} pd_Builder;

void pd_builder_init(pd_Builder *b);
void pd_builder_free(pd_Builder *b);
void pd_builder_clear(pd_Builder *b);

/* emit an instruction; returns its index */
size_t pd_emit(pd_Builder *b, pd_Op op, pd_Reg out, const pd_Reg *ins, int nIn);

/* convenience: 0/1/2-input emits */
size_t pd_emit0(pd_Builder *b, pd_Op op, pd_Reg out);
size_t pd_emit1(pd_Builder *b, pd_Op op, pd_Reg out, pd_Reg in0);
size_t pd_emit2(pd_Builder *b, pd_Op op, pd_Reg out, pd_Reg in0, pd_Reg in1);

/* allocate a fresh local slot, returns its reg */
pd_Reg pd_new_local(pd_Builder *b);

/* intern a constant; returns CONST reg */
pd_Reg pd_new_const(pd_Builder *b, double v);
pd_Reg pd_new_const_ptr(pd_Builder *b, double v);

/* intern a string literal; returns STR reg (offset into strings[]) */
pd_Reg pd_new_string(pd_Builder *b, const char *s, size_t len);

/* label management: returns a fresh label id (== instr index placeholder) */
size_t pd_label_here(pd_Builder *b);      /* emits a NOP anchor at current pos */
void   pd_patch_goto_target(pd_Builder *b, size_t gotoInstrIdx, size_t targetInstrIdx);

/* finalize: copy builder arrays into a heap program. returns 0 on error. */
int pd_builder_finish(pd_Builder *b, pd_Program *out);

/* free a finished program and its sub-functions */
void pd_program_free(pd_Program *p);

/* debug */
void pd_dump_program(const pd_Program *p, FILE *f);
const char *pd_op_name(pd_Op op);
const char *pd_fam_name(pd_Fam f);

#ifdef __cplusplus
}
#endif
#endif
