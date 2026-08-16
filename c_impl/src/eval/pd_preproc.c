/* pd_preproc.c — see pd_preproc.h.
 *
 * Design notes
 * ------------
 * The pass works line by line so the output keeps a 1:1 line mapping with the
 * input (directive lines and skipped branches are emitted as empty lines).
 * That matters because every diagnostic downstream reports original line
 * numbers.
 *
 * Only object-like macros are supported, which is all EVAL scripts use.
 * Macro bodies are themselves macro-expanded at use time (with a recursion
 * cap) so `#define A (B*2)` after `#define B 3` behaves as expected.
 *
 * Conditional expressions are evaluated by a small recursive-descent parser
 * over the *already macro-expanded* directive text. It supports the operators
 * that appear in practice: || && | & == != < <= > >= + - * / % ! ~ (), integer
 * and float literals, and `defined(NAME)`.
 */
#include "pd_preproc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PD_PP_MAX_MACROS 256
#define PD_PP_MAX_NEST   64
#define PD_PP_MAX_EXPAND 32   /* macro re-expansion depth cap */

typedef struct {
    char name[64];
    char *body;   /* malloc'd, may be "" for a bare #define */
} PPMacro;

typedef struct {
    PPMacro macros[PD_PP_MAX_MACROS];
    int nMacros;
    char *err;
    size_t errLen;
    int failed;
} PP;

/* ---- dynamic output buffer ---- */
typedef struct { char *p; size_t n, cap; } Buf;

static int buf_reserve(Buf *b, size_t extra) {
    if (b->n + extra + 1 <= b->cap) return 1;
    size_t nc = b->cap ? b->cap * 2 : 1024;
    while (nc < b->n + extra + 1) nc *= 2;
    char *np = realloc(b->p, nc);
    if (!np) return 0;
    b->p = np; b->cap = nc;
    return 1;
}
static int buf_add(Buf *b, const char *s, size_t len) {
    if (!buf_reserve(b, len)) return 0;
    memcpy(b->p + b->n, s, len);
    b->n += len;
    b->p[b->n] = 0;
    return 1;
}
static int buf_addc(Buf *b, char c) { return buf_add(b, &c, 1); }

static int ident_char(int c) { return isalnum((unsigned char)c) || c == '_'; }
static int ident_start(int c) { return isalpha((unsigned char)c) || c == '_'; }

static void pp_fail(PP *pp, int line, const char *msg) {
    if (!pp->failed) {
        snprintf(pp->err, pp->errLen, "preprocessor: %s at line %d", msg, line);
        pp->failed = 1;
    }
}

static PPMacro *pp_find(PP *pp, const char *name, size_t len) {
    for (int i = 0; i < pp->nMacros; i++) {
        if (strlen(pp->macros[i].name) == len &&
            strncmp(pp->macros[i].name, name, len) == 0)
            return &pp->macros[i];
    }
    return NULL;
}

static void pp_define(PP *pp, const char *name, size_t nlen, const char *body, int line) {
    if (nlen >= sizeof(((PPMacro*)0)->name)) { pp_fail(pp, line, "macro name too long"); return; }
    PPMacro *m = pp_find(pp, name, nlen);
    if (!m) {
        if (pp->nMacros >= PD_PP_MAX_MACROS) { pp_fail(pp, line, "too many macros"); return; }
        m = &pp->macros[pp->nMacros++];
        memcpy(m->name, name, nlen);
        m->name[nlen] = 0;
        m->body = NULL;
    }
    free(m->body);
    m->body = body ? strdup(body) : strdup("");
}

static void pp_undef(PP *pp, const char *name, size_t nlen) {
    PPMacro *m = pp_find(pp, name, nlen);
    if (!m) return;
    free(m->body);
    int idx = (int)(m - pp->macros);
    pp->macros[idx] = pp->macros[--pp->nMacros];
}

/* Expand object-like macros in `src` (length len) into `out`.
 * Identifiers inside string/char literals are left alone. */
