/* ed_test.c — minimal step-by-step tests for evaldraw host functions. */
#include "eval_impl/ed_runlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double timediff_ms(struct timespec *t0, struct timespec *t1) {
    return (t1->tv_sec - t0->tv_sec) * 1000.0 +
           (t1->tv_nsec - t0->tv_nsec) / 1e6;
}

static int run_test(const char *name, const char *src, int w, int h,
                    const char *outpath) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    fprintf(stderr, "[test] %s ... ", name);
    ed_Ctx *ctx = ed_compile(src, w, h);
    if (!ctx) {
        fprintf(stderr, "COMPILE FAILED\n");
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double compile_ms = timediff_ms(&t0, &t1);

    clock_gettime(CLOCK_MONOTONIC, &t0);
    ed_run_frame(ctx, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double run_ms = timediff_ms(&t0, &t1);

    ed_save_png(ctx, outpath);

    /* Check framebuffer is not all black. */
    int nonblack = 0;
    for (int i = 0; i < w * h; i++) {
        if (ctx->state.fb[i] != 0) {
            nonblack++;
            break;
        }
    }

    fprintf(stderr, "compile=%.1fms run=%.1fms nonblack=%s -> %s\n",
            compile_ms, run_ms, nonblack ? "YES" : "NO", outpath);

    ed_free(ctx);
    return nonblack ? 0 : 1;
}

int main(void) {
    int failures = 0;

    /* Test 1: cls + setpix — simplest possible. */
    failures += run_test("cls+setpix",
        "()cls(0,0,0); setcol(255,0,0); setpix(10,10); return(0);",
        32, 32, "/tmp/ed_t1.png");

    /* Test 2: cls with single param (hex color). */
    failures += run_test("cls(hex)",
        "()cls(0x0000FF); return(0);",
        32, 32, "/tmp/ed_t2.png");

    /* Test 3: 2D drawsph (filled circle). */
    failures += run_test("drawsph2d",
        "()cls(0); setcol(255,255,255); drawsph(16,16,8); return(0);",
        32, 32, "/tmp/ed_t3.png");

    /* Test 4: 2D lineto. */
    failures += run_test("lineto2d",
        "()cls(0); setcol(255,255,255); moveto(0,0); lineto(31,31); return(0);",
        32, 32, "/tmp/ed_t4.png");

    /* Test 5: 3D drawsph with setcam. */
    failures += run_test("drawsph3d",
        "()cls(0,0,0); clz(1e32); setcam(0,0,-4,0,0); setcol(255,0,0); drawsph(0,0,0,1); return(0);",
        64, 64, "/tmp/ed_t5.png");

    /* Test 6: 3D drawcone with setcam. */
    failures += run_test("drawcone3d",
        "()cls(0,0,0); clz(1e32); setcam(0,0,-4,0,0); setcol(0,255,0); "
        "drawcone(0,0,0,0.1, 0,0,1,0.1); return(0);",
        64, 64, "/tmp/ed_t6.png");

    /* Test 7: printnum. */
    failures += run_test("printnum",
        "()cls(0); setcol(255,255,255); setfont(8,8); moveto(2,2); printnum(42); return(0);",
        64, 32, "/tmp/ed_t7.png");

    /* Test 8: klock + animation (2 frames). */
    {
        fprintf(stderr, "[test] klock+anim ... ");
        ed_Ctx *ctx = ed_compile(
            "()cls(0); t=klock(); setcol(255,255,255); "
            "setpix(int(t*10)%32, 16); return(0);",
            32, 32);
        if (!ctx) { fprintf(stderr, "COMPILE FAILED\n"); failures++; }
        else {
            ed_run_frame(ctx, 0);
            ed_run_frame(ctx, 1);
            ed_save_png(ctx, "/tmp/ed_t8.png");
            int nonblack = 0;
            for (int i = 0; i < 32*32; i++)
                if (ctx->state.fb[i]) { nonblack = 1; break; }
            fprintf(stderr, "nonblack=%s -> /tmp/ed_t8.png\n", nonblack?"YES":"NO");
            if (!nonblack) failures++;
            ed_free(ctx);
        }
    }

    /* Test 9: conetest.kc (real script, small resolution). */
    {
        FILE *f = fopen("/Users/wurui/Downloads/evaldraw/demos/conetest.kc", "rb");
        if (!f) { fprintf(stderr, "[test] conetest: cannot open file\n"); failures++; }
        else {
            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
            char *src = malloc(sz+1); fread(src, 1, sz, f); src[sz]=0; fclose(f);

            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            ed_Ctx *ctx = ed_compile(src, 64, 64);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double compile_ms = timediff_ms(&t0, &t1);

            if (!ctx) { fprintf(stderr, "[test] conetest: COMPILE FAILED\n"); failures++; }
            else {
                clock_gettime(CLOCK_MONOTONIC, &t0);
                ed_run_frame(ctx, 0);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                double run_ms = timediff_ms(&t0, &t1);

                ed_save_png(ctx, "/tmp/ed_t9.png");
                int nonblack = 0;
                for (int i = 0; i < 64*64; i++)
                    if (ctx->state.fb[i]) { nonblack = 1; break; }
                fprintf(stderr, "[test] conetest: compile=%.1fms run=%.1fms nonblack=%s -> /tmp/ed_t9.png\n",
                        compile_ms, run_ms, nonblack?"YES":"NO");
                if (!nonblack) failures++;
                ed_free(ctx);
            }
            free(src);
        }
    }

    /* Test 9b: simple (x,y) per-pixel mode (uses default grid -4..4). */
    failures += run_test("pixel_xy",
        "(x,y) return(x/8);",
        32, 32, "/tmp/ed_t9b.png");

    /* Test 9c: simple (x,y,t) per-pixel mode. */
    failures += run_test("pixel_xyt",
        "(x,y,t) return(x/8 + t);",
        32, 32, "/tmp/ed_t9c.png");

    /* Test 10: ceilflor.kc — (x,y,t) per-pixel mode. */
    {
        FILE *f = fopen("/Users/wurui/Downloads/evaldraw/demos/ceilflor.kc", "rb");
        if (!f) { fprintf(stderr, "[test] ceilflor: cannot open file\n"); failures++; }
        else {
            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
            char *src = malloc(sz+1); fread(src, 1, sz, f); src[sz]=0; fclose(f);

            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            ed_Ctx *ctx = ed_compile(src, 64, 64);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double compile_ms = timediff_ms(&t0, &t1);

            if (!ctx) { fprintf(stderr, "[test] ceilflor: COMPILE FAILED\n"); failures++; }
            else {
                fprintf(stderr, "[test] ceilflor: nParams=%zu\n", ctx->prog.nParams);
                clock_gettime(CLOCK_MONOTONIC, &t0);
                ed_run_frame(ctx, 0);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                double run_ms = timediff_ms(&t0, &t1);

                ed_save_png(ctx, "/tmp/ed_t10.png");
                int nonblack = 0;
                for (int i = 0; i < 64*64; i++)
                    if (ctx->state.fb[i]) { nonblack = 1; break; }
                fprintf(stderr, "[test] ceilflor: compile=%.1fms run=%.1fms nonblack=%s -> /tmp/ed_t10.png\n",
                        compile_ms, run_ms, nonblack?"YES":"NO");
                if (!nonblack) failures++;
                ed_free(ctx);
            }
            free(src);
        }
    }

    /* Test 11: goldball2.kc (real script, small resolution). */
    {
        FILE *f = fopen("/Users/wurui/Downloads/evaldraw/demos/goldball2.kc", "rb");
        if (!f) { fprintf(stderr, "[test] goldball2: cannot open file\n"); failures++; }
        else {
            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
            char *src = malloc(sz+1); fread(src, 1, sz, f); src[sz]=0; fclose(f);

            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            ed_Ctx *ctx = ed_compile(src, 64, 64);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double compile_ms = timediff_ms(&t0, &t1);

            if (!ctx) { fprintf(stderr, "[test] goldball2: COMPILE FAILED\n"); failures++; }
            else {
                clock_gettime(CLOCK_MONOTONIC, &t0);
                ed_run_frame(ctx, 0);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                double run_ms = timediff_ms(&t0, &t1);

                ed_save_png(ctx, "/tmp/ed_t11.png");
                int nonblack = 0;
                for (int i = 0; i < 64*64; i++)
                    if (ctx->state.fb[i]) { nonblack = 1; break; }
                fprintf(stderr, "[test] goldball2: compile=%.1fms run=%.1fms nonblack=%s -> /tmp/ed_t11.png\n",
                        compile_ms, run_ms, nonblack?"YES":"NO");
                if (!nonblack) failures++;
                ed_free(ctx);
            }
            free(src);
        }
    }

    fprintf(stderr, "\n=== %d failures ===\n", failures);
    return failures ? 1 : 0;
}
