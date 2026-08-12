/* pd_runlib.h — ctypes-friendly high-level API for driving .pss host blocks.
 *
 * A thin wrapper over compile + polyhost + run, exposing opaque handles so
 * Python (via ctypes) can compile once and run many frames, each time
 * collecting the recorded gl* commands from a GLCmdBuf.
 *
 * The buffer contract (GLCmd stream) is the cross-backend rendering contract:
 * Python vispy / C GL / JS WebGL2 all replay it. */
#ifndef PD_RUNLIB_H
#define PD_RUNLIB_H

#include "glcmd.h"
#include "pd_polyhost_tex.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque compiled .pss host block + its host state. */
typedef struct pdrl_Ctx pdrl_Ctx;

/* Compile a host-block source with the recording polyhost. Returns NULL on
 * compile error (err filled). The returned ctx owns the program, host table,
 * poly state, and a GLCmdBuf; free with pdrl_free. */
pdrl_Ctx *pdrl_compile(const char *hostSrc, int xres, int yres, char *err, size_t errLen);

/* Reset the gl command buffer and run one frame (sets numframes). After this,
 * pdrl_glbuf(ctx) yields the recorded commands for this frame. Returns the
 * EVAL result (usually 0 for gl-drawing scripts). */
double pdrl_run_frame(pdrl_Ctx *ctx, double numframes);

/* Like pdrl_run_frame but runs the EVAL via the sljit JIT when enabled.
 * Falls back to the interpreter if the JIT is unavailable. */
double pdrl_run_frame_jit(pdrl_Ctx *ctx, double numframes);

/* Access the recorded gl command buffer (valid until next pdrl_run_frame). */
const GLCmdBuf *pdrl_glbuf(const pdrl_Ctx *ctx);

/* Update resolution (also updates the state's xres/yres). */
void pdrl_set_resolution(pdrl_Ctx *ctx, int xres, int yres);

/* Set deterministic clock: scale>0 makes klock() return numframes*scale
 * (so renders are reproducible for golden diffing). 0 = wall clock. */
void pdrl_set_clock_scale(pdrl_Ctx *ctx, double scale);

void pdrl_free(pdrl_Ctx *ctx);

/* Provide the .pss section block table (GLSL sources) so the recorded
 * GLCMD_SETSHADER commands can resolve shader source pointers. Call after
 * pdrl_compile and before running frames. The table is owned by the caller
 * until pdrl_free. */
void pdrl_install_tex_blocks(const pdrl_Block *blocks, int nblocks);

#ifdef __cplusplus
}
#endif
#endif
