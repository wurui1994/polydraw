/* test_fold.c — M6: Plan A (Pratt) vs Plan B (fold) differential + oracle corpus.
 *
 * The two expression parsers must produce identical results. We check:
 *   1. a corpus of expressions with oracle-verified values (from the original
 *      eval.c via /tmp/oraclebuild/oracle);
 *   2. a broader differential corpus where Plan A == Plan B (bit-tolerance);
 *   3. ~2000 randomly generated expressions where Plan A == Plan B.
 */
#include "test_main.h"
#include "../src/eval/pd_compile.h"
#include "../src/eval/pd_interp.h"

static double run_prog(pd_Program *prog) {
    double r = pd_run(prog, NULL, prog->globals, NULL);
    pd_program_free(prog);
    return r;
}

static double evalA(const char *src) {
    char err[256];
    pd_Program prog;
    if (!pd_compile(&prog, src, err, sizeof(err))) return NAN;
    return run_prog(&prog);
}

static double evalB(const char *src) {
    char err[256];
    pd_Program prog;
    if (!pd_compile_fold_host(&prog, src, NULL, err, sizeof(err))) return NAN;
    return run_prog(&prog);
}

/* compare two values: within eps, or both-NaN / both-same-sign-inf */
static int approx(double a, double b) {
    if (isnan(a) && isnan(b)) return 1;
    if (a == b) return 1;
    return fabs(a - b) < 1e-9;
}

/* ---- 1. oracle-verified corpus (values from the original eval.c) ---- */
typedef struct { const char *expr; double expect; } OracleCase;
static const OracleCase ORACLE[] = {
    { "42", 42 },
    { "2+3", 5 },
    { "10-3+2", 9 },
    { "6*7/2", 21 },
    { "2+3*4", 14 },
    { "2^10", 1024 },
    { "2^3^2", 64 },          /* (2^3)^2 left-assoc */
    { "-2^2", -4 },           /* leading -: -(2^2) */
    { "10%3", 1 },
    { "--5", 5 },
    { "+-+3", -3 },
    { "3*-2^2", 12 },         /* mid -: 3*((-2)^2) */
    { "-2+3", 1 },
    { "-2*3+4", -2 },
    { "-2^2+3", -1 },
    { "2*-3^2", 18 },
    { "4+-2^2", 8 },
    { "2+-3^2", 11 },
    { "1--2^2", -3 },
    { "2*-2^2", 8 },
    { "2^-3", 0.125 },
    { "2^-3^2", 0.015625 },
    { "2*-(3^2)", -18 },
    { "-(2+3)", -5 },
    { "2*-3-4", -10 },
    { "1+-2", -1 },
    { "--2", 2 },
    { "--2^2", -4 },
    { "2^+3", 8 },
    { "2^+-3", 0.125 },
    { "2^--3", 8 },
    { "2^-2^2", 0.0625 },
    { "-2*-2^2", -8 },
    { "1--2", 3 },
    { "2+-3", -1 },
    { "0-2^2", -4 },
    { "-(2)^2", -4 },
    { "-2^-2", -0.25 },
    { "2--2^2", -2 },
    { "2*3^2^3", 1458 },
    { "10-2*3+4/2", 6 },
    { "2*(3+4)^2", 98 },
    { "-2^(3-1)", -4 },
    { "2^2^2^2", 256 },
    { "x=(2+3)*4; x", 20 },
    { "x=(2+3)*(4+5); x", 45 },
    { "x=(2+3)*(4+5)/3; x", 15 },
    { "1+2<4", 1 },
    { "3<8", 1 },
    { "5==5", 1 },
    { "5!=6", 1 },
    { "1&&0", 0 },
    { "0||1", 1 },
    { "2+2==4", 1 },
    { "4>=4", 1 },
    { "3>2", 1 },
    { "2<=2", 1 },
    { "1||1&&0", 1 },
    { "1+1>1&&1", 1 },
    { "3*4==12&&2<3", 1 },
    { "2^2==4||0", 1 },
};

TEST(oracle_corpus) {
    for (size_t i = 0; i < sizeof(ORACLE)/sizeof(ORACLE[0]); i++) {
        double a = evalA(ORACLE[i].expr);
        double b = evalB(ORACLE[i].expr);
        if (!approx(a, ORACLE[i].expect) || !approx(b, ORACLE[i].expect)) {
            printf("  FAIL oracle[%zu] %s: A=%.16g B=%.16g expect=%.16g\n",
                   i, ORACLE[i].expr, a, b, ORACLE[i].expect);
            return 0;
        }
    }
    return 1;
}

