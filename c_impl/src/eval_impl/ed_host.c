/* ed_host.c — evaldraw host: state lifecycle + symbol registration.
 *
 * Creates the pd_Host table with all evaldraw symbols (host variables +
 * host functions) by delegating to ed_draw.c and ed_misc.c.
 */
#include "ed_host.h"
#include "ed_internal.h"

#include <stdlib.h>
#include <string.h>

/* Function registration tables from ed_draw.c and ed_misc.c. */
typedef struct { const char *sig; pd_HostFn fn; int variadic; } ed_FnReg;

const ed_FnReg *ed_get_draw_fns(void);
const ed_FnReg *ed_get_misc_fns(void);

/* ---- state lifecycle (defined in ed_host.h, implemented here) ---- */

void ed_state_init(ed_State *s, int xres, int yres) {
    memset(s, 0, sizeof(*s));
    s->xres = xres;
    s->yres = yres;
    s->dxres = xres;
    s->dyres = yres;
    s->fb = (uint32_t *)calloc((size_t)xres * yres, sizeof(uint32_t));
    s->zbuf = (double *)calloc((size_t)xres * yres, sizeof(double));
    s->mousx = xres / 2.0;
    s->mousy = yres / 2.0;
    s->curColor = 0xFFFFFF;
    s->curR = 255;
    s->curG = 255;
    s->curB = 255;
    s->fontW = 8;
    s->fontH = 8;
    s->vpX0 = 0;
    s->vpY0 = 0;
    s->vpX1 = xres;
    s->vpY1 = yres;
    s->vpHx = xres / 2.0;
    s->vpHy = yres / 2.0;
    s->vpHz = xres / 2.0;
    s->startTime = 0; /* set by runlib before first frame */

    /* Default grid for 1D/2D graphing modes: setgrid(-4, 3, 4, -3). */
    s->gridX0 = -4.0; s->gridY0 = 3.0;
    s->gridX1 = 4.0;  s->gridY1 = -3.0;
    s->stl_scalefac = 1.0;

    /* GL immediate-mode renderer: idle (no active begin). */
    s->glMode = -1;
    s->glCount = 0;
    s->glCurTex = -1;
    s->glCurTu = 0; s->glCurTv = 0;

    /* Default camera: looking down +Z from origin. */
    s->camFx = 0; s->camFy = 0; s->camFz = 1;
    s->camRx = 1; s->camRy = 0; s->camRz = 0;
    s->camDx = 0; s->camDy = 1; s->camDz = 0;
}

void ed_state_free(ed_State *s) {
    free(s->fb);
    s->fb = NULL;
    free(s->zbuf);
    s->zbuf = NULL;
    free(s->logBuf);
    s->logBuf = NULL;
    s->logLen = 0;
    s->logCap = 0;
    free(s->glTexData);
    s->glTexData = NULL;
    s->glTexW = s->glTexH = s->glTexCh = 0;
    free(s->picData);
    s->picData = NULL;
    s->picName[0] = '\0';
    s->picW = s->picH = 0;
}

void ed_state_resize(ed_State *s, int xres, int yres) {
    if (s->xres == xres && s->yres == yres)
        return;
    free(s->fb);
    free(s->zbuf);
    s->xres = xres;
    s->yres = yres;
    s->dxres = xres;
    s->dyres = yres;
    s->fb = (uint32_t *)calloc((size_t)xres * yres, sizeof(uint32_t));
    s->zbuf = (double *)calloc((size_t)xres * yres, sizeof(double));
    s->vpX0 = 0;
    s->vpY0 = 0;
    s->vpX1 = xres;
    s->vpY1 = yres;
    s->vpHx = xres / 2.0;
    s->vpHy = yres / 2.0;
    s->vpHz = xres / 2.0;
}

/* ---- host variable registration ---- */

static void ed_install_vars(pd_Host *h, ed_State *s) {
    /* Framebuffer dimensions. */
    pd_host_add_var(h, "XRES", &s->dxres);
    pd_host_add_var(h, "YRES", &s->dyres);
    pd_host_add_var(h, "VOXRES", &s->voxres);

    /* Input state. */
    pd_host_add_var(h, "MOUSX", &s->mousx);
    pd_host_add_var(h, "MOUSY", &s->mousy);
    pd_host_add_var(h, "DMOUSZ", &s->dmousz);
    pd_host_add_var(h, "BSTATUS", &s->bstatus);

    /* Frame state. */
    pd_host_add_var(h, "NUMFRAMES", &s->numframes);
    pd_host_add_var(h, "FRAMEINIT", &s->frameinit);
    pd_host_add_var(h, "USING6DOF", &s->using6dof);
    pd_host_add_var(h, "USINGSTEREO", &s->usingstereo);
    pd_host_add_var(h, "STL_SCALEFAC", &s->stl_scalefac);

    /* Arrays: keystatus[256] and glob[32]. */
    /* The host system registers arrays by binding the first element's
     * address; the compiler handles indexing. */
    pd_host_add_var(h, "KEYSTATUS", s->keystatus);
    pd_host_add_var(h, "GLOB", s->glob);

    /* OpenGL immediate-mode constant enums (evaldraw convention). */
    static double k_points=0, k_lines=1, k_line_loop=2, k_line_strip=3,
                   k_triangles=4, k_tri_strip=5, k_tri_fan=6, k_quads=7,
                   k_quad_strip=8, k_polygon=9;
    pd_host_add_var(h, "GL_POINTS", &k_points);
    pd_host_add_var(h, "GL_LINES", &k_lines);
    pd_host_add_var(h, "GL_LINE_LOOP", &k_line_loop);
    pd_host_add_var(h, "GL_LINE_STRIP", &k_line_strip);
    pd_host_add_var(h, "GL_TRIANGLES", &k_triangles);
    pd_host_add_var(h, "GL_TRIANGLE_STRIP", &k_tri_strip);
    pd_host_add_var(h, "GL_TRIANGLE_FAN", &k_tri_fan);
    pd_host_add_var(h, "GL_QUADS", &k_quads);
    pd_host_add_var(h, "GL_QUAD_STRIP", &k_quad_strip);
    pd_host_add_var(h, "GL_POLYGON", &k_polygon);
}

/* ---- host function registration ---- */

static void ed_install_fns(pd_Host *h) {
    const ed_FnReg *tables[] = {
        ed_get_draw_fns(),
        ed_get_misc_fns(),
    };

    for (int t = 0; t < 2; t++) {
        const ed_FnReg *reg = tables[t];
        for (int i = 0; reg[i].sig; i++) {
            pd_host_add_fn(h, reg[i].sig, reg[i].fn, reg[i].variadic);
        }
    }
}

/* ---- public entry point ---- */

void ed_host_install(pd_Host *h, ed_State *s) {
    h->state = (struct pd_PolyState *)s;
    ed_install_vars(h, s);
    ed_install_fns(h);
}
