/* pd_compile.c — top-level compile pipeline. */
#include "pd_compile.h"
#include "pd_lexer.h"
#include "pd_parser.h"
#include "pd_interp.h"
#include "pd_host.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

static int pd_compile_impl(pd_Program *prog, const char *src, const pd_Host *host,
                           int useFold, char *err, size_t errLen) {
    pd_TokenStream ts; pd_lex_init(&ts);
    if (!pd_lex(&ts, src) || !ts.ok) {
        snprintf(err, errLen, "lex error: %s", ts.err);
        pd_lex_free(&ts);
        return 0;
    }
    pd_Builder b; pd_builder_init(&b);
    pd_Parser p; pd_parser_init(&p, &b, &ts);
    p.host = host;
    p.useFold = useFold;
    pd_parser_install_builtins(&p);
    if (host) pd_host_install(host, &p);
    pd_parse_program(&p);
    if (!p.ok) {
        snprintf(err, errLen, "%s", p.err);
        pd_lex_free(&ts);
        pd_builder_free(&b);
        free(p.globals);
        return 0;
    }

    /* ---- link stage: choose the entry point (this is NOT done during
     * parsing). If the top-level body is empty (the user wrote only user
     * function definitions, e.g. an explicit `main(){}`), promote the MAIN
     * function to the entry program. Otherwise the top-level body is the entry
     * and MAIN (if present) stays as an ordinary, uncalled function. */
    int mainIdx = pd_parser_find_func(&p, "MAIN");
    if (mainIdx >= 0 && b.nInstr <= 1) {
        /* Promote the explicit MAIN function to the entry program. The top-level
         * body is empty here, so MAIN becomes the entry instead.
         * Re-index CALL targets: dropping the MAIN slot from the function table
         * shifts every later function down by one. */
        for (int f = 0; f < p.nFuncs; f++) {
            for (int i = 0; i < (int)p.funcs[f].nInstr; i++) {
                pd_Instr *ins = &p.funcs[f].instr[i];
                if (ins->op == PD_CALL && ins->aux > mainIdx) ins->aux--;
            }
        }
        pd_Program promoted = p.funcs[mainIdx];
        for (int i = mainIdx; i + 1 < p.nFuncs; i++) p.funcs[i] = p.funcs[i + 1];
        p.nFuncs--;
        /* discard the empty top-level entry builder */
        pd_Program empty;
        int okEmpty = pd_builder_finish(&b, &empty);
        if (!okEmpty) {
            snprintf(err, errLen, "entry build failed: %s", b.err);
            pd_program_free(&empty);
            pd_lex_free(&ts);
            pd_builder_free(&b);
            free(p.globals);
            return 0;
        }
        pd_program_free(&empty);
        pd_lex_free(&ts);
        pd_builder_free(&b);
        free(p.globals);
        *prog = promoted;
        prog->globals = p.globals;
        prog->nGlobals = p.nGlobals;
        prog->funcs = p.funcs;
        prog->nFuncs = p.nFuncs;
        p.funcs = NULL;
        p.globals = NULL;
        if (host) pd_host_attach(prog, host);
        return 1;
    }

    int ok = pd_builder_finish(&b, prog);
    if (!ok) {
        snprintf(err, errLen, "build finish failed: %s", b.err);
        pd_lex_free(&ts);
        pd_builder_free(&b);
        free(p.globals);
        return 0;
    }
    prog->globals = p.globals;
    prog->nGlobals = p.nGlobals;
    p.globals = NULL;
    /* transfer user functions */
    prog->funcs = p.funcs;
    prog->nFuncs = p.nFuncs;
    p.funcs = NULL;
    if (host) pd_host_attach(prog, host);
    pd_lex_free(&ts);
    pd_builder_free(&b);
    return ok;
}

int pd_compile_host(pd_Program *prog, const char *src, const pd_Host *host, char *err, size_t errLen) {
    return pd_compile_impl(prog, src, host, 0, err, errLen);
}

int pd_compile_fold_host(pd_Program *prog, const char *src, const pd_Host *host, char *err, size_t errLen) {
    return pd_compile_impl(prog, src, host, 1, err, errLen);
}

int pd_compile(pd_Program *prog, const char *src, char *err, size_t errLen) {
    return pd_compile_host(prog, src, NULL, err, errLen);
}

double pd_eval(const char *src, char *err, size_t errLen) {
    pd_Program prog;
    if (!pd_compile(&prog, src, err, errLen)) return NAN;
    double r = pd_run(&prog, NULL, prog.globals, NULL);
    pd_program_free(&prog);
    return r;
}
