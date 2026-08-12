/* pd_polyhost_tex.c — recording host externs for textures, shaders,
 * uniforms, and the remaining GL calls (glsettex/glgettex/glbindtexture/
 * glactivetexture/glcapture/glsetshader/gluniform-family/setfov/gluLookAt).
 *
 * The host is the authoritative pixel source: settex(file) decodes the
 * image with stb_image into per-texel doubles, settex(array) copies the
 * EVAL array, and gettex reads the snapshot back. The renderer replays a
 * GLCMD_SETTEXDATA carrying the snapshot pointer, so host pixels and GPU
 * texels always agree. (Capture renders into textures via the GPU; the
 * host snapshot is stale for those — see Plan/05_Graphics.md.)
 *
 * Single-ctx assumption: the loc/name tables below are per-process
 * statics, matching the render pipeline's one-ctx usage.
 */
#include "pd_polyhost_tex.h"
#include "../eval/pd_section.h"
#include "../eval/pd_interp.h"   /* pd_run's pd_Ctx not needed; keep minimal */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_SIMD
#include "stb_image.h"

/* ---- per-ctx state (single-ctx pipeline) ----
 * The recording target (glbuf) is carried via the pd_Host (h->glbuf), set by
 * pd_polyhost_install_tex. The texture/block/loc tables remain process statics
 * bound to the currently-installed program (one active compile per process). */
static const pdrl_Block *g_blocks = NULL;
static int g_nblocks = 0;

pd_Tex pd_tex[PD_MAX_TEX];

/* glGetUniformLoc id table: names live in the program's string table */
#define PD_MAX_LOCS 64
static const char *g_locnames[PD_MAX_LOCS];
static int g_nlocs = 0;

/* Persistent id→name map for uniform locations. glGetUniformLoc is usually
 * called once (guarded by numframes==0) and cached; the id→name mapping must
 * survive the per-frame GLCmdBuf reset so the renderer can resolve a uniform
 * id to its name during replay of any later frame. */
const char *pd_polyhost_get_locname(int id) {
    if (id >= 0 && id < g_nlocs) return g_locnames[id];
    return NULL;
}

/* cap on the number of floats in a single glUniform*V upload */
#define PD_MAX_UNIFVAL 4096

/* gluLookAt / glMultMatrix scratch */
static double g_mat[16];

/* ---- helpers ---- */

static const char *str_arg(const double *a, int idx)
{
    const char *s = NULL;
    memcpy(&s, &a[idx], sizeof(void*));
    return s;
}

static double *ptr_arg(const double *a, int idx)
{
    double *p = NULL;
    memcpy(&p, &a[idx], sizeof(void*));
    return p;
}

static int blk_find(int type, int index)
{
    for (int i = 0; i < g_nblocks; i++)
        if (g_blocks[i].type == type && g_blocks[i].index == index)
            return i;
    return -1;
}

static int blk_find_name(int type, const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < g_nblocks; i++)
        if (g_blocks[i].type == type && strcasecmp(g_blocks[i].name, name) == 0)
            return i;
    return -1;
}

/* clamp a block index like the original setshader_int (out of range → 0) */
static int blk_clamp(int type, int index)
{
    int first = blk_find(type, 0);
    if (first < 0) return -1;
    if (blk_find(type, index) < 0) return first;
    return blk_find(type, index);
}

static void emit(pd_Host *h, GLCmdOp op, int mode, double a, double b, double c, double d)
{
    GLCmd *cmd = glcmd_push(h->glbuf);
    if (cmd) { cmd->op = (int)op; cmd->mode = mode;
               cmd->a = a; cmd->b = b; cmd->c = c; cmd->d = d; cmd->s = NULL; }
}

/* ---- textures ---- */

/* decode an image file into a BGRA32 double snapshot (one packed
 * 0xAABBGGRR double per texel). Returns 1 on success, 0 if not found. */
static int decode_file(const char *file, int *w, int *h, double **out)
{
    int ch = 0;
    unsigned char *px = stbi_load(file, w, h, &ch, 4);
    if (!px) return 0;
    size_t n = (size_t)(*w) * (*h);
    double *pix = malloc(n * sizeof(double));
    if (!pix) { stbi_image_free(px); return 0; }
    for (size_t i = 0; i < n; i++) {
        unsigned char *q = px + i * 4;
        unsigned int v = ((unsigned int)q[3] << 24) | ((unsigned int)q[2] << 16) |
                         ((unsigned int)q[1] << 8) | q[0];
        pix[i] = (double)v;
    }
    stbi_image_free(px);
    *out = pix;
    return 1;
}

