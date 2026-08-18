/* ed_main.c — command-line entry point for evaldraw .kc scripts.
 *
 * Usage:
 *   evaldraw file.kc [--frame N] [--w W] [--h H] [-o out.png]
 *
 * Compiles and runs the script for N+1 frames, then saves the final
 * framebuffer to a PNG file.
 */
#include "eval_impl/ed_runlib.h"
#include "eval/pd_jit.h"
#include "eval/pd_ir.h"
#include "eval/pd_compile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path, size_t *outLen) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    if (outLen)
        *outLen = rd;
    return buf;
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    const char *script = NULL;
    const char *outpath = NULL;
    int frame = 30;
    int w = 640, h = 480;
    int jit_mode = 0; /* 0=off (default), 1=force on, 2=auto */
    int accum = 0;    /* --accum: cross-frame accumulation (persist mode) */

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--frame") == 0 && i + 1 < argc) frame = atoi(argv[++i]);
        else if (strcmp(argv[i], "--w") == 0 && i + 1 < argc)     w = atoi(argv[++i]);
        else if (strcmp(argv[i], "--h") == 0 && i + 1 < argc)     h = atoi(argv[++i]);
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)      outpath = argv[++i];
        else if (strcmp(argv[i], "--jit") == 0)                   jit_mode = 1;
        else if (strcmp(argv[i], "--no-jit") == 0)                jit_mode = 0;
        else if (strcmp(argv[i], "--accum") == 0)                 accum = 1;
        else if (strcmp(argv[i], "-O") == 0 || strcmp(argv[i], "--optimize") == 0)
            pd_set_optimize(1);
        else if (argv[i][0] != '-')                               script = argv[i];
    }

    if (!script) {
        fprintf(stderr,
            "evaldraw — software renderer for .kc scripts\n"
            "Usage:\n"
            "  evaldraw file.kc [--frame N] [--w W] [--h H] [-o out.png]\n"
            "                 [--accum] [--jit|--no-jit]\n"
            "  --accum : cross-frame accumulation mode (static vars persist)\n");
        return 1;
    }

    size_t len = 0;
    char *src = read_file(script, &len);
    if (!src) {
        fprintf(stderr, "cannot read %s\n", script);
        return 2;
    }

    ed_Ctx *ctx = ed_compile(src, w, h);
    if (!ctx) {
        free(src);
        return 1;
    }
    free(src);

    /* set the script directory for texture file search */
    {
        const char *slash = strrchr(script, '/');
        if (slash) {
            size_t dlen = (size_t)(slash - script);
            if (dlen >= sizeof(ctx->state.scriptDir)) dlen = sizeof(ctx->state.scriptDir) - 1;
            memcpy(ctx->state.scriptDir, script, dlen);
            ctx->state.scriptDir[dlen] = 0;
        } else {
            ctx->state.scriptDir[0] = '.';
            ctx->state.scriptDir[1] = 0;
        }
    }

    ed_set_clock_scale(ctx, 1.0 / 60.0);
    ctx->state.persist = accum;

    if (getenv("DUMP")) {
        pd_dump_program(&ctx->prog, stderr);
        for (size_t fi = 0; fi < ctx->prog.nFuncs; fi++) {
            fprintf(stderr, ";; === FUNC %zu ===\n", fi);
            pd_dump_program(&ctx->prog.funcs[fi], stderr);
        }
    }

    int use_jit = (jit_mode == 1) ? 1 : (jit_mode == 2 ? pd_jit_available() : 0);
    double (*run_frame)(ed_Ctx *, double) =
        use_jit ? ed_run_frame_jit : ed_run_frame;
    if (use_jit)
        fprintf(stderr, "evaldraw: using JIT backend (%s)\n",
                pd_jit_backend_name());

    /* Default (non-accum) mode: each frame is driven entirely by its frame
     * number — the script's `t` is `frame * clockScale` and `frameinit` is
     * re-set per pixel (see ed_runlib.c) — so frame N depends only on N and
     * we render exactly the requested frame without iterating 0..N-1.
     *
     * --accum mode: static variables (globals) and `frameinit` (only on
     * frame 0) persist across frames, so we must step 0..N to let state
     * accumulate. Only the final frame's image is saved. */
    if (ctx->state.persist) {
        for (int f = 0; f <= frame; f++)
            run_frame(ctx, (double)f);
    } else {
        run_frame(ctx, (double)frame);
    }

    char outbuf[512];
    if (!outpath) {
        snprintf(outbuf, sizeof(outbuf), "%s_f%d.png", script, frame);
        outpath = outbuf;
    }

    if (ed_save_png(ctx, outpath) != 0) {
        fprintf(stderr, "cannot write %s\n", outpath);
        ed_free(ctx);
        return 1;
    }

    printf("wrote %s (%dx%d, frame %d)\n", outpath, w, h, frame);
    ed_free(ctx);
    return 0;
}
