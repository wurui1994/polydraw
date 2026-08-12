/* parser-fold.c — Plan B: linked-list fold expression parser.
 *
 * Faithful port of eval.c:parsefunc's expression parsing (eval.c:1988-4224).
 * The original parses an expression into a flat linked list of operand nodes
 * (each node owns a value register) with the operator BETWEEN node (i-1) and
 * node (i) stored ON node i (gop[i]), plus a next-pointer list (gnext[]).
 * After collecting the whole expression, a per-priority fold loop
 * (eval.c:4205) repeatedly applies the operator at the front of the list:
 *
 *   for i in 0..6:
 *     for z from first node while gnext[z] < nodeCount:
 *       if oprio[gop[gnext[z]]] != i: z = gnext[z]; continue;
 *       emit r[z] = r[z] OP r[gnext[z]]; gnext[z] = gnext[gnext[z]];
 *
 * giving the original's exact precedence (oprio: ^=0, * / %=1, + -=2,
 * < <= > >= =3, == != =4, &&=5, ||=6) and LEFT-associativity for every
 * operator (including ^), plus the two unary-sign rules:
 *   - leading sign at expression start becomes a binary operator with an
 *     implicit 0 left operand (eval.c:1995, the "-x^2" hack);
 *   - mid-expression consecutive +/- toggle negit (eval.c:2019) which
 *     negates the IMMEDIATE next operand (constant fold or NEGMOV), so
 *     ^ binds AFTER the negation (3*-2^2 = 3*((-2)^2)).
 *
 * Nested expressions (parens, call args, array indices) recurse through
 * pd_parse_expr, so when p->useFold is set every level uses this parser.
 * Operands are parsed by pd_parse_primary (shared with Plan A).
 */
#include "pd_parser.h"
#include "pd_host.h"

#include <stdio.h>
#include <string.h>

/* operator precedence LUT, eval.c:7352 */
static int fold_oprio(pd_Op op) {
    switch (op) {
    case PD_POW:                      return 0;
    case PD_TIMES: case PD_SLASH:
    case PD_PERC:                     return 1;
    case PD_PLUS:  case PD_MINUS:     return 2;
    case PD_LES:   case PD_LESEQ:
    case PD_MOR:   case PD_MOREQ:     return 3;
    case PD_EQU:   case PD_NEQU:      return 4;
    case PD_LAND:                     return 5;
    case PD_LOR:                      return 6;
    default:                          return 255;
    }
}

/* current token as a binary operator, or PD_OP_END if it isn't one */
static pd_Op fold_binop(pd_Parser *p) {
    pd_Tok *t = pd_cur(p);
    if (t->kind != PD_TOK_PUNCT) return PD_OP_END;
    if (t->len == 1) {
        switch (t->text[0]) {
        case '^': return PD_POW;
        case '*': return PD_TIMES;
        case '/': return PD_SLASH;
        case '%': return PD_PERC;
        case '+': return PD_PLUS;
        case '-': return PD_MINUS;
        case '<': return PD_LES;
        case '>': return PD_MOR;
        default:  return PD_OP_END;
        }
    }
    if (t->len == 2) {
        if (t->text[0] == '<' && t->text[1] == '=') return PD_LESEQ;
        if (t->text[0] == '>' && t->text[1] == '=') return PD_MOREQ;
        if (t->text[0] == '=' && t->text[1] == '=') return PD_EQU;
        if (t->text[0] == '!' && t->text[1] == '=') return PD_NEQU;
        if (t->text[0] == '&' && t->text[1] == '&') return PD_LAND;
        if (t->text[0] == '|' && t->text[1] == '|') return PD_LOR;
    }
    return PD_OP_END;
}

/* end of an expression segment: a token that is neither operand nor operator.
 * These stop the current fold (the caller consumes them): ) , ; ] for nested
 * expressions, and = / compound-assigns which belong to the assignment
 * statement handler. */
static int fold_expr_end(pd_Parser *p) {
    pd_Tok *t = pd_cur(p);
    if (t->kind == PD_TOK_EOF) return 1;
    if (t->kind != PD_TOK_PUNCT) return 0;
    if (t->len == 1) {
        switch (t->text[0]) {
        case ')': case ',': case ';': case ']':
        case '=': case '[':
            return 1;
        default:
            return 0;
        }
    }
    if (t->len == 2 && t->text[1] == '=' &&
        (t->text[0] == '+' || t->text[0] == '-' || t->text[0] == '*' ||
         t->text[0] == '/' || t->text[0] == '%' || t->text[0] == '^'))
        return 1;
    return 0;
}

