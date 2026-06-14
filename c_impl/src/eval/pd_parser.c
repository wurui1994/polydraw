/* pd_parser.c — Pratt-style expression parser + recursive-descent statements.
 *
 * Plan A from Plan/03_Parser.md. The expression parser uses binding-power
 * (precedence) to handle all binary/unary operators with a single compact
 * loop. Statements use classic recursive descent.
 *
 * Operator precedence (lower number = binds tighter), from eval.c:7352:
 *    ^       : 1   (right-assoc)
 *    * / %   : 2   (left)
 *    + -     : 3   (left)
 *    < <= > >= : 4 (left)
 *    == !=   : 5   (left)
 *    &&      : 6   (left)
 *    ||      : 7   (left)
 *    = += ...: 8   (right, lowest, assignment)
 */
#include "pd_parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* sentinel meaning "not inside a loop" for breakLabel/contLabel */
#define PD_NO_LOOP INT_MIN

/* ---- built-in function table ---- */
typedef struct {
    const char *name;     /* upper-case */
    pd_Op op1;            /* opcode for 1-arg form */
    pd_Op op2;            /* opcode for 2-arg form (PD_OP_END if N/A) */
} pd_Builtin;

static const pd_Builtin BUILTINS[] = {
    /* name      1-arg op          2-arg op */
    { "ABS",    PD_FABS,  PD_OP_END },
    { "FABS",   PD_FABS,  PD_OP_END },
    { "ACOS",   PD_ACOS,  PD_OP_END },
    { "ASIN",   PD_ASIN,  PD_OP_END },
    { "ATAN",   PD_ATAN,  PD_OP_END },
    { "ATN",    PD_ATAN,  PD_OP_END },  /* alias */
    { "CEIL",   PD_CEIL,  PD_OP_END },
    { "COS",    PD_COS,   PD_OP_END },
    { "EXP",    PD_EXP,   PD_OP_END },
    { "FACT",   PD_FACT,  PD_OP_END },
    { "FLOOR",  PD_FLOOR, PD_OP_END },
    { "INT",    PD_ROUND0,PD_OP_END },
    { "LOG",    PD_LOG,   PD_LOGB   },   /* 1 or 2 arg */
    { "SGN",    PD_SGN,   PD_OP_END },
    { "SIN",    PD_SIN,   PD_OP_END },
    { "SQR",    PD_SQRT,  PD_OP_END },   /* alias */
    { "SQRT",   PD_SQRT,  PD_OP_END },
    { "TAN",    PD_TAN,   PD_OP_END },
    { "UNIT",   PD_UNIT,  PD_OP_END },
    { "ATAN2",  PD_OP_END,PD_ATAN2  },
    { "FMOD",   PD_OP_END,PD_FMOD   },
    { "MIN",    PD_OP_END,PD_MIN    },
    { "MAX",    PD_OP_END,PD_MAX    },
    { "POW",    PD_OP_END,PD_POW    },
    { "FADD",   PD_OP_END,PD_FADD   },
    { NULL, 0, 0 }
};
/* RND/NRND are parameterless; PI is a constant. Handled specially. */

/* ---- parser init/free ---- */
void pd_parser_init(pd_Parser *p, pd_Builder *b, pd_TokenStream *ts) {
    memset(p, 0, sizeof(*p));
    p->b = b; p->ts = ts; p->tok = 0;
    p->ok = 1;
    p->breakLabel = PD_NO_LOOP; p->contLabel = PD_NO_LOOP;
}

void pd_parser_free(pd_Parser *p) {
    /* symbols are inline; nothing to free */
    (void)p;
}

/* ---- token helpers ---- */
pd_Tok *pd_cur(pd_Parser *p) {
    return (p->tok < p->ts->nToks) ? &p->ts->toks[p->tok] : &p->ts->toks[p->ts->nToks-1];
}
/* peek a token at a specific index without advancing */
static const pd_Tok *pd_cur_at_idx(const pd_Parser *p, size_t idx) {
    if (idx >= p->ts->nToks) return &p->ts->toks[p->ts->nToks-1];
    return &p->ts->toks[idx];
}
#define pd_cur_at(idx) pd_cur_at_idx(p, (idx))
pd_Tok *pd_eat(pd_Parser *p) {
    pd_Tok *t = pd_cur(p);
    if (p->tok < p->ts->nToks) p->tok++;
    return t;
}
int pd_accept(pd_Parser *p, pd_TokKind kind, const char *text) {
    pd_Tok *t = pd_cur(p);
    if (t->kind != kind) return 0;
    if (text && (t->len != strlen(text) || strncmp(t->text, text, t->len) != 0)) return 0;
    p->tok++;
    return 1;
}
/* accept a PUNCT token matching text */
static int accept_punct(pd_Parser *p, const char *text) { return pd_accept(p, PD_TOK_PUNCT, text); }
static int accept_ident(pd_Parser *p, const char *text) { return pd_accept(p, PD_TOK_IDENT, text); }

static int pd_expect_punct(pd_Parser *p, const char *text) {
    if (accept_punct(p, text)) return 1;
    snprintf(p->err, sizeof(p->err), "expected '%s' at line %d", text, pd_cur(p)->origLine);
    p->ok = 0; p->errLine = pd_cur(p)->origLine;
    return 0;
}

/* a wrapper to match the header's pd_expect (kept for compat) */
int pd_expect(pd_Parser *p, const char *text) { return pd_expect_punct(p, text); }

static void pd_error(pd_Parser *p, const char *msg) {
    if (p->ok) {
        snprintf(p->err, sizeof(p->err), "%s at line %d", msg, pd_cur(p)->origLine);
        p->errLine = pd_cur(p)->origLine;
        p->ok = 0;
    }
}

/* ---- symbol table ---- */
static pd_Sym *sym_find(pd_Parser *p, const char *name, int nParams) {
    /* exact name+arity match (for functions); for non-funcs arity ignored */
    for (int i = p->nSyms - 1; i >= 0; i--) {
        pd_Sym *s = &p->syms[i];
        if (strncmp(s->name, name, sizeof(s->name)) != 0) continue;
        if (s->kind == PD_SYM_BUILTIN || s->kind == PD_SYM_EXT_FUNC || s->kind == PD_SYM_FUNC) {
            if (nParams >= 0 && s->nParams != nParams) {
                /* try overload chain */
                int next = s->nextOverload;
                while (next >= 0) {
                    pd_Sym *o = &p->syms[next];
                    if (o->nParams == nParams) return o;
                    next = o->nextOverload;
                }
                continue;
            }
        }
        return s;
    }
    return NULL;
}

/* find any symbol by name ignoring arity (first match) */
static pd_Sym *sym_find_name(pd_Parser *p, const char *name) {
    for (int i = p->nSyms - 1; i >= 0; i--) {
        if (strncmp(p->syms[i].name, name, sizeof(p->syms[i].name)) == 0)
            return &p->syms[i];
    }
    return NULL;
}

static pd_Sym *sym_add(pd_Parser *p, const char *name, pd_SymKind kind) {
    if (p->nSyms >= PD_MAX_SYMS) { pd_error(p, "too many symbols"); return NULL; }
    pd_Sym *s = &p->syms[p->nSyms++];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, sizeof(s->name)-1);
    s->kind = kind;
    s->nextOverload = -1;
    s->funcIdx = -1;
    return s;
}
/* public wrappers for host.c */
pd_Sym *pd_parser_sym_add(pd_Parser *p, const char *name, pd_SymKind kind) { return sym_add(p, name, kind); }
pd_Sym *pd_parser_sym_find(pd_Parser *p, const char *name) { return sym_find_name(p, name); }

