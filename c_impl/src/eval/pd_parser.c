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
                /* implicit declaration: create a local var (EVAL auto-declares) */
                s = declare_local(p, name);
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

        /* array access? name[expr] */
        if (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len == 1 && pd_cur(p)->text[0] == '[') {
            pd_eat(p); /* [ */
            pd_Reg idx = parse_expr_prec(p, 0);
            if (!pd_expect_punct(p, "]")) return idx;
            pd_Sym *s = sym_find_name(p, name);
            if (!s) s = declare_local(p, name);
            pd_Reg out = pd_new_local(p->b);
            size_t ii = pd_emit2(p->b, PD_PEEK, out, s->reg, idx);
            p->b->instr[ii].aux = s->arraySize;
            /* remember lvalue info for assignment */
            p->lastLValue = s;
            p->lastLValueIsArrayIndex = 1;
            p->lastArrayIdx = idx;
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
            /* array index: parse to find assign after ] */
            p->tok++; /* [ */
            int depth=1;
            while (pd_cur(p)->kind != PD_TOK_EOF && depth>0) {
                if (pd_cur(p)->kind==PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]=='[') depth++;
                else if (pd_cur(p)->kind==PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]==']') depth--;
                p->tok++;
            }
        }
        pd_Tok *nt = pd_cur(p);
        if (nt->kind == PD_TOK_PUNCT &&
            ((nt->len==1 && nt->text[0]=='=') || (nt->len==2 && nt->text[1]=='='))) {
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

/* Evaluate a constant expression: either a NUMBER token or an enum/const
 * symbol name. Returns the integer value, or -1 with p->ok=0 on error. */
static long eval_const_expr(pd_Parser *p) {
    pd_Tok *t = pd_cur(p);
    if (t->kind == PD_TOK_NUMBER) { pd_eat(p); return (long)t->num; }
    if (t->kind == PD_TOK_IDENT) {
        char name[40]; size_t nl = t->len < sizeof(name) ? t->len : sizeof(name)-1;
        memcpy(name, t->text, nl); name[nl] = 0;
        pd_Sym *s = sym_find_name(p, name);
        if (s && s->kind == PD_SYM_CONST) {
            pd_eat(p);
            /* const value is stored in the const table at reg.off/8 */
            return (long)p->b->consts[s->reg.off / 8];
        }
        pd_error(p, "expected constant");
        return -1;
    }
    pd_error(p, "expected constant");
    return -1;
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
        if (accept_punct(p, "[")) {
            isArr = 1;
            arrSize = eval_const_expr(p);
            if (!p->ok) return;
            /* support multi-dim by multiplying (e.g. a[3][4]) */
            while (accept_punct(p, "[")) {
                long d = eval_const_expr(p);
                if (!p->ok) return;
                arrSize *= d;
                pd_expect_punct(p, "]");
            }
            pd_expect_punct(p, "]");
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

        /* initializer: = expr (for scalar) */
        if (accept_punct(p, "=")) {
            pd_Reg v = pd_parse_expr(p);
            if (!p->ok) return;
            /* store into global[0] of this array */
            pd_emit1(p->b, PD_MOV, s->reg, v);
        }
        if (accept_punct(p, ",")) continue;
        break;
    }
}

int pd_parse_stmt(pd_Parser *p) {
    pd_Tok *t = pd_cur(p);
    if (t->kind == PD_TOK_IDENT) {
        if (accept_ident(p, "IF")) { parse_if(p); return 1; }
        if (accept_ident(p, "WHILE")) { parse_while(p); return 1; }
        if (accept_ident(p, "FOR")) { parse_for(p); return 1; }
        if (accept_ident(p, "DO")) { parse_do_while(p); return 1; }
        if (accept_ident(p, "ENUM")) { parse_enum(p); accept_punct(p, ";"); return 1; }
        if (accept_ident(p, "STATIC")) { parse_static(p); accept_punct(p, ";"); return 1; }
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

/* ---- top-level program parse ---- */
int pd_parse_program(pd_Parser *p) {
    /* parse statements until EOF. The last bare expression (no trailing
     * assignment) becomes the implicit return value. */
    pd_Reg zero = pd_new_const(p->b, 0.0);
    p->lastValueReg = zero;
    while (p->ok && pd_cur(p)->kind != PD_TOK_EOF) {
        pd_parse_stmt(p);
        if (!p->ok) break;
    }
    /* emit final return of the last seen value-expression */
    pd_emit1(p->b, PD_RETURN, pdR(PD_FAM_VOID,0), p->lastValueReg);
    return p->ok;
}
