/* TDD tests for host (external) function registration & dispatch. */
#include "test_main.h"
#include "../src/eval/pd_compile.h"
#include "../src/eval/pd_host.h"
#include "../src/eval/pd_interp.h"

extern void test_register_all(void);

/* ---- test host functions ---- */
/* double(double): e.g. a "square" function */
static double hf_square(pd_Host *h, int n, const double *a) { (void)h;(void)n; return a[0]*a[0]; }
/* double(double,double): add */
static double hf_add2(pd_Host *h, int n, const double *a) { (void)h;(void)n; return a[0]+a[1]; }
/* void(double): emit (return 0) */
static double hf_emit(pd_Host *h, int n, const double *a) { (void)h;(void)n; (void)a; return 0; }
/* no-arg: return a constant */
static double hf_answer(pd_Host *h, int n, const double *a) { (void)h;(void)n; (void)a; return 42.0; }
/* variadic printf-like: just count args */
static double hf_count(pd_Host *h, int n, const double *a) { (void)h;(void)a; return (double)n; }

static double g_xres = 320.0, g_yres = 240.0;

/* build a host table with the standard test symbols */
static pd_Host make_host(void) {
    pd_Host h; pd_host_init(&h);
    pd_host_add_fn(&h, "SQUARE()",    hf_square, 0);
    pd_host_add_fn(&h, "ADD2(,)",      hf_add2,   0);
    pd_host_add_fn(&h, "EMIT()",       hf_emit,   0);
    pd_host_add_fn(&h, "ANSWER()",     hf_answer, 0);
    pd_host_add_fn(&h, "COUNT(,.)",    hf_count,  1); /* variadic */
    pd_host_add_var(&h, "XRES",        &g_xres);
    pd_host_add_var(&h, "YRES",        &g_yres);
    return h;
}

static double evh(const char *src, const pd_Host *h) {
    char err[256];
    pd_Program prog;
    if (!pd_compile_host(&prog, src, h, err, sizeof(err))) {
        printf("  [compile error: %s]\n", err);
        return NAN;
    }
    double r = pd_run(&prog, NULL, prog.globals, NULL);
    pd_program_free(&prog);
    return r;
}

TEST(host_fn_1arg)   { pd_Host h=make_host(); ASSERT_NEAR(evh("square(5)", &h), 25, 1e-12); return 1; }
TEST(host_fn_2arg)   { pd_Host h=make_host(); ASSERT_NEAR(evh("add2(3,4)", &h), 7, 1e-12); return 1; }
TEST(host_fn_0arg)   { pd_Host h=make_host(); ASSERT_NEAR(evh("answer()", &h), 42, 1e-12); return 1; }
TEST(host_fn_void)   { pd_Host h=make_host(); ASSERT_NEAR(evh("emit(7)", &h), 0, 1e-12); return 1; }
TEST(host_fn_variadic){ pd_Host h=make_host(); ASSERT_NEAR(evh("count(1,2,3,4)", &h), 4, 1e-12); return 1; }
TEST(host_fn_in_expr){ pd_Host h=make_host(); ASSERT_NEAR(evh("square(3)+add2(1,2)", &h), 12, 1e-12); return 1; }
TEST(host_fn_nested) { pd_Host h=make_host(); ASSERT_NEAR(evh("square(square(2))", &h), 16, 1e-12); return 1; }
TEST(host_fn_in_loop){ pd_Host h=make_host();
    ASSERT_NEAR(evh("s=0; for(i=1;i<=3;i+=1){s=add2(s,i)}; s", &h), 6, 1e-12); return 1; }

TEST(host_var_read)  { pd_Host h=make_host(); ASSERT_NEAR(evh("xres", &h), 320, 1e-12); return 1; }
TEST(host_var_arith) { pd_Host h=make_host(); ASSERT_NEAR(evh("xres*yres", &h), 76800, 1e-9); return 1; }
TEST(host_var_in_expr){ pd_Host h=make_host(); ASSERT_NEAR(evh("xres/2 + yres", &h), 400, 1e-12); return 1; }
TEST(host_var_updates){ g_xres = 100; pd_Host h=make_host();
    ASSERT_NEAR(evh("xres", &h), 100, 1e-12); g_xres = 320; return 1; }

TEST(host_gl_pattern){ /* simulate glBegin/glVertex/glEnd as no-ops */
    pd_Host h; pd_host_init(&h);
    pd_host_add_fn(&h, "GLBEGIN()",   hf_emit, 0);
    pd_host_add_fn(&h, "GLVERTEX(,)", hf_emit, 0);
    pd_host_add_fn(&h, "GLVERTEX(,,)",hf_emit, 0);
    pd_host_add_fn(&h, "GLEND()",     hf_emit, 0);
    /* a glBegin/glVertex/glEnd sequence must compile & run without error */
    double r = evh("glbegin(7); glvertex(1,2); glvertex(3,4); glend(); 99", &h);
    ASSERT_NEAR(r, 99, 1e-12);
    return 1;
}

static test_fn_t tests[] = {
    test_run_host_fn_1arg, test_run_host_fn_2arg, test_run_host_fn_0arg,
    test_run_host_fn_void, test_run_host_fn_variadic, test_run_host_fn_in_expr,
    test_run_host_fn_nested, test_run_host_fn_in_loop,
    test_run_host_var_read, test_run_host_var_arith, test_run_host_var_in_expr,
    test_run_host_var_updates, test_run_host_gl_pattern,
    NULL
};

void test_register_all(void) {
    for (int i = 0; tests[i]; i++) tests[i]();
}

int main(void) { RUN(); return test_fail ? 1 : 0; }