void pd_parser_install_builtins(pd_Parser *p) {
    /* built-in funcs */
    for (int i = 0; BUILTINS[i].name; i++) {
        const pd_Builtin *bi = &BUILTINS[i];
        if (bi->op1 != PD_OP_END) {
            pd_Sym *s = sym_add(p, bi->name, PD_SYM_BUILTIN);
            s->nParams = 1;
            s->reg = pdR(PD_FAM_VOID, bi->op1); /* encode op in reg.off */
        }
        if (bi->op2 != PD_OP_END) {
            /* if same name already added (LOG), link as overload */
            pd_Sym *prev = sym_find_name(p, bi->name);
            pd_Sym *s = sym_add(p, bi->name, PD_SYM_BUILTIN);
            s->nParams = 2;
            s->reg = pdR(PD_FAM_VOID, bi->op2);
            if (prev) {
                s->nextOverload = prev->nextOverload;
                prev->nextOverload = (int)(s - p->syms);
            }
        }
    }
    /* PI constant */
    {
        pd_Sym *s = sym_add(p, "PI", PD_SYM_CONST);
        s->reg = pd_new_const(p->b, 3.14159265358979323);
    }
    /* E — not a real EVAL builtin but harmless if scripts use it; skip to stay faithful */
}

int pd_parser_add_ext(pd_Parser *p, const char *proto, void *ptr) {
    /* proto format: "NAME" or "NAME(,,,)" with N commas = N params */
    char name[40]; size_t ni = 0;
    size_t i = 0;
    while (proto[i] && proto[i] != '(' && ni < sizeof(name)-1) name[ni++] = proto[i++];
    name[ni] = 0;
    int nParams = 0;
    int isFunc = (proto[i] == '(');
    int hasArray = 0;
    if (isFunc) {
        i++; /* ( */
        if (proto[i] == ')') { nParams = 0; i++; }
        else {
            nParams = 1;
            while (proto[i]) {
                if (proto[i] == ',') nParams++;
                else if (proto[i] == '[') hasArray = 1;
                else if (proto[i] == ')') { i++; break; }
                i++;
            }
        }
    }
    pd_Sym *prev = sym_find_name(p, name);
    pd_SymKind kind = isFunc ? PD_SYM_EXT_FUNC : PD_SYM_EXT_VAR;
    pd_Sym *s = sym_add(p, name, kind);
    if (!s) return -1;
    s->nParams = nParams;
    /* encode pointer in a CONST slot via bit-cast. We use a dedicated EXT reg
     * whose off is the index of the pointer stored in a hidden consts slot.
     * For simplicity here, store the pointer's low bits as the EXT index in
     * a side table on the program. We punt: store as a CONST-encoded double. */
    double dptr; memcpy(&dptr, &ptr, sizeof(void*));
    s->reg = pd_new_const(p->b, dptr);
    s->reg.fam = PD_FAM_EXT;
    s->arraySize = hasArray ? 1 : 0; /* caller sets real size elsewhere if needed */
    if (prev && prev->kind == kind) {
        s->nextOverload = prev->nextOverload;
        prev->nextOverload = (int)(s - p->syms);
    }
    return (int)(s - p->syms);
}

/* ---- local variable declaration ---- */
static pd_Sym *declare_local(pd_Parser *p, const char *name) {
    pd_Sym *s = sym_add(p, name, PD_SYM_VAR);
    if (!s) return NULL;
    s->reg = pd_new_local(p->b);
    return s;
}

/* ---- forward decls ---- */
static pd_Reg parse_expr_prec(pd_Parser *p, int minPrec);
static pd_Reg parse_primary(pd_Parser *p);
static pd_Reg parse_unary(pd_Parser *p);

/* precedence lookup for infix operators. Returns -1 if not an infix op. */
typedef struct { const char *tok; int prec; int rightAssoc; pd_Op op; } BinOp;
static const BinOp BINOPS[] = {
    /* prec: lower = tighter (matches eval.c numbering shifted so 1=lowest).
     * We invert: our "prec" means higher binds tighter. */
    { "^",  7, 1, PD_POW   },
    { "*",  6, 0, PD_TIMES },
    { "/",  6, 0, PD_SLASH },
    { "%",  6, 0, PD_PERC  },
    { "+",  5, 0, PD_PLUS  },
    { "-",  5, 0, PD_MINUS },
    { "<",  4, 0, PD_LES   },
    { "<=", 4, 0, PD_LESEQ },
    { ">",  4, 0, PD_MOR   },
    { ">=", 4, 0, PD_MOREQ },
    { "==", 3, 0, PD_EQU   },
    { "!=", 3, 0, PD_NEQU  },
    { "&&", 2, 0, PD_LAND  },
    { "||", 1, 0, PD_LOR   },
    { NULL, 0, 0, 0 }
};
/* assignment handled separately (lowest precedence, right-assoc, lvalue) */
static const BinOp ASSIGNOPS[] = {
    { "=",  0, 1, PD_MOV },
    { "+=", 0, 1, PD_PLUS },
    { "-=", 0, 1, PD_MINUS },
    { "*=", 0, 1, PD_TIMES },
    { "/=", 0, 1, PD_SLASH },
    { "%=", 0, 1, PD_PERC },
    { NULL, 0, 0, 0 }
};

/* ---- Pratt core: parse_expr_prec ---- */
/* Unary minus/plus handling: in EVAL, -2^2 must equal -(2^2) = -4, so unary
 * minus binds LOOSER than ^. We implement this by giving unary prefix a
 * right-binding-power just below ^ (which is prec 7), so any ^ to the right
 * of the unary op is consumed first. */
#define UNARY_PREC 6   /* looser than ^(7), tighter than *(6)... actually we
                        * want unary to be looser than ^ but tighter than *.
                        * Using 6.5 isn't possible in int; use a scheme:
                        * unary parses rhs at prec 7 (so ^ binds), but a
                        * subsequent ^ at the unary level re-enters. Simpler:
                        * handle unary INSIDE the pratt loop as a pseudo-op. */

static pd_Reg parse_expr_prec(pd_Parser *p, int minPrec) {
    /* Prefix: handle unary +/- here so precedence interacts correctly.
     * Count consecutive +/- and remember if net-negate. */
    int negate = 0;
    while (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len == 1 &&
           (pd_cur(p)->text[0] == '+' || pd_cur(p)->text[0] == '-')) {
        if (pd_cur(p)->text[0] == '-') negate ^= 1;
        pd_eat(p);
    }
    pd_Reg left = parse_primary(p);
    if (!p->ok) return left;
    /* apply unary negate AFTER the whole rhs (including ^) is parsed,
     * by deferring: we wrap at the end. But ^ must bind tighter than unary.
     * Trick: parse the rhs at minPrec, but allow ^ to bind by giving the
     * negation a binding power of UNARY_PREC. Since we already consumed
     * the prefix, the pratt loop below will consume ^ at prec>=minPrec.
     * To make -2^2 = -(2^2), we need the ^ to be consumed as part of `left`
     * BEFORE applying negate. The pratt loop does that: it folds ^ into
     * left. Then we negate the combined result. So just run the loop then
     * negate at the end. */
    for (;;) {
        pd_Tok *t = pd_cur(p);
        if (t->kind != PD_TOK_PUNCT) break;
        /* plain '=' is assignment — stop (handled by caller). But two-char
         * operators like ==, <=, >=, != are comparisons, NOT assignments.
         * Only +=,-=,*=,/=,%=,^= are compound assignments. */
        if (t->len == 1 && t->text[0] == '=') break;
        if (t->len == 2 && t->text[1] == '=' &&
            (t->text[0]=='+'||t->text[0]=='-'||t->text[0]=='*'||t->text[0]=='/'||t->text[0]=='%')) {
            /* compound assignment — stop, handled by caller */
            break;
        }

        /* find matching binop */
        const BinOp *bo = NULL;
        for (int i = 0; BINOPS[i].tok; i++) {
            if (t->len == (int)strlen(BINOPS[i].tok) &&
                strncmp(t->text, BINOPS[i].tok, t->len) == 0) {
                bo = &BINOPS[i]; break;
            }
        }
        if (!bo) break;
        if (bo->prec < minPrec) break;
        pd_eat(p); /* consume operator */
        int nextMin = bo->rightAssoc ? bo->prec : bo->prec + 1;
        pd_Reg right = parse_expr_prec(p, nextMin);
        if (!p->ok) return left;
        pd_Reg out = pd_new_local(p->b);
        pd_emit2(p->b, bo->op, out, left, right);
        left = out;
    }
    /* apply deferred unary negate now, after ^ has bound into `left` */
    if (negate) {
        pd_Reg out = pd_new_local(p->b);
        pd_emit1(p->b, PD_NEGMOV, out, left);
        left = out;
    }
    return left;
}

