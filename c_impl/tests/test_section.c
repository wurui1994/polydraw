/* TDD tests for the .pss section parser. */
#include "test_main.h"
#include "../src/eval/pd_section.h"

extern void test_register_all(void);

TEST(minimal_script) {
    const char *src = "glquad(1);\n@v\nvoid main(){}\n@f\nvoid main(){}\n";
    pd_SectionList sl;
    ASSERT(pd_section_parse(&sl, src));
    ASSERT_EQ(sl.nSecs, 3);
    ASSERT_EQ(sl.secs[0].type, PD_SEC_HOST);
    ASSERT_EQ(sl.secs[1].type, PD_SEC_VERTEX);
    ASSERT_EQ(sl.secs[2].type, PD_SEC_FRAGMENT);
    return 1;
}

TEST(host_only) {
    const char *src = "x = 5;\n";
    pd_SectionList sl;
    ASSERT(pd_section_parse(&sl, src));
    ASSERT_EQ(sl.nSecs, 1);
    ASSERT_EQ(sl.secs[0].type, PD_SEC_HOST);
    return 1;
}

TEST(explicit_host_marker) {
    const char *src = "@v\nvs\n@h\nhost code\n@f\nfs\n";
    pd_SectionList sl;
    ASSERT(pd_section_parse(&sl, src));
    const pd_Section *h = pd_section_host(&sl);
    ASSERT(h != NULL);
    ASSERT_EQ(h->type, PD_SEC_HOST);
    return 1;
}

TEST(interference_like) {
    /* mimic ken/interference.pss structure */
    const char *src =
        "glBegin(GL_QUADS);\n"
        "glColor(klock(),0,0);\n"
        "glVertex(-2,+2,-1);\n"
        "glEnd();\n"
        "@v: //================================\n"
        "varying vec4 p, c;\n"
        "@f: //================================\n"
        "varying vec4 p, c;\n";
    pd_SectionList sl;
    ASSERT(pd_section_parse(&sl, src));
    ASSERT_EQ(sl.nSecs, 3);
    ASSERT_EQ(sl.secs[0].type, PD_SEC_HOST);
    ASSERT_EQ(sl.secs[1].type, PD_SEC_VERTEX);
    ASSERT_EQ(sl.secs[2].type, PD_SEC_FRAGMENT);
    return 1;
}

TEST(multiple_shaders) {
    /* name capture is not yet fully implemented; this just verifies that
     * multiple @f blocks are detected as separate sections */
    const char *src =
        "code\n@v\nvs1\n@f\nfs0\n@f\nfs1\n";
    pd_SectionList sl;
    ASSERT(pd_section_parse(&sl, src));
    ASSERT_EQ(sl.nSecs, 4);
    ASSERT_EQ(sl.secs[0].type, PD_SEC_HOST);
    ASSERT_EQ(sl.secs[1].type, PD_SEC_VERTEX);
    ASSERT_EQ(sl.secs[2].type, PD_SEC_FRAGMENT);
    ASSERT_EQ(sl.secs[3].type, PD_SEC_FRAGMENT);
    return 1;
}

static test_fn_t tests[] = {
    test_run_minimal_script, test_run_host_only, test_run_explicit_host_marker,
    test_run_interference_like, test_run_multiple_shaders,
    NULL
};

void test_register_all(void) {
    for (int i = 0; tests[i]; i++) tests[i]();
}

int main(void) { RUN(); return test_fail ? 1 : 0; }