/* ---- 2. differential corpus: both plans must agree ---- */
static const char *DIFF[] = {
    "42", "2+3", "(2+3)*4", "((2+3)*(4+5))", "2+3*4", "2^3^2",
    "-2^2", "3*-2^2", "-2+3", "-2*3+4", "-2^2+3", "2*-3^2", "4+-2^2",
    "2+-3^2", "1--2^2", "2*-2^2", "2^-3", "2^-3^2", "2*-(3^2)",
    "-(2+3)", "2*-3-4", "1+-2", "--2", "--2^2", "2^+3", "2^+-3",
    "2^--3", "2^-2^2", "-2*-2^2", "1--2", "2+-3", "0-2^2", "-(2)^2",
    "-2^-2", "2--2^2", "2*3^2^3", "10-2*3+4/2", "(2+3)*(4+5)/3",
    "2*(3+4)^2", "-2^(3-1)", "2^2^2^2", "1+2<4", "3<8", "5==5",
    "5!=6", "1&&0", "0||1", "2+2==4", "4>=4", "3>2", "2<=2", "1||1&&0",
    "1+1>1&&1", "3*4==12&&2<3", "2^2==4||0",
    "a=1, b=2; a+b", "x=2^3^2; y=-x; y", "r=0; if(2+3*4==14){r=1}; r",
    "p=1; for(i=1;i<=5;i+=1){p*=i}; p", "a=5; b=2; a%b; a-b%3",
    "static s; s=2^2^2; s+1", "t=3; t^t", "q=2; q*=-3^2; q",
    "n=6; f=1; while(n>1){f*=n; n-=1}; f",
    "a=2; b=3; a*b", "a=2; b=3; a^-3", "a=-2^2; b=a*3; b",
    "x=10; x+=5; x", "x=10; x*=3; x", "s=0; for(i=0;i<5;i+=1){s+=i}; s",
    "s=0; for(i=0;i<5;i+=1){s+=i*2}; s", "i=0; while(i<4){i+=1}; i",
    "r=0; if(3<8){r=42}else{r=0}; r", "a=3; b=4; c=a*b^2; c",
    "static arr[8]; arr[3]=7; arr[3]", "static arr[8]; for(i=0;i<8;i+=1){arr[i]=i*2}; arr[5]",
    "static m[2][3]; m[1][2]=9; m[1][2]", "enum{N=4}; N^2",
    "abs(-9)+sqrt(16)", "min(3,8)+max(3,8)", "int(2.9)+int(-2.9)",
    "fact(5)", "f(x){x*x} f(6)", "fib(n){if(n<2){n}else{fib(n-1)+fib(n-2)}} fib(10)",
    "-3*(4+2)", "7^0", "1/3*3", "2/3*3", "2^-1*4", "1e3+2", "0.5*0.5",
    "1-2-3-4", "10/2/2", "2+3+4+5", "2-3-4", "5^2^1^3",
    "-2^0", "0^-1", "2^0.5", "2^3*4", "2*3^4",
};

TEST(diff_corpus) {
    for (size_t i = 0; i < sizeof(DIFF)/sizeof(DIFF[0]); i++) {
        double a = evalA(DIFF[i]);
        double b = evalB(DIFF[i]);
        if (!approx(a, b)) {
            printf("  FAIL diff[%zu] %s: A=%.16g B=%.16g\n", i, DIFF[i], a, b);
            return 0;
        }
    }
    return 1;
}

/* ---- 3. random fuzz: generate expressions, compare A vs B ---- */
static const char *FOP2[] = { "+", "-", "*", "/", "^", "%" };
static const char *FOP1[] = { "<", "<=", ">", ">=", "==", "!=", "&&", "||" };
static unsigned frand_state = 12345;
static unsigned frand(void) { frand_state = frand_state * 1103515245u + 12345u; return frand_state >> 16; }
static int fpick(int n) { return (int)(frand() % (unsigned)n); }

static void gen_expr(char *buf, size_t cap, size_t *o, int depth) {
    if (depth <= 0 || (frand() % 100) < 38) {
        /* atom: small number (negate sometimes) or a preset var a/b/c */
        int neg = (frand() % 3) == 0;
        if (frand() & 1)
            *o += snprintf(buf + *o, cap - *o, "%s%d", neg ? "-" : "", 1 + fpick(9));
        else
            *o += snprintf(buf + *o, cap - *o, "%s%c", neg ? "-" : "", "abc"[fpick(3)]);
        return;
    }
    const char *op = (frand() % 3 == 0) ? FOP1[fpick(8)] : FOP2[fpick(6)];
    char l[256], r[256]; size_t lo = 0, ro = 0;
    gen_expr(l, sizeof(l), &lo, depth - 1);
    gen_expr(r, sizeof(r), &ro, depth - 1);
    int paren = (frand() % 3 == 0);
    if (paren) *o += snprintf(buf + *o, cap - *o, "(%s%s%s)", l, op, r);
    else       *o += snprintf(buf + *o, cap - *o, "%s%s%s", l, op, r);
}

TEST(fuzz_diff) {
    for (int it = 0; it < 2000; it++) {
        char expr[512], src[1024];
        size_t o = 0;
        gen_expr(expr, sizeof(expr), &o, 4);
        /* evaluate under 3 random variable assignments so an A==B match on a
         * single input cannot hide a per-value divergence */
        for (int run = 0; run < 3; run++) {
            snprintf(src, sizeof(src), "a=%d; b=%d; c=%d; %s",
                     1 + fpick(9), 1 + fpick(9), 1 + fpick(9), expr);
            double a = evalA(src);
            double b = evalB(src);
            if (!approx(a, b)) {
                printf("  FAIL fuzz[%d] %s (a,b,c run %d): A=%.16g B=%.16g\n",
                       it, expr, run, a, b);
                return 0;
            }
        }
    }
    return 1;
}

void test_register_all(void) {
    test_run_oracle_corpus();
    test_run_diff_corpus();
    test_run_fuzz_diff();
}

int main(void) {
    RUN();
    return test_fail ? 1 : 0;
}
