/* ed_util.c — camera, projection, logging, and font helpers.
 *
 * These are shared utility functions used by the host function
 * implementations in ed_host.c.
 */
#include "ed_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- camera ---- */

void ed_cam_update_vectors(ed_State *s) {
    double ch = cos(s->camHang);
    double sh = sin(s->camHang);
    double cv = cos(s->camVang);
    double sv = sin(s->camVang);

    /* Forward: starts at +Z, rotated by hang (around Y) then vang (around right).
     * At hang=0, vang=0: forward = (0, 0, 1). */
    s->camFx = sh * cv;
    s->camFy = sv;
    s->camFz = ch * cv;

    /* Right: perpendicular to forward in the horizontal plane.
     * At hang=0: right = (1, 0, 0). */
    s->camRx = ch;
    s->camRy = 0.0;
    s->camRz = -sh;

    /* Down = cross(right, forward).
     * At hang=0, vang=0: down = (0, 1, 0) (screen y is downward). */
    s->camDx = s->camRy * s->camFz - s->camRz * s->camFy;
    s->camDy = s->camRz * s->camFx - s->camRx * s->camFz;
    s->camDz = s->camRx * s->camFy - s->camRy * s->camFx;
}

/* ---- 3D projection ---- */

void ed_project3d(ed_State *s, double x, double y, double z,
                  double *sx, double *sy, double *depth) {
    double dx = x - s->camX;
    double dy = y - s->camY;
    double dz = z - s->camZ;

    /* Transform world point into camera space. */
    double cx = dx * s->camRx + dy * s->camRy + dz * s->camRz; /* right   */
    double cy = dx * s->camDx + dy * s->camDy + dz * s->camDz; /* down    */
    double cz = dx * s->camFx + dy * s->camFy + dz * s->camFz; /* forward */

    if (cz < 0.001)
        cz = 0.001;

    *sx = cx / cz * s->vpHz + s->vpHx;
    *sy = cy / cz * s->vpHz + s->vpHy;
    *depth = cz;
}

/* ---- logging ---- */

void ed_log(ed_State *s, const char *str, size_t len) {
    if (!s->logBuf) {
        fwrite(str, 1, len, stdout);
        return;
    }
    if (s->logLen + len + 1 > s->logCap) {
        size_t newcap = s->logCap ? s->logCap * 2 : 1024;
        while (newcap < s->logLen + len + 1)
            newcap *= 2;
        s->logBuf = realloc(s->logBuf, newcap);
        s->logCap = newcap;
    }
    memcpy(s->logBuf + s->logLen, str, len);
    s->logLen += len;
    s->logBuf[s->logLen] = '\0';
}

/* ---- 5x7 bitmap font (ASCII 32-127) ---- */

