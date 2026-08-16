/* ed_draw.c — evaldraw host: framebuffer & 2D/3D drawing functions.
 *
 * Implements: cls, clz, setcol, setpix, getpix, moveto, lineto,
 * drawsph, drawcone, setcam, setview, setfont, printnum, printchar.
 */
#include "ed_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* cls(col) or cls(r,g,b) — clear framebuffer to a color. */
static double hf_cls(pd_Host *h, int n, const double *a) {
    ed_State *s = ST(h);
    uint32_t col;
    if (n >= 3)
        col = ed_pack_rgb((int)a[0], (int)a[1], (int)a[2]);
    else if (n >= 1)
        col = (uint32_t)(unsigned)(int64_t)a[0] & 0xFFFFFF;
    else
        col = 0;

    size_t total = (size_t)s->xres * s->yres;
    for (size_t i = 0; i < total; i++)
        s->fb[i] = col;
    return 0;
}

/* clz(z) — clear Z-buffer to a value (default: far). */
static double hf_clz(pd_Host *h, int n, const double *a) {
    ed_State *s = ST(h);
    double z = (n >= 1) ? a[0] : 1e30;
    size_t total = (size_t)s->xres * s->yres;
    for (size_t i = 0; i < total; i++)
        s->zbuf[i] = z;
    return 0;
}

/* setcol(col) or setcol(r,g,b) — set current drawing color. */
static double hf_setcol(pd_Host *h, int n, const double *a) {
    ed_State *s = ST(h);
    if (n >= 3) {
        s->curR = a[0];
        s->curG = a[1];
        s->curB = a[2];
        s->curColor = ed_pack_rgb((int)a[0], (int)a[1], (int)a[2]);
    } else if (n >= 1) {
        uint32_t col = (uint32_t)(unsigned)(int64_t)a[0] & 0xFFFFFF;
        s->curColor = col;
        int r, g, b;
        ed_unpack_rgb(col, &r, &g, &b);
        s->curR = r;
        s->curG = g;
        s->curB = b;
    }
    return 0;
}

/* setpix(x,y) or setpix(x,y,z) — plot a pixel at current color. */
static double hf_setpix(pd_Host *h, int n, const double *a) {
    ed_State *s = ST(h);
    if (n < 2)
        return 0;
    int x = (int)a[0];
    int y = (int)a[1];
    if (n >= 3)
        ed_put_pixel_z(s, x, y, a[2], ed_current_color(s));
    else
        ed_put_pixel(s, x, y, ed_current_color(s));
    return 0;
}

/* getpix(x,y,&r,&g,&b) — read pixel color + return Z depth. */
static double hf_getpix(pd_Host *h, int n, const double *a) {
    ed_State *s = ST(h);
    if (n < 5)
        return 0;
    int x = (int)a[0];
    int y = (int)a[1];
    if (!ed_in_bounds(s, x, y))
        return 1e30;
    int idx = y * s->xres + x;

    /* args 2,3,4 are &r,&g,&b — stored as raw pointer values in doubles. */
    double *pr, *pg, *pb;
    memcpy(&pr, &a[2], sizeof(void *));
    memcpy(&pg, &a[3], sizeof(void *));
    memcpy(&pb, &a[4], sizeof(void *));

    int r, g, b;
    ed_unpack_rgb(s->fb[idx], &r, &g, &b);
    if (pr) *pr = r;
    if (pg) *pg = g;
    if (pb) *pb = b;
    return s->zbuf[idx];
}

/* moveto(x,y) or moveto(x,y,z) — move the drawing cursor. */
static double hf_moveto(pd_Host *h, int n, const double *a) {
    ed_State *s = ST(h);
    if (n >= 3) {
        s->curX = a[0];
        s->curY = a[1];
        s->curZ = a[2];
    } else if (n >= 2) {
        s->curX = a[0];
        s->curY = a[1];
        s->curZ = 0;
    }
    return 0;
}