static void pp_expand(PP *pp, const char *src, size_t len, Buf *out, int depth) {
    if (depth > PD_PP_MAX_EXPAND) { buf_add(out, src, len); return; }
    size_t i = 0;
    while (i < len) {
        char c = src[i];
        /* copy string / char literals verbatim */
        if (c == '"' || c == '\'') {
            char q = c;
            buf_addc(out, src[i++]);
            while (i < len) {
                if (src[i] == '\\' && i + 1 < len) { buf_addc(out, src[i]); buf_addc(out, src[i+1]); i += 2; continue; }
                buf_addc(out, src[i]);
                if (src[i] == q) { i++; break; }
                i++;
            }
            continue;
        }
        /* line comments: keep as-is, nothing to expand */
        if (c == '/' && i + 1 < len && src[i+1] == '/') {
            buf_add(out, src + i, len - i);
            return;
        }
        if (ident_start((unsigned char)c)) {
            size_t s = i;
            while (i < len && ident_char((unsigned char)src[i])) i++;
            size_t nlen = i - s;
            PPMacro *m = pp_find(pp, src + s, nlen);
            if (m) {
                /* Re-expand the body so nested macros resolve. */
                pp_expand(pp, m->body, strlen(m->body), out, depth + 1);
            } else {
                buf_add(out, src + s, nlen);
            }
            continue;
        }
        buf_addc(out, c);
        i++;
    }
}

/* ---- constant expression evaluation for #if / #elif ---- */
typedef struct { const char *s; PP *pp; int line; } CX;

static void cx_ws(CX *c) { while (*c->s == ' ' || *c->s == '\t') c->s++; }
static double cx_or(CX *c);

static double cx_primary(CX *c) {
    cx_ws(c);
    if (*c->s == '(') { c->s++; double v = cx_or(c); cx_ws(c); if (*c->s == ')') c->s++; return v; }
    if (*c->s == '!') { c->s++; return cx_primary(c) == 0.0 ? 1.0 : 0.0; }
    if (*c->s == '~') { c->s++; return (double)(~(long)cx_primary(c)); }
    if (*c->s == '-') { c->s++; return -cx_primary(c); }
    if (*c->s == '+') { c->s++; return cx_primary(c); }
    if (isdigit((unsigned char)*c->s) || *c->s == '.') {
        char *end = NULL;
        double v = strtod(c->s, &end);
        /* accept the 0x form too */
        if (end && end > c->s + 1 && (c->s[1] == 'x' || c->s[1] == 'X')) {
            v = (double)strtol(c->s, &end, 16);
        }
        c->s = end ? end : c->s + 1;
        /* skip integer suffixes */
        while (*c->s == 'u' || *c->s == 'U' || *c->s == 'l' || *c->s == 'L') c->s++;
        return v;
    }
    if (ident_start((unsigned char)*c->s)) {
        const char *st = c->s;
        while (ident_char((unsigned char)*c->s)) c->s++;
        size_t n = (size_t)(c->s - st);
        if (n == 7 && strncmp(st, "defined", 7) == 0) {
            cx_ws(c);
            int paren = 0;
            if (*c->s == '(') { paren = 1; c->s++; cx_ws(c); }
            const char *ns = c->s;
            while (ident_char((unsigned char)*c->s)) c->s++;
            size_t nn = (size_t)(c->s - ns);
            if (paren) { cx_ws(c); if (*c->s == ')') c->s++; }
            return pp_find(c->pp, ns, nn) ? 1.0 : 0.0;
        }
        /* Undefined identifiers evaluate to 0, as in C. */
        return 0.0;
    }
    return 0.0;
}