/* public entry: parse full expression */
pd_Reg pd_parse_expr(pd_Parser *p) { return parse_expr_prec(p, 0); }

/* parse_unary folded into parse_expr_prec above (so unary minus has the
 * correct precedence relative to ^). */

/* ---- primary: number, ident, (expr), call, array access ---- */
static pd_Reg parse_primary(pd_Parser *p) {
    pd_Tok *t = pd_eat(p);
    p->lastLValue = NULL;
    p->lastLValueIsArrayIndex = 0;
    /* address-of prefix: &ident (EVAL pass-by-reference). For now, returns
     * the variable's value; full pointer semantics come with host arrays. */
    if (t->kind == PD_TOK_PUNCT && t->len==1 && t->text[0]=='&') {
        /* parse the following identifier as a normal primary */
        return parse_primary(p);
    }
    /* string-prefix dollar-ident or dollar-string: EVAL string arg. Return 0. */
    if (t->kind == PD_TOK_PUNCT && t->len==1 && t->text[0] == 0x24) {
        if (pd_cur(p)->kind == PD_TOK_IDENT || pd_cur(p)->kind == PD_TOK_STRING) pd_eat(p);
        pd_Reg out = pd_new_local(p->b);
        pd_Reg z = pd_new_const(p->b, 0.0);
        pd_emit1(p->b, PD_MOV, out, z);
        return out;
    }
    if (t->kind == PD_TOK_NUMBER) {
        pd_Reg c = pd_new_const(p->b, t->num);
        pd_Reg out = pd_new_local(p->b);
        pd_emit1(p->b, PD_MOV, out, c);
        return out;
    }
    if (t->kind == PD_TOK_STRING) {
        /* string literal — store as STR reg. For now just returns a const=0
         * placeholder; full string handling (printf args) comes later. */
        pd_Reg out = pd_new_local(p->b);
        pd_Reg zero = pd_new_const(p->b, 0.0);
        pd_emit1(p->b, PD_MOV, out, zero);
        (void)t;
        return out;
    }
    if (t->kind == PD_TOK_PUNCT && t->len == 1 && t->text[0] == '(') {
        pd_Reg r = parse_expr_prec(p, 0);
        if (!pd_expect_punct(p, ")")) return r;
        return r;
    }
    if (t->kind == PD_TOK_IDENT) {
        char name[40];
        size_t nl = t->len < sizeof(name) ? t->len : sizeof(name)-1;
        memcpy(name, t->text, nl); name[nl] = 0;

        /* parameterless builtins: RND, NRND */
        if (strcmp(name, "RND") == 0) {
            pd_Reg out = pd_new_local(p->b);
            pd_emit0(p->b, PD_RND, out);
            return out;
        }
        if (strcmp(name, "NRND") == 0) {
            pd_Reg out = pd_new_local(p->b);
            pd_emit0(p->b, PD_NRND, out);
            return out;
        }

        /* function call? look ahead for '(' */
        if (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len == 1 && pd_cur(p)->text[0] == '(') {
            /* collect args */
            pd_eat(p); /* ( */
            pd_Reg args[16]; int nArgs = 0;
            if (!(pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len == 1 && pd_cur(p)->text[0] == ')')) {
                for (;;) {
                    if (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len == 1 && pd_cur(p)->text[0] == ',') {
                        /* blank arg → 0 */
                        args[nArgs++] = pd_new_const(p->b, 0.0);
                        pd_eat(p);
                        continue;
                    }
                    args[nArgs++] = parse_expr_prec(p, 0);
                    if (!p->ok) { return args[nArgs-1]; }
                    if (accept_punct(p, ",")) {
                        /* if next is ) it was a trailing comma - treat as blank */
                        if (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len == 1 && pd_cur(p)->text[0] == ')') {
                            args[nArgs++] = pd_new_const(p->b, 0.0);
                            break;
                        }
                        continue;
                    }
                    break;
                }
            }
            if (!pd_expect_punct(p, ")")) { pd_Reg z = pd_new_const(p->b,0); return z; }

            /* resolve function symbol (try exact arity, else overload) */
            pd_Sym *s = sym_find(p, name, nArgs);
            if (!s) s = sym_find_name(p, name);
            if (!s) {
                /* unknown function — EVAL is lenient: emit a CALL to an
                 * unresolved function (aux=-1 → returns 0 at runtime). This
                 * lets scripts with unregistered host funcs still compile. */
                pd_Reg out = pd_new_local(p->b);
                size_t idx = p->b->nInstr;
                if (pd_emit(p->b, PD_CALL, out, args, nArgs) == (size_t)-1) {
                    pd_error(p, "emit fail"); return out;
                }
                p->b->instr[idx].aux = -1; /* unresolved */
                return out;
            }
            if (s->kind == PD_SYM_BUILTIN) {
                pd_Op op = (pd_Op)s->reg.off;
                pd_Reg out = pd_new_local(p->b);
                if (nArgs == 1) pd_emit1(p->b, op, out, args[0]);
                else if (nArgs == 2) pd_emit2(p->b, op, out, args[0], args[1]);
                else { pd_error(p, "bad arg count"); }
                return out;
            }
            if (s->kind == PD_SYM_EXT_FUNC || s->kind == PD_SYM_FUNC) {
                pd_Reg out = pd_new_local(p->b);
                size_t idx = p->b->nInstr;
                if (pd_emit(p->b, PD_CALL, out, args, nArgs) == (size_t)-1) {
                    pd_error(p, "emit fail"); return out;
                }
                if (s->kind == PD_SYM_FUNC) {
                    p->b->instr[idx].aux = s->funcIdx;  /* user function index */
                } else {
                    /* external (host) function: encode host index in aux as
                     * (-1000 - idx) to distinguish from user funcs (>=0) */
                    p->b->instr[idx].aux = -1000 - s->funcIdx;
                }
                return out;
            }
            /* variable used as function — error or 0 */
            pd_error(p, "not a function");
            return pd_new_const(p->b, 0.0);
        }

        /* array access? name[expr] or name[i0][i1]... (multi-dim, flattened).
         * For buf3d[5][3][2]: buf3d[a][b][c] => int(a)*3*2 + int(b)*2 + c.
         * Inner indices are truncated toward 0; the last index is used raw. */
        if (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len == 1 && pd_cur(p)->text[0] == '[') {
            pd_Sym *s = sym_find_name(p, name);
            if (!s) s = declare_local(p, name);
            pd_Reg idxs[8]; int nd = 0;
            while (accept_punct(p, "[")) {
                pd_Reg ix = parse_expr_prec(p, 0);
                if (!p->ok) return ix;
                if (!pd_expect_punct(p, "]")) return ix;
                if (nd < 8) idxs[nd++] = ix;
            }
            /* flatten: flat = sum_d trunc(idxs[d]) * product(dims[d+1..nDims-1]) */
            pd_Reg flat;
            if (nd > 1) {
                pd_Reg acc = pd_new_const(p->b, 0.0);
                for (int d = 0; d < nd; d++) {
                    pd_Reg term = idxs[d];
                    /* truncate inner indices toward 0 (EVAL array indices) */
                    if (d < nd - 1) {
                        pd_Reg tr = pd_new_local(p->b);
                        pd_emit1(p->b, PD_ROUND0, tr, term);
                        term = tr;
                    }
                    /* multiply by product of remaining dims (using known sizes) */
                    int stride = 1;
                    int symDims = (s->nDims > 0) ? s->nDims : nd;
                    for (int e = d + 1; e < symDims; e++) stride *= s->dims[e];
                    if (stride != 1) {
                        pd_Reg st = pd_new_const(p->b, (double)stride);
                        pd_Reg mul = pd_new_local(p->b);
                        pd_emit2(p->b, PD_TIMES, mul, term, st);
                        term = mul;
                    }
                    pd_Reg sum = pd_new_local(p->b);
                    pd_emit2(p->b, PD_PLUS, sum, acc, term);
                    acc = sum;
                }
                flat = acc;
            } else {
                flat = idxs[0];
            }
            pd_Reg out = pd_new_local(p->b);
            size_t ii = pd_emit2(p->b, PD_PEEK, out, s->reg, flat);
            p->b->instr[ii].aux = s->arraySize;
            /* remember lvalue info for assignment */
            p->lastLValue = s;
            p->lastLValueIsArrayIndex = 1;
            p->lastArrayIdx = flat;
            return out;
        }

        /* plain variable reference */
        pd_Sym *s = sym_find_name(p, name);
        if (!s) {
            /* auto-declare local */
            s = declare_local(p, name);
        }
        /* scalar statics are PD_SYM_ARRAY with arraySize==0 → treat as scalar */
        if (s->kind == PD_SYM_CONST || s->kind == PD_SYM_VAR || s->kind == PD_SYM_PARAM ||
            s->kind == PD_SYM_EXT_VAR ||
            (s->kind == PD_SYM_ARRAY && s->arraySize == 0)) {
            /* remember lvalue for potential assignment */
            if (s->kind == PD_SYM_VAR || s->kind == PD_SYM_PARAM || s->kind == PD_SYM_EXT_VAR ||
                (s->kind == PD_SYM_ARRAY && s->arraySize == 0)) {
                p->lastLValue = s;
                p->lastLValueIsArrayIndex = 0;
            }
            pd_Reg out = pd_new_local(p->b);
            pd_emit1(p->b, PD_MOV, out, s->reg);
            return out;
        }
        /* function referenced without call — treat as 0 */
        pd_Reg out = pd_new_local(p->b);
        pd_Reg z = pd_new_const(p->b, 0.0);
        pd_emit1(p->b, PD_MOV, out, z);
        return out;
    }
    pd_error(p, "unexpected token in expression");
    return pd_new_const(p->b, 0.0);
}

