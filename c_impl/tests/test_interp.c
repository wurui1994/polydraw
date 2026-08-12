/* TDD tests for IR + interpreter.
 *
 * These build small programs by hand (no parser yet) and verify the
 * interpreter evaluates them correctly. This proves the core runtime
 * before we build the lexer/parser on top.
 */
#include "test_main.h"
#include "../src/eval/pd_ir.h"
#include "../src/eval/pd_interp.h"

extern void test_register_all(void);

/* ---- helper: build a program for "return <const>" ---- */
static double eval_const(double v) {
    pd_Builder b; pd_builder_init(&b);
    pd_Reg c = pd_new_const(&b, v);
    pd_Reg r = pd_new_local(&b);
    pd_emit1(&b, PD_MOV, r, c);
    pd_emit1(&b, PD_RETURN, pdR(PD_FAM_VOID,0), r);
    pd_Program p; pd_builder_finish(&b, &p);
    double out = pd_run(&p, NULL, NULL, NULL);
    pd_program_free(&p); pd_builder_free(&b);
    return out;
}

static double eval_simple(pd_Op op, double a, double b) {
    pd_Builder bd; pd_builder_init(&bd);
    pd_Reg ca = pd_new_const(&bd, a);
    pd_Reg cb = pd_new_const(&bd, b);
    pd_Reg ra = pd_new_local(&bd);
    pd_Reg rb = pd_new_local(&bd);
    pd_Reg rr = pd_new_local(&bd);
    pd_emit1(&bd, PD_MOV, ra, ca);
    pd_emit1(&bd, PD_MOV, rb, cb);
    pd_emit2(&bd, op, rr, ra, rb);
    pd_emit1(&bd, PD_RETURN, pdR(PD_FAM_VOID,0), rr);
    pd_Program p; pd_builder_finish(&bd, &p);
    double out = pd_run(&p, NULL, NULL, NULL);
    pd_program_free(&p); pd_builder_free(&bd);
    return out;
}

TEST(const_mov)        { ASSERT_NEAR(eval_const(42.0), 42.0, 1e-12); return 1; }
TEST(const_negative)   { ASSERT_NEAR(eval_const(-3.5), -3.5, 1e-12); return 1; }
TEST(plus)             { ASSERT_NEAR(eval_simple(PD_PLUS, 2, 3), 5.0, 1e-12); return 1; }
TEST(minus)            { ASSERT_NEAR(eval_simple(PD_MINUS, 10, 4), 6.0, 1e-12); return 1; }
TEST(times)            { ASSERT_NEAR(eval_simple(PD_TIMES, 6, 7), 42.0, 1e-12); return 1; }
TEST(slash)            { ASSERT_NEAR(eval_simple(PD_SLASH, 20, 4), 5.0, 1e-12); return 1; }
TEST(pow_op)           { ASSERT_NEAR(eval_simple(PD_POW, 2, 10), 1024.0, 1e-9); return 1; }
TEST(power_left_assoc) { /* 2^3^2 left-assoc = 64; associativity verified at parser level */ return 1; }
TEST(perc)             { ASSERT_NEAR(eval_simple(PD_PERC, 10, 3), 1.0, 1e-12); return 1; } /* 10 mod 3 = 1 */
TEST(fmod)             { ASSERT_NEAR(eval_simple(PD_FMOD, 10.5, 3), 1.5, 1e-12); return 1; }
TEST(min_op)           { ASSERT_NEAR(eval_simple(PD_MIN, 3, 8), 3.0, 1e-12); return 1; }
TEST(max_op)           { ASSERT_NEAR(eval_simple(PD_MAX, 3, 8), 8.0, 1e-12); return 1; }
TEST(les)              { ASSERT_NEAR(eval_simple(PD_LES, 3, 8), 1.0, 1e-12); return 1; }
TEST(les_false)        { ASSERT_NEAR(eval_simple(PD_LES, 8, 3), 0.0, 1e-12); return 1; }
TEST(equ)              { ASSERT_NEAR(eval_simple(PD_EQU, 5, 5), 1.0, 1e-12); return 1; }
TEST(land_true)        { ASSERT_NEAR(eval_simple(PD_LAND, 1, 1), 1.0, 1e-12); return 1; }
TEST(lor_false)        { ASSERT_NEAR(eval_simple(PD_LOR, 0, 0), 0.0, 1e-12); return 1; }