static double cx_mul(CX *c) {
    double v = cx_primary(c);
    for (;;) {
        cx_ws(c);
        char op = *c->s;
        if (op == '*' || op == '/' || op == '%') {
            c->s++;
            double r = cx_primary(c);
            if (op == '*') v *= r;
            else if (op == '/') v = (r != 0.0) ? v / r : 0.0;
            else v = (r != 0.0) ? (double)((long)v % (long)r) : 0.0;
        } else break;
    }
    return v;
}
static double cx_add(CX *c) {
    double v = cx_mul(c);
    for (;;) {
        cx_ws(c);
        if (*c->s == '+' && c->s[1] != '+') { c->s++; v += cx_mul(c); }
        else if (*c->s == '-' && c->s[1] != '-') { c->s++; v -= cx_mul(c); }
        else break;
    }
    return v;
}
static double cx_rel(CX *c) {
    double v = cx_add(c);
    for (;;) {
        cx_ws(c);
        if (c->s[0] == '<' && c->s[1] == '=') { c->s += 2; v = (v <= cx_add(c)); }
        else if (c->s[0] == '>' && c->s[1] == '=') { c->s += 2; v = (v >= cx_add(c)); }
        else if (c->s[0] == '<' && c->s[1] != '<') { c->s += 1; v = (v <  cx_add(c)); }
        else if (c->s[0] == '>' && c->s[1] != '>') { c->s += 1; v = (v >  cx_add(c)); }
        else break;
    }
    return v;
}
static double cx_eq(CX *c) {
    double v = cx_rel(c);
    for (;;) {
        cx_ws(c);
        if (c->s[0] == '=' && c->s[1] == '=') { c->s += 2; v = (v == cx_rel(c)); }
        else if (c->s[0] == '!' && c->s[1] == '=') { c->s += 2; v = (v != cx_rel(c)); }
        else break;
    }
    return v;
}
static double cx_band(CX *c) {
    double v = cx_eq(c);
    for (;;) {
        cx_ws(c);
        if (c->s[0] == '&' && c->s[1] != '&') { c->s++; v = (double)((long)v & (long)cx_eq(c)); }
        else break;
    }
    return v;
}
static double cx_bor(CX *c) {
    double v = cx_band(c);
    for (;;) {
        cx_ws(c);
        if (c->s[0] == '|' && c->s[1] != '|') { c->s++; v = (double)((long)v | (long)cx_band(c)); }
        else break;
    }
    return v;
}
static double cx_and(CX *c) {
    double v = cx_bor(c);
    for (;;) {
        cx_ws(c);
        if (c->s[0] == '&' && c->s[1] == '&') { c->s += 2; double r = cx_bor(c); v = (v != 0.0 && r != 0.0); }
        else break;
    }
    return v;
}
static double cx_or(CX *c) {
    double v = cx_and(c);
    for (;;) {
        cx_ws(c);
        if (c->s[0] == '|' && c->s[1] == '|') { c->s += 2; double r = cx_and(c); v = (v != 0.0 || r != 0.0); }
        else break;
    }
    return v;
}

static int pp_eval_cond(PP *pp, const char *text, int line) {
    /* macro-expand first, then evaluate */
    Buf ex = {0};
    pp_expand(pp, text, strlen(text), &ex, 0);
    CX c; c.s = ex.p ? ex.p : ""; c.pp = pp; c.line = line;
    double v = cx_or(&c);
    free(ex.p);
    return v != 0.0;
}

/* ---- directive recognition ---- */
/* Returns the directive keyword length if `p` (already past '#') starts with
 * `kw` followed by a non-identifier char. */
static int is_kw(const char *p, const char *kw) {
    size_t n = strlen(kw);
    if (strncmp(p, kw, n) != 0) return 0;
    if (ident_char((unsigned char)p[n])) return 0;
    return (int)n;
}