/* placeholder 32x32 "IMAGE NOT FOUND" bitmap, verbatim from polydraw.c */
static const int imagenotfoundbmp[32] = {
    0x7ce39138,0x05145b10,0x04145510,0x3dd45110,0x0517d110,0x05145110,0x7de45138,0x00000000,
    0x01f39100,0x00445300,0x00445500,0x00445900,0x00445100,0x00445100,0x00439100,0x00000000,
    0x3d144e7c,0x45345104,0x45545104,0x4594513c,0x45145104,0x45145104,0x3d138e04,0x00000000,
    0x00400000,0x00200000,0x00200600,0x0027c600,0x00200000,0x00200600,0x00400600,0x00000000,
};

static void make_placeholder(int *w, int *h, double **out)
{
    /* Seed once so the placeholder noise is deterministic across runs (and
     * across the interp vs JIT differential runs, which must compare byte
     * for byte). The original uses rand() unsweded; we pin it to a fixed
     * seed so the result is reproducible. */
    static int seeded = 0;
    if (!seeded) { srand(0x9e3779b1u); seeded = 1; }
    *w = 32; *h = 32;
    double *pix = malloc(32 * 32 * sizeof(double));
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            if (imagenotfoundbmp[y] & (1 << x)) { pix[y * 32 + x] = (double)0xf0102030u; continue; }
            /* note: the original always writes gbmp[0] here (a bug we
             * keep for byte-identical behaviour) */
            pix[y * 32 + x] = (double)(((rand() << 15) + rand()) & 0x1f1f1f) + 0xff506070;
        }
    *out = pix;
}

/* glsettex(tex, file) / glsettex(tex, file, colmode) */
static double rh_glSetTexFile(pd_Host *h, int n, const double *a)
{
    int tex = (int)a[0];
    if (tex < 0 || tex >= PD_MAX_TEX) return -1;
    const char *file = str_arg(a, 1);
    if (!file) return -2;
    int colmode = n >= 3 ? (int)a[2] : (KGL_MIPMAP + KGL_REPEAT);
    int coltype = colmode & 15;

    /* skip re-load when the same file+colmode is already in (original
     * semantics: glsettex is only called once per texture normally) */
    if (pd_tex[tex].valid && strcmp(pd_tex[tex].nam, file) == 0 &&
        pd_tex[tex].colmode == colmode)
        return 0;

    int w = 0, th = 0;
    double *pix = NULL;
    if (!decode_file(file, &w, &th, &pix)) {
        w = th = 32;
        make_placeholder(&w, &th, &pix);
    }
    free(pd_tex[tex].pixels);
    pd_tex[tex].pixels = pix;
    pd_tex[tex].w = w; pd_tex[tex].h = th; pd_tex[tex].z = 1;
    pd_tex[tex].colmode = colmode;
    pd_tex[tex].valid = 1;
    pd_tex[tex].nam[0] = 0;
    strncpy(pd_tex[tex].nam, file, sizeof(pd_tex[tex].nam) - 1);

    GLCmd *cmd = glcmd_push(h->glbuf);
    if (cmd) {
        cmd->op = GLCMD_SETTEXDATA; cmd->mode = colmode;
        cmd->a = tex; cmd->b = w; cmd->c = th; cmd->d = 1;
        cmd->s = (const char*)pix;
    }
    return 0;
}