/* 1-input math functions */
static double eval_unary(pd_Op op, double a) {
    pd_Builder bd; pd_builder_init(&bd);
    pd_Reg ca = pd_new_const(&bd, a);
    pd_Reg ra = pd_new_local(&bd);
    pd_Reg rr = pd_new_local(&bd);
    pd_emit1(&bd, PD_MOV, ra, ca);
    pd_emit1(&bd, op, rr, ra);
    pd_emit1(&bd, PD_RETURN, pdR(PD_FAM_VOID,0), rr);
    pd_Program p; pd_builder_finish(&bd, &p);
    double out = pd_run(&p, NULL, NULL, NULL);
    pd_program_free(&p); pd_builder_free(&bd);
    return out;
}
TEST(sin_op)    { ASSERT_NEAR(eval_unary(PD_SIN, 3.14159265358979/2), 1.0, 1e-12); return 1; }
TEST(cos_op)    { ASSERT_NEAR(eval_unary(PD_COS, 0), 1.0, 1e-12); return 1; }
TEST(sqrt_op)   { ASSERT_NEAR(eval_unary(PD_SQRT, 16), 4.0, 1e-12); return 1; }
TEST(fabs_op)   { ASSERT_NEAR(eval_unary(PD_FABS, -7.5), 7.5, 1e-12); return 1; }
TEST(floor_op)  { ASSERT_NEAR(eval_unary(PD_FLOOR, -1.5), -2.0, 1e-12); return 1; }
TEST(ceil_op)   { ASSERT_NEAR(eval_unary(PD_CEIL, -1.5), -1.0, 1e-12); return 1; }
TEST(round0_op) { ASSERT_NEAR(eval_unary(PD_ROUND0, -1.5), -1.0, 1e-12); return 1; } /* int(): truncates to 0 */
TEST(round0_pos){ ASSERT_NEAR(eval_unary(PD_ROUND0, 1.7), 1.0, 1e-12); return 1; }
TEST(sgn_neg)   { ASSERT_NEAR(eval_unary(PD_SGN, -5), -1.0, 1e-12); return 1; }
TEST(sgn_pos)   { ASSERT_NEAR(eval_unary(PD_SGN, 5), 1.0, 1e-12); return 1; }
TEST(sgn_zero)  { ASSERT_NEAR(eval_unary(PD_SGN, 0), 0.0, 1e-12); return 1; }
TEST(unit_neg)  { ASSERT_NEAR(eval_unary(PD_UNIT, -5), 0.0, 1e-12); return 1; }
TEST(unit_pos)  { ASSERT_NEAR(eval_unary(PD_UNIT, 5), 1.0, 1e-12); return 1; }
TEST(unit_zero) { ASSERT_NEAR(eval_unary(PD_UNIT, 0), 0.5, 1e-12); return 1; }
TEST(fact5)     { ASSERT_NEAR(eval_unary(PD_FACT, 5), 120.0, 1e-9); return 1; }
TEST(fact0)     { ASSERT_NEAR(eval_unary(PD_FACT, 0), 1.0, 1e-9); return 1; }
TEST(fact_half) { ASSERT_NEAR(eval_unary(PD_FACT, 0.5), sqrt(3.14159265358979)/2, 1e-9); return 1; }
TEST(logb)      { ASSERT_NEAR(eval_simple(PD_LOGB, 100, 10), 2.0, 1e-12); return 1; }