/* ---- statement parser (recursive descent) ---- */
/* Forward: parse a block { ... } or single statement */
static void parse_block_or_stmt(pd_Parser *p);
static void parse_if(pd_Parser *p);
static void parse_while(pd_Parser *p);
static void parse_for(pd_Parser *p);
static void parse_do_while(pd_Parser *p);

/* parse an expression-statement, handling assignment specially.
 * Returns 1 if the parsed statement was a "value" expression (the EVAL
 * convention: the last value-expression without ';' is the return value). */
static int parse_expr_stmt(pd_Parser *p) {
    /* could be: lvalue = expr   or   expr */
    /* We must detect assignment BEFORE consuming the lvalue so we can target
     * the symbol's real storage. Strategy: peek — if pattern is IDENT '=' or
     * IDENT '[' ... ']' '=' or IDENT <compound-assign>, parse lvalue specially. */
    pd_Tok *t = pd_cur(p);
    int isAssign = 0;
    pd_Sym *lvSym = NULL;
    int lvIsArray = 0;
    pd_Op assignOp = PD_MOV;

    /* peek: IDENT followed by assign op (possibly after [expr]) */
    if (t->kind == PD_TOK_IDENT) {
        /* save state to backtrack */
        size_t save = p->tok;
        char name[40]; size_t nl = t->len < sizeof(name) ? t->len : sizeof(name)-1;
        memcpy(name, t->text, nl); name[nl] = 0;
        /* skip ident */
        p->tok++;
        if (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]=='[') {
            /* array index: skip all [..] dimensions to find the assign op. */
            while (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]=='[') {
                p->tok++; /* [ */
                int depth=1;
                while (pd_cur(p)->kind != PD_TOK_EOF && depth>0) {
                    if (pd_cur(p)->kind==PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]=='[') depth++;
                    else if (pd_cur(p)->kind==PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]==']') depth--;
                    p->tok++;
                }
            }
        }
        pd_Tok *nt = pd_cur(p);
        /* assignment ops: '=' (len1) or '+=' '-=' '*=' '/=' '%=' '^=' (len2,
         * second char '=' and first char in +-* /%^). NOT '==' '<=' '>=' '!=' */
        if (nt->kind == PD_TOK_PUNCT &&
            ((nt->len==1 && nt->text[0]=='=') ||
             (nt->len==2 && nt->text[1]=='=' &&
              (nt->text[0]=='+'||nt->text[0]=='-'||nt->text[0]=='*'||
               nt->text[0]=='/'||nt->text[0]=='%'||nt->text[0]=='^')))) {
            isAssign = 1;
            for (int i = 0; ASSIGNOPS[i].tok; i++) {
                if (nt->len==(int)strlen(ASSIGNOPS[i].tok) && strncmp(nt->text,ASSIGNOPS[i].tok,nt->len)==0) {
                    assignOp = ASSIGNOPS[i].op; break;
                }
            }
            /* check if it was array */
            int hadArray = 0;
            for (size_t k = save+1; k < p->tok; k++) {
                if (p->ts->toks[k].kind==PD_TOK_PUNCT && p->ts->toks[k].len==1 && p->ts->toks[k].text[0]=='[') { hadArray=1; break; }
            }
            lvIsArray = hadArray;
        }
        /* restore */
        p->tok = save;
        if (isAssign) {
            lvSym = sym_find_name(p, name);
            if (!lvSym) lvSym = declare_local(p, name);
        }
    }

    if (isAssign) {
        /* re-parse lvalue to consume tokens but discard its value-reg;
         * we will store directly into lvSym->reg */
        p->lastLValue = NULL;
        pd_Reg left = pd_parse_expr(p);
        (void)left;
        /* consume assign op */
        pd_eat(p);
        pd_Reg value = pd_parse_expr(p);
        if (!p->ok) return 0;
        accept_punct(p, ";");
        if (lvIsArray) {
            /* POKE family: out = array base (lvSym->reg), in[0]=value, in[1]=idx */
            pd_Op pop = (assignOp==PD_MOV) ? PD_POKE :
                        (assignOp==PD_PLUS)?PD_POKEPLUS:
                        (assignOp==PD_MINUS)?PD_POKEMINUS:
                        (assignOp==PD_TIMES)?PD_POKETIMES:
                        (assignOp==PD_SLASH)?PD_POKESLASH:PD_POKEPERC;
            /* we need the index reg; re-derive: lastArrayIdx was set by parse_primary */
            size_t ii = pd_emit2(p->b, pop, lvSym->reg, value, p->lastArrayIdx);
            p->b->instr[ii].aux = lvSym->arraySize;
        } else {
            if (assignOp == PD_MOV) {
                pd_emit1(p->b, PD_MOV, lvSym->reg, value);
            } else {
                pd_Reg cur = pd_new_local(p->b);
                pd_emit1(p->b, PD_MOV, cur, lvSym->reg);
                pd_Reg combined = pd_new_local(p->b);
                pd_emit2(p->b, assignOp, combined, cur, value);
                pd_emit1(p->b, PD_MOV, lvSym->reg, combined);
                /* assignment-expression value is the combined result */
                pd_Reg valResult = pd_new_local(p->b);
                pd_emit1(p->b, PD_MOV, valResult, lvSym->reg);
                p->lastValueReg = valResult;
                return 0;
            }
        }
        /* assignment is a value expression in EVAL: its value is the RHS */
        pd_Reg valResult = pd_new_local(p->b);
        pd_emit1(p->b, PD_MOV, valResult, value);
        p->lastValueReg = valResult;
        return 0;
    }

    /* postfix ++ / -- as a STATEMENT: "IDENT++" or "IDENT--" (EVAL form).
     * The lexer splits ++ into two '+', so detect: IDENT followed by two
     * identical + or - chars with nothing else between. Must check BEFORE
     * the expression parser treats the first + as a binary operator. */
    if (t->kind == PD_TOK_IDENT) {
        /* look ahead: IDENT then '+' '+' or '-' '-' */
        size_t a1 = p->tok + 1;
        if (a1 + 1 < p->ts->nToks &&
            p->ts->toks[a1].kind == PD_TOK_PUNCT && p->ts->toks[a1].len==1 &&
            (p->ts->toks[a1].text[0]=='+' || p->ts->toks[a1].text[0]=='-') &&
            p->ts->toks[a1+1].kind == PD_TOK_PUNCT && p->ts->toks[a1+1].len==1 &&
            p->ts->toks[a1+1].text[0] == p->ts->toks[a1].text[0]) {
            /* it's IDENT++ or IDENT-- */
            char name[40]; size_t nl = t->len<sizeof(name)?t->len:sizeof(name)-1;
            memcpy(name, t->text, nl); name[nl]=0;
            char c1 = p->ts->toks[a1].text[0];
            pd_eat(p); /* IDENT */
            pd_eat(p); pd_eat(p); /* ++ or -- */
            pd_Sym *lv = sym_find_name(p, name);
            if (!lv) lv = declare_local(p, name);
            if (lv->kind == PD_SYM_VAR || lv->kind == PD_SYM_PARAM || lv->kind == PD_SYM_EXT_VAR ||
                (lv->kind == PD_SYM_ARRAY && lv->arraySize == 0)) {
                pd_Op op = (c1=='+') ? PD_PLUS : PD_MINUS;
                pd_Reg one = pd_new_const(p->b, 1.0);
                pd_Reg cur = pd_new_local(p->b);
                pd_Reg combined = pd_new_local(p->b);
                pd_emit1(p->b, PD_MOV, cur, lv->reg);
                pd_emit2(p->b, op, combined, cur, one);
                pd_emit1(p->b, PD_MOV, lv->reg, combined);
            } else if (lv->kind == PD_SYM_ARRAY) {
                /* array element increment not supported in postfix; ignore */
            }
            accept_punct(p, ";");
            return 0;
        }
    }

    pd_Reg left = pd_parse_expr(p);
    if (!p->ok) return 0;
    /* bare expression: this is the "value" form. */
    p->lastValueReg = left;
    accept_punct(p, ";");
    return 1;
}

