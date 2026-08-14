/* phaseprof — split per-frame cost: EVAL(JIT/interp) vs GL replay vs readback.
 * Builds on framebench but times each phase separately over N frames.
 *
 * Usage: phaseprof file.pss [--frames N] [--res R]
 */
#include "render/pd_runlib.h"
#include "render/gl_renderer.h"
#include "eval/pd_jit.h"
#include "eval/pd_section.h"

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

int main(int argc, char **argv) {
    const char *script = NULL; int n = 120, res = 320;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i+1 < argc) n = atoi(argv[++i]);
        else if (strcmp(argv[i], "--res") == 0 && i+1 < argc) res = atoi(argv[++i]);
        else if (argv[i][0] != '-') script = argv[i];
    }
    if (!script) { fprintf(stderr, "usage: phaseprof file.pss [--frames N] [--res R]\n"); return 1; }

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
    if (!ctx) { fprintf(stderr, "compile err: %s\n", err); return 1; }
    pdrl_set_clock_scale(ctx, 1.0 / 60.0);

    pd_GLRenderer *rd = pd_gl_renderer_create(res, res, 73.74);
    if (!rd) { fprintf(stderr, "GL renderer failed\n"); return 1; }
    const pd_Section *vs = pd_section_find(&sl, PD_SEC_VERTEX, NULL);
    const pd_Section *fs = pd_section_find(&sl, PD_SEC_FRAGMENT, NULL);
    char *vb = NULL, *fb = NULL;
    if (vs) { size_t l = vs->end - vs->start; vb = malloc(l+1); memcpy(vb, src + vs->start, l); vb[l]=0; }
    if (fs) { size_t l = fs->end - fs->start; fb = malloc(l+1); memcpy(fb, src + fs->start, l); fb[l]=0; }
    pd_gl_renderer_set_shaders(rd, vb, fb);

    unsigned char *rgba = malloc((size_t)res * res * 4);

    for (int mode = 0; mode < 2; mode++) {
        const char *name = mode ? "llvm" : "interp";
        /* warmup (JIT compile once) */
        for (int f = 0; f < 4; f++) {
            if (mode) pdrl_run_frame_jit(ctx, (double)f);
            else      pdrl_run_frame(ctx, (double)f);
            pd_gl_renderer_render(rd, pdrl_glbuf(ctx));
            pd_gl_renderer_read_rgba(rd, rgba);
        }
        double t_run = 0, t_rend = 0, t_read = 0, t_all = 0;
        double t0 = now_sec();
        for (int f = 0; f < n; f++) {
            double a = now_sec();
            if (mode) pdrl_run_frame_jit(ctx, (double)f);
            else      pdrl_run_frame(ctx, (double)f);
            double b = now_sec();
            pd_gl_renderer_render(rd, pdrl_glbuf(ctx));
            double c = now_sec();
            pd_gl_renderer_read_rgba(rd, rgba);
            double d = now_sec();
            t_run += b - a; t_rend += c - b; t_read += d - c;
        }
        t_all = now_sec() - t0;
        printf("%-7s run=%7.2f ms  render=%7.2f ms  read=%6.2f ms  total=%7.2f ms  (%5.1f fps)  draws/frame=%zu\n",
               name, t_run*1e3/n, t_rend*1e3/n, t_read*1e3/n, t_all*1e3/n, n/t_all,
               pd_gl_renderer_draw_calls(rd));
    }

    free(rgba);
    pd_gl_renderer_destroy(rd);
    pdrl_free(ctx);
    free(vb); free(fb); free(host); free(src);
    return 0;
}
