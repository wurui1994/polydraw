/* test_jit.c — differential test: sljit JIT vs interpreter must be
 * bit-identical. Part A compares EVAL program results + globals.
 * Part B compares the recorded GLCmdBuf for the 5 target .pss scripts.
 */
#include "test_main.h"
#include "../src/eval/pd_compile.h"
#include "../src/eval/pd_interp.h"
#include "../src/eval/pd_jit.h"
#include "../src/eval/pd_section.h"
#include "../src/render/pd_runlib.h"
#include "../src/render/pd_polyhost_tex.h"
#include "../src/render/glcmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern void test_register_all(void);

/* ---------- Part A: pure EVAL bit-exactness ---------- */
static int cmp_prog(const char *src) {
    char err[256];
    pd_Program prog;
    if (!pd_compile(&prog, src, err, sizeof(err))) {
        printf("  [compile error: %s] for [%s]\n", err, src);
        return 0;
    }
    size_t ng = prog.nGlobals ? prog.nGlobals : 1;
    double *g1 = (double*)calloc(ng, sizeof(double));
    double *g2 = (double*)calloc(ng, sizeof(double));
    pd_srand(12345);
    double ri = pd_run(&prog, NULL, g1, NULL);
    pd_srand(12345);
    double rj = pd_run_jit(&prog, NULL, g2, NULL);

    int ok = 1;
    if (memcmp(&ri, &rj, sizeof(double)) != 0 && !(isnan(ri) && isnan(rj))) {
        printf("  [return mismatch] %s\n    interp=%.17g  jit=%.17g\n", src, ri, rj);
        ok = 0;
    }
    if (memcmp(g1, g2, ng * sizeof(double)) != 0) {
        printf("  [globals mismatch] %s\n", src);
        ok = 0;
    }
    free(g1); free(g2);
    pd_program_free(&prog);
    return ok;
}

TEST(jit_available)      { ASSERT(pd_jit_available()); return 1; }
TEST(jit_enabled_default){ ASSERT(pd_jit_enabled()); return 1; }

TEST(a_arith)   { return cmp_prog("2+3*4 - 10/2 + 1"); }
TEST(a_parens)  { return cmp_prog("(2+3)*(4-1)"); }
TEST(a_power)   { return cmp_prog("2^3^2"); }
TEST(a_mod)     { return cmp_prog("17 % 5"); }
TEST(a_negmod)  { return cmp_prog("-7 % 3"); }
TEST(a_assign)  { return cmp_prog("x=10; y=3; x*y + x/y"); }
TEST(a_cmp)     { return cmp_prog("a=3;b=7; (a<b)+(a==a)-(b<=a)"); }
TEST(a_logic)   { return cmp_prog("a=0;b=1; r=(a&&b)+(a||b); if(a==0) r+=1; r"); }
TEST(a_ternary) { return cmp_prog("r=0; if(1){r=42}else{r=0}; r"); }
TEST(a_while)   { return cmp_prog("i=0;s=0; while(i<100){s+=i;i+=1}; s"); }
TEST(a_for)     { return cmp_prog("s=0; for(i=1;i<=100;i+=1){s+=i}; s"); }
TEST(a_break)   { return cmp_prog("i=0; while(1){ if(i>=7) break; i+=1 }; i"); }
TEST(a_nested)  { return cmp_prog("s=0; for(i=0;i<10;i+=1){for(j=0;j<i;j+=1){s+=1}}; s"); }

TEST(a_math1) {
    return cmp_prog("a=1.5;b=2.5;"
        "r=sin(a)+cos(b)+sqrt(a*b)+exp(a-b)+log(a+b)+abs(a-b);"
        "r=floor(a)+ceil(b)+min(a,b)+max(a,b)+pow(a,b)+atan2(a,b)+fmod(a,b);"
        "r=atan(a)+asin(b/3)+acos(a/3)+tan(a)+r; r");
}
TEST(a_fact)    { return cmp_prog("fact(5)+fact(10)*0+fact(0)"); }
TEST(a_sgn)     { return cmp_prog("sgn(-3)+sgn(0)*10+sgn(4)"); }
TEST(a_unit)    { return cmp_prog("unit(-2)+unit(0)+unit(3)"); }
TEST(a_round)   { return cmp_prog("round0(2.7)+round0(-2.7)+round0(0.4)"); }
TEST(a_rnd)     { return cmp_prog("srand(42); x=rnd(); y=rnd(); x+y*1e-9"); }
TEST(a_nrnd)    { return cmp_prog("srand(7); a=nrnd(); b=nrnd(); a+b"); }

