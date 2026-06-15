/* test_render.c — verify the gl command recording layer.
 * Compiles a host block that issues gl calls and checks the recorded buffer. */
#include "test_main.h"
#include "../src/render/pd_runlib.h"

extern void test_register_all(void);

/* helper: find first command of a given op, return index or -1 */
static int find_op(const GLCmdBuf *b, int op) {
    for (size_t i = 0; i < b->n; i++) if (b->cmds[i].op == op) return (int)i;
    return -1;
}

TEST(record_begin_vertex_end) {
    char err[128];
    const char *src =
        "glColor(0.5,0.25,0.125);"
        "glBegin(GL_POLYGON);"
        " glTexCoord(0,0); glVertex(-1,-1,-1);"
        " glTexCoord(1,0); glVertex( 1,-1,-1);"
        " glTexCoord(1,1); glVertex( 1, 1,-1);"
        "glEnd();";
    pdrl_Ctx *ctx = pdrl_compile(src, 64, 48, err, sizeof err);
    if (!ctx) { printf("  compile err: %s\n", err); return 0; }
    pdrl_run_frame(ctx, 0);
    const GLCmdBuf *b = pdrl_glbuf(ctx);
    /* expect: COLOR, BEGIN(POLYGON=9), TEXCOORD, VERTEX, TEXCOORD, VERTEX, TEXCOORD, VERTEX, END */
    int ib = find_op(b, GLCMD_BEGIN);
    ASSERT(ib >= 0);
    ASSERT_EQ(b->cmds[ib].mode, PDGL_POLYGON);
    int ie = find_op(b, GLCMD_END);
    ASSERT(ie > ib);
    /* count vertices between begin and end */
    int nv = 0;
    for (int i = ib; i < ie; i++) if (b->cmds[i].op == GLCMD_VERTEX) nv++;
    ASSERT_EQ(nv, 3);
    /* first vertex should be (-1,-1,-1) */
    int iv = -1;
    for (int i = ib; i < ie; i++) if (b->cmds[i].op == GLCMD_VERTEX) { iv = i; break; }
    ASSERT(iv >= 0);
    ASSERT_NEAR(b->cmds[iv].a, -1, 1e-12);
    ASSERT_NEAR(b->cmds[iv].b, -1, 1e-12);
    ASSERT_NEAR(b->cmds[iv].c, -1, 1e-12);
    pdrl_free(ctx);
    return 1;
}

TEST(record_gl_constant_resolved) {
    /* GL_POLYGON must resolve to 9 via the host-var registration */
    char err[128];
    const char *src = "glBegin(GL_POLYGON); glEnd();";
    pdrl_Ctx *ctx = pdrl_compile(src, 64, 48, err, sizeof err);
    if (!ctx) { printf("  compile err: %s\n", err); return 0; }
    pdrl_run_frame(ctx, 0);
    const GLCmdBuf *b = pdrl_glbuf(ctx);
    int ib = find_op(b, GLCMD_BEGIN);
    ASSERT(ib >= 0);
    ASSERT_EQ(b->cmds[ib].mode, PDGL_POLYGON);
    pdrl_free(ctx);
    return 1;
}

TEST(record_per_frame_reset) {
    char err[128];
    pdrl_Ctx *ctx = pdrl_compile("glBegin(GL_POINTS); glEnd();", 8, 8, err, sizeof err);
    if (!ctx) return 0;
    pdrl_run_frame(ctx, 0);
    const GLCmdBuf *b = pdrl_glbuf(ctx);
    size_t n0 = b->n;
    ASSERT(n0 > 0);
    /* second frame: buffer reset, should not accumulate */
    pdrl_run_frame(ctx, 1);
    ASSERT_EQ(b->n, n0);
    pdrl_free(ctx);
    return 1;
}

static test_fn_t tests[] = {
    test_run_record_begin_vertex_end,
    test_run_record_gl_constant_resolved,
    test_run_record_per_frame_reset,
    NULL
};

void test_register_all(void) {
    for (int i = 0; tests[i]; i++) tests[i]();
}

int main(void) { RUN(); return test_fail ? 1 : 0; }