/* glsettex(tex, &arr, w [,h [,z]], coltype) — 1D/2D/3D array upload */
static double rh_glSetTexArray(pd_Host *h, int n, const double *a)
{
    if (getenv("PD_DEBUG_GL"))
        fprintf(stderr, "rh_glSetTexArray n=%d a0=%.0f a1=%p a2=%.0f a3=%.0f a4=%.0f\n",
                n, a[0], (void*)(uintptr_t)a[1], n>=3?a[2]:-1, n>=4?a[3]:-1, n>=5?a[4]:-1);
    int tex = (int)a[0];
    if (tex < 0 || tex >= PD_MAX_TEX) return -1;
    double *arr = ptr_arg(a, 1);
    if (!arr) return -2;
    int w = (int)a[2];
    /* original kglsettexarray1/2/3: coltype is ALWAYS the last arg.
     *   4-arg: (tex,&arr,w,coltype)        → h=1, z=1
     *   5-arg: (tex,&arr,w,h,coltype)      → h=a[3], z=1
     *   6-arg: (tex,&arr,w,h,z,coltype)    → h=a[3], z=a[4] */
    int th = (n >= 5) ? (int)a[3] : 1;
    int z  = (n >= 6) ? (int)a[4] : 1;
    int colmode = (int)a[n - 1];
    int coltype = colmode & 15;
    if (w < 1 || th < 1 || z < 1) return -1;
    int elem = (coltype == KGL_VEC4) ? 4 : 1;
    size_t count = (size_t)w * th * z * elem;
    if (count > 67108864) return -1;

    double *pix = malloc(count * sizeof(double));
    if (!pix) return -1;
    memcpy(pix, arr, count * sizeof(double));

    if (getenv("PD_DEBUG_TEX")) {
        fprintf(stderr, "SETTEXDATA tex=%d %dx%dx%d colmode=%d count=%zu\n",
                tex, w, th, z, colmode, count);
        unsigned int v0 = (unsigned int)(uint64_t)pix[0];
        unsigned int v1 = (unsigned int)(uint64_t)pix[1];
        unsigned int vm = (unsigned int)(uint64_t)pix[count/2];
        fprintf(stderr, "  px0=0x%08x px1=0x%08x pxmid=0x%08x\n", v0, v1, vm);
    }

    free(pd_tex[tex].pixels);
    pd_tex[tex].pixels = pix;
    pd_tex[tex].w = w; pd_tex[tex].h = th; pd_tex[tex].z = z;
    pd_tex[tex].colmode = colmode;
    pd_tex[tex].valid = 1;
    pd_tex[tex].nam[0] = 0;

    GLCmd *cmd = glcmd_push(h->glbuf);
    if (cmd) {
        cmd->op = GLCMD_SETTEXDATA; cmd->mode = colmode;
        cmd->a = tex; cmd->b = w; cmd->c = th; cmd->d = z;
        cmd->s = (const char*)pix;
    }
    return 0;
}

/* glgettex(tex, &arr, w, h, coltype) — read the host snapshot back.
 * (The coltype arg is ignored, as in the original: the texture's own
 * coltype governs the conversion.) */
static double rh_glGetTexArray(pd_Host *h, int n, const double *a)
{
    int tex = (int)a[0];
    if (tex < 0 || tex >= PD_MAX_TEX) return -1;
    double *arr = ptr_arg(a, 1);
    if (!arr) return -2;
    int xs = (int)a[2], ys = (int)a[3];
    if (xs < 1 || ys < 1 || (double)xs * ys > 67108864) return -1;
    if (!pd_tex[tex].valid) { memset(arr, 0, (size_t)xs * ys * 8); return 0; }
    if ((size_t)xs * ys > (size_t)pd_tex[tex].w * pd_tex[tex].h) return -1;

    int coltype = pd_tex[tex].colmode & 15;
    int elem = (coltype == KGL_VEC4) ? 4 : 1;
    const double *src = pd_tex[tex].pixels;
    if (coltype == KGL_BGRA32 || coltype == KGL_FLOAT || coltype == KGL_VEC4) {
        for (int y = 0; y < ys; y++)
            memcpy(arr + (size_t)y * xs * elem, src + (size_t)y * pd_tex[tex].w * elem,
                   (size_t)xs * elem * sizeof(double));
    } else {
        /* CHAR/SHORT/INT: extract the red channel (GL luminance readback) */
        for (size_t i = 0; i < (size_t)xs * ys; i++) {
            unsigned int v = (unsigned int)(uint64_t)src[i];
            arr[i] = (double)((v >> 16) & 0xFF);
        }
    }
    return 0;
}

/* glbindtexture(tex) — bind to the current active unit */
static double rh_glBindTex(pd_Host *h, int n, const double *a)
{
    GLCmd *cmd = glcmd_push(h->glbuf);
    if (cmd) { cmd->op = GLCMD_BINDTEX; cmd->a = n >= 1 ? a[0] : 0; }
    return 0;
}

