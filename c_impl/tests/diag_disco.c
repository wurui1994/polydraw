/* diag_disco.c — TEMPORARY diagnostic: locate the exact interp-vs-JIT
 * divergence in "disco ball.pss". Prints the first differing GLCmd index and
 * the surrounding commands, plus the globals after each run. */
#include "../src/eval/pd_compile.h"
#include "../src/eval/pd_interp.h"
#include "../src/eval/pd_jit.h"
#include "../src/eval/pd_section.h"
#include "../src/render/pd_runlib.h"
#include "../src/render/glcmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)sz + 1);
    size_t rd = fread(b, 1, (size_t)sz, f);
    fclose(f); b[rd] = 0; return b;
}

static const char *opname(int op) {
    static const char *n[] = {"CLEAR","BEGIN","END","VERTEX","COLOR","TEXCOORD",
        "NORMAL","PUSHMAT","POPMAT","TRANSLATE","ROTATE","SCALE","MATRIXMODE",
        "LOADIDENT","PERSP","ORTHO","VIEWPORT","QUAD","ENABLE","DISABLE",
        "BLENDFUNC","CULLFACE","LINEWIDTH","SETTEXDATA","BINDTEX","ACTIVETEX",
        "CAPTURE","CAPTUREEND","SETFOV","SETSHADER","UNIFORMLOC","UNIFORM",
        "MULTMATRIX"};
    if (op >= 0 && op < (int)(sizeof(n)/sizeof(n[0]))) return n[op];
    return "?";
}

static void dump(const GLCmdBuf *b, size_t from, size_t to, const char *tag) {
    for (size_t i = from; i < to && i < b->n; i++) {
        const GLCmd *c = &b->cmds[i];
        printf("  %s @%zu %-10s mode=%d a=%.17g b=%.17g c=%.17g d=%.17g\n",
               tag, i, opname(c->op), c->mode, c->a, c->b, c->c, c->d);
    }
}

int main(void) {
    const char *path = "/Users/wurui/Documents/polydraw/tigrou/disco ball.pss";
    char *src = slurp(path);
    if (!src) { printf("cannot read\n"); return 1; }
    pd_SectionList sl;
    pd_section_parse(&sl, src);
    const pd_Section *hs = pd_section_host(&sl);
    size_t n = hs->end - hs->start;
    char *host = malloc(n + 1);
    memcpy(host, src + hs->start, n); host[n] = 0;

    char err[256];
    pdrl_Ctx *ci = pdrl_compile(host, 640, 480, err, sizeof err);
    pdrl_Ctx *cj = pdrl_compile(host, 640, 480, err, sizeof err);
    pdrl_set_clock_scale(ci, 1.0 / 60.0);
    pdrl_set_clock_scale(cj, 1.0 / 60.0);

    pd_tex_free_all();
    pd_srand(1);
    double ri = pdrl_run_frame(ci, 30);
    /* snapshot interp buffer (shallow: we only read a/b/c/d/op/mode) */
    const GLCmdBuf *bi0 = pdrl_glbuf(ci);
    size_t ni = bi0->n;
    GLCmd *snap = malloc(sizeof(GLCmd) * ni);
    memcpy(snap, bi0->cmds, sizeof(GLCmd) * ni);
    GLCmdBuf bi; bi.cmds = snap; bi.n = ni; bi.cap = ni;

    pd_tex_free_all();
    pd_srand(1);
    double rj = pdrl_run_frame_jit(cj, 30);
    const GLCmdBuf *bj = pdrl_glbuf(cj);

    printf("ret: interp=%.17g jit=%.17g\n", ri, rj);
    printf("n:   interp=%zu jit=%zu\n", bi.n, bj->n);

    size_t lim = bi.n < bj->n ? bi.n : bj->n;
    size_t first = (size_t)-1;
    for (size_t i = 0; i < lim; i++) {
        const GLCmd *x = &bi.cmds[i], *y = &bj->cmds[i];
        if (x->op != y->op || x->mode != y->mode ||
            memcmp(&x->a, &y->a, 4 * sizeof(double)) != 0) { first = i; break; }
    }
    if (first == (size_t)-1) {
        printf("common prefix identical up to %zu; tail differs only in length\n", lim);
        printf("--- interp tail ---\n"); dump(&bi, lim > 6 ? lim - 6 : 0, bi.n, "I");
        printf("--- jit tail ---\n");    dump(bj, lim > 6 ? lim - 6 : 0, bj->n, "J");
    } else {
        printf("first difference @%zu\n", first);
        size_t f = first > 4 ? first - 4 : 0;
        printf("--- interp ---\n"); dump(&bi, f, first + 5, "I");
        printf("--- jit ---\n");    dump(bj, f, first + 5, "J");
    }

    /* globals after each run */
    size_t ng = 0;
    printf("globals differ?\n");
    (void)ng;

    free(snap); free(host); free(src);
    pdrl_free(ci); pdrl_free(cj);
    return 0;
}
