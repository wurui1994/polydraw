/* ed_runlib.c — compile + run pipeline for evaldraw .kc scripts.
 *
 * Supports all evaldraw graphing modes:
 *   ()              — called once per frame; script draws via host functions
 *   (x)             — called per column; returns grayscale
 *   (x,y)           — called per pixel; returns grayscale
 *   (x,y,t)         — called per pixel; returns grayscale
 *   (x,y,&r,&g,&b)  — called per pixel; returns RGB via reference params
 */
#include "ed_runlib.h"
#include "ed_internal.h"
#include "eval/pd_compile.h"
#include "eval/pd_interp.h"
#include "eval/pd_jit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/*
 * Evaldraw .kc files use the syntax `() code;` or `(x) code;` as the
 * entry point — the code after `)` is the function body, without braces.
 * Our compiler expects `(){ code; }`. This function wraps the body in
 * braces by inserting `{` right after the `)` and appending `}` at the end.
 *
 * If the source already uses `(){ ... }` syntax (brace immediately after
 * the closing paren), it is left unchanged.
 */
static char *ed_preprocess(const char *source) {
    const char *p = source;
    while (*p) {
        if (*p == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
        } else if (*p == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(*p == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
        } else if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
        } else {
            break;
        }
    }

    if (*p != '(')
        return strdup(source);

    int depth = 0;
    const char *paren_start = p;
    while (*p) {
        if (*p == '(') depth++;
        else if (*p == ')') { depth--; if (depth == 0) break; }
        p++;
    }
    if (*p != ')')
        return strdup(source);

    const char *after_paren = p + 1;

    /* Skip whitespace and comments to check if already followed by `{`. */
    const char *q = after_paren;
    for (;;) {
        if (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') {
            q++;
        } else if (*q == '/' && q[1] == '/') {
            while (*q && *q != '\n') q++;
        } else if (*q == '/' && q[1] == '*') {
            q += 2;
            while (*q && !(*q == '*' && q[1] == '/')) q++;
            if (*q) q += 2;
        } else {
            break;
        }
    }
    if (*q == '{')
        return strdup(source);

    size_t before_len = (size_t)(paren_start - source);
    size_t paren_len = (size_t)(after_paren - paren_start);
    size_t body_len = strlen(after_paren);

    char *result = (char *)malloc(before_len + paren_len + 1 + body_len + 2 + 1);
    if (!result) return strdup(source);

    memcpy(result, source, before_len);
    memcpy(result + before_len, paren_start, paren_len);
    result[before_len + paren_len] = '{';
    memcpy(result + before_len + paren_len + 1, after_paren, body_len);
    result[before_len + paren_len + 1 + body_len] = '}';
    result[before_len + paren_len + 1 + body_len + 1] = '\0';
    return result;
}

ed_Ctx *ed_compile(const char *source, int xres, int yres) {
    ed_Ctx *ctx = (ed_Ctx *)calloc(1, sizeof(ed_Ctx));
    if (!ctx) return NULL;

    ctx->xres = xres;
    ctx->yres = yres;
    ctx->clockScale = 1.0 / 60.0;

    ed_state_init(&ctx->state, xres, yres);
    ctx->state.clockScale = ctx->clockScale;
    ctx->state.startTime = (double)clock() / CLOCKS_PER_SEC;

    pd_host_init(&ctx->host);
    ed_host_install(&ctx->host, &ctx->state);

    char *processed = ed_preprocess(source);

    char err[256];
    if (!pd_compile_host(&ctx->prog, processed, &ctx->host, err, sizeof(err))) {
        fprintf(stderr, "evaldraw: compile error: %s\n", err);
        free(processed);
        ed_free(ctx);
        return NULL;
    }

    free(processed);
    return ctx;
}

/* ---- per-pixel mode helpers ---- */

/* For (x,y,&r,&g,&b) or (x,y,t,&r,&g,&b) mode: the ref params r,g,b are
 * written by the script as pointers. We allocate slots and pass their
 * addresses. */