/* glactivetexture(unit) — original masks to unit & 3 */
static double rh_glActiveTex(pd_Host *h, int n, const double *a)
{
    GLCmd *cmd = glcmd_push(h->glbuf);
    if (cmd) { cmd->op = GLCMD_ACTIVETEX; cmd->a = (double)(((int)(n >= 1 ? a[0] : 0)) & 3); }
    return 0;
}

/* glcapture() — screen capture mode; glcapture(tex,w,h,coltype) — offscreen */
static double rh_glCapture(pd_Host *h, int n, const double *a)
{
    if (getenv("PD_DEBUG_GL")) fprintf(stderr, "rh_glCapture n=%d a0=%g\n", n, n>=1?a[0]:-1);
    if (n == 0) {
        GLCmd *cmd = glcmd_push(h->glbuf);
        if (cmd) { cmd->op = GLCMD_CAPTURE; cmd->a = -1; cmd->b = 0; cmd->c = 0; cmd->mode = 0; }
    } else if (n == 4) {
        GLCmd *cmd = glcmd_push(h->glbuf);
        if (cmd) { cmd->op = GLCMD_CAPTURE; cmd->a = a[0]; cmd->b = a[1]; cmd->c = a[2]; cmd->mode = (int)a[3]; }
    }
    return 0;
}

static double rh_glEndCapture(pd_Host *h, int n, const double *a)
{
    GLCmd *cmd = glcmd_push(h->glbuf);
    if (cmd) { cmd->op = GLCMD_CAPTUREEND; cmd->a = n >= 1 ? a[0] : 0; }
    return 0;
}

/* ---- shaders & uniforms ---- */

/* glsetshader(d): v block 0, f block (int)d (original setshader_int(0,-1,d))
 * glsetshader(vname, fname[, gname]): named lookup */
static double rh_glSetShader(pd_Host *h, int n, const double *a)
{
    if (getenv("PD_DEBUG_GL")) fprintf(stderr, "rh_glSetShader n=%d a0=%g\n", n, n>=1?a[0]:-1);
    int vi = 0, fi = 0;
    if (n == 1) {
        fi = (int)a[0];
    } else if (n >= 2) {
        const char *vn = str_arg(a, 0);
        const char *fn = str_arg(a, 1);
        int vb = vn ? blk_find_name(PD_SEC_VERTEX, vn) : -1;
        int fb = fn ? blk_find_name(PD_SEC_FRAGMENT, fn) : -1;
        if (vb < 0) vb = blk_clamp(PD_SEC_VERTEX, 0);
        if (fb < 0) fb = blk_clamp(PD_SEC_FRAGMENT, 0);
        GLCmd *cmd = glcmd_push(h->glbuf);
        if (cmd) {
            cmd->op = GLCMD_SETSHADER;
            cmd->a = (double)(uintptr_t)(vb >= 0 ? g_blocks[vb].src : NULL);
            cmd->b = (double)(uintptr_t)(fb >= 0 ? g_blocks[fb].src : NULL);
        }
        return 0;
    }
    vi = blk_clamp(PD_SEC_VERTEX, vi);
    fi = blk_clamp(PD_SEC_FRAGMENT, fi);
    GLCmd *cmd = glcmd_push(h->glbuf);
    if (cmd) {
        cmd->op = GLCMD_SETSHADER;
        cmd->a = (double)(uintptr_t)(vi >= 0 ? g_blocks[vi].src : NULL);
        cmd->b = (double)(uintptr_t)(fi >= 0 ? g_blocks[fi].src : NULL);
    }
    return 0;
}

/* glGetUniformLoc(name) — assign a monotonic id shared with the renderer */
static double rh_glGetUniformLoc(pd_Host *h, int n, const double *a)
{
    const char *name = str_arg(a, 0);
    if (!name) return 0;
    for (int i = 0; i < g_nlocs; i++)
        if (strcmp(g_locnames[i], name) == 0) return (double)i;
    if (g_nlocs >= PD_MAX_LOCS) return 0;
    int id = g_nlocs++;
    g_locnames[id] = name;
    GLCmd *cmd = glcmd_push(h->glbuf);
    if (cmd) { cmd->op = GLCMD_UNIFORMLOC; cmd->a = id; cmd->s = name; }
    return (double)id;
}

/* glUniform* family. Each variant is its own host fn (the host dispatch
 * only knows (nargs, args)); the variant closure carries kind/count/comps.
 * Scalar forms pack up to 3 values into b/c/d (4f loses the 4th slot — no
 * target script uses it). Array forms copy a float buffer into `s`. */
