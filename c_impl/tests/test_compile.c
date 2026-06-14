/* TDD tests for the full compile pipeline: source -> IR -> run.
 *
 * These are the "real" EVAL behaviour tests. Each test compiles an EVAL
 * source snippet and checks the evaluated result.
 */
#include "test_main.h"
#include "../src/eval/pd_compile.h"

extern void test_register_all(void);

static double ev(const char *src) {
    char err[256];
    double r = pd_eval(src, err, sizeof(err));
    if (isnan(r)) printf("  [compile error: %s]\n", err);
    return r;
}

static int ev_ok(const char *src) {
    char err[256];
    pd_Program prog;
    int ok = pd_compile(&prog, src, err, sizeof(err));
    if (ok) pd_program_free(&prog);
    else printf("  [compile error: %s]\n", err);
    return ok;
}

/* ---- expressions ---- */
TEST(num_literal)        { ASSERT_NEAR(ev("42"), 42, 1e-12); return 1; }
TEST(neg_literal)        { ASSERT_NEAR(ev("-7"), -7, 1e-12); return 1; }
TEST(add)                { ASSERT_NEAR(ev("2+3"), 5, 1e-12); return 1; }
TEST(add_sub)            { ASSERT_NEAR(ev("10-3+2"), 9, 1e-12); return 1; }
TEST(mul_div)            { ASSERT_NEAR(ev("6*7/2"), 21, 1e-12); return 1; }
TEST(pemdas1)            { ASSERT_NEAR(ev("2+3*4"), 14, 1e-12); return 1; }
TEST(pemdas2)            { ASSERT_NEAR(ev("(2+3)*4"), 20, 1e-12); return 1; }
TEST(power)              { ASSERT_NEAR(ev("2^10"), 1024, 1e-9); return 1; }
TEST(power_right_assoc)  { ASSERT_NEAR(ev("2^3^2"), 512, 1e-9); return 1; } /* 2^(3^2) */
TEST(neg_power)          { ASSERT_NEAR(ev("-2^2"), -4, 1e-9); return 1; }    /* -(2^2) */
TEST(modulo)             { ASSERT_NEAR(ev("10%3"), 1, 1e-12); return 1; }
TEST(unary_chain)        { ASSERT_NEAR(ev("--5"), 5, 1e-12); return 1; }
TEST(unary_chain2)       { ASSERT_NEAR(ev("+-+3"), -3, 1e-12); return 1; }

/* ---- builtins ---- */
TEST(sin_halfpi)         { ASSERT_NEAR(ev("sin(PI/2)"), 1.0, 1e-12); return 1; }
TEST(sqrt)               { ASSERT_NEAR(ev("sqrt(16)"), 4, 1e-12); return 1; }
TEST(abs)                { ASSERT_NEAR(ev("abs(-9)"), 9, 1e-12); return 1; }
TEST(floor_ceil)         { ASSERT_NEAR(ev("floor(2.7)+ceil(2.1)"), 5, 1e-12); return 1; }
TEST(log2arg)            { ASSERT_NEAR(ev("log(100,10)"), 2, 1e-12); return 1; }
TEST(log1arg)            { ASSERT_NEAR(ev("log(2.718281828)"), 1, 1e-6); return 1; }
TEST(min_max)            { ASSERT_NEAR(ev("min(3,8)+max(3,8)"), 11, 1e-12); return 1; }
TEST(atan_alias)         { ASSERT_NEAR(ev("atn(0)"), atan(0.0), 1e-12); return 1; }
TEST(sqr_alias)          { ASSERT_NEAR(ev("sqr(25)"), 5, 1e-12); return 1; }
TEST(int_truncates)      { ASSERT_NEAR(ev("int(2.9)"), 2, 1e-12); return 1; }
TEST(int_neg)            { ASSERT_NEAR(ev("int(-2.9)"), -2, 1e-12); return 1; }
TEST(sgn)                { ASSERT_NEAR(ev("sgn(-5)"), -1, 1e-12); return 1; }
TEST(unit)               { ASSERT_NEAR(ev("unit(0)"), 0.5, 1e-12); return 1; }
TEST(fact5)              { ASSERT_NEAR(ev("fact(5)"), 120, 1e-9); return 1; }

/* ---- comparisons / logic ---- */
TEST(less_true)          { ASSERT_NEAR(ev("3<8"), 1, 1e-12); return 1; }
TEST(equal_true)         { ASSERT_NEAR(ev("5==5"), 1, 1e-12); return 1; }
TEST(not_equal)          { ASSERT_NEAR(ev("5!=6"), 1, 1e-12); return 1; }
TEST(and_logic)          { ASSERT_NEAR(ev("1&&0"), 0, 1e-12); return 1; }
TEST(or_logic)           { ASSERT_NEAR(ev("0||1"), 1, 1e-12); return 1; }
TEST(chain_cmp)          { ASSERT_NEAR(ev("2+2==4"), 1, 1e-12); return 1; }

/* ---- variables & assignment ---- */
TEST(assign_return)      { ASSERT_NEAR(ev("x=5"), 5, 1e-12); return 1; }
TEST(var_use)            { ASSERT_NEAR(ev("x=10; x*2"), 20, 1e-12); return 1; }
TEST(var_multi)          { ASSERT_NEAR(ev("a=3; b=4; a*b"), 12, 1e-12); return 1; }
TEST(compound_add)       { ASSERT_NEAR(ev("x=10; x+=5; x"), 15, 1e-12); return 1; }
TEST(compound_mul)       { ASSERT_NEAR(ev("x=10; x*=3; x"), 30, 1e-12); return 1; }
TEST(var_case_insens)    { ASSERT_NEAR(ev("Foo=7; FOO+foo"), 14, 1e-12); return 1; }