/* lineto(x,y) or lineto(x,y,z) — draw a line from cursor to (x,y[,z]). */
static double hf_lineto(pd_Host *h, int n, const double *a) {
    ed_State *s = ST(h);
    uint32_t col = ed_current_color(s);

    if (n >= 3) {
        /* 3D line: project both endpoints, draw with Z-test. */
        double x1 = a[0], y1 = a[1], z1 = a[2];
        double sx0, sy0, d0, sx1, sy1, d1;
        ed_project3d(s, s->curX, s->curY, s->curZ, &sx0, &sy0, &d0);
        ed_project3d(s, x1, y1, z1, &sx1, &sy1, &d1);
        s->curX = x1;
        s->curY = y1;
        s->curZ = z1;

        int steps = (int)(fabs(sx1 - sx0) + fabs(sy1 - sy0)) + 1;
        for (int i = 0; i <= steps; i++) {
            double t = steps ? (double)i / steps : 0;
            int x = (int)(sx0 + (sx1 - sx0) * t);
            int y = (int)(sy0 + (sy1 - sy0) * t);
            double z = d0 + (d1 - d0) * t;
            ed_put_pixel_z(s, x, y, z, col);
        }
    } else if (n >= 2) {
        /* 2D Bresenham line. */
        int x0 = (int)s->curX, y0 = (int)s->curY;
        int x1 = (int)a[0], y1 = (int)a[1];
        s->curX = a[0];
        s->curY = a[1];

        int dx = abs(x1 - x0);
        int dy = abs(y1 - y0);
        int sx = x0 < x1 ? 1 : -1;
        int sy = y0 < y1 ? 1 : -1;
        int err = dx - dy;
        int x = x0, y = y0;

        for (;;) {
            ed_put_pixel(s, x, y, col);
            if (x == x1 && y == y1)
                break;
            int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x += sx; }
            if (e2 <  dx) { err += dx; y += sy; }
        }
    }
    return 0;
}

/* drawsph(x,y,z,rad) or drawsph(x,y,rad) — draw a sphere/circle. */
static double hf_drawsph(pd_Host *h, int n, const double *a) {
    ed_State *s = ST(h);
    uint32_t col = ed_current_color(s);

    if (n >= 4) {
        /* 3D sphere: project center, draw filled circle with shading. */
        double x = a[0], y = a[1], z = a[2], rad = a[3];
        double sx, sy, depth;
        ed_project3d(s, x, y, z, &sx, &sy, &depth);
        if (depth < 0.001)
            return 0;

        double prad = rad / depth * s->vpHz;
        if (prad < 0)
            prad = -prad;
        /* Ensure at least 1 pixel radius so sub-pixel spheres are visible. */
        if (prad < 0.5)
            prad = 0.5;
        /* Clamp radius to avoid huge loops when projection goes wrong. */
        int maxr = s->xres > s->yres ? s->xres : s->yres;
        if (prad > maxr)
            prad = maxr;
        int r = (int)(prad + 0.5);
        int cx = (int)sx, cy = (int)sy;

        int lr, lg, lb;
        ed_unpack_rgb(col, &lr, &lg, &lb);

        for (int py = -r; py <= r; py++) {
            for (int px = -r; px <= r; px++) {
                double dist2 = (double)(px * px + py * py);
                if (dist2 <= prad * prad) {
                    /* Simple Lambertian shading from upper-left. */
                    double nx = px / prad;
                    double ny = py / prad;
                    double nz = sqrt(1.0 - nx * nx - ny * ny);
                    if (nz < 0)
                        nz = 0;
                    double shade = 0.3 + 0.7 * (nx * (-0.577) + ny * (-0.577) + nz * 0.577);
                    if (shade < 0) shade = 0;
                    if (shade > 1) shade = 1;
                    ed_put_pixel_z(s, cx + px, cy + py, depth,
                        ed_pack_rgb((int)(lr * shade), (int)(lg * shade), (int)(lb * shade)));
                }
            }
        }
    } else if (n >= 3) {
        /* 2D filled circle. */
        double x = a[0], y = a[1], rad = a[2];
        int r = (int)(rad < 0 ? -rad : rad);
        int cx = (int)x, cy = (int)y;

        if (rad > 0) {
            for (int py = -r; py <= r; py++)
                for (int px = -r; px <= r; px++)
                    if (px * px + py * py <= r * r)
                        ed_put_pixel(s, cx + px, cy + py, col);
        } else {
            /* Negative radius = outline only. */
            for (int py = -r; py <= r; py++)
                for (int px = -r; px <= r; px++) {
                    int d2 = px * px + py * py;
                    if (d2 <= r * r && d2 >= (r - 1) * (r - 1))
                        ed_put_pixel(s, cx + px, cy + py, col);
                }
        }
    }
    return 0;
}