static double emit_uniform_scalar(pd_Host *h, int kind, int count, int n, const double *a)
{
    GLCmd *cmd = glcmd_push(h->glbuf);
    if (!cmd) return 0;
    cmd->op = GLCMD_UNIFORM;
    /* mode packing: high byte = kind (0 float / 1 int);
     *               2nd byte   = components per vector (1..4);
     *               low 2 byte = element count (scalars: same as comps). */
    cmd->mode = ((unsigned)(kind & 0xFF) << 24) |
                ((unsigned)(count & 0xFF) << 16) |
                ((unsigned)count & 0xFFFF);
    cmd->a = n >= 1 ? a[0] : 0;          /* loc */
    cmd->b = n >= 2 ? a[1] : 0;
    cmd->c = n >= 3 ? a[2] : 0;
    cmd->d = n >= 4 ? a[3] : 0;
    cmd->s = NULL;
    return 0;
}
static double emit_uniform_array(pd_Host *h, int kind, int comps, int n, const double *a)
{
    int id = (int)a[0];
    /* The reference host (kglUniformNfv) treats the 2nd argument as the
     * number of *floats* copied from the EVAL array AND passes it straight
     * through as the element count to the real glUniformNfv. The driver then
     * reads count*comps floats. Match that contract: allocate count*comps
     * floats (so the driver never reads past our buffer), fill the leading
     * `count` floats from the EVAL array (the real payload), zero-fill the
     * rest. The buffer is owned by the command and freed on glcmd reset/free. */
    int count = n >= 2 ? (int)a[1] : 0;
    if (count < 0) count = 0;
    double *arr = n >= 3 ? ptr_arg(a, 2) : NULL;
    int total = count * comps;
    if (total < 0) total = 0;
    if (total > PD_MAX_UNIFVAL) total = PD_MAX_UNIFVAL;
    /* Store the payload in the driver's native element type: floats for the
     * FV variants, 32-bit ints for IV (the renderer replays the matching
     * glUniform*iv, so a bit-cast float buffer would be garbage to GLint). */
    int isInt = (kind & 0xFF) == PD_UNI_I;
    void *buf = malloc((size_t)total * (isInt ? sizeof(int32_t) : sizeof(float)));
    if (!buf) return 0;
    for (int i = 0; i < total; i++) {
        double v = (i < count && arr) ? arr[i] : 0.0;
        if (isInt) ((int32_t*)buf)[i] = (int32_t)v;
        else       ((float*)buf)[i] = (float)v;
    }
    GLCmd *cmd = glcmd_push(h->glbuf);
    if (!cmd) { free(buf); return 0; }
    cmd->op = GLCMD_UNIFORM;
    cmd->mode = ((unsigned)(kind & 0xFF) << 24) |
                ((unsigned)(comps & 0xFF) << 16) |
                ((unsigned)count & 0xFFFF);
    cmd->a = id;
    cmd->s = (const char*)buf;   /* freed by glcmd_reset / glcmd_free */
    return 0;
}

#define MK_UNI_F(N, C)  static double N(pd_Host *h, int n, const double *a){ return emit_uniform_scalar(h, PD_UNI_F, C, n, a); }
#define MK_UNI_I(N, C)  static double N(pd_Host *h, int n, const double *a){ return emit_uniform_scalar(h, PD_UNI_I, C, n, a); }
#define MK_UNI_FV(N, C) static double N(pd_Host *h, int n, const double *a){ return emit_uniform_array(h, PD_UNI_F, C, n, a); }
#define MK_UNI_IV(N, C) static double N(pd_Host *h, int n, const double *a){ return emit_uniform_array(h, PD_UNI_I, C, n, a); }
MK_UNI_F(rh_glUniform1f, 1) MK_UNI_F(rh_glUniform2f, 2) MK_UNI_F(rh_glUniform3f, 3) MK_UNI_F(rh_glUniform4f, 4)
MK_UNI_I(rh_glUniform1i, 1) MK_UNI_I(rh_glUniform2i, 2) MK_UNI_I(rh_glUniform3i, 3) MK_UNI_I(rh_glUniform4i, 4)
MK_UNI_FV(rh_glUniform1fv, 1) MK_UNI_FV(rh_glUniform2fv, 2) MK_UNI_FV(rh_glUniform3fv, 3) MK_UNI_FV(rh_glUniform4fv, 4)
MK_UNI_IV(rh_glUniform1iv, 1) MK_UNI_IV(rh_glUniform2iv, 2) MK_UNI_IV(rh_glUniform3iv, 3) MK_UNI_IV(rh_glUniform4iv, 4)

