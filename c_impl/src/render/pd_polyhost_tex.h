/* pd_polyhost_tex.h — host-side texture snapshots + recording externs.
 *
 * The host is the authoritative pixel source: glsettex(file) decodes the
 * image (stb_image) into a per-texel double snapshot, glsettex(array)
 * copies the EVAL array, and glgettex reads the snapshot back. The GL
 * renderer replays a GLCMD_SETTEXDATA carrying the snapshot pointer, so
 * host pixels and GPU texels always agree. */
#ifndef PD_POLYHOST_TEX_H
#define PD_POLYHOST_TEX_H

#include "pd_host.h"
#include "glcmd.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PD_MAX_TEX 32

/* one section block as handed to glsetshader (from the .pss @v/@g/@f) */
typedef struct {
    const char *src;   /* null-terminated source copy (owned by the ctx) */
    char        name[64];
    int         type;  /* PD_SEC_VERTEX / PD_SEC_GEOMETRY / PD_SEC_FRAGMENT */
    int         index; /* per-type sequence number */
} pdrl_Block;

/* KGL_* colmode bits (mirror polydraw.c) */
#define KGL_BGRA32 0
#define KGL_CHAR   1
#define KGL_SHORT  2
#define KGL_INT    3
#define KGL_FLOAT  4
#define KGL_VEC4   5
#define KGL_LINEAR        (0 << 4)
#define KGL_NEAREST       (1 << 4)
#define KGL_MIPMAP        (2 << 4)
#define KGL_MIPMAP2       (3 << 4)
#define KGL_MIPMAP1       (4 << 4)
#define KGL_MIPMAP0       (5 << 4)
#define KGL_REPEAT         (0 << 8)
#define KGL_MIRRORED_REPEAT (1 << 8)
#define KGL_CLAMP          (2 << 8)
#define KGL_CLAMP_TO_EDGE  (3 << 8)

/* one host texture snapshot (pixels owned by us; free with pd_tex_free) */
typedef struct {
    int     valid;    /* 1 once settex has stored pixels */
    int     w, h, z;  /* dimensions (z=1 for 2D, >1 for 3D) */
    int     colmode;  /* full colmode: coltype|filter|wrap bits */
    char    nam[64];  /* source file name ("" for arrays/capture) */
    double *pixels;   /* per-texel doubles (BGRA32: packed 0xAABBGGRR) */
} pd_Tex;

/* install texture recording externs on top of pd_polyhost_install_render.
 * blocks = the .pss section blocks (for glsetshader name/index lookup),
 * owned by the caller (the runlib ctx) and valid until pdrl_free. */
void pd_polyhost_install_tex(pd_Host *h, GLCmdBuf *glbuf,
                             const pdrl_Block *blocks, int nblocks);

/* access the block table (used by pdrl_compile_sections to build it) */
const pdrl_Block *pd_tex_blocks(int *nblocks);

/* set the section block table (call after pd_polyhost_install_tex, before
 * running frames — the recorded GLCMD_SETSHADER replays look up GLSL source
 * pointers here). The table is owned by the caller until pdrl_free. */
void pd_polyhost_set_blocks(const pdrl_Block *blocks, int nblocks);

/* free all snapshot pixel buffers (call from pdrl_free) */
void pd_tex_free_all(void);

/* resolve a cached glGetUniformLoc id to its name (persists across the
 * per-frame GLCmdBuf reset, unlike GLCMD_UNIFORMLOC). Returns NULL if the
 * id was never registered. Used by the renderer to map uniforms to names. */
const char *pd_polyhost_get_locname(int id);

#ifdef __cplusplus
}
#endif
#endif
