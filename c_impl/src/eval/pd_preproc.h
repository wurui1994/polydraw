/* pd_preproc.h — C-style preprocessor pass run before the lexer.
 *
 * EVAL scripts use a small subset of the C preprocessor:
 *   #define NAME body            (object-like macros only)
 *   #if <const-expr>  / #elif <const-expr> / #else / #endif
 *   #ifdef NAME       / #ifndef NAME
 *
 * The pass rewrites the source into a new buffer with directives removed,
 * inactive conditional branches blanked out, and object-like macros expanded.
 * Line structure is preserved (removed lines become empty) so that error
 * messages keep pointing at the original line numbers.
 */
#ifndef PD_PREPROC_H
#define PD_PREPROC_H

#include <stddef.h>

/* Preprocess `src`. On success returns a malloc'd NUL-terminated buffer that
 * the caller must free(). On failure returns NULL and fills `err`. */
char *pd_preprocess(const char *src, char *err, size_t errLen);

#endif
