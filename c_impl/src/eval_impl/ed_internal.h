/* ed_internal.h — shared helpers for evaldraw host implementation.
 *
 * Internal header used by ed_host.c and ed_util.c. Not part of the public
 * API (use ed_host.h for that).
 */
#ifndef ED_INTERNAL_H
#define ED_INTERNAL_H

#include "ed_host.h"
#include <math.h>

/* Shorthand: extract the ed_State pointer from a pd_Host. */
#define ST(h) ((ed_State *)(h)->state)

/* Clamp an integer to [lo, hi]. */
static inline int ed_clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Pack three 0-255 components into a 24-bit color. */
static inline uint32_t ed_pack_rgb(int r, int g, int b) {
    return ((uint32_t)ed_clamp(r, 0, 255) << 16) |
           ((uint32_t)ed_clamp(g, 0, 255) << 8) |
           (uint32_t)ed_clamp(b, 0, 255);
}

/* Unpack a 24-bit color into three 0-255 components. */
static inline void ed_unpack_rgb(uint32_t col, int *r, int *g, int *b) {
    *r = (col >> 16) & 0xFF;
    *g = (col >> 8) & 0xFF;
    *b = col & 0xFF;
}

/* Bounds check: is (x, y) inside the framebuffer? */
static inline int ed_in_bounds(ed_State *s, int x, int y) {
    return x >= 0 && x < s->xres && y >= 0 && y < s->yres;
}

/* Plot a single pixel (no Z-test). */
static inline void ed_put_pixel(ed_State *s, int x, int y, uint32_t col) {
    if (ed_in_bounds(s, x, y))
        s->fb[y * s->xres + x] = col;
}

/* Plot a pixel with Z-buffer test. */
static inline void ed_put_pixel_z(ed_State *s, int x, int y, double z, uint32_t col) {
    if (!ed_in_bounds(s, x, y))
        return;
    int idx = y * s->xres + x;
    if (z < s->zbuf[idx]) {
        s->zbuf[idx] = z;
        s->fb[idx] = col;
    }
}

/* Get the current drawing color as a packed uint32. */
static inline uint32_t ed_current_color(ed_State *s) {
    return (uint32_t)s->curColor;
}

/* ---- functions implemented in ed_util.c ---- */

/* Recompute camera right/down/forward vectors from hang/vang. */
void ed_cam_update_vectors(ed_State *s);

/* Project a 3D world point to screen coordinates + depth. */
void ed_project3d(ed_State *s, double x, double y, double z,
                  double *sx, double *sy, double *depth);

/* Append text to the printf log buffer (or stdout if no buffer). */
void ed_log(ed_State *s, const char *str, size_t len);

/* Draw a single ASCII character using the built-in 5x7 font. */
void ed_draw_char(ed_State *s, int x0, int y0, char ch, uint32_t col);

/* Rasterize the pending GL primitive (used to auto-flush geometry that an
 * EVAL script left open without an explicit glend()). */
void ed_gl_flush(ed_State *s);

#endif /* ED_INTERNAL_H */