TEST(a_enum)    { return cmp_prog("enum{N=10}; static a[N]; s=0; "
                                  "for(i=0;i<N;i+=1){a[i]=i*2}; "
                                  "for(i=0;i<N;i+=1){s+=a[i]}; s"); }
TEST(a_arrpow2) { return cmp_prog("static a[4]; a[1]=9; a[5]+a[1]"); }
TEST(a_arr3d)   { return cmp_prog("static b[2][3][4]; b[1][2][3]=7; b[1][2][3]"); }

/* user-defined function (JIT entry delegates the call to the interpreter,
 * so recursion is still interpreter-driven — must still match exactly) */
TEST(a_func_fact) {
    return cmp_prog("func f(n){ if(n<=1) return 1; return n*f(n-1) } f(10)");
}
TEST(a_func_use) {
    return cmp_prog("func sq(x){ return x*x } s=0; "
                    "for(i=1;i<=10;i+=1){ s+=sq(i) }; s");
}
TEST(a_func_param) {
    return cmp_prog("func g(a,b){ return a*a+b*b } g(3,4)+g(5,12)");
}

/* ---------- Part B: render GLCmdBuf equivalence ---------- */
static char *read_file(const char *path, size_t *outLen) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f); fclose(f);
    buf[rd] = 0; if (outLen) *outLen = rd; return buf;
}

static GLCmdBuf glcmd_copy(const GLCmdBuf *src) {
    GLCmdBuf dst; dst.n = src->n; dst.cap = src->n;
    dst.cmds = (GLCmd*)malloc(sizeof(GLCmd) * (src->n ? src->n : 1));
    for (size_t i = 0; i < src->n; i++) {
        dst.cmds[i] = src->cmds[i];
        if (src->cmds[i].op == GLCMD_UNIFORM && src->cmds[i].s) {
            int count = src->cmds[i].mode & 0xFFFF;
            int comps = (src->cmds[i].mode >> 16) & 0xFF;
            int n = count * comps;
            if (n > 0) {
                float *cp = (float*)malloc((size_t)n * sizeof(float));
                memcpy(cp, src->cmds[i].s, (size_t)n * sizeof(float));
                dst.cmds[i].s = (const char*)cp;
            }
        } else if (src->cmds[i].op == GLCMD_SETTEXDATA && src->cmds[i].s) {
            /* deep-copy: the source pixels live in the shared pd_tex[] table
             * which a later pd_tex_free_all() (between the two differential
             * runs) frees. Keep a private copy so the snapshot stays valid. */
            size_t px = (size_t)src->cmds[i].b * (size_t)src->cmds[i].c *
                        (size_t)src->cmds[i].d;
            double *cp = (double*)malloc(px * sizeof(double));
            memcpy(cp, src->cmds[i].s, px * sizeof(double));
            dst.cmds[i].s = (const char*)cp;
        } else if (src->cmds[i].op == GLCMD_MULTMATRIX && src->cmds[i].s) {
            double *cp = (double*)malloc(16 * sizeof(double));
            memcpy(cp, src->cmds[i].s, 16 * sizeof(double));
            dst.cmds[i].s = (const char*)cp;
        }
    }
    return dst;
}
static void glcmd_free_copy(GLCmdBuf *b) {
    for (size_t i = 0; i < b->n; i++)
        if (b->cmds[i].op == GLCMD_UNIFORM && b->cmds[i].s)
            free((void*)(void*)b->cmds[i].s);
        else if (b->cmds[i].op == GLCMD_SETTEXDATA && b->cmds[i].s)
            free((void*)(void*)b->cmds[i].s);
        else if (b->cmds[i].op == GLCMD_MULTMATRIX && b->cmds[i].s)
            free((void*)(void*)b->cmds[i].s);
    free(b->cmds);
}
static int glcmd_equal(const GLCmdBuf *a, const GLCmdBuf *b) {
    if (a->n != b->n) {
        printf("    [diff] n mismatch: interp=%zu jit=%zu\n", a->n, b->n);
        return 0;
    }
    for (size_t i = 0; i < a->n; i++) {
        const GLCmd *x = &a->cmds[i], *y = &b->cmds[i];
        if (x->op != y->op || x->mode != y->mode) {
            printf("    [diff] @%zu op: interp=%d mode=%d jit=%d mode=%d\n",
                   i, x->op, x->mode, y->op, y->mode);
            return 0;
        }
        if (memcmp(&x->a, &y->a, 4 * sizeof(double)) != 0) {
            printf("    [diff] @%zu op=%d a/b/c/d: i=(%.17g,%.17g,%.17g,%.17g) j=(%.17g,%.17g,%.17g,%.17g)\n",
                   i, x->op, x->a, x->b, x->c, x->d, y->a, y->b, y->c, y->d);
            return 0;
        }
        if (x->op == GLCMD_UNIFORM && x->s && y->s) {
            int count = x->mode & 0xFFFF;
            int comps = (x->mode >> 16) & 0xFF;
            int n = count * comps;
            if (n > 0 && memcmp(x->s, y->s, (size_t)n * sizeof(float)) != 0) {
                printf("    [diff] @%zu UNIFORM mode=%d first floats: i=(%a,%a) j=(%a,%a)\n",
                       i, x->mode, ((float*)x->s)[0], ((float*)x->s)[1],
                       ((float*)y->s)[0], ((float*)y->s)[1]);
                return 0;
            }
        } else if (x->op == GLCMD_SETTEXDATA || x->op == GLCMD_MULTMATRIX) {
            if (x->op == GLCMD_SETTEXDATA) {
                /* content compare: pixels are per-ctx arrays, identical content */
                size_t px = (size_t)x->b * (size_t)x->c * (size_t)x->d;
                if (!x->s || !y->s || memcmp(x->s, y->s, px * sizeof(double)) != 0) {
                    printf("    [diff] @%zu SETTEXDATA content mismatch (tex=%g px=%zu)\n", i, x->a, px);
                    return 0;
                }
            } else { /* MULTMATRIX: 16 doubles */
                if (!x->s || !y->s || memcmp(x->s, y->s, 16 * sizeof(double)) != 0) {
                    printf("    [diff] @%zu MULTMATRIX content mismatch\n", i);
                    return 0;
                }
            }
        } else if (x->s != y->s) {
            printf("    [diff] @%zu ptr mismatch op=%d mode=%d i=%p j=%p\n",
                   i, x->op, x->mode, (const void*)x->s, (const void*)y->s);
            return 0;
        }
    }
    return 1;
}

