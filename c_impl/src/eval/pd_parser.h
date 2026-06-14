/* pd_parser.h — Pratt-style recursive-descent parser.
 *
 * Plan A from Plan/03_Parser.md: elegant Pratt parser for expressions,
 * recursive descent for statements. Produces pd_Builder IR.
 *
 * Symbol table holds: built-in functions (SIN/COS/...), enum constants,
 * local variables, function parameters, user-defined functions, and
 * external (host) symbols registered via pd_add_builtin.
 */
#ifndef PD_PARSER_H
#define PD_PARSER_H

#include "pd_ir.h"
#include "pd_lexer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Symbol kinds ---- */
typedef enum {
    PD_SYM_VAR = 0,      /* local variable (double) */
    PD_SYM_PARAM,        /* function parameter */
    PD_SYM_CONST,        /* enum constant (compile-time value) */
    PD_SYM_BUILTIN,      /* built-in: ABS/SIN/.../PI/RND/NRND (1 or 2-arg funcs) */
    PD_SYM_EXT_VAR,      /* external variable (host: xres, etc) */
    PD_SYM_EXT_FUNC,     /* external function (host: glBegin, etc) */
    PD_SYM_FUNC,         /* user-defined function */
    PD_SYM_ARRAY         /* array (local or global static) */
} pd_SymKind;

typedef struct {
    char       name[40];   /* upper-case */
    pd_SymKind kind;
    int        nParams;    /* for FUNC/BUILTIN/EXT_FUNC: # params; <0 means overload-list */
    pd_Reg     reg;        /* storage (LOCAL/PARAM/CONST/EXT/GLOBAL) */
    /* for arrays: element count (compile-time). 0 if not array. */
    int        arraySize;
    /* linked-list index for function overloading by # params */
    int        nextOverload;
    /* for user FUNC: index into program->funcs[] */
    int        funcIdx;
} pd_Sym;

#define PD_MAX_SYMS 1024

typedef struct {
    pd_Builder *b;
    pd_TokenStream *ts;
    size_t    tok;          /* current token index */

    pd_Sym    syms[PD_MAX_SYMS];
    int       nSyms;

    /* globals storage (shared across all functions in a script) */
    double   *globals;
    size_t    nGlobals;     /* current count of allocated global doubles */
    size_t   *pNGlobals;    /* pointer to host's nGlobals counter (unused) */
    size_t    globalsCap;

    /* user functions table (mirrors program->funcs) */
    pd_Program *funcs;
    size_t     *pNFuncs;

    /* error reporting */
    char      err[256];
    int       errLine;
    int       ok;

    /* loop context for break/continue */
    int       breakLabel;   /* -1 if not in loop */
    int       contLabel;

    /* tracks the lvalue symbol of the most-recently parsed primary, so
     * parse_expr_stmt can implement assignment. NULL if not assignable. */
    pd_Sym        *lastLValue;
    int            lastLValueIsArrayIndex;  /* lvalue was name[idx] */
    pd_Reg         lastArrayIdx;            /* index reg if array-index lvalue */
    pd_Reg         lastValueReg;            /* last bare-expression result (for return) */
} pd_Parser;

void pd_parser_init(pd_Parser *p, pd_Builder *b, pd_TokenStream *ts);
void pd_parser_free(pd_Parser *p);

/* Add a symbol to the table (used by host registration). Returns pointer. */
pd_Sym *pd_parser_sym_add(pd_Parser *p, const char *name, pd_SymKind kind);
/* Find a symbol by name (ignores arity). */
pd_Sym *pd_parser_sym_find(pd_Parser *p, const char *name);

/* ---- built-in registration (called once at startup) ---- */
/* Registers ABS/SIN/COS/.../PI/RND/NRND into the parser's symbol table.
 * External host symbols are added separately via pd_add_host_symbol. */
void pd_parser_install_builtins(pd_Parser *p);

/* Add an external (host) symbol. For variables: kind=EXT_VAR, value ptr.
 * For functions: kind=EXT_FUNC, name with prototype "NAME(,,)" syntax.
 * Returns the symbol index or -1 on table-full. */
int pd_parser_add_ext(pd_Parser *p, const char *proto, void *ptr);

/* ---- top-level entry: parse the whole program ---- */
/* Parses the entry function body. Returns 1 on success, 0 on error. */
int pd_parse_program(pd_Parser *p);

/* ---- expression parser ---- */
/* Parses an expression, returns the result Reg. minPrec is the minimum
 * operator precedence to consider (0 = accept everything). */
pd_Reg pd_parse_expr(pd_Parser *p);

/* Parse a single statement. Used by tests. */
int pd_parse_stmt(pd_Parser *p);

/* helper: look at current token */
pd_Tok *pd_cur(pd_Parser *p);
pd_Tok *pd_eat(pd_Parser *p);   /* advance, return old */
int pd_accept(pd_Parser *p, pd_TokKind kind, const char *text);
int pd_expect(pd_Parser *p, const char *text);

#ifdef __cplusplus
}
#endif
#endif