/* drawcone(x0,y0,z0,r0, x1,y1,z1,r1) or 2D variant. */
static double hf_drawcone(pd_Host *h, int n, const double *a) {
    ed_State *s = ST(h);
    uint32_t col = ed_current_color(s);

    if (n >= 8) {
        /* 3D cone: project endpoints, draw thick line with Z-test. */
        double x0 = a[0], y0 = a[1], z0 = a[2], r0 = a[3];
        double x1 = a[4], y1 = a[5], z1 = a[6], r1 = a[7];
        double sx0, sy0, d0, sx1, sy1, d1;
        ed_project3d(s, x0, y0, z0, &sx0, &sy0, &d0);
        ed_project3d(s, x1, y1, z1, &sx1, &sy1, &d1);
        if (d0 < 0.001) d0 = 0.001;
        if (d1 < 0.001) d1 = 0.001;

        double pr0 = r0 / d0 * s->vpHz;
        double pr1 = r1 / d1 * s->vpHz;
        int maxr = s->xres > s->yres ? s->xres : s->yres;
        if (pr0 > maxr) pr0 = maxr;
        if (pr1 > maxr) pr1 = maxr;
        int steps = 100;

        for (int i = 0; i <= steps; i++) {
            double t = (double)i / steps;
            double cx = sx0 + (sx1 - sx0) * t;
            double cy = sy0 + (sy1 - sy0) * t;
            double cz = d0 + (d1 - d0) * t;
            double cr = pr0 + (pr1 - pr0) * t;
            int r = (int)(cr + 0.5);
            int xi = (int)cx, yi = (int)cy;
            for (int py = -r; py <= r; py++)
                for (int px = -r; px <= r; px++)
                    if (px * px + py * py <= r * r)
                        ed_put_pixel_z(s, xi + px, yi + py, cz, col);
        }
    } else if (n >= 6) {
        /* 2D thick line. */
        double x0 = a[0], y0 = a[1], r0 = a[2];
        double x1 = a[3], y1 = a[4], r1 = a[5];
        int steps = 100;
        for (int i = 0; i <= steps; i++) {
            double t = (double)i / steps;
            double cx = x0 + (x1 - x0) * t;
            double cy = y0 + (y1 - y0) * t;
            double cr = r0 + (r1 - r0) * t;
            int r = (int)(cr + 0.5);
            int xi = (int)cx, yi = (int)cy;
            for (int py = -r; py <= r; py++)
                for (int px = -r; px <= r; px++)
                    if (px * px + py * py <= r * r)
                        ed_put_pixel(s, xi + px, yi + py, col);
        }
    }
    return 0;
}

/* setcam(x,y,z,hang,vang) or setcam with 12 explicit vectors. */
static double hf_setcam(pd_Host *h, int n, const double *a) {
    ed_State *s = ST(h);
    if (n >= 5) {
        s->camX = a[0];
        s->camY = a[1];
        s->camZ = a[2];
        s->camHang = a[3];
        s->camVang = a[4];
        ed_cam_update_vectors(s);
    } else if (n >= 12) {
        s->camX = a[0];  s->camY = a[1];  s->camZ = a[2];
        s->camRx = a[3]; s->camRy = a[4]; s->camRz = a[5];
        s->camDx = a[6]; s->camDy = a[7]; s->camDz = a[8];
        s->camFx = a[9]; s->camFy = a[10]; s->camFz = a[11];
    }
    return 0;
}

/* setview(x0,y0,x1,y1,hx,hy[,hz]) — set viewport and projection center. */
static double hf_setview(pd_Host *h, int n, const double *a) {
    ed_State *s = ST(h);
    if (n >= 7) {
        s->vpX0 = a[0]; s->vpY0 = a[1];
        s->vpX1 = a[2]; s->vpY1 = a[3];
        s->vpHx = a[4]; s->vpHy = a[5]; s->vpHz = a[6];
    } else if (n >= 4) {
        s->vpX0 = a[0]; s->vpY0 = a[1];
        s->vpX1 = a[2]; s->vpY1 = a[3];
        s->vpHx = (a[0] + a[2]) / 2.0;
        s->vpHy = (a[1] + a[3]) / 2.0;
        s->vpHz = s->vpHx;
    }
    return 0;
}

/* setfont(w,h) — set font cell size. */
static double hf_setfont(pd_Host *h, int n, const double *a) {
    ed_State *s = ST(h);
    if (n >= 2) {
        s->fontW = a[0];
        s->fontH = a[1];
    }
    return 0;
}

