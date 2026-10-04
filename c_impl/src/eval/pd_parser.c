/* pd_parser.c — Pratt-style expression parser + recursive-descent statements.
 *
 * Plan A from Plan/03_Parser.md. The expression parser uses binding-power
 * (precedence) to handle all binary/unary operators with a single compact
 * loop. Statements use classic recursive descent.
 *
 * Operator precedence (lower number = binds tighter), from eval.c:7352:
 *    ^       : 0   (left-assoc, matches original eval.c fold loop)
 *    * / %   : 2   (left)
 *    + -     : 3   (left)
 *    < <= > >= : 4 (left)
 *    == !=   : 5   (left)
 *    &&      : 6   (left)
 *    ||      : 7   (left)
 *    = += ...: 8   (right, lowest, assignment)
 */
#include "pd_parser.h"
#include "pd_host.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <math.h>
#include "pd_interp.h"

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
    /* lowercase aliases — scripts in the wild call min()/max() with lowercase
     * (e.g. tigrou/clock.pss, particules_morphing.pss). Without these the call
     * resolved to an undefined function (aux=-1) and returned NaN, poisoning
     * normals/uniforms and making every capture-based blur render black. */
    { "min",    PD_OP_END,PD_MIN    },
    { "max",    PD_OP_END,PD_MAX    },
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
    p->curScopeId = 0; p->nextScopeId = 1;
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

/* forward decls (definitions live further down) */
static int peek_is_ident(pd_Parser *p, int ahead);
static int peek_is_punct(pd_Parser *p, int ahead, char c);
static int struct_type_find(pd_Parser *p, const char *name);
static int struct_field_index(pd_Parser *p, int typeIdx, const char *field);