/* ---- control flow ---- */
/* NOTE: in EVAL, `if` is a statement (not an expression), so its block has
 * no value. The return value is the LAST top-level expression. These tests
 * use a trailing expression to capture the effect. */
TEST(if_true)            { ASSERT_NEAR(ev("r=0; if(1){r=42}else{r=0}; r"), 42, 1e-12); return 1; }
TEST(if_false)           { ASSERT_NEAR(ev("r=0; if(0){r=42}else{r=99}; r"), 99, 1e-12); return 1; }
TEST(while_loop)         { ASSERT_NEAR(ev("i=0; s=0; while(i<5){s+=i; i+=1}; s"), 10, 1e-12); return 1; }
TEST(for_loop)           { ASSERT_NEAR(ev("s=0; for(i=0;i<5;i+=1){s+=i}; s"), 10, 1e-12); return 1; }
TEST(for_loop_body)      { ASSERT_NEAR(ev("p=1; for(i=1;i<=5;i+=1){p*=i}; p"), 120, 1e-9); return 1; }
TEST(break_in_loop)      { ASSERT_NEAR(ev("i=0; while(1){ if(i>=3) break; i+=1 }; i"), 3, 1e-12); return 1; }

/* ---- value expression (EVAL convention: last expr = return) ---- */
TEST(last_value)         { ASSERT_NEAR(ev("1; 2; 3"), 3, 1e-12); return 1; }
TEST(trailing_expr)      { ASSERT_NEAR(ev("x=5; x*x"), 25, 1e-12); return 1; }

/* ---- comments & whitespace ---- */
TEST(line_comment)       { ASSERT_NEAR(ev("2+3 // comment\n+1"), 6, 1e-12); return 1; }
TEST(block_comment)      { ASSERT_NEAR(ev("2 /* hi */ + 3"), 5, 1e-12); return 1; }

/* ---- error cases ---- */
TEST(empty_returns_zero) { ASSERT_NEAR(ev(""), 0, 1e-12); return 1; }
TEST(missing_paren_err)  { ASSERT(!ev_ok("(2+3")); return 1; }

/* ---- enum & static & arrays ---- */
TEST(enum_basic)         { ASSERT_NEAR(ev("enum{A,B,C}; A+B+C"), 3, 1e-12); return 1; }
TEST(enum_with_init)     { ASSERT_NEAR(ev("enum{A=10,B,C=20}; A+B+C"), 41, 1e-12); return 1; }
TEST(enum_implicit_seq)  { ASSERT_NEAR(ev("enum{X=5,Y,Z}; Z"), 7, 1e-12); return 1; }
TEST(static_scalar)      { ASSERT_NEAR(ev("static g; g=42; g"), 42, 1e-12); return 1; }
TEST(static_array_write) { ASSERT_NEAR(ev("static a[4]; a[1]=7; a[1]"), 7, 1e-12); return 1; }
TEST(static_array_pow2)  { /* size 4 power-of-2 → index wraps: a[5]==a[1] */
    ASSERT_NEAR(ev("static a[4]; a[1]=9; a[5]"), 9, 1e-12); return 1; }
TEST(static_array_nonpow2){ /* size 3 → OOB becomes index 0 */
    ASSERT_NEAR(ev("static a[3]; a[1]=9; a[5]"), 0, 1e-12); return 1; }
TEST(enum_as_array_size){ ASSERT_NEAR(ev("enum{N=4}; static a[N]; a[2]=5; a[2]"), 5, 1e-12); return 1; }
TEST(array_sum_loop)    {
    ASSERT_NEAR(ev("enum{N=10}; static a[N]; s=0; for(i=0;i<N;i+=1){a[i]=i*2}; for(i=0;i<N;i+=1){s+=a[i]}; s"), 90, 1e-9);
    return 1;
}
TEST(static_preserves)   { /* globals persist across the single run */
    ASSERT_NEAR(ev("static counter; counter+=1; counter+=1; counter"), 2, 1e-12); return 1;
}

static test_fn_t tests[] = {
    test_run_num_literal, test_run_neg_literal, test_run_add, test_run_add_sub,
    test_run_mul_div, test_run_pemdas1, test_run_pemdas2, test_run_power,
    test_run_power_right_assoc, test_run_neg_power, test_run_modulo,
    test_run_unary_chain, test_run_unary_chain2,
    test_run_sin_halfpi, test_run_sqrt, test_run_abs, test_run_floor_ceil,
    test_run_log2arg, test_run_log1arg, test_run_min_max, test_run_atan_alias,
    test_run_sqr_alias, test_run_int_truncates, test_run_int_neg,
    test_run_sgn, test_run_unit, test_run_fact5,
    test_run_less_true, test_run_equal_true, test_run_not_equal,
    test_run_and_logic, test_run_or_logic, test_run_chain_cmp,
    test_run_assign_return, test_run_var_use, test_run_var_multi,
    test_run_compound_add, test_run_compound_mul, test_run_var_case_insens,
    test_run_if_true, test_run_if_false, test_run_while_loop, test_run_for_loop,
    test_run_for_loop_body, test_run_break_in_loop,
    test_run_last_value, test_run_trailing_expr,
    test_run_line_comment, test_run_block_comment,
    test_run_empty_returns_zero, test_run_missing_paren_err,
    test_run_enum_basic, test_run_enum_with_init, test_run_enum_implicit_seq,
    test_run_static_scalar, test_run_static_array_write,
    test_run_static_array_pow2, test_run_static_array_nonpow2,
    test_run_enum_as_array_size, test_run_array_sum_loop, test_run_static_preserves,
    NULL
};

void test_register_all(void) {
    for (int i = 0; tests[i]; i++) tests[i]();
}

int main(void) { RUN(); return test_fail ? 1 : 0; }