/* ---- misc ---- */

/* setfov(fov) — recorded; the renderer applies it at the next frame start */
static double rh_setfov(pd_Host *h, int n, const double *a)
{
    GLCmd *cmd = glcmd_push(h->glbuf);
    if (cmd) { cmd->op = GLCMD_SETFOV; cmd->a = n >= 1 ? a[0] : 0; }
    return n >= 1 ? a[0] : 0;
}

/* gluPerspective: projection mode + identity + perspective (original
 * kgluPerspective leaves the matrix mode set to PROJECTION) */
static double rh_gluPerspective(pd_Host *h, int n, const double *a)
{
    emit(h, GLCMD_MATRIXMODE, 1, 0, 0, 0, 0);
    emit(h, GLCMD_LOADIDENTITY, 0, 0, 0, 0, 0);
    emit(h, GLCMD_PERSPECTIVE, 0, n >= 1 ? a[0] : 0, n >= 2 ? a[1] : 1,
         n >= 3 ? a[2] : 0.1, n >= 4 ? a[3] : 1000);
    return 0;
}

/* gluLookAt: compute the matrix here (host and renderer share the same
 * column-major layout) and hand it over as a MULTMATRIX command.
 * Faithful transcription of polydraw_src/polydraw.c:567 (args are
 * eye xyz, center xyz, up xyz; the matrix REPLACES the modelview, which is
 * identity each frame, so MULTMATRIX-after-identity == glLoadMatrix). */
static double rh_gluLookAt(pd_Host *h, int n, const double *a)
{
    if (n < 9) return 0;
    double px = a[0], py = a[1], pz = a[2];   /* eye    */
    double cx = a[3], cy = a[4], cz = a[5];   /* center */
    double ux = a[6], uy = a[7], uz = a[8];   /* up     */
    double f[3], r[3], d[3], mat[16];

    f[0] = px - cx; f[1] = py - cy; f[2] = pz - cz;
    double t = 1.0 / sqrt(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
    f[0] *= t; f[1] *= t; f[2] *= t;

    r[0] = f[2]*uy - f[1]*uz;
    r[1] = f[0]*uz - f[2]*ux;
    r[2] = f[1]*ux - f[0]*uy;
    t = 1.0 / sqrt(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]);
    r[0] *= t; r[1] *= t; r[2] *= t;

    d[0] = f[1]*r[2] - f[2]*r[1];
    d[1] = f[2]*r[0] - f[0]*r[2];
    d[2] = f[0]*r[1] - f[1]*r[0];

    mat[0] = r[0]; mat[4] = r[1]; mat[ 8] = r[2];
    mat[12] = -(mat[0]*px + mat[4]*py + mat[ 8]*pz);
    mat[1] = d[0]; mat[5] = d[1]; mat[ 9] = d[2];
    mat[13] = -(mat[1]*px + mat[5]*py + mat[ 9]*pz);
    mat[2] = f[0]; mat[6] = f[1]; mat[10] = f[2];
    mat[14] = -(mat[2]*px + mat[6]*py + mat[10]*pz);
    mat[3] = 0.0; mat[7] = 0.0; mat[11] = 0.0; mat[15] = 1.0;

    memcpy(g_mat, mat, sizeof(mat));
    GLCmd *cmd = glcmd_push(h->glbuf);
    if (cmd) { cmd->op = GLCMD_MULTMATRIX; cmd->s = (const char*)g_mat; }
    return 0;
}

/* glMultMatrix(&m) — multiply the current matrix by the script's array */
static double rh_glMultMatrix(pd_Host *h, int n, const double *a)
{
    double *m = ptr_arg(a, 0);
    GLCmd *cmd = glcmd_push(h->glbuf);
    if (cmd) { cmd->op = GLCMD_MULTMATRIX; cmd->s = (const char*)m; }
    return 0;
}

/* timing + ARB program params: no-ops (glklock* is host timing; the
 * glProgram*Param calls only affect ARB assembly shaders, unsupported) */
static double rh_noop(pd_Host *h, int n, const double *a) { (void)h;(void)n; (void)a; return 0; }
static double rh_noop0(pd_Host *h, int n, const double *a) { (void)h;(void)n; (void)a; return 0; }