/* linked-list fold node */
typedef struct {
    pd_Reg reg;   /* node value; updated in place as the fold runs */
    pd_Op  op;    /* gop: operator stored ON this node (combines prev+this) */
    int    next;  /* gnext: index of the next node (== globi at the end) */
} FoldNode;

#define PD_FOLD_MAX_NODES 4096

static void fold_error(pd_Parser *p, const char *msg) {
    if (p->ok) {
        snprintf(p->err, sizeof(p->err), "%s at line %d", msg, pd_cur(p)->origLine);
        p->errLine = pd_cur(p)->origLine;
        p->ok = 0;
    }
}

pd_Reg pd_fold_parse_expr(pd_Parser *p) {
    FoldNode nodes[PD_FOLD_MAX_NODES];
    int globi = 0;      /* count of created nodes (index of the next one) */
    int negit = 1;
    int firstEntry = 1;

    /* ---- begit (eval.c:1992): sign handling at entry / after operators ---- */
begit:
    if (firstEntry) {
        firstEntry = 0;
        if (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len == 1 &&
            (pd_cur(p)->text[0] == '-' || pd_cur(p)->text[0] == '+')) {
            /* Hack for first '-'/'+' to fix priority of "-x^2": insert an
             * implicit 0 node; the sign becomes a binary operator below. */
            pd_Reg z = pd_new_const(p->b, 0.0);
            nodes[0].reg = pd_new_local(p->b);
            pd_emit1(p->b, PD_MOV, nodes[0].reg, z);
            nodes[0].op  = PD_OP_END;
            nodes[0].next = 1;
            globi = 1;
            nodes[1].op = PD_OP_END;
        }
        negit = 1;
    } else {
        /* Hack to make "2^(+3)" not translate as "2+3": consume consecutive
         * +/- as unary signs toggling negit (a '-' flips the sign). */
        negit = 1;
        while (pd_cur(p)->kind == PD_TOK_PUNCT && pd_cur(p)->len == 1 &&
               (pd_cur(p)->text[0] == '+' || pd_cur(p)->text[0] == '-')) {
            if (pd_cur(p)->text[0] == '-') negit = -negit;
            pd_eat(p);
        }
    }

    /* ---- one operand, or an (malformed) operator, or end ---- */
    {
        pd_Op op = fold_binop(p);
        if (op != PD_OP_END) {
            /* operator where an operand was expected: overwrite gop like the
             * original (only reachable for e.g. "2 * / 3" or a leading non-sign
             * operator like "*2", whose gop is unused by the fold). */
            nodes[globi].op = op;
            pd_eat(p);
            goto begit;
        }
        if (fold_expr_end(p)) goto done;

        pd_Reg reg = pd_parse_primary(p);
        if (!p->ok) return reg;
        if (negit < 0) {
            pd_Reg out = pd_new_local(p->b);
            pd_emit1(p->b, PD_NEGMOV, out, reg);
            reg = out;
        }
        nodes[globi].reg = reg;
        nodes[globi].next = globi + 1;
        globi++;
        nodes[globi].op = PD_OP_END;
    }

    /* ---- operator scan (eval.c:2024 while loop) ---- */
    for (;;) {
        pd_Op op = fold_binop(p);
        if (op == PD_OP_END) break;          /* end of expression segment */
        nodes[globi].op = op;
        pd_eat(p);
        goto begit;                           /* unary signs before the RHS */
    }

done:
    /* trailing operator with no operand → "missing parameter" (eval.c:4195) */
    if (nodes[globi].op != PD_OP_END) {
        fold_error(p, "missing parameter");
        return pd_new_const(p->b, 0.0);
    }
    if (globi == 0) return pd_new_const(p->b, 0.0);   /* null expression */

    /* ---- fold loop (eval.c:4205): per-priority passes over the list ---- */
    for (int i = 0; i < 7; i++) {
        for (int z = 0; nodes[z].next < globi; ) {
            int nx = nodes[z].next;
            if (fold_oprio(nodes[nx].op) != i) { z = nx; continue; }
            pd_Reg out = pd_new_local(p->b);
            pd_emit2(p->b, nodes[nx].op, out, nodes[z].reg, nodes[nx].reg);
            nodes[z].reg = out;
            nodes[z].next = nodes[nx].next;   /* unlink the folded node */
        }
    }
    return nodes[0].reg;
}