static int render_cmp(const char *pss_path) {
    char *src = read_file(pss_path, NULL);
    if (!src) { printf("  [cannot read %s]\n", pss_path); return 0; }
    pd_SectionList sl;
    if (!pd_section_parse(&sl, src)) { free(src); return 0; }
    const pd_Section *hs = pd_section_host(&sl);
    if (!hs) { free(src); return 0; }
    size_t n = hs->end - hs->start;
    char *host = (char*)malloc(n + 1);
    memcpy(host, src + hs->start, n); host[n] = 0;

    char err[256];
    /* Use independent contexts so texture caching (glsettex records a
     * SETTEXDATA only on first load) doesn't make the two runs differ. */
    pdrl_Ctx *ci = pdrl_compile(host, 640, 480, err, sizeof(err));
    pdrl_Ctx *cj = pdrl_compile(host, 640, 480, err, sizeof(err));
    int ok = 1;
    if (!ci || !cj) { printf("  [compile err: %s] %s\n", err, pss_path); ok = 0; }
    else {
        pdrl_set_clock_scale(ci, 1.0 / 60.0);
        pdrl_set_clock_scale(cj, 1.0 / 60.0);
        /* the global texture snapshot table is process-wide; reset it
         * before each run so glsettex records a SETTEXDATA every time and
         * the two recorded GLCmdBufs stay comparable. */
        pd_tex_free_all();
        pd_srand(1);
        pdrl_run_frame(ci, 30);
        GLCmdBuf gi = glcmd_copy(pdrl_glbuf(ci));
        pd_tex_free_all();
        pd_srand(1);
        pdrl_run_frame_jit(cj, 30);
        const GLCmdBuf *gj = pdrl_glbuf(cj);
        if (!glcmd_equal(&gi, gj)) {
            printf("  [glbuf mismatch] %s (interp n=%zu jit n=%zu)\n", pss_path, gi.n, gj->n);
            ok = 0;
        }
        glcmd_free_copy(&gi);
        pdrl_free(ci);
        pdrl_free(cj);
    }
    free(host); free(src);
    return ok;
}

TEST(b_drawsph)   { return render_cmp("/Users/wurui/Documents/polydraw/ken/drawsph.pss"); }
TEST(b_balls2k)   { return render_cmp("/Users/wurui/Documents/polydraw/tigrou/balls2k.pss"); }
TEST(b_metaballs) { return render_cmp("/Users/wurui/Documents/polydraw/tigrou/metaballs.pss"); }
TEST(b_ballsk)    { return render_cmp("/Users/wurui/Documents/polydraw/tigrou/ballsk.pss"); }
TEST(b_disco)     { return render_cmp("/Users/wurui/Documents/polydraw/tigrou/disco ball.pss"); }

/* ---------- Part C: M5 three-way bit-exactness (interp vs LLVM vs sljit) ----------
 * Forces each JIT backend independently (via the enable toggles) and asserts
 * all three produce byte-identical results. Directly satisfies the roadmap
 * M5 acceptance: "LLVM JIT vs 解释器 vs sljit 三方逐位一致". */
