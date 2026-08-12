/* pd_runlib.c — see pd_runlib.h. Wires compile + recording polyhost + run. */
#include "pd_runlib.h"
#include "../eval/pd_compile.h"
#include "../eval/pd_interp.h"
#include "../eval/pd_jit.h"
#include "../pd_polyhost.h"
#include "pd_polyhost_tex.h"

#include <stdlib.h>
#include <string.h>

struct pdrl_Ctx {
    pd_Program  prog;
    pd_Host     host;
    pd_PolyState state;
    GLCmdBuf    glbuf;
};

pdrl_Ctx *pdrl_compile(const char *hostSrc, int xres, int yres, char *err, size_t errLen) {
    pdrl_Ctx *ctx = (pdrl_Ctx*)calloc(1, sizeof(pdrl_Ctx));
    if (!ctx) { if (err && errLen) snprintf(err, errLen, "out of memory"); return NULL; }
    pd_polystate_init(&ctx->state);
    ctx->state.xres = xres; ctx->state.yres = yres;
    glcmd_init(&ctx->glbuf);
    pd_polyhost_install_render(&ctx->host, &ctx->state, &ctx->glbuf);
    pd_polyhost_install_tex(&ctx->host, &ctx->glbuf, NULL, 0);
    if (!pd_compile_host(&ctx->prog, hostSrc, &ctx->host, err, errLen)) {
        pdrl_free(ctx);
        return NULL;
    }
    return ctx;
}

double pdrl_run_frame(pdrl_Ctx *ctx, double numframes) {
    ctx->state.numframes = numframes;
    glcmd_reset(&ctx->glbuf);
    return pd_run(&ctx->prog, NULL, ctx->prog.globals, NULL);
}

double pdrl_run_frame_jit(pdrl_Ctx *ctx, double numframes) {
    ctx->state.numframes = numframes;
    glcmd_reset(&ctx->glbuf);
    return pd_run_jit(&ctx->prog, NULL, ctx->prog.globals, NULL);
}

const GLCmdBuf *pdrl_glbuf(const pdrl_Ctx *ctx) { return &ctx->glbuf; }

void pdrl_set_resolution(pdrl_Ctx *ctx, int xres, int yres) {
    ctx->state.xres = xres; ctx->state.yres = yres;
}

void pdrl_set_clock_scale(pdrl_Ctx *ctx, double scale) {
    ctx->state.clockScale = scale;
}

void pdrl_free(pdrl_Ctx *ctx) {
    if (!ctx) return;
    pd_program_free(&ctx->prog);
    glcmd_free(&ctx->glbuf);
    pd_tex_free_all();
    free(ctx->state.logBuf);
    free(ctx);
}

/* Provide the .pss section block table so GLCMD_SETSHADER replays can
 * resolve GLSL source pointers. The table is owned by the caller until
 * pdrl_free. */
void pdrl_install_tex_blocks(const pdrl_Block *blocks, int nblocks) {
    pd_polyhost_set_blocks(blocks, nblocks);
}
