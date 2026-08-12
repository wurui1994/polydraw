/* polydraw-render — render a .pss script offscreen with a real GL context.
 *
 * Full pipeline in C, no Python involved:
 *   .pss → section split → EVAL host block (recorded as GLCmd stream)
 *       → offscreen GL (CGL/EGL) → FBO → glReadPixels → PNG
 *
 * Usage:
 *   polydraw-render file.pss [--frame N] [--w W] [--h H] [--fovy D] [-o out.png]
 *
 * Defaults match the reference pipeline: frame 30, 640x480, fovy 73.74
 * (setfov(90) effective), clock scale 1/60 for deterministic klock().
 */
#include "render/pd_runlib.h"
#include "render/gl_renderer.h"
#include "render/pd_polyhost_tex.h"
#include "eval/pd_section.h"
#include "eval/pd_jit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static char *read_file(const char *path, size_t *outLen) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = 0;
    if (outLen) *outLen = rd;
    return buf;
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL); setbuf(stderr, NULL);
    const char *script = NULL;
    const char *outpath = NULL;
    int frame = 30, w = 640, h = 480, dump_shaders = 0;
    int jit_mode = 2;   /* 2=auto, 1=force on, 0=force off */
    double fovy = 73.74;   /* setfov(90) effective, matches the reference */

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--frame") == 0 && i+1 < argc) frame = atoi(argv[++i]);
        else if (strcmp(argv[i], "--w") == 0 && i+1 < argc)     w = atoi(argv[++i]);
        else if (strcmp(argv[i], "--h") == 0 && i+1 < argc)     h = atoi(argv[++i]);
        else if (strcmp(argv[i], "--fovy") == 0 && i+1 < argc)  fovy = atof(argv[++i]);
        else if (strcmp(argv[i], "-o") == 0 && i+1 < argc)      outpath = argv[++i];
        else if (strcmp(argv[i], "--dump-shaders") == 0)        dump_shaders = 1;
        else if (strcmp(argv[i], "--jit") == 0)                 jit_mode = 1;
        else if (strcmp(argv[i], "--no-jit") == 0)              jit_mode = 0;
        else if (argv[i][0] != '-')                             script = argv[i];
    }
    if (!script) {
        fprintf(stderr,
            "polydraw-render — offscreen GL renderer for .pss scripts\n"
            "Usage:\n"
            "  polydraw-render file.pss [--frame N] [--w W] [--h H]\n"
            "                   [--fovy DEG] [-o out.png]\n");
        return 1;
    }

    /* ---- read + split the .pss into host / @v / @f sections ---- */
    size_t len = 0;
    char *src = read_file(script, &len);
    if (!src) { fprintf(stderr, "cannot read %s\n", script); return 2; }
    pd_SectionList sl;
    if (!pd_section_parse(&sl, src)) {
        fprintf(stderr, "section parse error: %s\n", sl.err);
        free(src); return 1;
    }
    const pd_Section *hs = pd_section_host(&sl);
    if (!hs) { fprintf(stderr, "no host block in %s\n", script); free(src); return 1; }

    /* copy each block into its own null-terminated buffer (the source is
     * shared; sections must not rely on in-place termination) */
    char *host_buf = NULL, *vert_buf = NULL, *frag_buf = NULL;
    const pd_Section *vs = pd_section_find(&sl, PD_SEC_VERTEX, NULL);
    const pd_Section *fs = pd_section_find(&sl, PD_SEC_FRAGMENT, NULL);
    const pd_Section *blocks[3] = {hs, vs, fs};
    char **bufs[3] = {&host_buf, &vert_buf, &frag_buf};
    for (int i = 0; i < 3; i++) {
        if (!blocks[i]) continue;
        size_t n = blocks[i]->end - blocks[i]->start;
        *bufs[i] = malloc(n + 1);
        if (!*bufs[i]) { fprintf(stderr, "out of memory\n"); free(src); return 1; }
        memcpy(*bufs[i], src + blocks[i]->start, n);
        (*bufs[i])[n] = 0;
    }
    const char *host_src = host_buf;
    const char *vert_src = vert_buf;
    const char *frag_src = frag_buf;

    /* Build the full section block table (one null-terminated buffer per
     * section) so scripts with multiple named @v/@f shaders work, and hand
     * it to the host so recorded GLCMD_SETSHADER commands can resolve the
     * GLSL source pointers (by name or per-type index). */
    pdrl_Block *gblocks = calloc((size_t)sl.nSecs, sizeof(pdrl_Block));
    int gnb = 0;
    int idxByType[4] = {0, 0, 0, 0};
    for (int i = 0; i < sl.nSecs; i++) {
        const pd_Section *sec = &sl.secs[i];
        size_t n = sec->end - sec->start;
        char *b = malloc(n + 1);
        if (!b) { fprintf(stderr, "out of memory\n"); free(src); return 1; }
        memcpy(b, src + sec->start, n);
        b[n] = 0;
        pdrl_Block *bk = &gblocks[gnb++];
        bk->src   = b;
        strncpy(bk->name, sec->name, sizeof(bk->name) - 1);
        bk->type  = (int)sec->type;
        bk->index = idxByType[sec->type]++;
    }
    pdrl_install_tex_blocks(gblocks, gnb);

    if (dump_shaders) {
        fprintf(stderr, "hs=[%zu,%zu) vs=[%zu,%zu) fs=[%zu,%zu)\n",
                hs->start, hs->end, vs ? vs->start : 0, vs ? vs->end : 0,
                fs ? fs->start : 0, fs ? fs->end : 0);
        fprintf(stderr, "=== vert_src ===\n%s\n=== frag_src ===\n%s\n", 
                vert_src ? vert_src : "(none)", frag_src ? frag_src : "(none)");
    }

    /* ---- compile host block ---- */
    char err[256];
    pdrl_Ctx *ctx = pdrl_compile(host_src, w, h, err, sizeof(err));
    if (!ctx) { fprintf(stderr, "compile error: %s\n", err); free(src); return 1; }
    pdrl_set_clock_scale(ctx, 1.0 / 60.0);   /* deterministic klock() */

    /* ---- GL renderer (created first so GL state persists across frames
     * like the reference app: textures/captures uploaded on an early frame
     * stay live for every later frame) ---- */
    pd_GLRenderer *rd = pd_gl_renderer_create(w, h, fovy);
    if (!rd) {
        fprintf(stderr, "polydraw-render: GL offscreen renderer failed\n");
        pdrl_free(ctx); free(src); return 1;
    }
    pd_gl_renderer_set_shaders(rd, vert_src, frag_src);

    /* JIT selection: auto → use the JIT backend if any is compiled in
     * (LLVM preferred, then sljit); --jit forces on, --no-jit forces the
     * interpreter. The JIT compiles the whole host program up front and
     * runs each frame through it, identical to the interpreter. */
    int use_jit = (jit_mode == 1) ? 1 : (jit_mode == 2 ? pd_jit_available() : 0);
    double (*run_frame)(pdrl_Ctx *, double) =
        use_jit ? pdrl_run_frame_jit : pdrl_run_frame;
    if (use_jit)
        fprintf(stderr, "polydraw-render: using JIT backend (%s)\n",
                pd_jit_backend_name());

    for (int f = 0; f <= frame; f++) {
        run_frame(ctx, (double)f);
        pd_gl_renderer_render(rd, pdrl_glbuf(ctx));
    }

    /* ---- read back + write PNG (flip: GL bottom-left → image top-left) ---- */
    unsigned char *rgba = malloc((size_t)w * h * 4);
    unsigned char *rgb  = malloc((size_t)w * h * 3);
    if (!rgba || !rgb) { fprintf(stderr, "out of memory\n"); return 1; }
    pd_gl_renderer_read_rgba(rd, rgba);
    for (int y = 0; y < h; y++) {
        const unsigned char *row = rgba + (size_t)(h - 1 - y) * w * 4;
        unsigned char *dst = rgb + (size_t)y * w * 3;
        for (int x = 0; x < w; x++) {
            dst[x*3+0] = row[x*4+0];
            dst[x*3+1] = row[x*4+1];
            dst[x*3+2] = row[x*4+2];
        }
    }
    char outbuf[512];
    if (!outpath) {
        snprintf(outbuf, sizeof(outbuf), "%s_f%d.png", script, frame);
        outpath = outbuf;
    }
    int ok = stbi_write_png(outpath, w, h, 3, rgb, w * 3);
    if (!ok) { fprintf(stderr, "cannot write %s\n", outpath); return 1; }
    printf("wrote %s (%dx%d, frame %d, fovy %.2f)\n", outpath, w, h, frame, fovy);

    free(rgb);
    free(rgba);
    pd_gl_renderer_destroy(rd);
    pdrl_free(ctx);
    free(host_buf);
    free(vert_buf);
    free(frag_buf);
    for (int i = 0; i < gnb; i++) free((void *)gblocks[i].src);
    free(gblocks);
    free(src);
    return 0;
}