/* ---- GL constants registered as host vars ---- */
static double c_GL_TEXTURE0[8];
static double c_GL_TEXTURE_1D = 0x0DE0, c_GL_TEXTURE_2D = 0x0DE1;
static double c_GL_TEXTURE_3D = 0x806F, c_GL_TEXTURE_CUBE_MAP = 0x8513;
static double c_KGL[12];

void pd_polyhost_install_tex(pd_Host *h, GLCmdBuf *glbuf,
                             const pdrl_Block *blocks, int nblocks)
{
    h->glbuf = glbuf;
    g_blocks = blocks;
    g_nblocks = nblocks;
    memset(pd_tex, 0, sizeof(pd_tex));
    g_nlocs = 0;

    /* textures */
    pd_host_add_fn(h, "GLSETTEX(,$)",       rh_glSetTexFile, 0);
    pd_host_add_fn(h, "GLSETTEX(,$,)",      rh_glSetTexFile, 0);
    pd_host_add_fn(h, "GLSETTEX(,&,,)",     rh_glSetTexArray, 0);
    pd_host_add_fn(h, "GLSETTEX(,&,,,)",    rh_glSetTexArray, 0);
    pd_host_add_fn(h, "GLSETTEX(,&,,,,)",   rh_glSetTexArray, 0);
    pd_host_add_fn(h, "GLGETTEX(,&,,,)",    rh_glGetTexArray, 0);
    pd_host_add_fn(h, "GLBINDTEXTURE()",    rh_glBindTex, 0);
    pd_host_add_fn(h, "GLACTIVETEXTURE()",  rh_glActiveTex, 0);
    pd_host_add_fn(h, "GLCAPTURE()",        rh_glCapture, 0);
    pd_host_add_fn(h, "GLCAPTURE(,,,)",     rh_glCapture, 0);
    pd_host_add_fn(h, "GLCAPTUREEND()",     rh_glEndCapture, 0);

    /* shaders & uniforms */
    pd_host_add_fn(h, "GLSETSHADER()",      rh_glSetShader, 0);
    pd_host_add_fn(h, "GLSETSHADER($,$)",   rh_glSetShader, 0);
    pd_host_add_fn(h, "GLSETSHADER($,$,$)", rh_glSetShader, 0);
    pd_host_add_fn(h, "GLGETUNIFORMLOC($)", rh_glGetUniformLoc, 0);
    pd_host_add_fn(h, "GLUNIFORM1F(,)",     rh_glUniform1f, 0);
    pd_host_add_fn(h, "GLUNIFORM2F(,,)",    rh_glUniform2f, 0);
    pd_host_add_fn(h, "GLUNIFORM3F(,,,)",   rh_glUniform3f, 0);
    pd_host_add_fn(h, "GLUNIFORM4F(,,,,)",  rh_glUniform4f, 0);
    pd_host_add_fn(h, "GLUNIFORM1I(,)",     rh_glUniform1i, 0);
    pd_host_add_fn(h, "GLUNIFORM2I(,,)",    rh_glUniform2i, 0);
    pd_host_add_fn(h, "GLUNIFORM3I(,,,)",   rh_glUniform3i, 0);
    pd_host_add_fn(h, "GLUNIFORM4I(,,,,)",  rh_glUniform4i, 0);
    pd_host_add_fn(h, "GLUNIFORM1FV(,,&)",  rh_glUniform1fv, 0);
    pd_host_add_fn(h, "GLUNIFORM2FV(,,&)",  rh_glUniform2fv, 0);
    pd_host_add_fn(h, "GLUNIFORM3FV(,,&)",  rh_glUniform3fv, 0);
    pd_host_add_fn(h, "GLUNIFORM4FV(,,&)",  rh_glUniform4fv, 0);
    pd_host_add_fn(h, "GLUNIFORM1IV(,,&)",  rh_glUniform1iv, 0);
    pd_host_add_fn(h, "GLUNIFORM2IV(,,&)",  rh_glUniform2iv, 0);
    pd_host_add_fn(h, "GLUNIFORM3IV(,,&)",  rh_glUniform3iv, 0);
    pd_host_add_fn(h, "GLUNIFORM4IV(,,&)",  rh_glUniform4iv, 0);

    /* matrix / misc */
    pd_host_add_fn(h, "GLUPERSPECTIVE(,,,)", rh_gluPerspective, 0);
    pd_host_add_fn(h, "GLULOOKAT(,,,,,,,,)", rh_gluLookAt, 0);
    pd_host_add_fn(h, "GLMULTMATRIX(&)",     rh_glMultMatrix, 0);
    pd_host_add_fn(h, "SETFOV()",            rh_setfov, 0);
    pd_host_add_fn(h, "GLKLOCKSTART()",      rh_noop, 0);
    pd_host_add_fn(h, "GLKLOCKELAPSED()",    rh_noop0, 0);
    pd_host_add_fn(h, "GLPROGRAMLOCALPARAM(,,,,)", rh_noop0, 0);
    pd_host_add_fn(h, "GLPROGRAMENVPARAM(,,,,)",   rh_noop0, 0);

    /* GL_TEXTURE0..7, GL_TEXTURE_1D/2D/3D/CUBE */
    for (int i = 0; i < 8; i++) c_GL_TEXTURE0[i] = 0x84C0 + i;
    pd_host_add_var(h, "GL_TEXTURE0", &c_GL_TEXTURE0[0]);
    pd_host_add_var(h, "GL_TEXTURE1", &c_GL_TEXTURE0[1]);
    pd_host_add_var(h, "GL_TEXTURE2", &c_GL_TEXTURE0[2]);
    pd_host_add_var(h, "GL_TEXTURE3", &c_GL_TEXTURE0[3]);
    pd_host_add_var(h, "GL_TEXTURE4", &c_GL_TEXTURE0[4]);
    pd_host_add_var(h, "GL_TEXTURE5", &c_GL_TEXTURE0[5]);
    pd_host_add_var(h, "GL_TEXTURE6", &c_GL_TEXTURE0[6]);
    pd_host_add_var(h, "GL_TEXTURE7", &c_GL_TEXTURE0[7]);
    pd_host_add_var(h, "GL_TEXTURE_1D", &c_GL_TEXTURE_1D);
    pd_host_add_var(h, "GL_TEXTURE_2D", &c_GL_TEXTURE_2D);
    pd_host_add_var(h, "GL_TEXTURE_3D", &c_GL_TEXTURE_3D);
    pd_host_add_var(h, "GL_TEXTURE_CUBE_MAP", &c_GL_TEXTURE_CUBE_MAP);

    /* KGL_* colmode constants */
    static const char *knames[] = {
        "KGL_BGRA32", "KGL_CHAR", "KGL_SHORT", "KGL_INT", "KGL_FLOAT", "KGL_VEC4",
        "KGL_LINEAR", "KGL_NEAREST", "KGL_MIPMAP", "KGL_MIPMAP2",
        "KGL_MIPMAP1", "KGL_MIPMAP0",
    };
    static const double kvals[] = {
        KGL_BGRA32, KGL_CHAR, KGL_SHORT, KGL_INT, KGL_FLOAT, KGL_VEC4,
        KGL_LINEAR, KGL_NEAREST, KGL_MIPMAP, KGL_MIPMAP2,
        KGL_MIPMAP1, KGL_MIPMAP0,
    };
    for (int i = 0; i < 12; i++) {
        c_KGL[i] = kvals[i];
        pd_host_add_var(h, knames[i], &c_KGL[i]);
    }
    c_KGL[0] = KGL_REPEAT;
    pd_host_add_var(h, "KGL_REPEAT", &c_KGL[0]);
    c_KGL[1] = KGL_MIRRORED_REPEAT;
    pd_host_add_var(h, "KGL_MIRRORED_REPEAT", &c_KGL[1]);
    c_KGL[2] = KGL_CLAMP;
    pd_host_add_var(h, "KGL_CLAMP", &c_KGL[2]);
    c_KGL[3] = KGL_CLAMP_TO_EDGE;
    pd_host_add_var(h, "KGL_CLAMP_TO_EDGE", &c_KGL[3]);
}

void pd_polyhost_set_blocks(const pdrl_Block *blocks, int nblocks) {
    g_blocks = blocks;
    g_nblocks = nblocks;
}

void pd_tex_free_all(void)
{
    for (int i = 0; i < PD_MAX_TEX; i++) {
        free(pd_tex[i].pixels);
        pd_tex[i].pixels = NULL;
        pd_tex[i].valid = 0;
    }
}