/* printnum(value) — render a number as text at the cursor. */
static double hf_printnum(pd_Host *h, int n, const double *a) {
    ed_State *s = ST(h);
    if (n < 1)
        return 0;
    char buf[32];
    snprintf(buf, sizeof(buf), "%g", a[0]);
    uint32_t col = ed_current_color(s);
    int x = (int)s->curX;
    int y = (int)s->curY;
    for (const char *p = buf; *p; p++) {
        ed_draw_char(s, x, y, *p, col);
        x += (int)s->fontW;
    }
    s->curX = x;
    return 0;
}

/* printchar(ch) — render a single character at the cursor. */
static double hf_printchar(pd_Host *h, int n, const double *a) {
    ed_State *s = ST(h);
    if (n < 1)
        return 0;
    char ch = (char)(int)a[0];
    ed_draw_char(s, (int)s->curX, (int)s->curY, ch, ed_current_color(s));
    s->curX += s->fontW;
    return 0;
}

/* ---- OpenGL immediate-mode software renderer ----
 * EVAL scripts use the OpenGL-style immediate mode (glbegin/glvertex/glend).
 * Since polydraw has no real GL, we emulate it with a vertex accumulator and
 * a simple perspective-projected, Z-buffered, Gouraud-shaded rasterizer. */

/* GL primitive modes (evaldraw/evaldraw convention, OpenGL-style enums). */
enum {
    ED_GL_POINTS = 0,
    ED_GL_LINES = 1,
    ED_GL_LINE_LOOP = 2,
    ED_GL_LINE_STRIP = 3,
    ED_GL_TRIANGLES = 4,
    ED_GL_TRIANGLE_STRIP = 5,
    ED_GL_TRIANGLE_FAN = 6,
    ED_GL_QUADS = 7,
    ED_GL_QUAD_STRIP = 8,
    ED_GL_POLYGON = 9
};

static double hf_glbegin(pd_Host *h, int n, const double *a) {
    ed_State *s = ST(h);
    /* close any primitive still open from a previous glbegin() (scripts
     * frequently omit the matching glend()). */
    if (s->glMode >= 0 && s->glCount > 0) ed_gl_flush(s);
    s->glMode = (int)a[0];
    s->glCount = 0;
    return 0;
}

static double hf_glvertex(pd_Host *h, int n, const double *a) {
    ed_State *s = ST(h);
    if (s->glMode < 0 || s->glCount >= 256) return 0;
    int i = s->glCount++;
    s->glVx[i] = a[0]; s->glVy[i] = a[1]; s->glVz[i] = n >= 3 ? a[2] : 0;
    /* inherit current color / texcoord */
    int r, g, b; ed_unpack_rgb(ed_current_color(s), &r, &g, &b);
    s->glCr[i] = r; s->glCg[i] = g; s->glCb[i] = b;
    s->glTu[i] = s->glCurTu; s->glTv[i] = s->glCurTv;
    return 0;
}

static double hf_gltexcoord(pd_Host *h, int n, const double *a) {
    ed_State *s = ST(h);
    s->glCurTu = a[0];
    s->glCurTv = n >= 2 ? a[1] : 0;
    return 0;
}

static double hf_glcolor(pd_Host *h, int n, const double *a) {
    ed_State *s = ST(h);
    uint32_t col;
    if (n >= 3)
        col = ed_pack_rgb((int)a[0], (int)a[1], (int)a[2]);
    else
        col = (uint32_t)(unsigned)(int64_t)a[0] & 0xFFFFFF;
    s->curColor = col;
    int r, g, b; ed_unpack_rgb(col, &r, &g, &b);
    s->curR = r; s->curG = g; s->curB = b;
    return 0;
}

static double hf_glsettex(pd_Host *h, int n, const double *a) {
    ed_State *s = ST(h);
    /* texture loading is not yet wired; just record the id (use a[0] as id,
     * or try to map a string path). For now enable textured shading flag. */
    s->glCurTex = (int)a[0];
    return 0;
}

static double hf_glnormal(pd_Host *h, int n, const double *a) {
    (void)h; (void)n; (void)a; return 0; /* lighting is flat; ignore */
}

