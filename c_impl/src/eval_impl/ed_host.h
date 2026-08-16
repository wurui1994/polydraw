/* ed_host.h — EvalDraw host functions (software framebuffer rendering).
 *
 * Provides a pd_Host pre-populated with the host symbols an evaldraw .kc
 * script expects: cls, clz, setcol, setpix, getpix, moveto, lineto,
 * drawsph, drawcone, setcam, setview, setfont, printnum, printchar,
 * klock, rgb, rgba, noise, refresh, etc.
 *
 * Unlike polydraw's GL-command-recording host, this one renders directly
 * into a software RGB framebuffer + Z-buffer, matching evaldraw's
 * software-rendering model.
 */
#ifndef ED_HOST_H
#define ED_HOST_H

#include "eval/pd_host.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* State the evaldraw host functions read/write. The host application updates
 * these each frame (xres/yres from window size, mousx/mousy from cursor, etc). */
typedef struct ed_State {
    /* framebuffer */
    uint32_t *fb;          /* RGB packed: 0x00RRGGBB, size = xres*yres */
    double   *zbuf;        /* Z-buffer, size = xres*yres */
    int       xres, yres;  /* framebuffer dimensions (as int for indexing) */
    double    dxres, dyres;/* same as doubles (for host var binding) */

    /* current drawing color (0x00RRGGBB) */
    double    curColor;    /* packed 24-bit color as double */
    double    curR, curG, curB; /* components 0-255 (for 3-param setcol) */

    /* cursor position (for moveto/lineto/printnum/printchar) */
    double    curX, curY, curZ;

    /* camera (for 3D functions: drawsph, drawcone, lineto 3D) */
    double    camX, camY, camZ;     /* camera position */
    double    camHang, camVang;      /* horizontal/vertical angles (radians) */
    /* derived camera vectors (computed from hang/vang) */
    double    camFx, camFy, camFz;  /* forward */
    double    camRx, camRy, camRz;  /* right */
    double    camDx, camDy, camDz;  /* down */

    /* viewport (for 3D projection) */
    double    vpX0, vpY0, vpX1, vpY1; /* viewport rect */
    double    vpHx, vpHy, vpHz;       /* projection: sx = x/z*hz + hx, sy = y/z*hz + hy */

    /* grid mapping for 1D/2D graphing modes (setgrid) */
    double    gridX0, gridY0, gridX1, gridY1;

    /* font */
    double    fontW, fontH;  /* current font cell size */

    /* input state */
    double    mousx, mousy;
    double    dmousz;
    double    bstatus;
    double    keystatus[256];
    double    glob[32];

    /* frame state */
    double    numframes;
    double    frameinit;
    int       persist;   /* cross-frame accumulation mode: when set, static
                          * variables (globals) and frameinit (only on frame 0)
                          * carry forward across frames instead of being reset
                          * each frame. Enabled by the --accum CLI flag. */
    double    voxres;
    double    using6dof;
    double    usingstereo;
    double    stl_scalefac;

    /* timing */
    double    startTime;
    double    clockScale;  /* >0 = deterministic mode */

    /* printf output buffer (or NULL = stdout) */
    char     *logBuf;
    size_t    logLen, logCap;

    /* refresh callback (called by refresh() — in headless mode, a no-op) */
    int       refreshCount;

    /* --- OpenGL immediate-mode software renderer state --- */
    int       glMode;       /* current glbegin mode, -1 = none */
    int       glCount;      /* vertices accumulated since glbegin */
    double    glVx[256], glVy[256], glVz[256];   /* accumulated vertex coords */
    double    glCr[256], glCg[256], glCb[256];   /* per-vertex color */
    double    glTu[256], glTv[256];              /* per-vertex texcoords */
    double    glCurTu, glCurTv;                  /* current texcoord */
    int       glCurTex;     /* current texture id (-1 = none) */
} ed_State;

void ed_state_init(ed_State *s, int xres, int yres);
void ed_state_free(ed_State *s);
void ed_state_resize(ed_State *s, int xres, int yres);

/* Populate a host table with all evaldraw symbols. The state's lifetime must
 * outlive the host table. */
void ed_host_install(pd_Host *h, ed_State *s);

#ifdef __cplusplus
}
#endif
#endif