static void run_pixel_rgb(ed_Ctx *ctx, double numframes, int useJit) {
    ed_State *s = &ctx->state;
    int w = s->xres, h = s->yres;
    double t = numframes * ctx->clockScale;
    size_t np = ctx->prog.nParams;

    double rgb[3] = {0, 0, 0};
    double params[6];

    /* Map pixel coordinates to grid coordinates using setgrid bounds. */
    double sx = (s->gridX1 - s->gridX0) / (w > 1 ? w - 1 : 1);
    double sy = (s->gridY1 - s->gridY0) / (h > 1 ? h - 1 : 1);

    /* Automatic batching: allocate the frame once; run every pixel of the
     * frame in a single loop via pd_run_ctx (non-JIT), instead of one
     * pd_run() malloc/free per pixel. */
    pd_Ctx *c = useJit ? NULL : pd_run_ctx_alloc(&ctx->prog, NULL,
                                                 ctx->prog.globals, NULL);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            params[0] = s->gridX0 + x * sx;  /* mapped x */
            params[1] = s->gridY0 + y * sy;  /* mapped y */
            int refStart = 2;
            if (np == 6) {
                params[2] = t;
                refStart = 3;
            }
            /* Store the rgb buffer addresses as BIT-CAST doubles (not integer
             * → double value conversion). The interpreter's pd_array_base
             * recovers them via memcpy, so a true bit-cast is required. */
            double pa, pb, pc;
            double *pr = &rgb[0], *pg = &rgb[1], *pb_ = &rgb[2];
            memcpy(&pa, &pr, sizeof(pr));
            memcpy(&pb, &pg, sizeof(pg));
            memcpy(&pc, &pb_, sizeof(pb_));
            params[refStart + 0] = pa;
            params[refStart + 1] = pb;
            params[refStart + 2] = pc;
            rgb[0] = 0; rgb[1] = 0; rgb[2] = 0;

            /* frameinit: in the default (non-persist) mode it is 1 for the
             * first pixel of each frame; in persist mode it is set once by
             * ed_run_frame (1 only on frame 0) and must NOT be clobbered here
             * so static vars initialized on frameinit carry across frames. */
            if (!s->persist)
                s->frameinit = (x == 0 && y == 0) ? 1.0 : 0.0;

            if (useJit) {
                pd_run_jit(&ctx->prog, params, ctx->prog.globals, NULL);
            } else {
                double *pp = (double*)c->params;
                for (int k = 0; k < 6; k++) pp[k] = params[k];
                pd_run_ctx(c);
            }

            uint32_t col = ((uint32_t)ed_clamp((int)rgb[0], 0, 255) << 16) |
                           ((uint32_t)ed_clamp((int)rgb[1], 0, 255) << 8) |
                           (uint32_t)ed_clamp((int)rgb[2], 0, 255);
            s->fb[y * w + x] = col;
        }
    }
    pd_run_ctx_free(c);
}

/* Map a scalar value to evaldraw's default spectrum color.
 *   v <= 0: blue
 *   v >= 1: red
 *   0 < v < 1: smooth blue→cyan→green→yellow→red */
static uint32_t ed_spectrum(double v) {
    if (v <= 0.0) return 0x0000FF; /* blue */
    if (v >= 1.0) return 0xFF0000; /* red */

    /* 5-stop gradient: blue(0) → cyan(0.25) → green(0.5) → yellow(0.75) → red(1) */
    static const int stops[5][3] = {
        {  0,   0, 255}, /* blue   */
        {  0, 255, 255}, /* cyan   */
        {  0, 255,   0}, /* green  */
        {255, 255,   0}, /* yellow */
        {255,   0,   0}, /* red    */
    };
    double pos = v * 4.0;
    int i = (int)pos;
    if (i >= 4) i = 3;
    double frac = pos - i;
    int r = (int)(stops[i][0] + (stops[i+1][0] - stops[i][0]) * frac);
    int g = (int)(stops[i][1] + (stops[i+1][1] - stops[i][1]) * frac);
    int b = (int)(stops[i][2] + (stops[i+1][2] - stops[i][2]) * frac);
    return ((uint32_t)ed_clamp(r, 0, 255) << 16) |
           ((uint32_t)ed_clamp(g, 0, 255) << 8) |
           (uint32_t)ed_clamp(b, 0, 255);
}

