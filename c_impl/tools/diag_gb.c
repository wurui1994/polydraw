/* Temporary diagnostic: count CALL in compiled program (with host + preprocess). */
#include "eval_impl/ed_runlib.h"
#include "eval/pd_ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(sz + 1); fread(b, 1, sz, f); b[sz] = 0; fclose(f);
    return b;
}

static void count_prog(const pd_Program *p, int *total_call, int *drawsph) {
    for (size_t i = 0; i < p->nInstr; i++) {
        if (p->instr[i].op == PD_CALL) {
            (*total_call)++;
            int aux = p->instr[i].aux;
            if (aux <= -1000 && (-1000 - aux) == 7) (*drawsph)++;
        }
    }
    for (size_t f = 0; f < p->nFuncs; f++)
        count_prog(&p->funcs[f], total_call, drawsph);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/Users/wurui/Downloads/evaldraw/demos/goldball2.kc";
    char *src = read_file(path);
    if (!src) { fprintf(stderr, "cannot read\n"); return 1; }
    ed_Ctx *ctx = ed_compile(src, 64, 64);
    if (!ctx) { fprintf(stderr, "compile failed\n"); return 1; }
    int tc = 0, ds = 0;
    count_prog(&ctx->prog, &tc, &ds);
    fprintf(stderr, "[diag] total CALL=%d drawsph(aux=-1007)=%d nInstr=%zu nFuncs=%zu nParams=%zu\n",
            tc, ds, ctx->prog.nInstr, ctx->prog.nFuncs, ctx->prog.nParams);
    ed_free(ctx);
    free(src);
    return 0;
}