static int pd_expect_punct(pd_Parser *p, const char *text) {
    if (accept_punct(p, text)) return 1;
    pd_Tok *c = pd_cur(p);
    char tk[32]; size_t tl = c->len < sizeof(tk)-1 ? c->len : sizeof(tk)-1;
    memcpy(tk, c->text, tl); tk[tl] = 0;
    snprintf(p->err, sizeof(p->err), "expected '%s' at line %d (got '%s' kind=%d)", text, pd_cur(p)->origLine, tk, c->kind);
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
/* A symbol is visible if it lives at file scope (0) or in the scope currently
 * being parsed. This keeps one function's locals out of another's. */
static int sym_visible(const pd_Parser *p, const pd_Sym *s) {
    if (s->scopeId == p->curScopeId) return 1;
    if (s->scopeId != 0) return 0;
    /* File-scope symbol. Host symbols, builtins, functions, enums and statics
     * are fine everywhere. But a VAR/PARAM created by bare top-level
     * statements is frame-relative, so hide it inside function bodies. */
    if (p->curScopeId != 0 && (s->kind == PD_SYM_VAR || s->kind == PD_SYM_PARAM))
        return 0;
    return 1;
}

/* Variadic host/builtin functions (EXT_FUNC / BUILTIN) ignore arity in
 * sym_find, so a same-name set that binds *different* handlers by arity
 * (e.g. GLSETTEX string-file vs array forms) must be resolved explicitly:
 * return the last-registered same-name, same-kind symbol whose arity matches
 * `nParams`, or NULL if there is none (caller falls back to variadic). */
static pd_Sym *sym_find_arity(pd_Parser *p, const pd_Sym *s, const char *name, int nParams, int ci) {
    for (int j = p->nSyms - 1; j >= 0; j--) {
        pd_Sym *o = &p->syms[j];
        if (ci ? (strncasecmp(o->name, name, sizeof(o->name)) != 0)
               : (strncmp(o->name, name, sizeof(o->name)) != 0)) continue;
        if (o->kind != s->kind) continue;
        if (!sym_visible(p, o)) continue;
        if (o->nParams == nParams) return o;
    }
    return NULL;
}

static pd_Sym *sym_find(pd_Parser *p, const char *name, int nParams) {
    if (getenv("PD_DEBUG_FUNCS")) {
        fprintf(stderr, "sym_find(name='%s' nParams=%d nSyms=%d) FUNCs: ", name, nParams, p->nSyms);
        for (int di = p->nSyms - 1; di >= 0; di--)
            if (p->syms[di].kind == PD_SYM_FUNC)
                fprintf(stderr, "[%s/%d]", p->syms[di].name, p->syms[di].nParams);
        fprintf(stderr, "\n");
    }
    /* exact name+arity match (for functions); for non-funcs arity ignored */
    for (int i = p->nSyms - 1; i >= 0; i--) {
        pd_Sym *s = &p->syms[i];
        if (strncmp(s->name, name, sizeof(s->name)) != 0) continue;
        if (getenv("PD_DEBUG_FUNCS"))
            fprintf(stderr, "  sym_find exact hit '%s' kind=%d vis=%d nP=%d want=%d fidx=%d\n",
                    s->name, s->kind, sym_visible(p, s), s->nParams, nParams, s->funcIdx);
        if (!sym_visible(p, s)) continue;
        if (s->kind == PD_SYM_BUILTIN || s->kind == PD_SYM_EXT_FUNC || s->kind == PD_SYM_FUNC) {
            /* Host (EXT_FUNC) and builtin functions are variadic — their
             * registered nParams is just a nominal value, so never reject a
             * call on arity (e.g. setcol(1) vs setcol(3), srand(1),
             * glvertex(3)). Only user FUNCs enforce exact arity via overloads.
             * BUT when the same name binds different handlers by arity (e.g.
             * GLSETTEX string-file vs array forms) pick the exact-arity
             * overload first; fall back to the last-registered symbol only if
             * no exact-arity overload exists. */
            if (nParams >= 0 && s->nParams != nParams) {
                if (s->kind == PD_SYM_EXT_FUNC || s->kind == PD_SYM_BUILTIN) {
                    pd_Sym *o = sym_find_arity(p, s, s->name, nParams, 0);
                    if (o) return o;
                    return s; /* variadic fallback */
                }
                /* user FUNC: try overload chain, else keep scanning */
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
    /* Case-insensitive fallback: the lexer/parser normalizes *definitions* to
     * upper-case (prescan registers "DRAWCADRAN", "DRAWCLOCK", ...) but leaves
     * *call sites* as the script wrote them (e.g. "drawcadran()" in
     * tigrou/clock.pss). Builtins like min()/max()/exp() are likewise called
     * lowercase while stored upper-case. Without this the call resolves to an
     * undefined function (aux=-1) returning NaN, which poisoned
     * normals/uniforms and made every capture-based blur render black. User
     * variables keep exact-case semantics. */
    for (int i = p->nSyms - 1; i >= 0; i--) {
        pd_Sym *s = &p->syms[i];
        if (s->kind != PD_SYM_BUILTIN && s->kind != PD_SYM_EXT_FUNC && s->kind != PD_SYM_FUNC) continue;
        if (strcasecmp(s->name, name) != 0) continue;
        if (!sym_visible(p, s)) continue;
        if (nParams >= 0 && s->nParams != nParams) {
            /* same arity-resolution as the exact loop above (see GLSETTEX) */
            if (s->kind == PD_SYM_EXT_FUNC || s->kind == PD_SYM_BUILTIN) {
                pd_Sym *o = sym_find_arity(p, s, s->name, nParams, 1);
                if (o) return o;
                return s; /* variadic fallback */
            }
            int next = s->nextOverload;
            while (next >= 0) {
                pd_Sym *o = &p->syms[next];
                if (o->nParams == nParams) return o;
                next = o->nextOverload;
            }
            continue;
        }
        if (getenv("PD_DEBUG_FUNCS"))
            fprintf(stderr, "sym_find fallback: '%s' -> '%s' (kind=%d nParams=%d)\n",
                    name, s->name, s->kind, s->nParams);
            return s;
        }
        return NULL;
        }

/* find any symbol by name ignoring arity (first match) */
static pd_Sym *sym_find_name(pd_Parser *p, const char *name) {
    for (int i = p->nSyms - 1; i >= 0; i--) {
        if (strncasecmp(p->syms[i].name, name, sizeof(p->syms[i].name)) == 0 &&
            sym_visible(p, &p->syms[i]))
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
    s->structType = -1;
    s->scopeId = p->curScopeId;
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
    s->reg = pd_new_const_ptr(p->b, dptr);
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

/* precedence lookup for infix operators. Returns -1 if not an infix op. */
typedef struct { const char *tok; int prec; int rightAssoc; pd_Op op; } BinOp;
static const BinOp BINOPS[] = {
    /* prec: lower = tighter (matches eval.c numbering shifted so 1=lowest).
     * We invert: our "prec" means higher binds tighter. */
    { "^",  7, 0, PD_POW   },
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
/* Unary operators bind tighter than every binary operator in BINOPS
 * (max binary prec is 7 for '^'). */
#define PD_PREC_UNARY 8
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
/* Unary +/- must match eval.c:parsefunc's two distinct sign rules:
 *  - FRESH boundary (minPrec==0: statement / call arg / paren contents /
 *    array index — each is an independent parsefunc call in the original):
 *    a leading sign becomes a BINARY operator with an implicit 0 left operand
 *    (the "-x^2" hack). So -2^2 = -(2^2) = -4, -2+3 = 1, --2 = 2.
 *  - MID-expression (minPrec>0: we are an operator's right operand):
 *    consecutive +/- toggle a negit that negates the IMMEDIATE operand
 *    before ^ binds. So 3*-2^2 = 3*((-2)^2) = 12, 2^--3 = 2^3 = 8. */
static pd_Reg parse_expr_prec(pd_Parser *p, int minPrec) {
    int fresh = (minPrec == 0);
    pd_Reg left;
    /* logical NOT: binds tighter than any binary operator and may be stacked
     * (!!x). Implemented as `operand == 0`, matching EVAL semantics. */
    if (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len == 1 &&
        pd_cur(p)->text[0] == '!') {
        pd_eat(p);
        pd_Reg operand = parse_expr_prec(p, PD_PREC_UNARY);
        if (!p->ok) return operand;
        pd_Reg zero = pd_new_const(p->b, 0.0);
        pd_Reg out = pd_new_local(p->b);
        pd_emit2(p->b, PD_EQU, out, operand, zero);
        left = out;
        goto pratt_loop;
    }
    if (fresh && pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len == 1 &&
        (pd_cur(p)->text[0] == '+' || pd_cur(p)->text[0] == '-')) {
        /* insert an implicit 0 left operand; the pratt loop below consumes
         * the sign as a binary operator */
        pd_Reg z = pd_new_const(p->b, 0.0);
        left = pd_new_local(p->b);
        pd_emit1(p->b, PD_MOV, left, z);
    } else {
        /* mid-expression: collect consecutive +/- into negit and negate the
         * immediate operand (^ to the right binds after the negation) */
        int negit = 1;
        while (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len == 1 &&
               (pd_cur(p)->text[0] == '+' || pd_cur(p)->text[0] == '-')) {
            if (pd_cur(p)->text[0] == '-') negit = -negit;
            pd_eat(p);
        }
        left = pd_parse_primary(p);
        if (!p->ok) return left;
        if (negit < 0) {
            pd_Reg out = pd_new_local(p->b);
            pd_emit1(p->b, PD_NEGMOV, out, left);
            left = out;
        }
    }
pratt_loop:
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
    return left;
}

/* public entry: parse full expression (routes to the Plan B fold parser when
 * p->useFold is set; both plans must produce identical results) */
pd_Reg pd_parse_expr(pd_Parser *p) {
    if (p->useFold) return pd_fold_parse_expr(p);
    return parse_expr_prec(p, 0);
}

/* ---- primary: number, ident, (expr), call, array access ---- */

/* CALL argument. A bare array symbol (not followed by `[`) passes its
 * base pointer — EVAL's `&`-typed host params (glsettex(0,buf,...),
 * glUniform3fv(loc,n,arr), glMultMatrix(&m), ...) receive the array
 * address, matching the original eval. Everything else is a normal
 * expression (indexed reads, scalars, string literals, nested calls). */
static pd_Reg parse_call_arg(pd_Parser *p) {
    pd_Tok *t = pd_cur(p);
    if (t->kind == PD_TOK_IDENT) {
        /* peek the token after the name: must be , or ) (bare reference).
         * A following '[' (subscript) or '.' (struct member) means the name is
         * not passed as a whole array. */
        const pd_Tok *nx = (p->tok + 1 < p->ts->nToks) ? &p->ts->toks[p->tok + 1] : NULL;
        int bare = nx && !(nx->kind == PD_TOK_PUNCT && nx->len == 1 &&
                           (nx->text[0] == '[' || nx->text[0] == '.'));
        if (bare) {
            char nm[40];
            size_t nl = t->len < sizeof(nm) ? t->len : sizeof(nm)-1;
            memcpy(nm, t->text, nl); nm[nl] = 0;
            pd_Sym *s = sym_find_name(p, nm);
            if (s && s->kind == PD_SYM_ARRAY && s->arraySize > 0) {
                pd_eat(p);
                pd_Reg out = pd_new_local(p->b);
                pd_emit1(p->b, PD_ADDR, out, s->reg);
                return out;
            }
        }
    }
    return pd_parse_expr(p);
}

pd_Reg pd_parse_primary(pd_Parser *p) {
    pd_Tok *t = pd_eat(p);
    p->lastLValue = NULL;
    p->lastLValueIsArrayIndex = 0;
    /* address-of prefix: &ident (EVAL pass-by-reference). Arrays resolve
     * to a bit-cast base pointer (PD_ADDR); scalars pass their value. */
    if (t->kind == PD_TOK_PUNCT && t->len==1 && t->text[0]=='&') {
        if (pd_cur(p)->kind == PD_TOK_IDENT) {
            pd_Tok *n = pd_cur(p);
            char nm[40];
            size_t nl = n->len < sizeof(nm) ? n->len : sizeof(nm)-1;
            memcpy(nm, n->text, nl); nm[nl] = 0;
            pd_Sym *s = sym_find_name(p, nm);
            if (s && s->kind == PD_SYM_ARRAY && s->arraySize > 0) {
                pd_eat(p);
                pd_Reg out = pd_new_local(p->b);
                pd_emit1(p->b, PD_ADDR, out, s->reg);
                return out;
            }
            /* scalar pass-by-reference: pass the address of the variable's slot */
            if (s && (s->kind == PD_SYM_VAR || s->kind == PD_SYM_PARAM ||
                      s->kind == PD_SYM_EXT_VAR ||
                      (s->kind == PD_SYM_ARRAY && s->arraySize == 0))) {
                pd_eat(p);
                pd_Reg out = pd_new_local(p->b);
                pd_emit1(p->b, PD_ADDRSLOT, out, s->reg);
                return out;
            }
        }
        return pd_parse_primary(p);
    }
    /* dollar-prefixed string arg: $ident or $"str" (EVAL string var ref).
     * The token after $ names a string; resolve to the STR reg. */
    if (t->kind == PD_TOK_PUNCT && t->len==1 && t->text[0] == 0x24) {
        pd_Tok *n = pd_cur(p);
        if (n->kind == PD_TOK_STRING) {
            pd_eat(p);
            return pd_new_string(p->b, n->text, n->len);
        }
        if (n->kind == PD_TOK_IDENT) {
            /* $ident: look up the string variable (currently: empty) */
            pd_eat(p);
            return pd_new_string(p->b, "", 0);
        }
        pd_Reg out = pd_new_local(p->b);
        pd_Reg z = pd_new_const(p->b, 0.0);
        pd_emit1(p->b, PD_MOV, out, z);
        return out;
    }
    if (t->kind == PD_TOK_NUMBER || t->kind == PD_TOK_CHAR) {
        pd_Reg c = pd_new_const(p->b, t->num);
        pd_Reg out = pd_new_local(p->b);
        pd_emit1(p->b, PD_MOV, out, c);
        return out;
    }
    if (t->kind == PD_TOK_STRING) {
        /* string literal → STR reg (offset into the program's string
         * table). CALL lowers STR args to bit-cast char* pointers for
         * host functions (glsettex("file"), glGetUniformLoc("name"), ...). */
        return pd_new_string(p->b, t->text, t->len);
    }
    if (t->kind == PD_TOK_PUNCT && t->len == 1 && t->text[0] == '(') {
        pd_Reg r = pd_parse_expr(p);
        if (!pd_expect_punct(p, ")")) return r;
        return r;
    }
    if (t->kind == PD_TOK_IDENT) {
        char name[40];
        size_t nl = t->len < sizeof(name) ? t->len : sizeof(name)-1;
        memcpy(name, t->text, nl); name[nl] = 0;

        /* parameterless builtins: RND, NRND. EVAL allows both bare `rnd`
         * and `rnd()` with empty parens; consume optional () here. */
        if (strcmp(name, "RND") == 0 || strcmp(name, "NRND") == 0) {
            if (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]=='(') {
                pd_eat(p); /* ( */
                pd_expect_punct(p, ")");
            }
            pd_Reg out = pd_new_local(p->b);
            pd_emit0(p->b, (name[0]=='R') ? PD_RND : PD_NRND, out);
            return out;
        }

        /* function call? look ahead for '(' */
        if (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len == 1 && pd_cur(p)->text[0] == '(') {
            /* callee's ref param mask (pass-by-reference => pass variable
             * address): the & lives on the DEFINITION (rotate(&x,&y,r));
             * EVAL call sites pass plain vars and the caller supplies the
             * variable's slot address for ref params. */
            pd_Sym *pre = sym_find_name(p, name);
            int refMask = (pre && (pre->kind == PD_SYM_FUNC || pre->kind == PD_SYM_EXT_FUNC)) ? pre->refMask : 0;
            /* collect args */
            pd_eat(p); /* ( */
            int argsCap = 16;
            pd_Reg *args = malloc(argsCap * sizeof(pd_Reg));
            if (!args) { pd_error(p, "oom"); return pd_new_const(p->b, 0.0); }
            int nArgs = 0;
            #define PUSH_ARG(v) do { \
                if (nArgs >= argsCap) { \
                    argsCap *= 2; \
                    pd_Reg *_n = realloc(args, argsCap * sizeof(pd_Reg)); \
                    if (!_n) { pd_error(p, "oom"); free(args); return pd_new_const(p->b, 0.0); } \
                    args = _n; \
                } \
                args[nArgs++] = (v); \
            } while (0)
            if (!(pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len == 1 && pd_cur(p)->text[0] == ')')) {
                for (;;) {
                    if (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len == 1 && pd_cur(p)->text[0] == ',') {
                        /* blank arg → 0 */
                        PUSH_ARG(pd_new_const(p->b, 0.0));
                        pd_eat(p);
                        continue;
                    }
                    if ((refMask & (1 << nArgs)) &&
                        pd_cur(p)->kind == PD_TOK_IDENT) {
                        /* ref param: pass the ADDRESS of the lvalue slot.
                         * A bare array passes its base (ADDR); a scalar
                         * variable passes its slot address (ADDRSLOT). */
                        const pd_Tok *nx = (p->tok + 1 < p->ts->nToks) ? &p->ts->toks[p->tok + 1] : NULL;
                        /* '[' subscript or '.' member both mean "not a whole
                         * object reference" */
                        int indexed = nx && nx->kind == PD_TOK_PUNCT && nx->len == 1 &&
                                      (nx->text[0] == '[' || nx->text[0] == '.');
                        char an[40]; size_t al = pd_cur(p)->len < sizeof(an)?pd_cur(p)->len:sizeof(an)-1;
                        memcpy(an, pd_cur(p)->text, al); an[al] = 0;
                        pd_Sym *as = sym_find_name(p, an);
                        if (getenv("ED_DEBUG_REFMASK"))
                            fprintf(stderr, "  [refarg] argIdx=%d name='%s' as=%p kind=%d indexed=%d refMaskBit=%d\n",
                                    nArgs, an, (void*)as, as?as->kind:-1, indexed, (refMask>>nArgs)&1);
                        if (!indexed && as && as->kind == PD_SYM_PARAM) {
                            /* ref parameter forwarding: the callee already
                             * holds a bit-cast pointer to the real variable.
                             * Forward that pointer value (PD_ADDR → derefs the
                             * param slot and re-bit-casts it), NOT the address
                             * of the param slot itself. */
                            pd_eat(p);
                            pd_Reg ao = pd_new_local(p->b);
                            pd_emit1(p->b, PD_ADDR, ao, as->reg);
                            PUSH_ARG(ao);
                            if (accept_punct(p, ",")) continue;
                            break;
                        }
                        if (!indexed && as &&
                            (as->kind == PD_SYM_VAR ||
                             as->kind == PD_SYM_EXT_VAR ||
                             (as->kind == PD_SYM_ARRAY && as->arraySize == 0))) {
                            pd_eat(p);
                            pd_Reg ao = pd_new_local(p->b);
                            pd_emit1(p->b, PD_ADDRSLOT, ao, as->reg);
                            PUSH_ARG(ao);
                            if (accept_punct(p, ",")) continue;
                            break;
                        }
                        if (!indexed && as && as->kind == PD_SYM_ARRAY && as->arraySize > 0) {
                            pd_eat(p);
                            pd_Reg ao = pd_new_local(p->b);
                            pd_emit1(p->b, PD_ADDR, ao, as->reg);
                            PUSH_ARG(ao);
                            if (accept_punct(p, ",")) continue;
                            break;
                        }
                    }
                    PUSH_ARG(parse_call_arg(p));
                    if (!p->ok) { pd_Reg r = args[nArgs-1]; free(args); return r; }
                    if (accept_punct(p, ",")) {
                        /* if next is ) it was a trailing comma - treat as blank */
                        if (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len == 1 && pd_cur(p)->text[0] == ')') {
                            PUSH_ARG(pd_new_const(p->b, 0.0));
                            break;
                        }
                        continue;
                    }
                    break;
                }
            }
            if (!pd_expect_punct(p, ")")) { pd_Reg z = pd_new_const(p->b,0); free(args); return z; }

            /* resolve function symbol (try exact arity, else overload) */
            pd_Sym *s = sym_find(p, name, nArgs);
            if (!s) s = sym_find_name(p, name);
            if (!s) {
                /* Unknown function. EVAL is lenient: rather than emit a broken
                 * CALL (aux=-1) that crashes at runtime, resolve it to a safe
                 * no-op host stub (returns 0) when a host table is available, or
                 * simply yield 0 otherwise. This lets scripts calling
                 * unimplemented host APIs (PLAYSOUND, GLDISABLE, PIC, ...) still
                 * compile and run without crashing. */
                pd_Reg out = pd_new_local(p->b);
                if (p->host) {
                    int hidx = pd_host_add_stub(p->host, name, nArgs);
                    if (hidx >= 0) {
                        size_t idx = p->b->nInstr;
                        if (pd_emit(p->b, PD_CALL, out, args, nArgs) == (size_t)-1) {
                            pd_error(p, "emit fail"); free(args); return out;
                        }
                        p->b->instr[idx].aux = -1000 - hidx; /* EXT_FUNC stub */
                        if (getenv("PD_DEBUG_FUNCS"))
                            fprintf(stderr, "[stub CALL] '%s' nArgs=%d hidx=%d\n", name, nArgs, hidx);
                        free(args); return out;
                    }
                }
                /* no host: just evaluate args for side effects, yield 0 */
                if (nArgs >= 1) pd_emit1(p->b, PD_FABS, out, args[0]);
                else { pd_Reg z = pd_new_const(p->b, 0.0); pd_emit1(p->b, PD_MOV, out, z); }
                if (getenv("PD_DEBUG_FUNCS"))
                    fprintf(stderr, "[unknown fn -> 0] '%s' nArgs=%d\n", name, nArgs);
                free(args); return out;
            }
            if (s->kind == PD_SYM_BUILTIN) {
                pd_Op op = (pd_Op)s->reg.off;
                pd_Reg out = pd_new_local(p->b);
                if (nArgs == 1) pd_emit1(p->b, op, out, args[0]);
                else if (nArgs == 2) pd_emit2(p->b, op, out, args[0], args[1]);
                else { pd_error(p, "bad arg count"); }
                free(args); return out;
            }
            if (s->kind == PD_SYM_EXT_FUNC || s->kind == PD_SYM_FUNC) {
                pd_Reg out = pd_new_local(p->b);
                size_t idx = p->b->nInstr;
                if (pd_emit(p->b, PD_CALL, out, args, nArgs) == (size_t)-1) {
                    pd_error(p, "emit fail"); free(args); return out;
                }
                if (s->kind == PD_SYM_FUNC) {
                    p->b->instr[idx].aux = s->funcIdx;  /* user function index */
                } else {
                    /* external (host) function: encode host index in aux as
                     * (-1000 - idx) to distinguish from user funcs (>=0) */
                    p->b->instr[idx].aux = -1000 - s->funcIdx;
                }
                free(args); return out;
            }
            /* variable used as function — error or 0 */
            pd_error(p, "not a function");
            free(args);
            return pd_new_const(p->b, 0.0);
        }

        /* array access? name[expr] or name[i0][i1]... (multi-dim, flattened).
         * For buf3d[5][3][2]: buf3d[a][b][c] => int(a)*3*2 + int(b)*2 + c.
         * Inner indices are truncated toward 0; the last index is used raw. */
        if (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len == 1 &&
            (pd_cur(p)->text[0] == '[' ||
             /* struct member access on a struct-typed variable: `v.field` is
              * lowered to one more index, so it flows through the same
              * flattening path as `v[...]`. */
             (pd_cur(p)->text[0] == '.' && peek_is_ident(p, 1) &&
              (sym_find_name(p, name) ? sym_find_name(p, name)->structType >= 0 : 0)))) {
            pd_Sym *s = sym_find_name(p, name);
            if (!s) s = declare_local(p, name);
            pd_Reg idxs[8]; int nd = 0;
            for (;;) {
                if (accept_punct(p, "[")) {
                    pd_Reg ix = pd_parse_expr(p);
                    if (!p->ok) return ix;
                    if (!pd_expect_punct(p, "]")) return ix;
                    if (nd < 8) idxs[nd++] = ix;
                    continue;
                }
                /* `.field` -> constant index of that field */
                if (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len == 1 &&
                    pd_cur(p)->text[0] == '.' && peek_is_ident(p, 1)) {
                    pd_eat(p); /* . */
                    pd_Tok *ft = pd_eat(p);
                    char fname[40]; size_t fl = ft->len < sizeof(fname) ? ft->len : sizeof(fname)-1;
                    memcpy(fname, ft->text, fl); fname[fl] = 0;
                    int fi = struct_field_index(p, s->structType, fname);
                    if (fi < 0) { pd_error(p, "unknown struct field"); return pd_new_const(p->b, 0.0); }
                    if (nd < 8) idxs[nd++] = pd_new_const(p->b, (double)fi);
                    continue;
                }
                break;
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
            if (s->kind == PD_SYM_PARAM && s->refParam) {
                /* by-reference param: read through the pointer */
                pd_Reg idx0 = pd_new_const(p->b, 0.0);
                pd_Reg out = pd_new_local(p->b);
                size_t ii = pd_emit2(p->b, PD_PEEK, out, s->reg, idx0);
                p->b->instr[ii].aux = 0;
                return out;
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
    char name[40]; name[0] = 0;

    /* peek: IDENT followed by assign op (possibly after [expr]) */
    if (t->kind == PD_TOK_IDENT) {
        /* save state to backtrack */
        size_t save = p->tok;
        size_t nl = t->len < sizeof(name) ? t->len : sizeof(name)-1;
        memcpy(name, t->text, nl); name[nl] = 0;
        /* skip ident */
        p->tok++;
        /* Skip any chain of `[expr]` subscripts and `.field` members so the
         * assignment operator after e.g. `a[i].f` is found. */
        for (;;) {
            pd_Tok *ct = pd_cur(p);
            if (ct->kind == PD_TOK_PUNCT && ct->len==1 && ct->text[0]=='[') {
                p->tok++; /* [ */
                int depth=1;
                while (pd_cur(p)->kind != PD_TOK_EOF && depth>0) {
                    if (pd_cur(p)->kind==PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]=='[') depth++;
                    else if (pd_cur(p)->kind==PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]==']') depth--;
                    p->tok++;
                }
                continue;
            }
            if (ct->kind == PD_TOK_PUNCT && ct->len==1 && ct->text[0]=='.' && peek_is_ident(p, 1)) {
                p->tok += 2; /* . field */
                continue;
            }
            break;
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
            /* Indexed lvalue? Either a subscript or a struct member access —
             * both lower to a POKE through the array base. */
            int hadArray = 0;
            for (size_t k = save+1; k < p->tok; k++) {
                if (p->ts->toks[k].kind==PD_TOK_PUNCT && p->ts->toks[k].len==1 &&
                    (p->ts->toks[k].text[0]=='[' || p->ts->toks[k].text[0]=='.')) { hadArray=1; break; }
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
        /* capture the LHS index NOW: parsing the RHS may read other arrays
         * and clobber p->lastArrayIdx, so save it before the value parse */
        pd_Reg lhsIdx = p->lastArrayIdx;
        /* consume assign op */
        pd_eat(p);
        pd_Reg value = pd_parse_expr(p);
        if (!p->ok) return 0;
        if (lvIsArray) {
            /* POKE family: out = array base (lvSym->reg), in[0]=value, in[1]=idx */
            pd_Op pop = (assignOp==PD_MOV) ? PD_POKE :
                        (assignOp==PD_PLUS)?PD_POKEPLUS:
                        (assignOp==PD_MINUS)?PD_POKEMINUS:
                        (assignOp==PD_TIMES)?PD_POKETIMES:
                        (assignOp==PD_SLASH)?PD_POKESLASH:PD_POKEPERC;
            size_t ii = pd_emit2(p->b, pop, lvSym->reg, value, lhsIdx);
            p->b->instr[ii].aux = lvSym->arraySize;
        } else if (lvSym->kind == PD_SYM_PARAM && lvSym->refParam) {
            /* by-reference param assignment: store through the pointer */
            pd_Op pop = (assignOp==PD_MOV) ? PD_POKE :
                        (assignOp==PD_PLUS)?PD_POKEPLUS:
                        (assignOp==PD_MINUS)?PD_POKEMINUS:
                        (assignOp==PD_TIMES)?PD_POKETIMES:
                        (assignOp==PD_SLASH)?PD_POKESLASH:PD_POKEPERC;
            pd_Reg idx0 = pd_new_const(p->b, 0.0);
            size_t ii = pd_emit2(p->b, pop, lvSym->reg, value, idx0);
            p->b->instr[ii].aux = 0;
        } else {
            if (assignOp == PD_MOV) {
                pd_emit1(p->b, PD_MOV, lvSym->reg, value);
            } else {
                pd_Reg cur = pd_new_local(p->b);
                pd_emit1(p->b, PD_MOV, cur, lvSym->reg);
                pd_Reg combined = pd_new_local(p->b);
                pd_emit2(p->b, assignOp, combined, cur, value);
                pd_emit1(p->b, PD_MOV, lvSym->reg, combined);
            }
        }
        /* assignment is a value expression in EVAL: its value is the RHS */
        pd_Reg valResult = pd_new_local(p->b);
        pd_emit1(p->b, PD_MOV, valResult, value);
        p->lastValueReg = valResult;
        if (accept_punct(p, ",")) { parse_expr_stmt(p); return 0; }
        accept_punct(p, ";");
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
            if ((lv->kind == PD_SYM_VAR || lv->kind == PD_SYM_EXT_VAR ||
                (lv->kind == PD_SYM_ARRAY && lv->arraySize == 0)) ||
                (lv->kind == PD_SYM_PARAM && !lv->refParam)) {
                pd_Op op = (c1=='+') ? PD_PLUS : PD_MINUS;
                pd_Reg one = pd_new_const(p->b, 1.0);
                pd_Reg cur = pd_new_local(p->b);
                pd_Reg combined = pd_new_local(p->b);
                pd_emit1(p->b, PD_MOV, cur, lv->reg);
                pd_emit2(p->b, op, combined, cur, one);
                pd_emit1(p->b, PD_MOV, lv->reg, combined);
            } else if (lv->kind == PD_SYM_PARAM && lv->refParam) {
                pd_Reg one = pd_new_const(p->b, 1.0);
                pd_Reg idx0 = pd_new_const(p->b, 0.0);
                pd_Op pop = (c1=='+') ? PD_POKEPLUS : PD_POKEMINUS;
                size_t ii = pd_emit2(p->b, pop, lv->reg, one, idx0);
                p->b->instr[ii].aux = 0;
            } else if (lv->kind == PD_SYM_ARRAY) {
                /* array element increment not supported in postfix; ignore */
            }
            if (accept_punct(p, ",")) { parse_expr_stmt(p); return 0; }
            accept_punct(p, ";");
            return 0;
        }
    }

    pd_Reg left = pd_parse_expr(p);
    if (!p->ok) return 0;
    /* bare expression: this is the "value" form. */
    p->lastValueReg = left;
    if (accept_punct(p, ",")) { parse_expr_stmt(p); return 1; }
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
    /* We need a break target but don't know the end label yet. Use a
     * sentinel (like parse_while/parse_for) and repoint break GOTOs after. */
    int brkSentinel = -(int)top - 1;  /* unique negative sentinel */
    size_t contLbl = top;             /* continue re-enters at loop top */
    p->breakLabel = brkSentinel; p->contLabel = (int)contLbl;
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
    /* repoint break GOTOs to here (after the loop) */
    size_t endLbl = pd_label_here(p->b);
    for (size_t k = top; k < p->b->nInstr; k++) {
        if (p->b->instr[k].op == PD_GOTO && p->b->instr[k].out.fam == PD_FAM_LABEL &&
            (int)p->b->instr[k].out.off == brkSentinel) {
            p->b->instr[k].out.off = (uint32_t)endLbl;
        }
    }
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
 * Returns a double so fractional enum values (e.g. radius = 0.5) survive;
 * array-dimension callers cast the result back to a long. */
/* Look ahead `ahead` tokens from the cursor and test for a 1-char punctuator. */
static int peek_is_punct(pd_Parser *p, int ahead, char c) {
    size_t k = p->tok + (size_t)ahead;
    if (k >= p->ts->nToks) return 0;
    pd_Tok *t = &p->ts->toks[k];
    return t->kind == PD_TOK_PUNCT && t->len == 1 && t->text[0] == c;
}

/* Look ahead `ahead` tokens from the cursor and test for an identifier. */
static int peek_is_ident(pd_Parser *p, int ahead) {
    size_t k = p->tok + (size_t)ahead;
    if (k >= p->ts->nToks) return 0;
    return p->ts->toks[k].kind == PD_TOK_IDENT;
}

/* Constant evaluation of a builtin call (e.g. sqrt(2) inside an enum value).
 * Only the pure-math builtins that take constant args are supported; anything
 * else returns 0 (the caller treats the value as a constant anyway). */
static double pd_const_eval_builtin(pd_Parser *p, const char *name, int nargs, double *a) {
    if (strcasecmp(name,"ABS")==0||strcasecmp(name,"FABS")==0) return fabs(a[0]);
    if (strcasecmp(name,"ACOS")==0) return acos(a[0]);
    if (strcasecmp(name,"ASIN")==0) return asin(a[0]);
    if (strcasecmp(name,"ATAN")==0||strcasecmp(name,"ATN")==0) return atan(a[0]);
    if (strcasecmp(name,"CEIL")==0) return ceil(a[0]);
    if (strcasecmp(name,"COS")==0) return cos(a[0]);
    if (strcasecmp(name,"EXP")==0) return exp(a[0]);
    if (strcasecmp(name,"FACT")==0) return pd_fact(a[0]);
    if (strcasecmp(name,"FLOOR")==0) return floor(a[0]);
    if (strcasecmp(name,"INT")==0) return (a[0]>=0)?floor(a[0]):-floor(-a[0]);
    if (strcasecmp(name,"LOG")==0) return (nargs==2)?log(a[0])/log(a[1]):log(a[0]);
    if (strcasecmp(name,"SGN")==0) return (a[0]>0)-(a[0]<0);
    if (strcasecmp(name,"SIN")==0) return sin(a[0]);
    if (strcasecmp(name,"SQR")==0||strcasecmp(name,"SQRT")==0) return sqrt(a[0]);
    if (strcasecmp(name,"TAN")==0) return tan(a[0]);
    if (strcasecmp(name,"UNIT")==0) return (a[0]==0.0)*0.5+(a[0]>0);
    if (strcasecmp(name,"ATAN2")==0) return atan2(a[0],a[1]);
    if (strcasecmp(name,"FMOD")==0) return fmod(a[0],a[1]);
    if (strcasecmp(name,"MIN")==0) return (a[1]<a[0])?a[1]:a[0];
    if (strcasecmp(name,"MAX")==0) return (a[1]>a[0])?a[1]:a[0];
    if (strcasecmp(name,"POW")==0) return pow(a[0],a[1]);
    if (strcasecmp(name,"FADD")==0) return a[0]+a[1];
    (void)p;
    return 0.0;
}

static double eval_const_expr(pd_Parser *p) {
    pd_Tok *t = pd_cur(p);
    double v;
    if (t->kind == PD_TOK_PUNCT && t->len==1 && t->text[0]=='(') {
        pd_eat(p);
        v = eval_const_expr(p);
        if (!p->ok) return -1;
        if (!pd_expect_punct(p, ")")) return -1;
        t = pd_cur(p);
    } else if (t->kind == PD_TOK_NUMBER || t->kind == PD_TOK_CHAR) {
        pd_eat(p);
        v = t->num;
        t = pd_cur(p);
    } else if (t->kind == PD_TOK_IDENT) {
        char name[40]; size_t nl = t->len < sizeof(name) ? t->len : sizeof(name)-1;
        memcpy(name, t->text, nl); name[nl] = 0;
        pd_Sym *s = sym_find_name(p, name);
        if (s && s->kind == PD_SYM_CONST) {
            pd_eat(p);
            v = p->b->consts[s->reg.off / 8];
            t = pd_cur(p);
        } else if (s && s->kind == PD_SYM_BUILTIN && peek_is_punct(p, 1, '(')) {
            pd_eat(p); /* name */
            pd_eat(p); /* ( */
            double args[4]; int nargs = 0;
            if (!(pd_cur(p)->kind==PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]==')')) {
                while (p->ok) {
                    if (nargs < 4) args[nargs] = eval_const_expr(p);
                    else eval_const_expr(p);
                    nargs++;
                    if (!p->ok) return -1;
                    if (pd_cur(p)->kind==PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]==',') { pd_eat(p); continue; }
                    break;
                }
            }
            if (!pd_expect_punct(p, ")")) return -1;
            v = pd_const_eval_builtin(p, name, nargs, args);
            t = pd_cur(p);
        } else { pd_error(p, "expected constant"); return -1; }
    } else { pd_error(p, "expected constant"); return -1; }
    /* handle trailing binary ops: ^ * / + - (^ is exponent in EVAL, right-assoc).
     * Recurse for ^ so 2^3^2 = 2^9; the others fold left-to-right here. */
    if (t->kind == PD_TOK_PUNCT && t->len==1 && t->text[0]=='^') {
        pd_eat(p);
        double r = eval_const_expr(p);
        if (!p->ok) return -1;
        long e = (long)r; if (e < 0) e = 0; /* const dim power can't be negative */
        double acc = 1.0;
        for (long i = 0; i < e; i++) acc *= v;
        v = acc;
        t = pd_cur(p);
    }
    while (t->kind == PD_TOK_PUNCT && t->len==1 &&
           (t->text[0]=='*'||t->text[0]=='/'||t->text[0]=='+'||t->text[0]=='-')) {
        char op = t->text[0];
        pd_eat(p);
        double r = eval_const_expr(p);
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
    double nextVal = 0;
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
        s->reg = pd_new_const(p->b, nextVal);
        nextVal++;
        if (accept_punct(p, ",")) continue;
        break;
    }
    pd_expect_punct(p, "}");
}

/* Look up a declared struct type by name; returns its index or -1. */
static int struct_type_find(pd_Parser *p, const char *name) {
    for (int i = 0; i < p->nStructTypes; i++)
        if (strcasecmp(p->structTypes[i].name, name) == 0) return i;
    return -1;
}

/* Resolve a field name to its index within a struct type; -1 if unknown. */
static int struct_field_index(pd_Parser *p, int typeIdx, const char *field) {
    if (typeIdx < 0 || typeIdx >= p->nStructTypes) return -1;
    pd_StructType *st = &p->structTypes[typeIdx];
    for (int i = 0; i < st->nFields; i++)
        if (strcasecmp(st->fields[i], field) == 0) return i;
    return -1;
}

/* struct { f0, f1, ... ; more, fields ; } TypeName ;
 *
 * Fields may carry an ignored type prefix (`double x, y;`). The type is
 * recorded as a flat ordered field list; instances are lowered to arrays with
 * an extra innermost dimension of nFields (see pd_StructType). */
static void parse_struct_decl(pd_Parser *p) {
    if (!pd_expect_punct(p, "{")) return;
    if (p->nStructTypes >= PD_MAX_STRUCT_TYPES) { pd_error(p, "too many struct types"); return; }
    pd_StructType st;
    memset(&st, 0, sizeof(st));

    while (p->ok) {
        pd_Tok *t = pd_cur(p);
        if (t->kind == PD_TOK_PUNCT && t->len == 1 && t->text[0] == '}') break;
        if (t->kind == PD_TOK_EOF) { pd_error(p, "unterminated struct body"); return; }
        if (accept_punct(p, ";") || accept_punct(p, ",")) continue;
        if (t->kind != PD_TOK_IDENT) { pd_error(p, "struct: expected field name"); return; }

        char nm[40]; size_t nl = t->len < sizeof(nm) ? t->len : sizeof(nm)-1;
        memcpy(nm, t->text, nl); nm[nl] = 0;
        pd_eat(p);

        /* An ignored type prefix: `double x` / `int n` -> the *next* ident is
         * the real field name. Detect it by a following identifier. */
        if (pd_cur(p)->kind == PD_TOK_IDENT) continue;

        /* array field (`p[3];`) occupies that many slots; each gets its own
         * synthetic entry so the flat layout stays correct. */
        long count = 1;
        if (accept_punct(p, "[")) {
            count = (long)eval_const_expr(p);
            if (!p->ok) return;
            if (count <= 0) { pd_error(p, "invalid struct field dimension"); return; }
            if (!pd_expect_punct(p, "]")) return;
        }
        for (long k = 0; k < count; k++) {
            if (st.nFields >= PD_MAX_STRUCT_FIELDS) { pd_error(p, "too many struct fields"); return; }
            /* only the first slot carries the visible name */
            if (k == 0) strncpy(st.fields[st.nFields], nm, sizeof(st.fields[0])-1);
            st.nFields++;
        }
    }
    if (!pd_expect_punct(p, "}")) return;

    pd_Tok *tn = pd_cur(p);
    if (tn->kind != PD_TOK_IDENT) { pd_error(p, "struct: expected type name"); return; }
    size_t tl = tn->len < sizeof(st.name) ? tn->len : sizeof(st.name)-1;
    memcpy(st.name, tn->text, tl); st.name[tl] = 0;
    pd_eat(p);

    p->structTypes[p->nStructTypes] = st;
    pd_Sym *s = sym_add(p, st.name, PD_SYM_STRUCT_TYPE);
    if (s) s->structType = p->nStructTypes;
    p->nStructTypes++;

    accept_punct(p, ";");
}

/* static name[size], name2[size2], name3, ... ;
 * name[size] allocates an array of `size` doubles in global storage.
 * name (no brackets) allocates a single double (scalar static).
 * Optional initializer: = expr (scalar only, or first element). */
static void parse_static(pd_Parser *p) {
    /* Optional struct type prefix shared by the whole declarator list:
     *   static pt_t a[4], b;   ->  both a and b have type pt_t. */
    int declType = -1;
    if (pd_cur(p)->kind == PD_TOK_IDENT && peek_is_ident(p, 1)) {
        char tn[40]; size_t tl = pd_cur(p)->len < sizeof(tn) ? pd_cur(p)->len : sizeof(tn)-1;
        memcpy(tn, pd_cur(p)->text, tl); tn[tl] = 0;
        int ti = struct_type_find(p, tn);
        if (ti >= 0) { declType = ti; pd_eat(p); }
    }

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
                long d = (long)eval_const_expr(p);
                if (!p->ok) return;
                if (d <= 0) { pd_error(p, "invalid array dimension"); return; }
                arrSize *= d;
                if (nDims < 8) dims[nDims++] = (int)d;
                if (!pd_expect_punct(p, "]")) return;
                if (!accept_punct(p, "[")) break;
            }
        }
        /* A struct-typed declarator gets one extra innermost dimension equal to
         * the field count, so `a[i].f` lowers to `a[i][fieldIndex]`. */
        if (declType >= 0) {
            int nf = p->structTypes[declType].nFields;
            if (nf > 0) {
                if (nDims < 8) dims[nDims++] = nf;
                arrSize *= nf;
                isArr = 1;
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
        s->structType = declType;
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
                        /* elided element: ",," or ", }" leaves the slot at 0.
                         * EVAL scripts use this to lay out sparse tables, e.g.
                         *   static note[16] = {65, , ,65, ,60,...};   */
                        pd_Tok *ct = pd_cur(p);
                        int elided = (ct->kind == PD_TOK_PUNCT && ct->len == 1 &&
                                      (ct->text[0] == ',' || ct->text[0] == '}'));
                        if (!elided) {
                            pd_Reg v = pd_parse_expr(p);
                            if (!p->ok) return;
                            /* store into global[elem] */
                            pd_Reg slot = pdR(PD_FAM_GLOBAL, (uint32_t)(baseOff + elem*8));
                            pd_emit1(p->b, PD_MOV, slot, v);
                        }
                        elem++;
                        if (accept_punct(p, ",")) {
                            /* trailing comma before }: list ends (EVAL allows it) */
                            if (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]=='}') break;
                            continue;
                        }
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

    /* Enter a fresh symbol scope. Everything added from here on (re-installed
     * host symbols, params, locals, statics, enums) belongs to THIS function
     * only: their regs index this function's consts/frame, so they must not
     * be reachable from any other function body. */
    int savedScope = p->curScopeId;
    int fnScope = p->nextScopeId++;
    p->curScopeId = fnScope;

    /* Builtins (PI) and host symbols (bstatus, glBegin, ...) were only
     * installed into the MAIN builder at compile start. Each function gets
     * its own builder/consts, so re-install them here: the symbol table
     * favours the most-recently-added entry on lookup, correctly rebinding
     * this function's PI/host-var register indices to ITS consts array. */
    pd_parser_install_builtins(p);
    if (p->host) pd_host_install(p->host, p);

    /* File-scope enum consts (e.g. `enum {GOLDRAT=...}`) were added into the
     * MAIN builder's const pool when encountered at top level. Each function
     * body uses its OWN program/consts array, so those regs would read the
     * wrong slot at runtime (garbage value). Re-bind every file-scope const
     * into THIS function's const pool, preserving its value from the parent
     * builder. ScopeId 0 == file scope (see sym_visible). */
    for (int si = 0; si < p->nSyms; si++) {
        pd_Sym *e = &p->syms[si];
        if (e->kind != PD_SYM_CONST || e->scopeId != 0) continue;
        if (e->reg.fam != PD_FAM_CONST) continue;
        double v = savedB->consts[e->reg.off / 8];
        pd_Sym *n = sym_add(p, e->name, PD_SYM_CONST);
        n->reg = pd_new_const(p->b, v);
    }

    /* save the symbol-table high-water mark: params/locals/host re-installs
     * added while parsing this body are popped again at `restore:`. The scope
     * id above is what guarantees correctness; this just reclaims slots. */
    int savedNSyms = p->nSyms;
    int savedBreak = p->breakLabel, savedCont = p->contLabel;
    p->breakLabel = PD_NO_LOOP; p->contLabel = PD_NO_LOOP;

    /* pre-allocate function slot & register symbol (for recursion).
     * If prescan_functions already registered this name, reuse its slot.
     * The function's own name lives at FILE scope so callers can see it. */
    p->curScopeId = savedScope;
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
    p->curScopeId = fnScope;   /* params/locals belong to the function */

    /* parse parameter list */
    int nParams = 0;
    int refFlag = 0;
    /* Parameter registers: grow on demand so functions with many parameters
     * (evaldraw allows far more than 16) don't overflow a fixed array. */
    int paramCap = 16;
    pd_Reg *paramRegs = (pd_Reg*)malloc(paramCap * sizeof(pd_Reg));
    if (!paramRegs) { pd_error(p, "oom"); return 0; }
    if (!(pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]==')')) {
        for (;;) {
            refFlag = 0;
            /* EVAL parameter forms (Plan section 4):
             *   a        double
             *   &a       pass-by-reference (double*)   -- prefix skipped, treated as double
             *   $a       string arg (char*)            -- prefix skipped, treated as double
             *   a[n]     array                          -- [..] skipped, treated as double
             *   a(,,)    function pointer               -- (..) skipped, treated as double
             * Optional C-style type prefix (double/void/... or a struct type name)
             * is also allowed. */
            int paramStructType = -1;
            while (pd_cur(p)->kind == PD_TOK_IDENT &&
                   !(pd_cur_at(p->tok+1)->kind == PD_TOK_PUNCT &&
                     pd_cur_at(p->tok+1)->len==1 &&
                     (pd_cur_at(p->tok+1)->text[0]==',' || pd_cur_at(p->tok+1)->text[0]==')'))) {
                /* skip a type-prefix ident (e.g. "double x") — heuristic: an
                 * ident followed by another ident is a type+name. If that
                 * prefix is a known struct type, remember it so the parameter
                 * becomes a struct-typed (array) parameter. */
                size_t nx = p->tok+1;
                if (pd_cur_at(nx)->kind != PD_TOK_IDENT) break;
                int ti = struct_type_find(p, pd_cur(p)->text);
                if (ti >= 0) paramStructType = ti;
                pd_eat(p);
            }
            if (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 &&
                (pd_cur(p)->text[0]=='&' || pd_cur(p)->text[0]=='$')) {
                if (pd_cur(p)->text[0]=='&') refFlag = 1; /* pass-by-reference */
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
            if (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]=='&') {
                refFlag = 1;
            }
            /* allocate a PARAM reg. A struct-typed parameter is an array with
             * an extra innermost dimension equal to the field count, so that
             * `pr.x` inside the body lowers to `pr[fieldIndex]`. */
            int paramIsStruct = (paramStructType >= 0);
            int nf = paramIsStruct ? p->structTypes[paramStructType].nFields : 0;
            if (nParams >= paramCap) {
                paramCap *= 2;
                pd_Reg *ng = (pd_Reg*)realloc(paramRegs, paramCap * sizeof(pd_Reg));
                if (!ng) { pd_error(p, "oom"); free(paramRegs); return 0; }
                paramRegs = ng;
            }
            paramRegs[nParams] = pdR(PD_FAM_PARAM, (uint32_t)(nParams * 8));
            pd_Sym *ps = sym_add(p, pname, PD_SYM_PARAM);
            if (!ps) {
                fprintf(stderr, "[parser] sym_add(NULL) for param '%s' nSyms=%d ok=%d\n",
                        pname, p->nSyms, p->ok);
                goto restore;
            }
            ps->reg = paramRegs[nParams];
            ps->refParam = refFlag;
            ps->structType = paramStructType;
            if (paramIsStruct && nf > 0) {
                ps->nDims = 1; ps->dims[0] = nf;
                ps->arraySize = nf;
            }
            if (refFlag && fnSym) fnSym->refMask |= (1 << nParams);
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
    p->curScopeId = savedScope;
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
    free(paramRegs);
    return 1;
}

int pd_parse_stmt(pd_Parser *p) {
    pd_Tok *t = pd_cur(p);
    /* bare compound block: { ... } used as a statement (EVAL allows free-standing
     * blocks, commonly left behind when an `if` is commented out). */
    if (t->kind == PD_TOK_PUNCT && t->len == 1 && t->text[0] == '{') {
        int saveScope = p->curScopeId;
        p->curScopeId = p->nextScopeId++;
        pd_eat(p); /* { */
        while (p->ok && !(pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]=='}')) {
            if (pd_cur(p)->kind == PD_TOK_EOF) { pd_error(p, "unterminated block"); break; }
            pd_parse_stmt(p);
        }
        pd_expect_punct(p, "}");
        p->curScopeId = saveScope;
        return 1;
    }
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
        if (accept_ident(p, "STRUCT")) { parse_struct_decl(p); return 1; }
        /* declaration of a struct-typed variable without `static`:
         *   pt_t p[4];   (EVAL treats these as statics) */
        if (peek_is_ident(p, 1)) {
            char tn[40]; size_t tl = t->len < sizeof(tn) ? t->len : sizeof(tn)-1;
            memcpy(tn, t->text, tl); tn[tl] = 0;
            if (struct_type_find(p, tn) >= 0) { parse_static(p); accept_punct(p, ";"); return 1; }
        }
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
                    /* if there's content between ( and ), it's nParams+1.
                     * Empty parens () → 0 params: `s > afterName` is true for
                     * "()" too (s = afterName+1), which wrongly counted
                     * parameterless functions like `drawcadran()` as 1-arg and
                     * made every call (drawcadran() with 0 args) fail to
                     * resolve (aux=-1) → NaN → black capture. */
                    if (s > afterName + 1) nParams++;
                    /* scan the param list for '&' prefixes (pass-by-reference)
                     * so the call site can pass addresses even when the
                     * function is defined AFTER the calling main body. */
                    int refMask = 0;
                    {
                        int pi = 0, pdepth = 0, expectName = 1;
                        for (size_t k = afterName + 1; k < s; k++) {
                            const pd_Tok *st = pd_cur_at(k);
                            if (st->kind==PD_TOK_PUNCT && st->len==1 && st->text[0]=='(') { pdepth++; continue; }
                            if (st->kind==PD_TOK_PUNCT && st->len==1 && st->text[0]==')') { pdepth--; continue; }
                            if (pdepth > 0) continue;
                            if (st->kind==PD_TOK_PUNCT && st->len==1 && st->text[0]==',') { pi++; expectName = 1; continue; }
                            if (st->kind==PD_TOK_PUNCT && st->len==1 && st->text[0]=='&') { if (expectName) refMask |= (1 << pi); continue; }
                            if (st->kind==PD_TOK_IDENT && expectName) expectName = 0;
                        }
                    }
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
                        if (fs) { fs->nParams = nParams; fs->funcIdx = fidx; fs->refMask = refMask; }
                    }
                }
            }
        }
        p->tok++;
    }
    p->tok = savedTok;
    if (getenv("PD_DEBUG_FUNCS")) {
        for (int i = 0; i < p->nSyms; i++) {
            const pd_Sym *s = &p->syms[i];
            if (s->kind == PD_SYM_FUNC)
                fprintf(stderr, "prescan func '%s' nParams=%d funcIdx=%d\n",
                        s->name, s->nParams, s->funcIdx);
        }
    }
}

/* ---- top-level program parse ---- */
/* Detect & parse the program's main body. EVAL allows three main shapes:
 *   (a) named modern main:  main() { body }   <- RECOMMENDED sugar for (b)
 *   (b) anonymous main:     () { body }       (explicit, may follow decls)
 *   (c) bare block:         { body }          (original EVAL treats a script
 *       whose first '(' lies inside the block as an implicit "()" main — i.e.
 *       a bare block IS the main. We accept this form directly.)
 * Returns 1 if a main body was parsed. */
static int try_parse_anon_main(pd_Parser *p) {
    /* Check for: ( ) {   or   ( params ) {   or   { */
    int hasParen = (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]=='(');
    int hasBareBrace = (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]=='{');

    if (hasParen) {
        /* Peek ahead to find matching ) and check if { follows. */
        int depth = 0;
        int tokIdx = p->tok;
        int foundClose = 0;
        for (;;) {
            pd_Tok *t = pd_cur_at(tokIdx);
            if (t->kind == PD_TOK_EOF) break;
            if (t->kind == PD_TOK_PUNCT && t->len==1 && t->text[0]=='(') depth++;
            else if (t->kind == PD_TOK_PUNCT && t->len==1 && t->text[0]==')') {
                depth--;
                if (depth == 0) { foundClose = 1; break; }
            }
            tokIdx++;
        }
        if (!foundClose) return 0;
        /* After the closing ')', the body may be:
         *   (a) a bare block:  { ... }                 (evaldraw modern form)
         *   (b) a bare statement list (no braces): EVAL
         *       per-pixel entry, e.g.  (x,y,t,&r,&g,&b) if(...) {...} ...
         * We treat (b) as an anonymous main too. The only case we must NOT
         * swallow here is a NAMED function definition `name(params){...}`
         * following the parens — but a per-pixel entry never has a leading
         * name, so `(params) IDENT (params) {` would be a function def and we
         * must let try_parse_function_def handle it. */
        pd_Tok *after = pd_cur_at(tokIdx + 1);
        if (after->kind == PD_TOK_IDENT && pd_cur_at(tokIdx + 2)->kind == PD_TOK_PUNCT &&
            pd_cur_at(tokIdx + 2)->len == 1 && pd_cur_at(tokIdx + 2)->text[0] == '(') {
            /* name ( params ) ... — a named function definition, not an
             * anonymous main. Let the caller's function-def path take it. */
            return 0;
        }
        /* otherwise: { ... } or bare statements — both are an anonymous main */
    }
    if (!hasParen && !hasBareBrace) return 0;

    if (hasParen) {
        pd_eat(p); /* ( */
    }

    /* The main block is a function too: its locals must not be visible to the
     * named functions parsed after it. Without this, `x` inside drawsph()
     * resolved to main's `x` symbol, whose LOCAL offset indexes a completely
     * different frame slot in the callee. */
    int savedScope = p->curScopeId, savedNSyms = p->nSyms;
    p->curScopeId = p->nextScopeId++;

    /* Parse parameter list inside the body scope so params are visible
     * in the body. This supports evaldraw-style entry points like
     * (x,y,t) { ... } or (x,y,&r,&g,&b) { ... }. */
    if (hasParen) {
        int nParams = 0;
        if (!(pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]==')')) {
            for (;;) {
                int refFlag = 0;
                /* Skip optional type prefix (e.g. "double x"). */
                while (pd_cur(p)->kind == PD_TOK_IDENT &&
                       !(pd_cur_at(p->tok+1)->kind == PD_TOK_PUNCT &&
                         pd_cur_at(p->tok+1)->len==1 &&
                         (pd_cur_at(p->tok+1)->text[0]==',' || pd_cur_at(p->tok+1)->text[0]==')'))) {
                    size_t nx = p->tok+1;
                    if (pd_cur_at(nx)->kind != PD_TOK_IDENT) break;
                    pd_eat(p);
                }
                /* & or $ prefix. */
                if (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 &&
                    (pd_cur(p)->text[0]=='&' || pd_cur(p)->text[0]=='$')) {
                    if (pd_cur(p)->text[0]=='&') refFlag = 1;
                    pd_eat(p);
                }
                pd_Tok *pt = pd_eat(p);
                if (pt->kind != PD_TOK_IDENT) { pd_error(p, "expected param name"); return 1; }
                char pname[40]; size_t pnl = pt->len < sizeof(pname)?pt->len:sizeof(pname)-1;
                memcpy(pname, pt->text, pnl); pname[pnl] = 0;
                /* Skip array dims [..]. */
                while (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]=='[') {
                    while (accept_punct(p, "[")) {
                        while (!(pd_cur(p)->kind==PD_TOK_EOF ||
                                 (pd_cur(p)->kind==PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]==']')))
                            pd_eat(p);
                        pd_expect_punct(p, "]");
                    }
                }
                pd_Sym *ps = sym_add(p, pname, PD_SYM_PARAM);
                if (ps) {
                    ps->reg = pdR(PD_FAM_PARAM, (uint32_t)(nParams * 8));
                    ps->refParam = refFlag;
                }
                nParams++;
                if (accept_punct(p, ",")) continue;
                break;
            }
        }
        pd_expect_punct(p, ")");
        p->b->nParams = nParams;
    }

    int bareBody = !(pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 && pd_cur(p)->text[0]=='{');
    if (!bareBody && !pd_expect_punct(p, "{")) return 1;
    int closed = 0;
    while (p->ok) {
        if (!bareBody && pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len==1 &&
            pd_cur(p)->text[0]=='}') { closed = 1; break; }
        if (bareBody && pd_cur(p)->kind == PD_TOK_EOF) break;
        pd_parse_stmt(p);
        if (!p->ok) break;
    }
    if (!bareBody) {
        if (!closed) pd_expect_punct(p, "}");
        else pd_eat(p); /* consume the } */
    }
    pd_emit1(p->b, PD_RETURN, pdR(PD_FAM_VOID,0), p->lastValueReg);
    p->curScopeId = savedScope;
    p->nSyms = savedNSyms;
    return 1;
}

