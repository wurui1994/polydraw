/* polydraw-eval — standalone CLI that compiles and runs an EVAL script.
 *
 * Usage:
 *   polydraw-eval 'expr'           # compile and evaluate, print result
 *   polydraw-eval -f file.pss      # compile host block of a .pss, run once
 *   polydraw-eval -f file.pss -n 3 # run N iterations (for loops with state)
 *   polydraw-eval -d 'expr'        # dump IR then run
 *
 * For -f, the .pss is split into sections (@v/@g/@f/@h); the host block is
 * compiled with the default polydraw host (printf, klock, xres, gl stubs).
 */
#include "eval/pd_compile.h"
#include "eval/pd_ir.h"
#include "eval/pd_interp.h"
#include "eval/pd_section.h"
#include "pd_polyhost.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* read entire file into a malloc'd buffer */
static char *read_file(const char *path, size_t *outLen) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, sz, f);
    fclose(f);
    buf[rd] = 0;
    if (outLen) *outLen = rd;
    return buf;
}

int main(int argc, char **argv) {
    const char *src = NULL;
    char *fileBuf = NULL;
    int dump = 0;
    int nRuns = 1;
    int useHost = 0;  /* attach polydraw host */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) dump = 1;
        else if (strcmp(argv[i], "-f") == 0 && i+1 < argc) {
            fileBuf = read_file(argv[++i], NULL);
            if (!fileBuf) { fprintf(stderr, "cannot read %s\n", argv[i]); return 2; }
            /* split into sections; use the host block */
            pd_SectionList sl;
            if (!pd_section_parse(&sl, fileBuf)) {
                fprintf(stderr, "section parse error: %s\n", sl.err);
                free(fileBuf); return 1;
            }
            const pd_Section *hs = pd_section_host(&sl);
            if (!hs) {
                fprintf(stderr, "no host block found in .pss\n");
                free(fileBuf); return 1;
            }
            /* null-terminate the host block in-place */
            char saved = fileBuf[hs->end];
            fileBuf[hs->end] = 0;
            src = fileBuf + hs->start;
            useHost = 1;
            (void)saved;
        }
        else if (strcmp(argv[i], "-n") == 0 && i+1 < argc) nRuns = atoi(argv[++i]);
        else if (argv[i][0] != '-') src = argv[i];
    }

    if (!src) {
        fprintf(stderr,
            "polydraw-eval — EVAL script compiler/runner\n"
            "Usage:\n"
            "  polydraw-eval 'expr'         evaluate expression, print result\n"
            "  polydraw-eval -f file.pss    run host block of a .pss once\n"
            "  polydraw-eval -d 'expr'      dump IR then run\n"
            "  polydraw-eval -f f.pss -n N  run N iterations\n");
        return 1;
    }

    /* set up polydraw host state if requested */
    pd_PolyState state;
    pd_Host host;
    const pd_Host *phost = NULL;
    if (useHost) {
        pd_polystate_init(&state);
        pd_polyhost_install(&host, &state);
        phost = &host;
    }

    char err[256];
    pd_Program prog;
    if (!pd_compile_host(&prog, src, phost, err, sizeof(err))) {
        fprintf(stderr, "compile error: %s\n", err);
        free(fileBuf);
        return 1;
    }

    if (dump) {
        printf("---- IR dump ----\n");
        pd_dump_program(&prog, stdout);
        printf("---- running ----\n");
    }

    double result = 0;
    for (int i = 0; i < nRuns; i++) {
        state.numframes = (double)i;
        result = pd_run(&prog, NULL, prog.globals, NULL);
    }

    if (nRuns == 1) {
        printf("= %g\n", result);
    } else {
        printf("after %d runs: = %g\n", nRuns, result);
    }
    /* flush any printf output from the host */
    if (useHost && state.logBuf) fflush(stdout);

    pd_program_free(&prog);
    free(fileBuf);
    free(state.logBuf);
    return 0;
}
