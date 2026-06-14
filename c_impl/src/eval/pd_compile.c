/* pd_compile.c — top-level compile pipeline. */
#include "pd_compile.h"
#include "pd_lexer.h"
#include "pd_parser.h"
#include "pd_interp.h"
#include "pd_host.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

int pd_compile_host(pd_Program *prog, const char *src, const pd_Host *host, char *err, size_t errLen) {
    pd_TokenStream ts; pd_lex_init(&ts);
    if (!pd_lex(&ts, src) || !ts.ok) {
        snprintf(err, errLen, "lex error: %s", ts.err);
        pd_lex_free(&ts);
        return 0;
    }
    pd_Builder b; pd_builder_init(&b);
    pd_Parser p; pd_parser_init(&p, &b, &ts);
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
    int ok = pd_builder_finish(&b, prog);
    if (!ok) snprintf(err, errLen, "build finish failed");
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
