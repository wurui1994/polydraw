/* framebench — REAL end-to-end frame-rate baseline (M8: 性能基准).
 *
 * Unlike bench.c (which times only the EVAL→GLCmd recording), this times the
 * FULL frame: EVAL recording + GL replay (pd_gl_renderer_render into the FBO)
 * + readback. This is the number that decides whether a script holds 60fps in
 * the viewer. Compares interpreter vs LLVM JIT (and sljit when present).
 *
 * Usage:
 *   framebench file.pss [--frames N]
 */
#include "render/pd_runlib.h"
#include "render/gl_renderer.h"
#include "eval/pd_jit.h"
#include "eval/pd_section.h"

/* gl_renderer.c references stbi_write_png (PD_DEBUG_CAP dump); provide the
 * implementation here like view_main.c does. */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__APPLE__)
#include <mach/mach_time.h>
static double now_sec(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}
#else
static double now_sec(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}
#endif

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)sz + 1); size_t rd = fread(b, 1, (size_t)sz, f);
    fclose(f); b[rd] = 0; return b;
}

static double run_full(pdrl_Ctx *ctx, pd_GLRenderer *rd, int n, int res, int use_jit) {
    unsigned char *rgba = malloc((size_t)res * res * 4);
    double t0 = now_sec();
    for (int f = 0; f < n; f++) {
        if (use_jit) pdrl_run_frame_jit(ctx, (double)f);
        else         pdrl_run_frame(ctx, (double)f);
        pd_gl_renderer_render(rd, pdrl_glbuf(ctx));
        pd_gl_renderer_read_rgba(rd, rgba);   /* force completion */
    }
    double dt = now_sec() - t0;
    free(rgba);
    return dt;
}

int main(int argc, char **argv) {
    const char *script = NULL; int n = 120, res = 320;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i+1 < argc) n = atoi(argv[++i]);
        else if (argv[i][0] != '-') script = argv[i];
    }
    if (!script) { fprintf(stderr, "usage: framebench file.pss [--frames N]\n"); return 1; }

    char *src = read_file(script);
    if (!src) { fprintf(stderr, "cannot read %s\n", script); return 2; }
    pd_SectionList sl;
    if (!pd_section_parse(&sl, src)) { fprintf(stderr, "section err: %s\n", sl.err); free(src); return 1; }
    const pd_Section *hs = pd_section_host(&sl);
    if (!hs) { fprintf(stderr, "no host block\n"); free(src); return 1; }
    size_t hl = hs->end - hs->start;
    char *host = malloc(hl + 1); memcpy(host, src + hs->start, hl); host[hl] = 0;

    char err[256];
    pdrl_Ctx *ctx = pdrl_compile(host, res, res, err, sizeof(err));
    if (!ctx) { fprintf(stderr, "compile err: %s\n", err); free(src); free(host); return 1; }
    pdrl_set_clock_scale(ctx, 1.0 / 60.0);

    pd_GLRenderer *rd = pd_gl_renderer_create(res, res, 73.74);
    if (!rd) { fprintf(stderr, "GL renderer failed\n"); pdrl_free(ctx); free(src); free(host); return 1; }
    const pd_Section *vs = pd_section_find(&sl, PD_SEC_VERTEX, NULL);
    const pd_Section *fs = pd_section_find(&sl, PD_SEC_FRAGMENT, NULL);
    char *vb = NULL, *fb = NULL;
    if (vs) { size_t l = vs->end - vs->start; vb = malloc(l+1); memcpy(vb, src+vs->start, l); vb[l]=0; }
    if (fs) { size_t l = fs->end - fs->start; fb = malloc(l+1); memcpy(fb, src+fs->start, l); fb[l]=0; }
    pd_gl_renderer_set_shaders(rd, vb, fb);

    int have_llvm = pd_llvm_available(), have_sljit = pd_sljit_available();

    /* warmup (pay JIT compile + GL init once) */
    pdrl_run_frame(ctx, 0); pd_gl_renderer_render(rd, pdrl_glbuf(ctx));
    if (have_llvm) { pd_llvm_set_enabled(1); if (have_sljit) pd_sljit_set_enabled(0); pdrl_run_frame_jit(ctx, 1); }
    if (have_sljit) { if (have_llvm) pd_llvm_set_enabled(0); pd_sljit_set_enabled(1); pdrl_run_frame_jit(ctx, 1); }

    double ti = run_full(ctx, rd, n, res, 0);
    double tl = 0, ts = 0;
    if (have_llvm)  { pd_llvm_set_enabled(1); if (have_sljit) pd_sljit_set_enabled(0); tl = run_full(ctx, rd, n, res, 1); }
    if (have_sljit) { if (have_llvm) pd_llvm_set_enabled(0); pd_sljit_set_enabled(1); ts = run_full(ctx, rd, n, res, 1); }
    pd_llvm_set_enabled(have_llvm); pd_sljit_set_enabled(have_sljit);

    printf("framebench %s  frames=%d  res=%dx%d\n", script, n, res, res);
    printf("  interp : %7.2f ms/frame  %7.1f fps  %s\n", ti*1e3/n, n/ti, n/ti >= 60 ? "OK(>=60)" : "BELOW 60");
    if (have_llvm)  printf("  llvm   : %7.2f ms/frame  %7.1f fps  %s  (%.2fx)\n", tl*1e3/n, n/tl, n/tl>=60?"OK(>=60)":"BELOW 60", ti/tl);
    if (have_sljit) printf("  sljit  : %7.2f ms/frame  %7.1f fps  %s  (%.2fx)\n", ts*1e3/n, n/ts, n/ts>=60?"OK(>=60)":"BELOW 60", ti/ts);

    pd_gl_renderer_destroy(rd);
    pdrl_free(ctx);
    free(vb); free(fb); free(host); free(src);
    return 0;
}
