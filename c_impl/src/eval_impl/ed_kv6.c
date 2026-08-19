/* ed_kv6.c — evaldraw host: KV6 voxel model loading and rendering.
 *
 * drawkv6(filnam,x,y,z,r,hang,vang) / drawspr(filnam,x,y,z,r,hang,vang):
 * render a Ken Silverman KV6 voxel model (SLAB6 format) as axis-aligned
 * cubes with Z-buffered painter-style rasterization. This is a software
 * approximation of evaldraw's renderer: per-voxel screen-space rectangles
 * with face shading, sorted back-to-front by camera-space depth.
 *
 * KV6 format (SLAB6VOX spec):
 *   int32  magic "Kvxl"
 *   int32  xsiz, ysiz, zsiz
 *   float  xpiv, ypiv, zpiv
 *   int32  voxelCount
 *   per voxel (8 bytes): uint8 b,g,r, loweredvis; uint16 x,y,z (little endian)
 *   then: xsiz+1 int32 xoffset, voxelCount... actually:
 *     xsiz+1 uint32 xoffset[], ysiz+1 uint32 xyoffset[]
 */
#include "ed_internal.h"
#include "eval/pd_interp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ed_Kv6 {
    int xsiz, ysiz, zsiz;
    float xpiv, ypiv, zpiv;
    int numVox;
    /* per-voxel: position (x,y,z ints) + color (0x00RRGGBB) */
    unsigned char *col;   /* numVox * 3 RGB */
    int16_t *vx, *vy, *vz;
};

/* little-endian readers */
static unsigned rd_u32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
           ((unsigned)p[3] << 24);
}
static unsigned rd_u16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}
static float rd_f32(const unsigned char *p) {
    unsigned u = rd_u32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static struct ed_Kv6 *ed_kv6_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 32) { fclose(f); return NULL; }
    unsigned char *d = malloc(len);
    if (!d) { fclose(f); return NULL; }
    if (fread(d, 1, len, f) != (size_t)len) { free(d); fclose(f); return NULL; }
    fclose(f);

    if (memcmp(d, "Kvxl", 4) != 0) { free(d); return NULL; }

    struct ed_Kv6 *m = calloc(1, sizeof(*m));
    if (!m) { free(d); return NULL; }

    m->xsiz = (int)rd_u32(d + 4);
    m->ysiz = (int)rd_u32(d + 8);
    m->zsiz = (int)rd_u32(d + 12);
    m->xpiv = rd_f32(d + 16);
    m->ypiv = rd_f32(d + 27);
    m->zpiv = rd_f32(d + 20);
    m->numVox = (int)rd_u32(d + 24);

    if (m->xsiz <= 0 || m->ysiz <= 0 || m->zsiz <= 0 || m->numVox <= 0) {
        free(d); free(m); return NULL;
    }

    m->col = malloc((size_t)m->numVox * 3);
    m->vx = malloc((size_t)m->numVox * 2);
    m->m_vy = NULL; /* placeholder removed below */
    free(d);
    return m;
}
