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
#include "eval/pd_section.h"

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
    const char *script = NULL;
    const char *outpath = NULL;
    int frame = 30, w = 640, h = 480, dump_shaders = 0;
    double fovy = 73.74;   /* setfov(90) effective, matches the reference */

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--frame") == 0 && i+1 < argc) frame = atoi(argv[++i]);
        else if (strcmp(argv[i], "--w") == 0 && i+1 < argc)     w = atoi(argv[++i]);
        else if (strcmp(argv[i], "--h") == 0 && i+1 < argc)     h = atoi(argv[++i]);
        else if (strcmp(argv[i], "--fovy") == 0 && i+1 < argc)  fovy = atof(argv[++i]);
        else if (strcmp(argv[i], "-o") == 0 && i+1 < argc)      outpath = argv[++i];
        else if (strcmp(argv[i], "--dump-shaders") == 0)        dump_shaders = 1;
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
    if (dump_shaders) {
        fprintf(stderr, "hs=[%zu,%zu) vs=[%zu,%zu) fs=[%zu,%zu)\n",
                hs->start, hs->end, vs ? vs->start : 0, vs ? vs->end : 0,
                fs ? fs->start : 0, fs ? fs->end : 0);
        fprintf(stderr, "=== vert_src ===\n%s\n=== frag_src ===\n%s\n", 
                vert_src ? vert_src : "(none)", frag_src ? frag_src : "(none)");
    }

    /* ---- compile host block, warm up frames sequentially ---- */
    char err[256];
    pdrl_Ctx *ctx = pdrl_compile(host_src, w, h, err, sizeof(err));
    if (!ctx) { fprintf(stderr, "compile error: %s\n", err); free(src); return 1; }
    pdrl_set_clock_scale(ctx, 1.0 / 60.0);   /* deterministic klock() */
    for (int f = 0; f <= frame; f++)
        pdrl_run_frame(ctx, (double)f);

    /* ---- GL renderer ---- */
    pd_GLRenderer *rd = pd_gl_renderer_create(w, h, fovy);
    if (!rd) {
        fprintf(stderr, "polydraw-render: GL offscreen renderer failed\n");
        pdrl_free(ctx); free(src); return 1;
    }
    pd_gl_renderer_set_shaders(rd, vert_src, frag_src);
    pd_gl_renderer_render(rd, pdrl_glbuf(ctx));

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
    free(src);
    return 0;
}