char *pd_preprocess(const char *src, char *err, size_t errLen) {
    PP pp;
    memset(&pp, 0, sizeof(pp));
    pp.err = err; pp.errLen = errLen;

    Buf out = {0};

    /* Conditional nesting state.
     *  active[]  : this level is currently emitting
     *  taken[]   : some branch at this level has already been taken
     *  parentOn[]: enclosing levels were all active */
    int active[PD_PP_MAX_NEST], taken[PD_PP_MAX_NEST], parentOn[PD_PP_MAX_NEST];
    int depth = 0;
    active[0] = 1; taken[0] = 1; parentOn[0] = 1;

    const char *s = src;
    int line = 1;
    int inBlockComment = 0;

    while (*s) {
        /* isolate the current line [s, eol) */
        const char *eol = strchr(s, '\n');
        size_t llen = eol ? (size_t)(eol - s) : strlen(s);

        /* Track block comments so a '#' inside a block comment is not treated
         * as a directive. */
        int lineStartsInComment = inBlockComment;
        {
            for (size_t i = 0; i < llen; i++) {
                if (!inBlockComment && s[i] == '/' && i + 1 < llen && s[i+1] == '*') { inBlockComment = 1; i++; }
                else if (inBlockComment && s[i] == '*' && i + 1 < llen && s[i+1] == '/') { inBlockComment = 0; i++; }
                else if (!inBlockComment && s[i] == '/' && i + 1 < llen && s[i+1] == '/') break;
            }
        }

        /* find first non-blank char */
        size_t k = 0;
        while (k < llen && (s[k] == ' ' || s[k] == '\t')) k++;

        int isDirective = (!lineStartsInComment && k < llen && s[k] == '#');

        if (isDirective) {
            const char *d = s + k + 1;
            while (*d == ' ' || *d == '\t') d++;
            size_t dlen = llen - (size_t)(d - s);
            /* directive argument text (strip trailing comment) */
            char argbuf[1024];
            size_t alen = 0;
            {
                const char *a = d;
                size_t rem = dlen;
                /* skip keyword */
                while (rem && ident_char((unsigned char)*a)) { a++; rem--; }
                while (rem && (*a == ' ' || *a == '\t')) { a++; rem--; }
                for (size_t i = 0; i < rem && alen + 1 < sizeof(argbuf); i++) {
                    if (a[i] == '/' && i + 1 < rem && (a[i+1] == '/' || a[i+1] == '*')) break;
                    argbuf[alen++] = a[i];
                }
                while (alen && (argbuf[alen-1] == ' ' || argbuf[alen-1] == '\t' || argbuf[alen-1] == '\r')) alen--;
                argbuf[alen] = 0;
            }

            int on = active[depth];

            if (is_kw(d, "if") || is_kw(d, "ifdef") || is_kw(d, "ifndef")) {
                if (depth + 1 >= PD_PP_MAX_NEST) { pp_fail(&pp, line, "#if nesting too deep"); break; }
                int parent = on;
                int val = 0;
                if (parent) {
                    if (is_kw(d, "ifdef")) {
                        const char *ns = argbuf; size_t nn = 0;
                        while (ident_char((unsigned char)ns[nn])) nn++;
                        val = pp_find(&pp, ns, nn) != NULL;
                    } else if (is_kw(d, "ifndef")) {
                        const char *ns = argbuf; size_t nn = 0;
                        while (ident_char((unsigned char)ns[nn])) nn++;
                        val = pp_find(&pp, ns, nn) == NULL;
                    } else {
                        val = pp_eval_cond(&pp, argbuf, line);
                    }
                }
                depth++;
                parentOn[depth] = parent;
                active[depth]   = parent && val;
                taken[depth]    = active[depth];
            } else if (is_kw(d, "elif")) {
                if (depth == 0) { pp_fail(&pp, line, "#elif without #if"); break; }
                int val = 0;
                if (parentOn[depth] && !taken[depth]) val = pp_eval_cond(&pp, argbuf, line);
                active[depth] = parentOn[depth] && !taken[depth] && val;
                if (active[depth]) taken[depth] = 1;
            } else if (is_kw(d, "else")) {
                if (depth == 0) { pp_fail(&pp, line, "#else without #if"); break; }
                active[depth] = parentOn[depth] && !taken[depth];
                if (active[depth]) taken[depth] = 1;
            } else if (is_kw(d, "endif")) {
                if (depth == 0) { pp_fail(&pp, line, "#endif without #if"); break; }
                depth--;
            } else if (on && is_kw(d, "define")) {
                const char *ns = argbuf; size_t nn = 0;
                while (ident_char((unsigned char)ns[nn])) nn++;
                const char *body = ns + nn;
                /* function-like macros are not supported; treat the whole
                 * remainder as the body (works for the object-like usage in
                 * EVAL scripts). */
                while (*body == ' ' || *body == '\t') body++;
                pp_define(&pp, ns, nn, body, line);
            } else if (on && is_kw(d, "undef")) {
                const char *ns = argbuf; size_t nn = 0;
                while (ident_char((unsigned char)ns[nn])) nn++;
                pp_undef(&pp, ns, nn);
            }
            /* unknown directives (#pragma, #include, ...) are ignored */

            /* directives emit an empty line to preserve numbering */
        } else if (active[depth]) {
            Buf ex = {0};
            pp_expand(&pp, s, llen, &ex, 0);
            if (ex.p) buf_add(&out, ex.p, ex.n);
            free(ex.p);
        }
        /* inactive lines emit nothing (just the newline below) */

        if (pp.failed) break;

        if (eol) { buf_addc(&out, '\n'); s = eol + 1; line++; }
        else break;
    }

    for (int i = 0; i < pp.nMacros; i++) free(pp.macros[i].body);

    if (pp.failed) { free(out.p); return NULL; }
    if (!out.p) { out.p = malloc(1); if (out.p) out.p[0] = 0; }
    return out.p;
}