/* rasterize one Gouraud-shaded triangle with Z-test */
static void ed_gl_triangle(ed_State *s,
        double ax, double ay, double az, double ar, double ag, double ab,
        double bx, double by, double bz, double br, double bg, double bb,
        double cx, double cy, double cz, double cr, double cg, double cb) {
    double sx0, sy0, d0, sx1, sy1, d1, sx2, sy2, d2;
    ed_project3d(s, ax, ay, az, &sx0, &sy0, &d0);
    ed_project3d(s, bx, by, bz, &sx1, &sy1, &d1);
    ed_project3d(s, cx, cy, cz, &sx2, &sy2, &d2);
    if (d0 < 0.001 || d1 < 0.001 || d2 < 0.001) return;
    int x0 = (int)(sx0 + 0.5), y0 = (int)(sy0 + 0.5);
    int x1 = (int)(sx1 + 0.5), y1 = (int)(sy1 + 0.5);
    int x2 = (int)(sx2 + 0.5), y2 = (int)(sy2 + 0.5);
    int minx = ed_clamp(((x0<x1?x0:x1)<x2?(x0<x1?x0:x1):x2) - 1, 0, s->xres-1);
    int maxx = ed_clamp(((x0>x1?x0:x1)>x2?(x0>x1?x0:x1):x2) + 1, 0, s->xres-1);
    int miny = ed_clamp(((y0<y1?y0:y1)<y2?(y0<y1?y0:y1):y2) - 1, 0, s->yres-1);
    int maxy = ed_clamp(((y0>y1?y0:y1)>y2?(y0>y1?y0:y1):y2) + 1, 0, s->yres-1);
    double det = (double)(y1 - y2) * (x0 - x2) + (double)(x2 - x1) * (y0 - y2);
    if (det == 0) return;
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            double l0 = ((double)(y1 - y2) * (x - x2) + (double)(x2 - x1) * (y - y2)) / det;
            double l1 = ((double)(y2 - y0) * (x - x2) + (double)(x0 - x2) * (y - y2)) / det;
            double l2 = 1.0 - l0 - l1;
            if (l0 < 0 || l1 < 0 || l2 < 0) continue;
            double z = l0 * d0 + l1 * d1 + l2 * d2;
            int r = (int)(l0 * ar + l1 * br + l2 * cr);
            int g = (int)(l0 * ag + l1 * bg + l2 * cg);
            int b = (int)(l0 * ab + l1 * bb + l2 * cb);
            ed_put_pixel_z(s, x, y, z,
                ed_pack_rgb(ed_clamp(r,0,255), ed_clamp(g,0,255), ed_clamp(b,0,255)));
        }
    }
}

static double hf_glend(pd_Host *h, int n, const double *a) {
    ed_State *s = ST(h);
    (void)n; (void)a;
    int m = s->glMode;
    ed_gl_flush(s);
    return 0;
}

/* Rasterize the currently-pending GL primitive (if any). EVAL scripts often
 * omit an explicit glend(); the reference runtime flushes pending geometry
 * when a new glbegin() is issued and at the end of the frame, so we mirror
 * that here. */
