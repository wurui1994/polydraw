/* ed_runlib.h — high-level compile + run API for evaldraw scripts.
 *
 * Usage:
 *   ed_Ctx *ctx = ed_compile(source_code, xres, yres);
 *   for (int frame = 0; frame < nframes; frame++)
 *       ed_run_frame(ctx);
 *   ed_save_png(ctx, "output.png");
 *   ed_free(ctx);
 */
#ifndef ED_RUNLIB_H
#define ED_RUNLIB_H

#include "ed_host.h"
#include "eval/pd_ir.h"
#include "eval/pd_host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ed_Ctx {
    /* compiled program (inline, not a pointer — matches pdrl_Ctx pattern) */
    pd_Program  prog;
    pd_Host     host;
    ed_State    state;

    /* config */
    int         xres, yres;
    double      clockScale; /* >0 = deterministic timing */
} ed_Ctx;

/* Compile a .kc source string into an evaldraw context. */
ed_Ctx *ed_compile(const char *source, int xres, int yres);

/* Run one frame of the compiled script (interpreter). */
double ed_run_frame(ed_Ctx *ctx, double numframes);

/* Run one frame via JIT (if available). */
double ed_run_frame_jit(ed_Ctx *ctx, double numframes);

/* Set the clock scale for deterministic klock(). */
void ed_set_clock_scale(ed_Ctx *ctx, double scale);

/* Save the current framebuffer to a PNG file. Returns 0 on success. */
int ed_save_png(ed_Ctx *ctx, const char *path);

/* Free all resources. */
void ed_free(ed_Ctx *ctx);

#ifdef __cplusplus
}
#endif
#endif
