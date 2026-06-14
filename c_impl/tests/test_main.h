/* Minimal test framework. No external deps.
 * Usage:
 *   TEST("name") { ASSERT(cond); ASSERT_EQ(a,b); return 1; }
 *   int main(){ RUN(); return test_fail ? 1 : 0; }
 */
#ifndef TEST_MAIN_H
#define TEST_MAIN_H

#include <stdio.h>
#include <string.h>
#include <math.h>

static int test_count = 0, test_pass = 0, test_fail = 0;
static const char *test_current = "";

#define TEST(name) \
    static int test_fn_##name(void); \
    static void test_run_##name(void) { \
        test_current = #name; test_count++; \
        if (test_fn_##name()) test_pass++; else test_fail++; \
    } \
    static int test_fn_##name(void)

#define RUN() do { \
    extern void test_register_all(void); \
    test_register_all(); \
    printf("---- %d tests: %d passed, %d failed\n", test_count, test_pass, test_fail); \
} while(0)

/* registration mechanism: each test file declares TEST_LIST entries */
typedef void (*test_fn_t)(void);
typedef struct { const char *name; test_fn_t fn; } test_entry_t;

/* Each test file defines TEST_LIST via X-macro pattern: define TEST_FILE
   before including this header in the .c test file, then it generates
   a register function. Simpler approach: collect in test_register_all. */

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  FAIL %s:%d: %s (in %s)\n", __FILE__, __LINE__, #cond, test_current); \
        return 0; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("  FAIL %s:%d: %s != %s (%lld != %lld) (in %s)\n", \
            __FILE__, __LINE__, #a, #b, (long long)(a), (long long)(b), test_current); \
        return 0; \
    } \
} while(0)

#define ASSERT_NEAR(a, b, eps) do { \
    double _da = (a), _db = (b), _de = (eps); \
    double _diff = fabs(_da - _db); \
    if (isnan(_da) || isnan(_db) || _diff > _de) { \
        printf("  FAIL %s:%d: %s != %s (%.16g vs %.16g, eps %.3g) (in %s)\n", \
            __FILE__, __LINE__, #a, #b, _da, _db, _de, test_current); \
        return 0; \
    } \
} while(0)

#define ASSERT_STREQ(a, b) do { \
    if (strcmp((a),(b)) != 0) { \
        printf("  FAIL %s:%d: \"%s\" != \"%s\" (in %s)\n", \
            __FILE__, __LINE__, (a), (b), test_current); \
        return 0; \
    } \
} while(0)

#endif