void ed_gl_flush(ed_State *s) {
    int m = s->glMode;
    int cnt = s->glCount;
    if (m < 0 || cnt == 0) { s->glMode = -1; return; }
    if (m == ED_GL_TRIANGLES) {
        for (int i = 0; i + 2 < cnt; i += 3)
            ed_gl_triangle(s,
                s->glVx[i],s->glVy[i],s->glVz[i],s->glCr[i],s->glCg[i],s->glCb[i],
                s->glVx[i+1],s->glVy[i+1],s->glVz[i+1],s->glCr[i+1],s->glCg[i+1],s->glCb[i+1],
                s->glVx[i+2],s->glVy[i+2],s->glVz[i+2],s->glCr[i+2],s->glCg[i+2],s->glCb[i+2]);
    } else if (m == ED_GL_QUADS) {
        for (int i = 0; i + 3 < cnt; i += 4) {
            ed_gl_triangle(s,
                s->glVx[i],s->glVy[i],s->glVz[i],s->glCr[i],s->glCg[i],s->glCb[i],
                s->glVx[i+1],s->glVy[i+1],s->glVz[i+1],s->glCr[i+1],s->glCg[i+1],s->glCb[i+1],
                s->glVx[i+2],s->glVy[i+2],s->glVz[i+2],s->glCr[i+2],s->glCg[i+2],s->glCb[i+2]);
            ed_gl_triangle(s,
                s->glVx[i],s->glVy[i],s->glVz[i],s->glCr[i],s->glCg[i],s->glCb[i],
                s->glVx[i+2],s->glVy[i+2],s->glVz[i+2],s->glCr[i+2],s->glCg[i+2],s->glCb[i+2],
                s->glVx[i+3],s->glVy[i+3],s->glVz[i+3],s->glCr[i+3],s->glCg[i+3],s->glCb[i+3]);
        }
    } else if (m == ED_GL_POLYGON || m == ED_GL_TRIANGLE_FAN) {
        for (int i = 1; i + 1 < cnt; i++)
            ed_gl_triangle(s,
                s->glVx[0],s->glVy[0],s->glVz[0],s->glCr[0],s->glCg[0],s->glCb[0],
                s->glVx[i],s->glVy[i],s->glVz[i],s->glCr[i],s->glCg[i],s->glCb[i],
                s->glVx[i+1],s->glVy[i+1],s->glVz[i+1],s->glCr[i+1],s->glCg[i+1],s->glCb[i+1]);
    } else if (m == ED_GL_TRIANGLE_STRIP) {
        for (int i = 0; i + 2 < cnt; i++) {
            int a0 = i, a1 = i+1, a2 = i+2;
            if (i & 1) { int t = a1; a1 = a2; a2 = t; }
            ed_gl_triangle(s,
                s->glVx[a0],s->glVy[a0],s->glVz[a0],s->glCr[a0],s->glCg[a0],s->glCb[a0],
                s->glVx[a1],s->glVy[a1],s->glVz[a1],s->glCr[a1],s->glCg[a1],s->glCb[a1],
                s->glVx[a2],s->glVy[a2],s->glVz[a2],s->glCr[a2],s->glCg[a2],s->glCb[a2]);
        }
    } else if (m == ED_GL_POINTS) {
        for (int i = 0; i < cnt; i++) {
            double sx, sy, d;
            ed_project3d(s, s->glVx[i], s->glVy[i], s->glVz[i], &sx, &sy, &d);
            if (d > 0.001) ed_put_pixel_z(s, (int)(sx+0.5), (int)(sy+0.5), d,
                ed_pack_rgb((int)s->glCr[i],(int)s->glCg[i],(int)s->glCb[i]));
        }
    } else if (m == ED_GL_LINES || m == ED_GL_LINE_STRIP || m == ED_GL_LINE_LOOP) {
        int last = (m == ED_GL_LINES) ? cnt : cnt - 1;
        for (int i = 0; i < last; i++) {
            int j = (m == ED_GL_LINES) ? i + 1 : i + 1;
            if (m == ED_GL_LINES && (i & 1)) continue;
            if (m == ED_GL_LINE_LOOP && i == cnt - 1) j = 0;
            double sx0, sy0, d0, sx1, sy1, d1;
            ed_project3d(s, s->glVx[i], s->glVy[i], s->glVz[i], &sx0, &sy0, &d0);
            ed_project3d(s, s->glVx[j], s->glVy[j], s->glVz[j], &sx1, &sy1, &d1);
            int steps = (int)(fabs(sx1 - sx0) + fabs(sy1 - sy0)) + 1;
            for (int k = 0; k <= steps; k++) {
                double t = steps ? (double)k / steps : 0;
                double z = d0 + (d1 - d0) * t;
                int r = (int)(s->glCr[i] + (s->glCr[j] - s->glCr[i]) * t);
                int g = (int)(s->glCg[i] + (s->glCg[j] - s->glCg[i]) * t);
                int b = (int)(s->glCb[i] + (s->glCb[j] - s->glCb[i]) * t);
                ed_put_pixel_z(s, (int)(sx0 + (sx1 - sx0) * t + 0.5),
                                  (int)(sy0 + (sy1 - sy0) * t + 0.5), z,
                    ed_pack_rgb(ed_clamp(r,0,255), ed_clamp(g,0,255), ed_clamp(b,0,255)));
            }
        }
    } else if (m == ED_GL_QUAD_STRIP) {
        for (int i = 0; i + 3 < cnt; i += 2) {
            ed_gl_triangle(s,
                s->glVx[i],s->glVy[i],s->glVz[i],s->glCr[i],s->glCg[i],s->glCb[i],
                s->glVx[i+1],s->glVy[i+1],s->glVz[i+1],s->glCr[i+1],s->glCg[i+1],s->glCb[i+1],
                s->glVx[i+2],s->glVy[i+2],s->glVz[i+2],s->glCr[i+2],s->glCg[i+2],s->glCb[i+2]);
            ed_gl_triangle(s,
                s->glVx[i+1],s->glVy[i+1],s->glVz[i+1],s->glCr[i+1],s->glCg[i+1],s->glCb[i+1],
                s->glVx[i+3],s->glVy[i+3],s->glVz[i+3],s->glCr[i+3],s->glCg[i+3],s->glCb[i+3],
                s->glVx[i+2],s->glVy[i+2],s->glVz[i+2],s->glCr[i+2],s->glCg[i+2],s->glCb[i+2]);
        }
    }
    s->glMode = -1;
    return;
}

