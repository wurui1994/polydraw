/* bench — performance baseline for the polydraw C pipeline (M8: 性能基准).
 *
 * Compiles a .pss host block once, then times N frames under three EVAL
 * drivers:
 *   - interp : pdrl_run_frame       (interpreter, the minimum guarantee)
 *   - llvm   : pdrl_run_frame_jit with sljit disabled (LLVM backend)
 *   - sljit  : pdrl_run_frame_jit with llvm disabled (sljit backend)
 *
 * Each driver replays frames 0..N-1 the same way the real renderer does, so
 * the numbers reflect end-to-end EVAL cost (the GL replay is not timed here;
 * only the EVAL->GLCmd recording is, which is where JIT matters). Prints
 * frames/sec for each and the JIT speedup vs the interpreter.
 *
 * Usage:
 *   bench file.pss [--frames N]
 */
#include "render/pd_runlib.h"
#include "eval/pd_jit.h"
#include "eval/pd_section.h"

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

static double bench(pdrl_Ctx *ctx, int n, int use_jit) {
    /* warmup: pay the one-time JIT compile (or interp init) before timing so
     * the reported number reflects steady-state per-frame cost, not the
     * compiler's first-invocation overhead. */
    for (int f = 0; f < 2; f++) {
        if (use_jit) pdrl_run_frame_jit(ctx, (double)f);
        else         pdrl_run_frame(ctx, (double)f);
    }
    double t0 = now_sec();
    for (int f = 2; f < n + 2; f++) {
        if (use_jit) pdrl_run_frame_jit(ctx, (double)f);
        else         pdrl_run_frame(ctx, (double)f);
    }
    return now_sec() - t0;
}

int main(int argc, char **argv) {
    const char *script = NULL;
    int n = 300, res = 320;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i+1 < argc) n = atoi(argv[++i]);
        else if (argv[i][0] != '-') script = argv[i];
    }
    if (!script) { fprintf(stderr, "usage: bench file.pss [--frames N]\n"); return 1; }

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

    int have_llvm = pd_llvm_available(), have_sljit = pd_sljit_available();

    double ti = bench(ctx, n, 0);
    double tl = 0, ts = 0;
    if (have_llvm)  { pd_llvm_set_enabled(1); if (have_sljit) pd_sljit_set_enabled(0); tl = bench(ctx, n, 1); }
    if (have_sljit) { if (have_llvm) pd_llvm_set_enabled(0); pd_sljit_set_enabled(1); ts = bench(ctx, n, 1); }
    pd_llvm_set_enabled(have_llvm); pd_sljit_set_enabled(have_sljit);

    printf("bench %s  frames=%d  res=%d\n", script, n, res);
    printf("  interp : %8.3f ms  %8.1f fps\n", ti*1e3, n/ti);
    if (have_llvm)  printf("  llvm   : %8.3f ms  %8.1f fps  (%.2fx vs interp)\n", tl*1e3, n/tl, ti/tl);
    if (have_sljit) printf("  sljit  : %8.3f ms  %8.1f fps  (%.2fx vs interp)\n", ts*1e3, n/ts, ti/ts);

    pdrl_free(ctx);
    free(host); free(src);
    return 0;
}
