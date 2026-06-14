/* pd_lexer.h — EVAL tokenizer.
 *
 * See Plan/01_Lexer.md. Converts source string into a token stream
 * while tracking original source positions for error reporting.
 *
 * Behaviour inherited from original kasm87() (eval.c:7014):
 *   - identifiers are case-FOLDED to uppercase (strings preserved)
 *   - whitespace collapsed (one space kept, needed for "GOTO label")
 *   - // line comments and block comments stripped
 *   - #opt(...) preprocessor directives skipped (with paren nesting)
 *   - string literals preserved with original case + escaped quotes
 *   - 0x.. hexadecimal numbers recognised
 */
#ifndef PD_LEXER_H
#define PD_LEXER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PD_TOK_EOF = 0,
    PD_TOK_NUMBER,
    PD_TOK_IDENT,
    PD_TOK_STRING,
    PD_TOK_PUNCT,      /* multi or single char punctuation */
    PD_TOK_ERROR
} pd_TokKind;

typedef struct {
    pd_TokKind kind;
    /* During lexing, bufOff holds an offset into ts->buf (IDENT/PUNCT/NUMBER)
     * or ts->strings (STRING). After pd_lex() finishes, .text is resolved
     * to a stable pointer and is the field callers should use. */
    size_t      bufOff;
    const char *text;
    size_t      len;
    double      num;      /* for NUMBER */
    size_t      origOff;  /* offset in the ORIGINAL source (error reporting) */
    int         origLine; /* 1-based line in original source */
} pd_Tok;

typedef struct {
    pd_Tok   *toks;
    size_t    nToks, capToks;

    /* string table: null-terminated entries concatenated */
    char    *strings;
    size_t   nStrings, capStrings;

    /* uppercase-normalized text buffer (identifiers/punct/number chars) */
    char    *buf;
    size_t   nBuf, capBuf;

    char     err[128];
    int      ok;
} pd_TokenStream;

void pd_lex_init(pd_TokenStream *t);
void pd_lex_free(pd_TokenStream *t);

/* Tokenize source. Returns 1 on success, 0 on lex error (t->err set,
 * but partial token stream may still be available). */
int pd_lex(pd_TokenStream *t, const char *src);

/* Debug dump */
void pd_lex_dump(const pd_TokenStream *t, FILE *f);

/* Helper: is a character a valid identifier continuation char? */
int pd_is_ident_char(int c);

#ifdef __cplusplus
}
#endif
#endif