static void parse_if(pd_Parser *p) {
    if (!pd_expect_punct(p, "(")) return;
    pd_Reg cond = pd_parse_expr(p);
    if (!pd_expect_punct(p, ")")) return;
    size_t jElse = pd_emit1(p->b, PD_IF0, pdR(PD_FAM_VOID,0), cond);
    parse_block_or_stmt(p);
    if (accept_ident(p, "ELSE")) {
        size_t jEnd = pd_emit0(p->b, PD_GOTO, pdR(PD_FAM_VOID,0));
        size_t elseLbl = pd_label_here(p->b);
        pd_patch_goto_target(p->b, jElse, elseLbl);
        parse_block_or_stmt(p);
        size_t endLbl = pd_label_here(p->b);
        pd_patch_goto_target(p->b, jEnd, endLbl);
    } else {
        size_t endLbl = pd_label_here(p->b);
        pd_patch_goto_target(p->b, jElse, endLbl);
    }
}

static void parse_while(pd_Parser *p) {
    size_t top = pd_label_here(p->b);
    if (!pd_expect_punct(p, "(")) return;
    pd_Reg cond = pd_parse_expr(p);
    if (!pd_expect_punct(p, ")")) return;
    size_t jEnd = pd_emit1(p->b, PD_IF0, pdR(PD_FAM_VOID,0), cond);
    int saveBrk = p->breakLabel, saveCont = p->contLabel;
    /* use a sentinel for breakLabel; we patch break GOTOs to endLbl later.
     * sentinel = -(jEnd+1) so it's distinguishable and never a real index. */
    int brkSentinel = -(int)jEnd - 1;
    p->breakLabel = brkSentinel; p->contLabel = (int)top;
    parse_block_or_stmt(p);
    p->breakLabel = saveBrk; p->contLabel = saveCont;
    size_t goBack = pd_emit0(p->b, PD_GOTO, pdR(PD_FAM_VOID,0));
    pd_patch_goto_target(p->b, goBack, top);
    size_t endLbl = pd_label_here(p->b);
    pd_patch_goto_target(p->b, jEnd, endLbl);
    /* repoint break GOTOs (which used the sentinel as out.off) to endLbl */
    for (size_t i = top; i < p->b->nInstr; i++) {
        if (p->b->instr[i].op == PD_GOTO && p->b->instr[i].out.fam == PD_FAM_LABEL &&
            (int)p->b->instr[i].out.off == brkSentinel) {
            p->b->instr[i].out.off = (uint32_t)endLbl;
        }
    }
}

static void parse_for(pd_Parser *p) {
    if (!pd_expect_punct(p, "(")) return;
    /* init */
    if (!accept_punct(p, ";")) {
        parse_expr_stmt(p);
        /* eat ; if not consumed */
    }
    accept_punct(p, ";");
    size_t top = pd_label_here(p->b);
    pd_Reg cond = pd_new_const(p->b, 1.0); /* default true */
    if (!(pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]==';')) {
        cond = pd_parse_expr(p);
    }
    if (!pd_expect_punct(p, ";")) return;
    size_t jEnd = pd_emit1(p->b, PD_IF0, pdR(PD_FAM_VOID,0), cond);
    /* iteration expression: it comes before ')' in source, but must execute
     * AFTER the body. We save the token range, parse the body first, then
     * re-enter the parser at the saved iter tokens. */
    size_t iterTokStart = p->tok;
    /* skip iter expr tokens up to ')' */
    int pdepth = 0;
    while (pd_cur(p)->kind != PD_TOK_EOF &&
           !(pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 &&
             pd_cur(p)->text[0]==')' && pdepth==0)) {
        if (pd_cur(p)->kind==PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]=='(') pdepth++;
        else if (pd_cur(p)->kind==PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]==')') pdepth--;
        p->tok++;
    }
    size_t iterTokEnd = p->tok;
    if (!pd_expect_punct(p, ")")) return;
    /* parse body */
    int saveBrk = p->breakLabel, saveCont = p->contLabel;
    int brkSentinel = -(int)jEnd - 1;
    p->breakLabel = brkSentinel;
    /* continue jumps to the iter expr, which we parse next; record a placeholder
     * label (instr index) that we patch after emitting iter */
    size_t contPlaceholder = pd_label_here(p->b); /* continue target = iter */
    p->contLabel = (int)contPlaceholder;
    parse_block_or_stmt(p);
    p->breakLabel = saveBrk; p->contLabel = saveCont;
    /* emit iter expression IR now (after body). Re-enter parser at saved tokens. */
    size_t iterLbl = pd_label_here(p->b);
    /* any continue GOTO that targeted contPlaceholder should land here; since
     * we used pd_label_here for contPlaceholder (= its instr index) and
     * continue-jumps stored that index directly, they already point at the
     * right place IF iterLbl == contPlaceholder. They're equal only if no
     * instructions were emitted between. In general they differ, so we must
     * patch. But continue-jumps used GOTO with out.off = contPlaceholder.
     * We scan and repoint them to iterLbl. */
    for (size_t k = contPlaceholder; k < p->b->nInstr; k++) {
        if (p->b->instr[k].op == PD_GOTO &&
            p->b->instr[k].out.fam == PD_FAM_LABEL &&
            p->b->instr[k].out.off == contPlaceholder) {
            p->b->instr[k].out.off = (uint32_t)iterLbl;
        }
    }
    if (iterTokEnd > iterTokStart) {
        size_t savedTok = p->tok;
        p->tok = iterTokStart;
        parse_expr_stmt(p);
        p->tok = savedTok;
    }
    size_t goBack = pd_emit0(p->b, PD_GOTO, pdR(PD_FAM_VOID,0));
    pd_patch_goto_target(p->b, goBack, top);
    size_t endLbl = pd_label_here(p->b);
    pd_patch_goto_target(p->b, jEnd, endLbl);
    /* repoint break GOTOs to endLbl */
    for (size_t k = top; k < p->b->nInstr; k++) {
        if (p->b->instr[k].op == PD_GOTO && p->b->instr[k].out.fam == PD_FAM_LABEL &&
            (int)p->b->instr[k].out.off == brkSentinel) {
            p->b->instr[k].out.off = (uint32_t)endLbl;
        }
    }
}

