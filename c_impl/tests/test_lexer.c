/* TDD tests for the lexer. */
#include "test_main.h"
#include "../src/eval/pd_lexer.h"

extern void test_register_all(void);

static pd_TokenStream lex_it(const char *src) {
    pd_TokenStream t; pd_lex_init(&t);
    pd_lex(&t, src);
    return t;
}

static const char *k(pd_TokKind k) {
    static const char *n[]={"EOF","NUM","IDENT","STR","PUNCT","ERR"};
    return n[k];
}

TEST(basic_numbers) {
    pd_TokenStream t = lex_it("3 3.14 .5 0xC8");
    ASSERT_EQ(t.nToks, 5); /* 4 nums + EOF */
    ASSERT_EQ(t.toks[0].kind, PD_TOK_NUMBER); ASSERT_NEAR(t.toks[0].num, 3, 1e-12);
    ASSERT_NEAR(t.toks[1].num, 3.14, 1e-12);
    ASSERT_NEAR(t.toks[2].num, 0.5, 1e-12);
    ASSERT_NEAR(t.toks[3].num, 0xC8, 1e-9);  /* 200 */
    pd_lex_free(&t); return 1;
}

TEST(identifiers_uppercased) {
    pd_TokenStream t = lex_it("sin SIN glBegin glvertex");
    ASSERT_EQ(t.nToks, 5);
    ASSERT_STREQ(t.toks[0].text, "SIN");
    ASSERT_STREQ(t.toks[1].text, "SIN");
    ASSERT_STREQ(t.toks[2].text, "GLBEGIN");
    ASSERT_STREQ(t.toks[3].text, "GLVERTEX");
    pd_lex_free(&t); return 1;
}

TEST(punct_multi_char) {
    /* NOTE: ++ and -- are intentionally NOT merged (so "--5" stays two unary -).
     * They lex as two separate + / - tokens. */
    pd_TokenStream t = lex_it("< = <= == != && || +=");
    ASSERT_EQ(t.toks[0].len, 1); ASSERT_STREQ(t.toks[0].text, "<");
    ASSERT_EQ(t.toks[1].len, 1); ASSERT_STREQ(t.toks[1].text, "=");
    ASSERT_STREQ(t.toks[2].text, "<=");
    ASSERT_STREQ(t.toks[3].text, "==");
    ASSERT_STREQ(t.toks[4].text, "!=");
    ASSERT_STREQ(t.toks[5].text, "&&");
    ASSERT_STREQ(t.toks[6].text, "||");
    ASSERT_STREQ(t.toks[7].text, "+=");
    pd_lex_free(&t); return 1;
}

TEST(comments_stripped) {
    pd_TokenStream t = lex_it("a // comment here\nb /* block */ c");
    ASSERT_EQ(t.nToks, 4); /* a b c EOF */
    ASSERT_STREQ(t.toks[0].text, "A");
    ASSERT_STREQ(t.toks[1].text, "B");
    ASSERT_STREQ(t.toks[2].text, "C");
    pd_lex_free(&t); return 1;
}

TEST(string_literal) {
    pd_TokenStream t = lex_it("\"hello world\"");
    ASSERT_EQ(t.toks[0].kind, PD_TOK_STRING);
    ASSERT_STREQ(t.toks[0].text, "hello world");
    pd_lex_free(&t); return 1;
}

TEST(string_case_preserved) {
    pd_TokenStream t = lex_it("\"MixedCase String\"");
    ASSERT_EQ(t.toks[0].kind, PD_TOK_STRING);
    ASSERT_STREQ(t.toks[0].text, "MixedCase String");
    pd_lex_free(&t); return 1;
}

TEST(string_escaped_quote) {
    pd_TokenStream t = lex_it("\"say \\\"hi\\\"\"");
    ASSERT_STREQ(t.toks[0].text, "say \"hi\"");
    pd_lex_free(&t); return 1;
}

TEST(opt_directive_skipped) {
    pd_TokenStream t = lex_it("#opt(nowin98) x = 1");
    ASSERT_STREQ(t.toks[0].text, "X");
    ASSERT_STREQ(t.toks[1].text, "=");
    ASSERT_NEAR(t.toks[2].num, 1, 1e-12);
    pd_lex_free(&t); return 1;
}

TEST(line_numbers_tracked) {
    pd_TokenStream t = lex_it("a\nb\n\nc");
    ASSERT_EQ(t.toks[0].origLine, 1);
    ASSERT_EQ(t.toks[1].origLine, 2);
    ASSERT_EQ(t.toks[2].origLine, 4);
    pd_lex_free(&t); return 1;
}

TEST(expression_tokens) {
    pd_TokenStream t = lex_it("2 + 3 * x^2");
    ASSERT_NEAR(t.toks[0].num, 2, 1e-12);
    ASSERT_STREQ(t.toks[1].text, "+");
    ASSERT_NEAR(t.toks[2].num, 3, 1e-12);
    ASSERT_STREQ(t.toks[3].text, "*");
    ASSERT_STREQ(t.toks[4].text, "X");
    ASSERT_STREQ(t.toks[5].text, "^");
    ASSERT_NEAR(t.toks[6].num, 2, 1e-12);
    pd_lex_free(&t); return 1;
}

TEST(unterminated_string_error) {
    pd_TokenStream t = lex_it("\"oops");
    ASSERT_EQ(t.ok, 0);
    ASSERT(t.err[0] != 0);
    pd_lex_free(&t); return 1;
}

TEST(empty_input) {
    pd_TokenStream t = lex_it("");
    ASSERT_EQ(t.nToks, 1);
    ASSERT_EQ(t.toks[0].kind, PD_TOK_EOF);
    pd_lex_free(&t); return 1;
}

TEST(comment_only) {
    pd_TokenStream t = lex_it("// just a comment\n/* and block */");
    ASSERT_EQ(t.nToks, 1);
    ASSERT_EQ(t.toks[0].kind, PD_TOK_EOF);
    pd_lex_free(&t); return 1;
}

TEST(function_call) {
    pd_TokenStream t = lex_it("sqrt(16)");
    ASSERT_STREQ(t.toks[0].text, "SQRT");
    ASSERT_STREQ(t.toks[1].text, "(");
    ASSERT_NEAR(t.toks[2].num, 16, 1e-12);
    ASSERT_STREQ(t.toks[3].text, ")");
    pd_lex_free(&t); return 1;
}

static test_fn_t tests[] = {
    test_run_basic_numbers, test_run_identifiers_uppercased,
    test_run_punct_multi_char, test_run_comments_stripped,
    test_run_string_literal, test_run_string_case_preserved,
    test_run_string_escaped_quote, test_run_opt_directive_skipped,
    test_run_line_numbers_tracked, test_run_expression_tokens,
    test_run_unterminated_string_error, test_run_empty_input,
    test_run_comment_only, test_run_function_call,
    NULL
};

void test_register_all(void) {
    for (int i = 0; tests[i]; i++) tests[i]();
}

int main(void) { RUN(); return test_fail ? 1 : 0; }