static void run_pixel_gray(ed_Ctx *ctx, double numframes, int nParams, int useJit) {
    ed_State *s = &ctx->state;
    int w = s->xres, h = s->yres;
    double t = numframes * ctx->clockScale;

    /* Map pixel coordinates to grid coordinates using setgrid bounds. */
    double sx = (s->gridX1 - s->gridX0) / (w > 1 ? w - 1 : 1);
    double sy = (s->gridY1 - s->gridY0) / (h > 1 ? h - 1 : 1);

    /* Automatic batching: instead of one pd_run() call per pixel (millions of
     * malloc/free + re-entry), allocate the frame ONCE and run every pixel of
     * the whole frame inside a single tight loop via pd_run_ctx. */
    pd_Ctx *c = useJit ? NULL : pd_run_ctx_alloc(&ctx->prog, NULL,
                                                 ctx->prog.globals, NULL);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            double g;
            if (useJit) {
                double params[3];
                params[0] = s->gridX0 + x * sx;
                params[1] = s->gridY0 + y * sy;
                if (nParams >= 3) params[2] = t;
                if (!s->persist)
                    s->frameinit = (x == 0 && y == 0) ? 1.0 : 0.0;
                g = pd_run_jit(&ctx->prog, params, ctx->prog.globals, NULL);
            } else {
                double *params = (double*)c->params;
                params[0] = s->gridX0 + x * sx;
                params[1] = s->gridY0 + y * sy;
                if (nParams >= 3) params[2] = t;
                if (!s->persist)
                    s->frameinit = (x == 0 && y == 0) ? 1.0 : 0.0;
                /* NOTE: frame is NOT zeroed per-pixel. Per-pixel evaldraw
                 * scripts assign every local before reading it, so the
                 * 167-slot memset (1.3KB × millions of pixels) was pure
                 * waste. Cross-pixel state (static vars) lives in globals,
                 * set on frameinit and reused — so the frame is left dirty
                 * across pixels intentionally. */
                g = pd_run_ctx(c);
            }
            s->fb[y * w + x] = ed_spectrum(g);
        }
    }
    pd_run_ctx_free(c);
}

/* ---- frame runner ---- */

double ed_run_frame(ed_Ctx *ctx, double numframes) {
    ctx->state.numframes = numframes;
    /* In persist mode `frameinit` is 1 only on the very first frame so that
     * static vars seeded under `if (frameinit)` are initialized once and then
     * accumulate across frames. In non-persist mode it is 1 only when the
     * frame number is 0 (a convenience; run_pixel_* overrides it per-pixel). */
    ctx->state.frameinit = (ctx->state.persist ? (numframes == 0.0)
                                               : (numframes <= 0.0)) ? 1.0 : 0.0;

    size_t np = ctx->prog.nParams;

    if (np == 0) {
        /* () mode: script draws via host functions. */
        double ret = pd_run(&ctx->prog, NULL, ctx->prog.globals, NULL);
        ed_gl_flush(&ctx->state);   /* flush any geometry left open w/o glend() */
        return ret;
    }

    /* Per-pixel modes. */
    if (np == 5 || np == 6) {
        /* (x,y,&r,&g,&b) or (x,y,t,&r,&g,&b) — RGB per pixel. */
        run_pixel_rgb(ctx, numframes, 0);
        return 0;
    }

    /* (x), (x,y), (x,y,t) — spectrum per pixel/column. */
    run_pixel_gray(ctx, numframes, (int)np, 0);
    return 0;
}

double ed_run_frame_jit(ed_Ctx *ctx, double numframes) {
    ctx->state.numframes = numframes;
    ctx->state.frameinit = (ctx->state.persist ? (numframes == 0.0)
                                               : (numframes <= 0.0)) ? 1.0 : 0.0;

    size_t np = ctx->prog.nParams;

    if (np == 0) {
        double ret = pd_run_jit(&ctx->prog, NULL, ctx->prog.globals, NULL);
        ed_gl_flush(&ctx->state);
        return ret;
    }

    if (np == 5 || np == 6) {
        run_pixel_rgb(ctx, numframes, 1);
        return 0;
    }

    run_pixel_gray(ctx, numframes, (int)np, 1);
    return 0;
}

void ed_set_clock_scale(ed_Ctx *ctx, double scale) {
    ctx->clockScale = scale;
    ctx->state.clockScale = scale;
}

int ed_save_png(ed_Ctx *ctx, const char *path) {
    int w = ctx->state.xres;
    int h = ctx->state.yres;

    unsigned char *rgb = (unsigned char *)malloc((size_t)w * h * 3);
    if (!rgb) return -1;

    for (int i = 0; i < w * h; i++) {
        uint32_t col = ctx->state.fb[i];
        rgb[i * 3 + 0] = (unsigned char)((col >> 16) & 0xFF);
        rgb[i * 3 + 1] = (unsigned char)((col >> 8) & 0xFF);
        rgb[i * 3 + 2] = (unsigned char)(col & 0xFF);
    }

    int ok = stbi_write_png(path, w, h, 3, rgb, w * 3);
    free(rgb);
    return ok ? 0 : -1;
}

void ed_free(ed_Ctx *ctx) {
    if (!ctx) return;
    pd_program_free(&ctx->prog);
    ed_state_free(&ctx->state);
    free(ctx);
}