static void parse_do_while(pd_Parser *p) {
    size_t top = pd_label_here(p->b);
    int saveBrk = p->breakLabel, saveCont = p->contLabel;
    size_t contLbl = pd_label_here(p->b);
    p->breakLabel = PD_NO_LOOP; p->contLabel = (int)contLbl;
    parse_block_or_stmt(p);
    p->breakLabel = saveBrk; p->contLabel = saveCont;
    if (!accept_ident(p, "WHILE")) { pd_error(p, "expected WHILE"); return; }
    if (!pd_expect_punct(p, "(")) return;
    pd_Reg cond = pd_parse_expr(p);
    if (!pd_expect_punct(p, ")")) return;
    accept_punct(p, ";");
    /* if cond, goto top */
    size_t jBack = pd_emit1(p->b, PD_IF1, pdR(PD_FAM_VOID,0), cond);
    pd_patch_goto_target(p->b, jBack, top);
}

static void parse_block_or_stmt(pd_Parser *p) {
    if (accept_punct(p, "{")) {
        while (p->ok && !(pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]=='}')) {
            pd_parse_stmt(p);
        }
        pd_expect_punct(p, "}");
        return;
    }
    pd_parse_stmt(p);
}

/* Evaluate a constant expression: number, enum/const name, or simple
 * arithmetic on them (n*3, N+1, etc). Used for array sizes & enum values.
 * Supports: NUMBER, CONST_NAME, (expr), and + - * / between them. */
static long eval_const_expr(pd_Parser *p) {
    pd_Tok *t = pd_cur(p);
    long v;
    if (t->kind == PD_TOK_PUNCT && t->len==1 && t->text[0]=='(') {
        pd_eat(p);
        v = eval_const_expr(p);
        if (!p->ok) return -1;
        if (!pd_expect_punct(p, ")")) return -1;
        t = pd_cur(p);
    } else if (t->kind == PD_TOK_NUMBER) {
        pd_eat(p);
        v = (long)t->num;
        t = pd_cur(p);
    } else if (t->kind == PD_TOK_IDENT) {
        char name[40]; size_t nl = t->len < sizeof(name) ? t->len : sizeof(name)-1;
        memcpy(name, t->text, nl); name[nl] = 0;
        pd_Sym *s = sym_find_name(p, name);
        if (s && s->kind == PD_SYM_CONST) {
            pd_eat(p);
            v = (long)p->b->consts[s->reg.off / 8];
        } else { pd_error(p, "expected constant"); return -1; }
        t = pd_cur(p);
    } else { pd_error(p, "expected constant"); return -1; }
    /* handle trailing binary ops: * / + - */
    while (t->kind == PD_TOK_PUNCT && t->len==1 &&
           (t->text[0]=='*'||t->text[0]=='/'||t->text[0]=='+'||t->text[0]=='-')) {
        char op = t->text[0];
        pd_eat(p);
        long r = eval_const_expr(p);
        if (!p->ok) return -1;
        if (op=='*') v *= r;
        else if (op=='/') v /= r;
        else if (op=='+') v += r;
        else v -= r;
        t = pd_cur(p);
    }
    return v;
}

/* enum { NAME, NAME=expr, NAME, ... }
 * Each NAME becomes a compile-time constant. Implicit value = prev+1, start 0. */
static void parse_enum(pd_Parser *p) {
    if (!pd_expect_punct(p, "{")) return;
    long nextVal = 0;
    for (;;) {
        pd_Tok *nt = pd_eat(p);
        if (nt->kind != PD_TOK_IDENT) { pd_error(p, "enum: expected name"); return; }
        char name[40]; size_t nl = nt->len < sizeof(name) ? nt->len : sizeof(name)-1;
        memcpy(name, nt->text, nl); name[nl] = 0;
        if (accept_punct(p, "=")) {
            nextVal = eval_const_expr(p);
            if (!p->ok) return;
        }
        pd_Sym *s = sym_add(p, name, PD_SYM_CONST);
        s->reg = pd_new_const(p->b, (double)nextVal);
        nextVal++;
        if (accept_punct(p, ",")) continue;
        break;
    }
    pd_expect_punct(p, "}");
}

/* static name[size], name2[size2], name3, ... ;
 * name[size] allocates an array of `size` doubles in global storage.
 * name (no brackets) allocates a single double (scalar static).
 * Optional initializer: = expr (scalar only, or first element). */
static void parse_static(pd_Parser *p) {
    for (;;) {
        pd_Tok *nt = pd_eat(p);
        if (nt->kind != PD_TOK_IDENT) { pd_error(p, "static: expected name"); return; }
        char name[40]; size_t nl = nt->len < sizeof(name) ? nt->len : sizeof(name)-1;
        memcpy(name, nt->text, nl); name[nl] = 0;
        int isArr = 0; long arrSize = 1;
        int dims[8]; int nDims = 0;
        /* N-dimensional array: name[d0][d1]...[dk] -> flat storage of d0*d1*..*dk.
         * Each bracket pair holds a constant size expression. */
        if (accept_punct(p, "[")) {
            isArr = 1;
            for (;;) {
                long d = eval_const_expr(p);
                if (!p->ok) return;
                if (d <= 0) { pd_error(p, "invalid array dimension"); return; }
                arrSize *= d;
                if (nDims < 8) dims[nDims++] = (int)d;
                if (!pd_expect_punct(p, "]")) return;
                if (!accept_punct(p, "[")) break;
            }
        }
        /* allocate from global storage */
        if (!p->globals) {
            p->globalsCap = 256;
            p->globals = calloc(p->globalsCap, sizeof(double));
        }
        while (p->nGlobals + arrSize > p->globalsCap) {
            p->globalsCap *= 2;
            p->globals = realloc(p->globals, p->globalsCap * sizeof(double));
            memset(p->globals + p->globalsCap/2, 0, p->globalsCap/2 * sizeof(double));
        }
        size_t baseOff = p->nGlobals * 8;
        p->nGlobals += arrSize;
        pd_Sym *s = sym_add(p, name, PD_SYM_ARRAY);
        s->reg = pdR(PD_FAM_GLOBAL, (uint32_t)baseOff);
        s->arraySize = isArr ? (int)arrSize : 0;
        s->nDims = nDims;
        for (int di = 0; di < nDims; di++) s->dims[di] = dims[di];

        /* initializer: = expr (scalar) or = { expr, expr, ... } (array list).
         * Array lists like {0} or {1,2,3} initialize elements; {0} (single
         * zero) zero-fills the whole array. */
        if (accept_punct(p, "=")) {
            if (accept_punct(p, "{")) {
                /* array initializer list */
                size_t elem = 0;
                if (!(pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]=='}')) {
                    for (;;) {
                        pd_Reg v = pd_parse_expr(p);
                        if (!p->ok) return;
                        /* store into global[elem] */
                        pd_Reg slot = pdR(PD_FAM_GLOBAL, (uint32_t)(baseOff + elem*8));
                        pd_emit1(p->b, PD_MOV, slot, v);
                        elem++;
                        if (accept_punct(p, ",")) continue;
                        break;
                    }
                }
                pd_expect_punct(p, "}");
            } else {
                pd_Reg v = pd_parse_expr(p);
                if (!p->ok) return;
                pd_emit1(p->b, PD_MOV, s->reg, v);
            }
        }
        if (accept_punct(p, ",")) continue;
        break;
    }
}

/* Forward */
static int try_parse_function_def(pd_Parser *p);

/* Check if the current position is a function definition:
 *   NAME ( params ) { body }
 *   ( params ) { body }      <- anonymous (main function)
 * If so, parse it (into a sub-program in p->funcs[]) and return 1.
 * Otherwise return 0 and leave the token stream unchanged. */