int pd_parse_program(pd_Parser *p) {
    /* The entry point. EVAL allows three ways to write the program body:
     *   (a) bare statements at top level  -> implicit main,
     *   (b) an anonymous main: () { ... } (or a bare { ... }),
     *   (c) a named main: main() { ... }  (modern, explicit).
     * The parser itself does NOT treat `main` specially: `main(){}` is parsed
     * exactly like any other user function (a function named MAIN, registered
     * as PD_SYM_FUNC). Which body becomes the entry point is decided later by
     * the compile / link stage (see pd_compile.c), not here. */
    pd_Reg zero = pd_new_const(p->b, 0.0);
    p->lastValueReg = zero;

    /* pre-register all named functions so forward references resolve */
    prescan_functions(p);

    /* main loop: parse top-level statements. An anonymous main () {...}
     * may appear anywhere (after static/enum decls). Once it's parsed, we
     * emit the final return; subsequent statements are named func defs. */
    if (getenv("PD_DEBUG_TOKS")) {
        for (size_t i = 0; i < p->ts->nToks; i++) {
            pd_Tok *t = &p->ts->toks[i];
            char buf[48]; size_t l = t->len < 47 ? t->len : 47;
            memcpy(buf, t->text, l); buf[l] = 0;
            fprintf(stderr, "TOK[%zu] line=%d kind=%d '%s'\n", i, t->origLine, t->kind, buf);
        }
    }
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

int pd_parser_find_func(pd_Parser *p, const char *name) {
    for (int i = 0; i < p->nSyms; i++) {
        pd_Sym *s = &p->syms[i];
        if (s->kind != PD_SYM_FUNC) continue;
        if (s->funcIdx < 0) continue;
        if (strcmp(s->name, name) == 0) return s->funcIdx;
    }
    return -1;
}
