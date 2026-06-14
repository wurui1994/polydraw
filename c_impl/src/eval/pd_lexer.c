/* pd_lexer.c — EVAL tokenizer implementation.
 *
 * Mirrors the lexical behaviour embedded in kasm87() (eval.c:7014) but
 * produces an explicit token stream rather than a compacted character
 * buffer.  See Plan/01_Lexer.md.
 */
#include "pd_lexer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

void pd_lex_init(pd_TokenStream *t) { memset(t, 0, sizeof(*t)); t->ok = 1; }

void pd_lex_free(pd_TokenStream *t) {
    free(t->toks);   t->toks = NULL;
    free(t->strings); t->strings = NULL;
    free(t->buf);    t->buf = NULL;
    memset(t, 0, sizeof(*t));
}

int pd_is_ident_char(int c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           c == '_';
}

static int is_ident_start(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int grow(void **p, size_t *cap, size_t need, size_t sz) {
    if (need <= *cap) return 1;
    size_t nc = *cap ? *cap * 2 : 32;
    while (nc < need) nc *= 2;
    void *np = realloc(*p, nc * sz);
    if (!np) return 0;
    *p = np; *cap = nc;
    return 1;
}

/* push a char into the upper-case buffer; return its index */
static size_t buf_push(pd_TokenStream *t, char c) {
    if (!grow((void**)&t->buf, &t->capBuf, t->nBuf + 1, 1)) { t->ok = 0; return 0; }
    size_t i = t->nBuf++;
    t->buf[i] = c;
    return i;
}

/* intern a string literal into the string table; return index */
static size_t strings_push(pd_TokenStream *t, const char *s, size_t len) {
    if (!grow((void**)&t->strings, &t->capStrings, t->nStrings + len + 1, 1)) { t->ok = 0; return 0; }
    size_t i = t->nStrings;
    memcpy(t->strings + i, s, len);
    t->nStrings += len;
    t->strings[t->nStrings++] = 0;
    return i;
}

static pd_Tok *new_tok(pd_TokenStream *t) {
    if (!grow((void**)&t->toks, &t->capToks, t->nToks + 1, sizeof(pd_Tok))) { t->ok = 0; return NULL; }
    pd_Tok *tk = &t->toks[t->nToks++];
    memset(tk, 0, sizeof(*tk));
    return tk;
}

/* multi-char punctuation; longest match first.
 * NOTE: ++ and -- are NOT merged here so that "--5" lexes as two unary '-'.
 * Prefix/postfix ++/-- as statement ops can be handled in the parser. */
static const char *PUNCT2[] = {"<=",">=","==","!=","&&","||","+=","-=","*=","/=","%=","^=",NULL};
static const char *VALID_PUNCT = "+-*/%^()[]{};,<>=&$."
                                 "<=>!&|";

static int starts_punct(char c) {
    return strchr(VALID_PUNCT, c) != NULL;
}

int pd_lex(pd_TokenStream *t, const char *src) {
    size_t i = 0;
    int line = 1;
    while (src[i]) {
        /* track line numbers (in original source) */
        char c = src[i];

        /* whitespace: collapse, but keep line counting */
        if (c == ' ' || c == '\t') { i++; continue; }
        if (c == '\r') { i++; continue; }
        if (c == '\n') { line++; i++; continue; }

        /* line comment // */
        if (c == '/' && src[i+1] == '/') {
            while (src[i] && src[i] != '\n') i++;
            continue;
        }
        /* block comment */
        if (c == '/' && src[i+1] == '*') {
            i += 2;
            while (src[i] && !(src[i] == '*' && src[i+1] == '/')) {
                if (src[i] == '\n') line++;
                i++;
            }
            if (src[i]) i += 2;
            continue;
        }
        /* #opt(...) skip with paren nesting */
        if (c == '#' && strncmp(src+i, "#opt(", 5) == 0) {
            i += 5;
            int depth = 1;
            while (src[i] && depth > 0) {
                if (src[i] == '(') depth++;
                else if (src[i] == ')') depth--;
                if (src[i] == '\n') line++;
                i++;
            }
            continue;
        }

        /* string literal */
        if (c == '"') {
            size_t origOff = i;
            int origLine = line;
            i++; /* skip opening quote */
            /* first pass into a temp; we read raw, processing \" escapes */
            char tmp[1024];
            size_t tl = 0;
            while (src[i] && src[i] != '"') {
                if (src[i] == '\\' && src[i+1] == '"') {
                    if (tl < sizeof(tmp)) tmp[tl++] = '"';
                    i += 2;
                } else {
                    if (src[i] == '\n') line++;
                    if (tl < sizeof(tmp)) tmp[tl++] = src[i];
                    i++;
                }
            }
            if (src[i] != '"') {
                snprintf(t->err, sizeof(t->err), "unterminated string");
                t->ok = 0;
                pd_Tok *tk = new_tok(t);
                tk->kind = PD_TOK_ERROR;
                tk->origOff = origOff;
                tk->origLine = origLine;
                goto finalize;
            }
            i++; /* skip closing quote */
            size_t si = strings_push(t, tmp, tl);
            pd_Tok *tk = new_tok(t);
            tk->kind = PD_TOK_STRING;
            tk->bufOff = si;   /* offset into strings[] */
            tk->len = tl;
            tk->origOff = origOff;
            tk->origLine = origLine;
            continue;
        }

        /* number: decimal, .5, hex */
        if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)src[i+1]))) {
            size_t origOff = i;
            int origLine = line;
            if (c == '0' && (src[i+1] == 'x' || src[i+1] == 'X')) {
                i += 2;
                while (isxdigit((unsigned char)src[i])) i++;
                long val = strtol(src + origOff, NULL, 16);
                pd_Tok *tk = new_tok(t);
                tk->kind = PD_TOK_NUMBER;
                tk->num = (double)val;
                size_t hstart = t->nBuf;
                for (size_t k = origOff; k < i; k++) buf_push(t, src[k]);
                buf_push(t, '\0');
                tk->bufOff = hstart;
                tk->len = i - origOff;
                tk->origOff = origOff; tk->origLine = origLine;
                continue;
            }
            while (isdigit((unsigned char)src[i])) i++;
            if (src[i] == '.') { i++; while (isdigit((unsigned char)src[i])) i++; }
            if (src[i] == 'e' || src[i] == 'E') {
                size_t j = i+1;
                if (src[j] == '+' || src[j] == '-') j++;
                if (isdigit((unsigned char)src[j])) { i = j; while (isdigit((unsigned char)src[i])) i++; }
            }
            pd_Tok *tk = new_tok(t);
            tk->kind = PD_TOK_NUMBER;
            tk->num = strtod(src + origOff, NULL);
            size_t dstart = t->nBuf;
            for (size_t k = origOff; k < i; k++) buf_push(t, src[k]);
            buf_push(t, '\0');
            tk->bufOff = dstart;
            tk->len = i - origOff;
            tk->origOff = origOff; tk->origLine = origLine;
            continue;
        }

        /* identifier */
        if (is_ident_start(c)) {
            size_t origOff = i;
            int origLine = line;
            size_t start = t->nBuf;
            while (pd_is_ident_char((unsigned char)src[i])) {
                char cc = src[i];
                if (cc >= 'a' && cc <= 'z') cc -= 32; /* fold to upper */
                buf_push(t, cc);
                i++;
            }
            buf_push(t, '\0');
            pd_Tok *tk = new_tok(t);
            tk->kind = PD_TOK_IDENT;
            tk->bufOff = start;   /* offset into buf[] (offset stable across realloc) */
            tk->len = t->nBuf - start - 1;
            tk->origOff = origOff; tk->origLine = origLine;
            continue;
        }

        /* punctuation: try 3-char, 2-char, then 1-char */
        if (starts_punct(c)) {
            size_t origOff = i;
            int origLine = line;
            int matched = 0;
            /* 2-char */
            for (int k = 0; PUNCT2[k]; k++) {
                if (strncmp(src+i, PUNCT2[k], 2) == 0) {
                    size_t start = t->nBuf;
                    buf_push(t, src[i]); buf_push(t, src[i+1]); buf_push(t, '\0');
                    pd_Tok *tk = new_tok(t);
                    tk->kind = PD_TOK_PUNCT;
                    tk->bufOff = start;
                    tk->len = 2;
                    tk->origOff = origOff; tk->origLine = origLine;
                    i += 2;
                    matched = 1;
                    break;
                }
            }
            if (matched) continue;
            size_t start = t->nBuf;
            buf_push(t, c); buf_push(t, '\0');
            pd_Tok *tk = new_tok(t);
            tk->kind = PD_TOK_PUNCT;
            tk->bufOff = start;
            tk->len = 1;
            tk->origOff = origOff; tk->origLine = origLine;
            i++;
            continue;
        }

        /* unknown character */
        snprintf(t->err, sizeof(t->err), "unexpected char '%c' (0x%02x) at line %d", c, (unsigned char)c, line);
        t->ok = 0;
        pd_Tok *tk = new_tok(t);
        tk->kind = PD_TOK_ERROR;
        tk->origOff = i; tk->origLine = line;
        goto finalize;
    }
    /* EOF token */
    pd_Tok *eof = new_tok(t);
    eof->kind = PD_TOK_EOF;
    eof->origOff = i; eof->origLine = line;
finalize:
    /* Resolve stable text pointers now that buffers will no longer realloc. */
    for (size_t k = 0; k < t->nToks; k++) {
        pd_Tok *tk = &t->toks[k];
        if (tk->kind == PD_TOK_STRING) tk->text = t->strings + tk->bufOff;
        else if (tk->kind == PD_TOK_NUMBER || tk->kind == PD_TOK_IDENT ||
                 tk->kind == PD_TOK_PUNCT)
            tk->text = t->buf + tk->bufOff;
    }
    return t->ok;
}

void pd_lex_dump(const pd_TokenStream *t, FILE *f) {
    static const char *kn[] = {"EOF","NUM","IDENT","STR","PUNCT","ERR"};
    for (size_t i = 0; i < t->nToks; i++) {
        const pd_Tok *tk = &t->toks[i];
        fprintf(f, "[%3zu] L%-3d %-6s ", i, tk->origLine, kn[tk->kind]);
        if (tk->kind == PD_TOK_NUMBER) fprintf(f, "%g", tk->num);
        else if (tk->len && tk->text) fprintf(f, "'%.*s'", (int)tk->len, tk->text);
        fprintf(f, "\n");
    }
}