static int try_parse_function_def(pd_Parser *p) {
    size_t save = p->tok;
    pd_Tok *t = pd_cur(p);

    /* optional name */
    char name[40] = {0};
    if (t->kind == PD_TOK_IDENT) {
        /* must not be a keyword handled above (already consumed those) */
        size_t nl = t->len < sizeof(name) ? t->len : sizeof(name)-1;
        memcpy(name, t->text, nl); name[nl] = 0;
        p->tok++;
    }
    /* expect '(' */
    if (!(pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]=='(')) {
        p->tok = save; return 0;
    }
    /* scan to matching ')' */
    int depth = 0;
    size_t scan = p->tok;
    while (pd_cur_at(scan)->kind != PD_TOK_EOF) {
        const pd_Tok *st = pd_cur_at(scan);
        if (st->kind == PD_TOK_PUNCT && st->len==1 && st->text[0]=='(') depth++;
        else if (st->kind == PD_TOK_PUNCT && st->len==1 && st->text[0]==')') {
            depth--;
            if (depth == 0) break;
        }
        scan++;
    }
    if (depth != 0) { p->tok = save; return 0; }
    /* after ')', expect '{' (function body) — if not, it's a call expression */
    const pd_Tok *after = pd_cur_at(scan + 1);
    if (!(after->kind == PD_TOK_PUNCT && after->len==1 && after->text[0]=='{')) {
        p->tok = save; return 0;
    }

    /* It's a function definition. Parse it. */
    /* consume name (already consumed if named) and '(' */
    p->tok = save;
    if (name[0]) pd_eat(p); /* name */
    pd_eat(p); /* ( */

    /* set up a sub-builder for this function */
    pd_Builder *savedB = p->b;
    pd_Builder fb; pd_builder_init(&fb);
    p->b = &fb;

    /* save & reset symbol table scope: parameters are local to this function.
     * Simplest: save nSyms, parse params (adding PARAM symbols), parse body,
     * then restore nSyms. Locals declared inside still leak to the saved
     * scope, but since each function is parsed fully before restoring, and
     * we look up symbols innermost-first, this is acceptable. */
    int savedNSyms = p->nSyms;
    int savedBreak = p->breakLabel, savedCont = p->contLabel;
    p->breakLabel = PD_NO_LOOP; p->contLabel = PD_NO_LOOP;

    /* pre-allocate function slot & register symbol (for recursion).
     * If prescan_functions already registered this name, reuse its slot. */
    pd_Sym *existing = name[0] ? sym_find_name(p, name) : NULL;
    int fidx;
    pd_Sym *fnSym = NULL;
    if (existing && existing->kind == PD_SYM_FUNC && existing->funcIdx >= 0) {
        /* reuse prescan-allocated slot */
        fidx = existing->funcIdx;
        fnSym = existing;
    } else {
        if (!p->funcs) {
            p->nFuncsAlloc = 16;
            p->funcs = calloc(p->nFuncsAlloc, sizeof(pd_Program));
        }
        if (p->nFuncs >= p->nFuncsAlloc) {
            p->nFuncsAlloc *= 2;
            p->funcs = realloc(p->funcs, p->nFuncsAlloc * sizeof(pd_Program));
        }
        fidx = (int)p->nFuncs;
        memset(&p->funcs[fidx], 0, sizeof(pd_Program));
        p->nFuncs++;
        if (name[0]) {
            fnSym = sym_add(p, name, PD_SYM_FUNC);
            if (fnSym) { fnSym->funcIdx = fidx; fnSym->nParams = -1; }
        }
    }

    /* parse parameter list */
    int nParams = 0;
    pd_Reg paramRegs[16];
    if (!(pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]==')')) {
        for (;;) {
            /* EVAL parameter forms (Plan section 4):
             *   a        double
             *   &a       pass-by-reference (double*)   -- prefix skipped, treated as double
             *   $a       string arg (char*)            -- prefix skipped, treated as double
             *   a[n]     array                          -- [..] skipped, treated as double
             *   a(,,)    function pointer               -- (..) skipped, treated as double
             * Optional C-style type prefix (double/void/...) is also allowed. */
            while (pd_cur(p)->kind == PD_TOK_IDENT &&
                   !(pd_cur_at(p->tok+1)->kind == PD_TOK_PUNCT &&
                     pd_cur_at(p->tok+1)->len==1 &&
                     (pd_cur_at(p->tok+1)->text[0]==',' || pd_cur_at(p->tok+1)->text[0]==')'))) {
                /* skip a type-prefix ident (e.g. "double x") — heuristic: an
                 * ident followed by another ident is a type+name */
                size_t nx = p->tok+1;
                if (pd_cur_at(nx)->kind != PD_TOK_IDENT) break;
                pd_eat(p);
            }
            if (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 &&
                (pd_cur(p)->text[0]=='&' || pd_cur(p)->text[0]=='$')) {
                pd_eat(p); /* consume & or $ prefix */
            }
            pd_Tok *pt = pd_eat(p);
            if (pt->kind != PD_TOK_IDENT) { pd_error(p, "expected param name"); goto restore; }
            char pname[40]; size_t pnl = pt->len < sizeof(pname)?pt->len:sizeof(pname)-1;
            memcpy(pname, pt->text, pnl); pname[pnl] = 0;
            /* array param: name[...] — skip the dimension(s) */
            if (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]=='[') {
                while (accept_punct(p, "[")) {
                    while (!(pd_cur(p)->kind==PD_TOK_EOF ||
                             (pd_cur(p)->kind==PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]==']'))) {
                        pd_eat(p);
                    }
                    pd_expect_punct(p, "]");
                }
            }
            /* function-pointer param: name(,,) — skip param-count parens */
            if (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]=='(') {
                int d=0;
                do {
                    if (pd_cur(p)->kind==PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]=='(') d++;
                    pd_eat(p);
                } while (d>0 && !(pd_cur(p)->kind==PD_TOK_EOF));
            }
            /* allocate a PARAM reg */
            paramRegs[nParams] = pdR(PD_FAM_PARAM, (uint32_t)(nParams * 8));
            pd_Sym *ps = sym_add(p, pname, PD_SYM_PARAM);
            ps->reg = paramRegs[nParams];
            nParams++;
            if (accept_punct(p, ",")) continue;
            break;
        }
    }
    if (fnSym) fnSym->nParams = nParams;
    if (!pd_expect_punct(p, ")")) goto restore;
    if (!pd_expect_punct(p, "{")) goto restore;

    /* parse body statements until } */
    p->lastValueReg = pd_new_const(p->b, 0.0);
    while (p->ok && !(pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]=='}')) {
        pd_parse_stmt(p);
        if (!p->ok) break;
    }
    pd_expect_punct(p, "}");
    /* implicit return of last value */
    pd_emit1(p->b, PD_RETURN, pdR(PD_FAM_VOID,0), p->lastValueReg);

restore:
    p->breakLabel = savedBreak; p->contLabel = savedCont;
    /* restore symbol scope but KEEP the function symbol (if newly added)
     * so the enclosing scope can call it. Parameters and locals are dropped.
     * If fnSym was pre-existing (from prescan), it's already in scope. */
    if (fnSym && fnSym >= &p->syms[savedNSyms] && fnSym < &p->syms[p->nSyms]) {
        /* fnSym was added during this function's parse (new) — keep it */
        if (fnSym != &p->syms[savedNSyms]) {
            p->syms[savedNSyms] = *fnSym;
        }
        p->nSyms = savedNSyms + 1;
    } else {
        /* fnSym pre-existed or NULL — just restore */
        p->nSyms = savedNSyms;
    }

    /* finalize the function into the pre-reserved p->funcs[fidx] slot.
     * If this was a reused prescan slot, nFuncs was already incremented. */
    if (p->ok) {
        pd_Program *fnp = &p->funcs[fidx];
        /* free any previous content (prescan left it zeroed) then finish */
        pd_builder_finish(p->b, fnp);
        fnp->nParams = nParams;
        fnp->globals = NULL;
        fnp->host = NULL;
        if (fnSym) fnSym->nParams = nParams;
    }
    p->b = savedB;
    return 1;
}