static int cmp_three(const char *src) {
    char err[256];
    pd_Program prog;
    if (!pd_compile(&prog, src, err, sizeof(err))) {
        printf("  [compile error: %s] for [%s]\n", err, src);
        return 0;
    }
    int r_llvm = pd_llvm_available(), r_sljit = pd_sljit_available();
    size_t ng = prog.nGlobals ? prog.nGlobals : 1;
    double *gi = (double*)calloc(ng, sizeof(double));
    double *gj = (double*)calloc(ng, sizeof(double));
    double *gk = (double*)calloc(ng, sizeof(double));

    /* interpreter */
    pd_srand(12345);
    double ri = pd_run(&prog, NULL, gi, NULL);

    /* LLVM only */
    pd_llvm_set_enabled(1);
    if (r_sljit) pd_sljit_set_enabled(0);
    pd_srand(12345);
    double rl = pd_run_jit(&prog, NULL, gj, NULL);

    /* sljit only (restore) */
    if (r_sljit) pd_sljit_set_enabled(1);
    pd_llvm_set_enabled(0);
    pd_srand(12345);
    double rs = pd_run_jit(&prog, NULL, gk, NULL);

    /* restore default (both enabled → LLVM preferred) */
    pd_llvm_set_enabled(r_llvm);
    if (r_sljit) pd_sljit_set_enabled(r_sljit);

    int ok = 1;
    if (memcmp(&ri, &rl, sizeof(double)) != 0 && !(isnan(ri) && isnan(rl))) {
        printf("  [interp vs LLVM mismatch] %s interp=%.17g llvm=%.17g\n", src, ri, rl); ok = 0;
    }
    if (memcmp(&ri, &rs, sizeof(double)) != 0 && !(isnan(ri) && isnan(rs))) {
        printf("  [interp vs sljit mismatch] %s interp=%.17g sljit=%.17g\n", src, ri, rs); ok = 0;
    }
    if (memcmp(gi, gj, ng * sizeof(double)) != 0) {
        printf("  [globals interp vs LLVM mismatch] %s\n", src); ok = 0;
    }
    if (memcmp(gi, gk, ng * sizeof(double)) != 0) {
        printf("  [globals interp vs sljit mismatch] %s\n", src); ok = 0;
    }
    free(gi); free(gj); free(gk);
    pd_program_free(&prog);
    return ok;
}

TEST(c_three_arith)  { return cmp_three("2+3*4 - 10/2 + 1"); }
TEST(c_three_pow)    { return cmp_three("2^3^2"); }
TEST(c_three_math)   { return cmp_three("a=1.5;b=2.5; sin(a)+cos(b)+sqrt(a*b)+exp(a-b)+log(a+b)+pow(a,b)+atan2(a,b)+fmod(a,b)"); }
TEST(c_three_rnd)    { return cmp_three("srand(42); x=rnd(); y=nrnd(); x+y"); }
TEST(c_three_while)  { return cmp_three("i=0;s=0; while(i<100){s+=i;i+=1}; s"); }
TEST(c_three_func)   { return cmp_three("func f(n){ if(n<=1) return 1; return n*f(n-1) } f(10)"); }
TEST(c_three_arr)    { return cmp_three("static a[4]; a[1]=9; a[5]+a[1]"); }

static test_fn_t tests[] = {
    test_run_jit_available, test_run_jit_enabled_default,
    test_run_a_arith, test_run_a_parens, test_run_a_power, test_run_a_mod,
    test_run_a_negmod, test_run_a_assign, test_run_a_cmp, test_run_a_logic,
    test_run_a_ternary, test_run_a_while, test_run_a_for, test_run_a_break,
    test_run_a_nested, test_run_a_math1, test_run_a_fact, test_run_a_sgn,
    test_run_a_unit, test_run_a_round, test_run_a_rnd, test_run_a_nrnd,
    test_run_a_enum, test_run_a_arrpow2, test_run_a_arr3d,
    test_run_a_func_fact, test_run_a_func_use, test_run_a_func_param,
    test_run_b_drawsph, test_run_b_balls2k, test_run_b_metaballs,
    test_run_b_ballsk, test_run_b_disco,
    test_run_c_three_arith, test_run_c_three_pow, test_run_c_three_math,
    test_run_c_three_rnd, test_run_c_three_while, test_run_c_three_func,
    test_run_c_three_arr,
    NULL
};

void test_register_all(void) {
    for (int i = 0; tests[i]; i++) tests[i]();
}

int main(void) { RUN(); return test_fail ? 1 : 0; }