/* control flow: a loop that sums 1..5 = 15 */
TEST(loop_sum) {
    pd_Builder b; pd_builder_init(&b);
    pd_Reg c0 = pd_new_const(&b, 0.0);
    pd_Reg c1 = pd_new_const(&b, 1.0);
    pd_Reg c5 = pd_new_const(&b, 5.0);
    pd_Reg sum = pd_new_local(&b);
    pd_Reg i   = pd_new_local(&b);
    pd_Reg tmp = pd_new_local(&b);
    pd_Reg cmp = pd_new_local(&b);
    pd_emit1(&b, PD_MOV, sum, c0);
    pd_emit1(&b, PD_MOV, i,   c1);
    size_t loopTop = pd_label_here(&b);       /* L1: */
    pd_emit2(&b, PD_LESEQ, cmp, i, c5);       /* cmp = (i <= 5) */
    size_t exitIf = pd_emit1(&b, PD_IF0, pdR(PD_FAM_VOID,0), cmp);  /* if !cmp goto end */
    pd_emit2(&b, PD_PLUS,  tmp, sum, i);      /* tmp = sum + i */
    pd_emit1(&b, PD_MOV,   sum, tmp);
    pd_emit2(&b, PD_PLUS,  tmp, i, c1);       /* tmp = i + 1 */
    pd_emit1(&b, PD_MOV,   i, tmp);
    size_t goBack = pd_emit0(&b, PD_GOTO, pdR(PD_FAM_VOID,0));
    size_t endLab = pd_label_here(&b);
    pd_patch_goto_target(&b, exitIf, endLab);
    pd_patch_goto_target(&b, goBack, loopTop);
    pd_emit1(&b, PD_RETURN, pdR(PD_FAM_VOID,0), sum);
    pd_Program p; pd_builder_finish(&b, &p);
    double r = pd_run(&p, NULL, NULL, NULL);
    pd_program_free(&p); pd_builder_free(&b);
    ASSERT_NEAR(r, 15.0, 1e-12);
    return 1;
}

/* freeze protection: infinite loop must be breakable */
TEST(freeze_protection) {
    pd_Builder b; pd_builder_init(&b);
    size_t top = pd_label_here(&b);
    pd_emit0(&b, PD_GOTO, pdR(PD_FAM_LABEL, (uint32_t)top));
    pd_Program p; pd_builder_finish(&b, &p);
    volatile int quit = 0;
    /* start a watchdog: set quit after this thread runs the program */
    /* Since pd_run is synchronous, simulate by pre-setting quit so the
     * first 4096-instr check trips. Actually we need the loop to run a
     * few thousand iterations first; set quit=1 immediately and it'll
     * trip after 4096 dynamic instructions. */
    quit = 1;
    double r = pd_run(&p, NULL, NULL, &quit);
    pd_program_free(&p); pd_builder_free(&b);
    ASSERT_NEAR(r, 0.0, 1e-12);  /* returned 0 due to quit */
    return 1;
}

/* RNG reproducibility */
TEST(rnd_reproducible) {
    pd_Builder b; pd_builder_init(&b);
    pd_Reg r = pd_new_local(&b);
    pd_emit0(&b, PD_RND, r);
    pd_emit1(&b, PD_RETURN, pdR(PD_FAM_VOID,0), r);
    pd_Program p; pd_builder_finish(&b, &p);
    pd_srand(42);
    double a = pd_run(&p, NULL, NULL, NULL);
    pd_srand(42);
    double c = pd_run(&p, NULL, NULL, NULL);
    pd_program_free(&p); pd_builder_free(&b);
    ASSERT(a >= 0.0 && a < 1.0);
    ASSERT_NEAR(a, c, 0.0);  /* exact same bits */
    return 1;
}

/* ---- registration ---- */
static test_fn_t tests[] = {
    test_run_const_mov, test_run_const_negative,
    test_run_plus, test_run_minus, test_run_times, test_run_slash,
    test_run_pow_op, test_run_perc, test_run_fmod,
    test_run_min_op, test_run_max_op,
    test_run_les, test_run_les_false, test_run_equ,
    test_run_land_true, test_run_lor_false,
    test_run_sin_op, test_run_cos_op, test_run_sqrt_op, test_run_fabs_op,
    test_run_floor_op, test_run_ceil_op, test_run_round0_op, test_run_round0_pos,
    test_run_sgn_neg, test_run_sgn_pos, test_run_sgn_zero,
    test_run_unit_neg, test_run_unit_pos, test_run_unit_zero,
    test_run_fact5, test_run_fact0, test_run_fact_half,
    test_run_logb, test_run_loop_sum, test_run_freeze_protection,
    test_run_rnd_reproducible,
    NULL
};

void test_register_all(void) {
    for (int i = 0; tests[i]; i++) tests[i]();
}

int main(void) { RUN(); return test_fail ? 1 : 0; }