int pd_parse_stmt(pd_Parser *p) {
    pd_Tok *t = pd_cur(p);
    /* label definition: IDENT :  (EVAL goto labels). We accept and skip them;
     * goto support is partial (goto itself is a no-op for now). */
    if (t->kind == PD_TOK_IDENT) {
        size_t a1 = p->tok + 1;
        if (a1 < p->ts->nToks &&
            p->ts->toks[a1].kind == PD_TOK_PUNCT && p->ts->toks[a1].len==1 &&
            p->ts->toks[a1].text[0] == ':') {
            /* skip "label:" and optional ";" */
            pd_eat(p); /* IDENT */
            pd_eat(p); /* : */
            accept_punct(p, ";");
            return 1;
        }
    }
    if (t->kind == PD_TOK_IDENT) {
        if (accept_ident(p, "IF")) { parse_if(p); return 1; }
        if (accept_ident(p, "WHILE")) { parse_while(p); return 1; }
        if (accept_ident(p, "FOR")) { parse_for(p); return 1; }
        if (accept_ident(p, "DO")) { parse_do_while(p); return 1; }
        if (accept_ident(p, "ENUM")) { parse_enum(p); accept_punct(p, ";"); return 1; }
        if (accept_ident(p, "STATIC")) { parse_static(p); accept_punct(p, ";"); return 1; }
        /* function definition: NAME(params) { ... } */
        if (try_parse_function_def(p)) return 1;
        if (accept_ident(p, "RETURN")) {
            if (!(pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]==';')) {
                pd_Reg v = pd_parse_expr(p);
                pd_emit1(p->b, PD_RETURN, pdR(PD_FAM_VOID,0), v);
            } else {
                pd_Reg z = pd_new_const(p->b, 0.0);
                pd_emit1(p->b, PD_RETURN, pdR(PD_FAM_VOID,0), z);
            }
            accept_punct(p, ";");
            return 1;
        }
        if (accept_ident(p, "BREAK")) {
            if (p->breakLabel != PD_NO_LOOP) {
                pd_emit0(p->b, PD_GOTO, pdR(PD_FAM_LABEL,(uint32_t)p->breakLabel));
            }
            accept_punct(p, ";");
            return 1;
        }
        if (accept_ident(p, "CONTINUE")) {
            if (p->contLabel != PD_NO_LOOP) {
                pd_emit0(p->b, PD_GOTO, pdR(PD_FAM_LABEL,(uint32_t)p->contLabel));
            }
            accept_punct(p, ";");
            return 1;
        }
        if (accept_ident(p, "GOTO")) {
            /* GOTO label; */
            pd_Tok *lt = pd_eat(p);
            if (lt->kind != PD_TOK_IDENT) { pd_error(p, "GOTO needs label"); return 0; }
            /* We don't implement forward-goto label table yet; emit a GOTO
             * with a placeholder that must be patched. Simplified: skip. */
            accept_punct(p, ";");
            return 1;
        }
        /* enum / static handled at top-level only */
    }
    if (t->kind == PD_TOK_PUNCT && t->len==1 && t->text[0]==';') {
        pd_eat(p); return 1;  /* empty statement */
    }
    /* expression statement */
    parse_expr_stmt(p);
    return 1;
}

/* Pre-scan: walk the token stream and pre-register all named function
 * definitions so forward references work. Allocates func slots in order.
 * Does NOT consume tokens (restores position at end). */
static void prescan_functions(pd_Parser *p) {
    size_t savedTok = p->tok;
    p->tok = 0;
    while (pd_cur(p)->kind != PD_TOK_EOF) {
        pd_Tok *t = pd_cur(p);
        /* look for: IDENT ( ... ) {   (function def) */
        if (t->kind == PD_TOK_IDENT) {
            /* find '(' right after name (no operators) */
            size_t afterName = p->tok + 1;
            if (afterName < p->ts->nToks &&
                pd_cur_at(afterName)->kind == PD_TOK_PUNCT &&
                pd_cur_at(afterName)->len==1 && pd_cur_at(afterName)->text[0]=='(') {
                /* scan to matching ')' */
                int depth = 0; size_t s = afterName;
                while (pd_cur_at(s)->kind != PD_TOK_EOF) {
                    const pd_Tok *st = pd_cur_at(s);
                    if (st->kind==PD_TOK_PUNCT && st->len==1 && st->text[0]=='(') depth++;
                    else if (st->kind==PD_TOK_PUNCT && st->len==1 && st->text[0]==')') {
                        depth--;
                        if (depth==0) break;
                    }
                    s++;
                }
                /* after ')', is it '{'? */
                const pd_Tok *after = pd_cur_at(s+1);
                if (after->kind==PD_TOK_PUNCT && after->len==1 && after->text[0]=='{') {
                    /* it's a function def. count params (commas at depth 1) */
                    int nParams = 0; depth = 0;
                    for (size_t k = afterName; k <= s; k++) {
                        const pd_Tok *st = pd_cur_at(k);
                        if (st->kind==PD_TOK_PUNCT && st->len==1 && st->text[0]=='(') depth++;
                        else if (st->kind==PD_TOK_PUNCT && st->len==1 && st->text[0]==')') depth--;
                        else if (st->kind==PD_TOK_PUNCT && st->len==1 && st->text[0]==',' && depth==1) nParams++;
                    }
                    /* if there's content between ( and ), it's nParams+1 */
                    if (s > afterName) nParams++;
                    /* allocate slot */
                    if (!p->funcs) {
                        p->nFuncsAlloc = 16;
                        p->funcs = calloc(p->nFuncsAlloc, sizeof(pd_Program));
                    }
                    if (p->nFuncs >= p->nFuncsAlloc) {
                        p->nFuncsAlloc *= 2;
                        p->funcs = realloc(p->funcs, p->nFuncsAlloc*sizeof(pd_Program));
                    }
                    int fidx = (int)p->nFuncs;
                    memset(&p->funcs[fidx], 0, sizeof(pd_Program));
                    p->nFuncs++;
                    /* register symbol (only if not already) */
                    char name[40]; size_t nl = t->len<sizeof(name)?t->len:sizeof(name)-1;
                    memcpy(name, t->text, nl); name[nl]=0;
                    pd_Sym *existing = sym_find_name(p, name);
                    if (!existing) {
                        pd_Sym *fs = sym_add(p, name, PD_SYM_FUNC);
                        if (fs) { fs->nParams = nParams; fs->funcIdx = fidx; }
                    }
                }
            }
        }
        p->tok++;
    }
    p->tok = savedTok;
}

/* ---- top-level program parse ---- */
/* Detect & parse an anonymous main: () { body }. Returns 1 if parsed. */
static int try_parse_anon_main(pd_Parser *p) {
    /* pd_cur_at takes an ABSOLUTE token index, so peek at tok+1 / tok+2. */
    if (!(pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]=='(' &&
          pd_cur_at(p->tok+1)->kind == PD_TOK_PUNCT && pd_cur_at(p->tok+1)->len==1 && pd_cur_at(p->tok+1)->text[0]==')' &&
          pd_cur_at(p->tok+2)->kind == PD_TOK_PUNCT && pd_cur_at(p->tok+2)->len==1 && pd_cur_at(p->tok+2)->text[0]=='{'))
        return 0;
    pd_eat(p); /* ( */
    pd_eat(p); /* ) */
    pd_expect_punct(p, "{");
    while (p->ok && !(pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]=='}')) {
        pd_parse_stmt(p);
        if (!p->ok) break;
    }
    pd_expect_punct(p, "}");
    pd_emit1(p->b, PD_RETURN, pdR(PD_FAM_VOID,0), p->lastValueReg);
    return 1;
}

int pd_parse_program(pd_Parser *p) {
    /* The entry point. EVAL allows either:
     *   (a) bare statements at top level (implicit main), or
     *   (b) an anonymous main: () { ... }  optionally followed by named funcs.
     * The last bare expression (no trailing assignment) is the return value. */
    pd_Reg zero = pd_new_const(p->b, 0.0);
    p->lastValueReg = zero;

    /* pre-register all named functions so forward references resolve */
    prescan_functions(p);

    /* main loop: parse top-level statements. An anonymous main () {...}
     * may appear anywhere (after static/enum decls). Once it's parsed, we
     * emit the final return; subsequent statements are named func defs. */
    int sawMain = 0;
    while (p->ok && pd_cur(p)->kind != PD_TOK_EOF) {
        if (!sawMain && try_parse_anon_main(p)) {
            sawMain = 1;
            continue;
        }
        pd_parse_stmt(p);
        if (!p->ok) break;
    }
    if (!sawMain) {
        /* implicit main: emit final return of last value-expression */
        pd_emit1(p->b, PD_RETURN, pdR(PD_FAM_VOID,0), p->lastValueReg);
    }
    return p->ok;
}