/* matrix / mode helpers — no-op stubs (scripts usually supply world coords) */
static double hf_glloadidentity(pd_Host *h, int n, const double *a) { (void)h;(void)n;(void)a; return 0; }
static double hf_gltranslate(pd_Host *h, int n, const double *a) { (void)h;(void)n;(void)a; return 0; }
static double hf_glscale(pd_Host *h, int n, const double *a) { (void)h;(void)n;(void)a; return 0; }
static double hf_glrotate(pd_Host *h, int n, const double *a) { (void)h;(void)n;(void)a; return 0; }
static double hf_glpushmatrix(pd_Host *h, int n, const double *a) { (void)h;(void)n;(void)a; return 0; }
static double hf_glpopmatrix(pd_Host *h, int n, const double *a) { (void)h;(void)n;(void)a; return 0; }
static double hf_glortho(pd_Host *h, int n, const double *a) { (void)h;(void)n;(void)a; return 0; }
static double hf_glfrustum(pd_Host *h, int n, const double *a) { (void)h;(void)n;(void)a; return 0; }
static double hf_glclearcolor(pd_Host *h, int n, const double *a) { (void)h;(void)n;(void)a; return 0; }

/* Table of (signature, function) pairs for drawing-related host functions. */
typedef struct { const char *sig; pd_HostFn fn; int variadic; } ed_FnReg;

static const ed_FnReg ed_draw_fns[] = {
    { "CLS(,,)",      hf_cls,      0 },
    { "CLZ()",        hf_clz,      0 },
    { "SETCOL(,,)",   hf_setcol,   0 },
    { "SETPIX(,,)",   hf_setpix,   0 },
    { "GETPIX(,$,$,$)", hf_getpix, 0 },
    { "MOVETO(,,)",   hf_moveto,   0 },
    { "LINETO(,,)",   hf_lineto,   0 },
    { "DRAWSPH(,,)",  hf_drawsph,  0 },
    { "DRAWCONE(,,)", hf_drawcone, 0 },
    { "SETCAM(,,)",   hf_setcam,   0 },
    { "SETVIEW(,,)",  hf_setview,  0 },
    { "SETFONT(,)",   hf_setfont, 0 },
    { "PRINTNUM()",   hf_printnum, 0 },
    { "PRINTCHAR()",  hf_printchar, 0 },
    /* OpenGL immediate-mode */
    { "GLBEGIN()",      hf_glbegin,      0 },
    { "GLEND()",        hf_glend,        0 },
    { "GLVERTEX(,,)",   hf_glvertex,     0 },
    { "GLTEXCOORD(,)",  hf_gltexcoord,   0 },
    { "GLCOLOR(,,)",    hf_glcolor,      0 },
    { "GLSETTEX()",     hf_glsettex,     0 },
    { "GLNORMAL(,,)",   hf_glnormal,     0 },
    { "GLLOADIDENTITY()", hf_glloadidentity, 0 },
    { "GLTRANSLATE(,,)", hf_gltranslate, 0 },
    { "GLSCALE(,,)",    hf_glscale,      0 },
    { "GLROTATE(,,,)",  hf_glrotate,     0 },
    { "GLPUSHMATRIX()", hf_glpushmatrix, 0 },
    { "GLPOPMATRIX()",  hf_glpopmatrix,  0 },
    { "GLORTHO(,,,,,)", hf_glortho,      0 },
    { "GLFRUSTUM(,,,,,)", hf_glfrustum,  0 },
    { "GLCLEARCOLOR(,,,)", hf_glclearcolor, 0 },
    { NULL, NULL, 0 }
};

/* Expose the table to ed_host.c via a function. */
const ed_FnReg *ed_get_draw_fns(void) {
    return ed_draw_fns;
}