static const uint8_t font5x7[96][7] = {
    /* 32 ' ' */ {0, 0, 0, 0, 0, 0, 0},
    /* 33 '!' */ {0, 0, 0, 0, 0, 0, 0},
    /* 34 '"' */ {0, 0, 0, 0, 0, 0, 0},
    /* 35 '#' */ {0, 0, 0, 0, 0, 0, 0},
    /* 36 '$' */ {0, 0, 0, 0, 0, 0, 0},
    /* 37 '%' */ {0, 0, 0, 0, 0, 0, 0},
    /* 38 '&' */ {0, 0, 0, 0, 0, 0, 0},
    /* 39 ''' */ {0, 0, 0, 0, 0, 0, 0},
    /* 40 '(' */ {0, 0, 0, 0, 0, 0, 0},
    /* 41 ')' */ {0, 0, 0, 0, 0, 0, 0},
    /* 42 '*' */ {0, 0, 0, 0, 0, 0, 0},
    /* 43 '+' */ {0, 0, 0, 0, 0, 0, 0},
    /* 44 ',' */ {0, 0, 0, 0, 0, 0, 0},
    /* 45 '-' */ {0, 0, 0, 0, 0, 0, 0},
    /* 46 '.' */ {0, 0, 0, 0, 0, 0, 0},
    /* 47 '/' */ {0, 0, 0, 0, 0, 0, 0},
    /* 48 '0' */ {0x3E, 0x51, 0x49, 0x45, 0x3E, 0, 0},
    /* 49 '1' */ {0x42, 0x7F, 0x40, 0, 0, 0, 0},
    /* 50 '2' */ {0x42, 0x61, 0x51, 0x49, 0x46, 0, 0},
    /* 51 '3' */ {0x21, 0x41, 0x45, 0x4B, 0x31, 0, 0},
    /* 52 '4' */ {0x18, 0x14, 0x12, 0x7F, 0x10, 0, 0},
    /* 53 '5' */ {0x27, 0x45, 0x45, 0x45, 0x39, 0, 0},
    /* 54 '6' */ {0x3C, 0x4A, 0x49, 0x49, 0x30, 0, 0},
    /* 55 '7' */ {0x01, 0x71, 0x09, 0x05, 0x03, 0, 0},
    /* 56 '8' */ {0x36, 0x49, 0x49, 0x49, 0x36, 0, 0},
    /* 57 '9' */ {0x06, 0x49, 0x49, 0x29, 0x1E, 0, 0},
    /* 58 ':' */ {0, 0, 0, 0, 0, 0, 0},
    /* 59 ';' */ {0, 0, 0, 0, 0, 0, 0},
    /* 60 '<' */ {0, 0, 0, 0, 0, 0, 0},
    /* 61 '=' */ {0, 0, 0, 0, 0, 0, 0},
    /* 62 '>' */ {0, 0, 0, 0, 0, 0, 0},
    /* 63 '?' */ {0, 0, 0, 0, 0, 0, 0},
    /* 64 '@' */ {0, 0, 0, 0, 0, 0, 0},
    /* 65 'A' */ {0x3E, 0x41, 0x49, 0x49, 0x7A, 0, 0},
    /* 66 'B' */ {0x7E, 0x11, 0x11, 0x11, 0x7E, 0, 0},
    /* 67 'C' */ {0x3E, 0x41, 0x41, 0x41, 0x22, 0, 0},
    /* 68 'D' */ {0x7F, 0x41, 0x41, 0x22, 0x1C, 0, 0},
    /* 69 'E' */ {0x7F, 0x49, 0x49, 0x49, 0x41, 0, 0},
    /* 70 'F' */ {0x7F, 0x09, 0x09, 0x09, 0x01, 0, 0},
    /* 71 'G' */ {0x3E, 0x41, 0x41, 0x51, 0x32, 0, 0},
    /* 72 'H' */ {0x7F, 0x08, 0x08, 0x08, 0x7F, 0, 0},
    /* 73 'I' */ {0, 0x41, 0x7F, 0x41, 0, 0, 0},
    /* 74 'J' */ {0x20, 0x40, 0x41, 0x3F, 0x01, 0, 0},
    /* 75 'K' */ {0x7F, 0x08, 0x14, 0x22, 0x41, 0, 0},
    /* 76 'L' */ {0x7F, 0x40, 0x40, 0x40, 0x40, 0, 0},
    /* 77 'M' */ {0x7F, 0x02, 0x0C, 0x02, 0x7F, 0, 0},
    /* 78 'N' */ {0x7F, 0x04, 0x08, 0x10, 0x7F, 0, 0},
    /* 79 'O' */ {0x3E, 0x41, 0x41, 0x41, 0x3E, 0, 0},
    /* 80 'P' */ {0x7F, 0x09, 0x09, 0x09, 0x06, 0, 0},
    /* 81 'Q' */ {0x1E, 0x21, 0x21, 0x21, 0x5E, 0, 0},
    /* 82 'R' */ {0x7F, 0x09, 0x19, 0x29, 0x46, 0, 0},
    /* 83 'S' */ {0x46, 0x49, 0x49, 0x49, 0x31, 0, 0},
    /* 84 'T' */ {0x01, 0x01, 0x7F, 0x01, 0x01, 0, 0},
    /* 85 'U' */ {0x3F, 0x40, 0x40, 0x40, 0x3F, 0, 0},
    /* 86 'V' */ {0x1F, 0x20, 0x40, 0x20, 0x1F, 0, 0},
    /* 87 'W' */ {0x3F, 0x40, 0x38, 0x40, 0x3F, 0, 0},
    /* 88 'X' */ {0x63, 0x14, 0x08, 0x14, 0x63, 0, 0},
    /* 89 'Y' */ {0x07, 0x08, 0x70, 0x08, 0x07, 0, 0},
    /* 90 'Z' */ {0x61, 0x51, 0x49, 0x45, 0x43, 0, 0},
    /* 91-127: filled with zeros for now (lowercase + symbols) */
    {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0},
};

void ed_draw_char(ed_State *s, int x0, int y0, char ch, uint32_t col) {
    int ci = (unsigned char)ch - 32;
    if (ci < 0 || ci >= 96)
        return;
    const uint8_t *glyph = font5x7[ci];
    for (int py = 0; py < 7; py++) {
        uint8_t row = glyph[py];
        for (int px = 0; px < 5; px++) {
            if (row & (1 << (4 - px)))
                ed_put_pixel(s, x0 + px, y0 + py, col);
        }
    }
}
