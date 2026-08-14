/* gl_renderer.c — replay a GLCmd stream through a real GL core-profile
 * context. See gl_renderer.h for the design notes. */
#include "gl_renderer.h"
#include "gl_offscreen.h"
#include "pd_polyhost_tex.h"   /* pd_polyhost_get_locname */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#elif defined(__linux__)
#include "glad/glad.h"
#endif

extern int stbi_write_png(const char *filename, int w, int h, int comp, const void *data, int stride_in_bytes);

/* ------------------------------------------------------------------ */
/* 4x4 column-major matrix helpers (double; matches the numpy reference) */

/* NOTE on matrix layout: every matrix here is a standard OpenGL
 * column-major array (c[col*4+row] = M[row][col]), matching the numpy
 * reference's clip math (clip = proj * modelview * v). The old code
 * stored translate/rotate transposed (matching nothing in the reference)
 * which broke every script using glTranslate/glRotate; verified correct
 * against golden renders below. */

static void mat4_identity(double m[16])
{
    memset(m, 0, 16 * sizeof(double));
    m[0] = m[5] = m[10] = m[15] = 1.0;
}

static void mat4_mul(double out[16], const double a[16], const double b[16])
{
    double r[16];
    for (int c = 0; c < 4; c++) {
        for (int rw = 0; rw < 4; rw++) {
            double s = 0;
            for (int k = 0; k < 4; k++)
                s += a[k * 4 + rw] * b[c * 4 + k];
            r[c * 4 + rw] = s;
        }
    }
    memcpy(out, r, sizeof(r));
}

/* gluPerspective, standard layout: c[col*4+row] = M[row][col], so the w
 * coefficient M[3][2] = 2fn/(n-f) lands at index 2*4+3 = 11 and the z
 * coefficient M[2][3] = -1 at index 3*4+2 = 14. (Matches the numpy
 * reference, which stores the same matrix row-major: m[3,2] and m[2,3].) */
static void mat4_perspective(double m[16], double fovy_deg, double aspect,
                             double near, double far)
{
    double f = 1.0 / tan(fovy_deg * M_PI / 360.0);
    memset(m, 0, 16 * sizeof(double));
    m[0]  = f / aspect;
    m[5]  = f;
    m[10] = (far + near) / (near - far);
    m[11] = -1.0;                             /* w coefficient, M[3][2] = -z_eye */
    m[14] = (2 * far * near) / (near - far); /* z coefficient, M[2][3] */
}

/* glOrtho with near=-1 far=1 default, standard layout: the translation
 * lands in column 3 (indices 12..14) and w' = w. */
static void mat4_ortho(double m[16], double l, double r, double b, double t,
                       double n, double f)
{
    memset(m, 0, 16 * sizeof(double));
    m[0] = 2.0 / (r - l);
    m[5] = 2.0 / (t - b);
    m[10] = -2.0 / (f - n);
    m[12] = -(r + l) / (r - l);
    m[13] = -(t + b) / (t - b);
    m[14] = -(f + n) / (f - n);
    m[15] = 1.0;
}

static void mat4_translate(double m[16], double x, double y, double z)
{
    double t[16]; mat4_identity(t);
    t[12] = x; t[13] = y; t[14] = z;
    double r[16]; mat4_mul(r, m, t); memcpy(m, r, sizeof(r));
}

static void mat4_rotate(double m[16], double ang_deg, double x, double y, double z)
{
    double a = ang_deg * M_PI / 180.0;
    double c = cos(a), s = sin(a);
    double len = sqrt(x * x + y * y + z * z);
    if (len == 0) return;
    x /= len; y /= len; z /= len;
    double k = 1 - c;
    /* standard column-major axis-angle rotation matrix */
    double t[16] = {
        c + x * x * k,      x * y * k - z * s, x * z * k + y * s, 0,
        y * x * k + z * s,  c + y * y * k,     y * z * k - x * s, 0,
        z * x * k - y * s,  z * y * k + x * s, c + z * z * k,     0,
        0, 0, 0, 1
    };
    double r[16]; mat4_mul(r, m, t); memcpy(m, r, sizeof(r));
}

static void mat4_scale(double m[16], double x, double y, double z)
{
    double t[16]; mat4_identity(t);
    t[0] = x; t[5] = y; t[10] = z;
    double r[16]; mat4_mul(r, m, t); memcpy(m, r, sizeof(r));
}

/* gluLookAt, standard column-major layout (multiplied onto the modelview). */
static const char *mvp_to_str(const double m[16]) {
    static char b[256];
    snprintf(b, sizeof(b), "{%.3f %.3f %.3f %.3f; %.3f %.3f %.3f %.3f; %.3f %.3f %.3f %.3f; %.3f %.3f %.3f %.3f}",
             m[0], m[4], m[8], m[12], m[1], m[5], m[9], m[13],
             m[2], m[6], m[10], m[14], m[3], m[7], m[11], m[15]);
    return b;
}
static void mat4_lookat(double out[16], double ex, double ey, double ez,
                        double cx, double cy, double cz,
                        double ux, double uy, double uz)
{
    double fx = cx - ex, fy = cy - ey, fz = cz - ez;
    double fl = sqrt(fx * fx + fy * fy + fz * fz);
    if (fl != 0) { fx /= fl; fy /= fl; fz /= fl; }
    /* side = f × up */
    double sx = fy * uz - fz * uy, sy = fz * ux - fx * uz, sz = fx * uy - fy * ux;
    double sl = sqrt(sx * sx + sy * sy + sz * sz);
    if (sl != 0) { sx /= sl; sy /= sl; sz /= sl; }
    /* up' = side × f */
    double ux2 = sy * fz - sz * fy, uy2 = sz * fx - sx * fz, uz2 = sx * fy - sy * fx;
    double m[16] = {
        sx, ux2, -fx, 0,
        sy, uy2, -fy, 0,
        sz, uz2, -fz, 0,
        0, 0, 0, 1
    };
    mat4_translate(m, -ex, -ey, -ez);
    memcpy(out, m, sizeof(m));
}

/* ------------------------------------------------------------------ */
/* GLSL adaptation: legacy (GL 2.x) shader → GLSL 330 core             */
/* gl_Vertex & friends become plain attributes (gl_* is reserved for   */
/* builtins in 330 core); the VBO layout maps to fixed locations.      */

static const char *VERT_PREFIX =
    "#version 330 core\n"
    "uniform mat4 u_mvp;\n"
    "layout(location=0) in vec4 a_vertex;\n"
    "layout(location=1) in vec4 a_color;\n"
    "layout(location=2) in vec4 a_texcoord;\n"
    "layout(location=3) in vec3 a_normal;\n";

static const char *FRAG_PREFIX =
    "#version 330 core\n"
    "out vec4 fragColor;\n";

/* replace all occurrences of `from` with `to` in a malloc'd copy */
static char *str_replace_all(const char *src, const char *from, const char *to)
{
    size_t fl = strlen(from), tl = strlen(to), sl = strlen(src);
    if (fl == 0) return strdup(src);
    size_t count = 0;
    const char *p = src;
    while ((p = strstr(p, from)) != NULL) { count++; p += fl; }
    char *out = malloc(sl + count * (tl - fl) + 1);
    if (!out) return NULL;
    char *w = out; p = src;
    while (count-- > 0) {
        const char *hit = strstr(p, from);
        size_t pre = (size_t)(hit - p);
        memcpy(w, p, pre); w += pre;
        memcpy(w, to, tl); w += tl;
        p = hit + fl;
    }
    strcpy(w, p);
    return out;
}

/* adapt legacy GLSL for the vertex stage:
 *   ftransform()   → (u_mvp * a_vertex)
 *   gl_Vertex etc  → a_vertex etc (gl_* is reserved in 330 core) */
static char *adapt_vertex(const char *src)
{
    char *s = str_replace_all(src, "varying", "out");
    if (!s) return NULL;
    char *t = str_replace_all(s, "ftransform()", "(u_mvp * a_vertex)");
    free(s);
    if (!t) return NULL;
    s = str_replace_all(t, "gl_Vertex", "a_vertex"); free(t);
    if (!s) return NULL;
    t = str_replace_all(s, "gl_Color", "a_color"); free(s);
    if (!t) return NULL;
    s = str_replace_all(t, "gl_MultiTexCoord0", "a_texcoord"); free(t);
    if (!s) return NULL;
    t = str_replace_all(s, "gl_Normal", "a_normal"); free(s);
    if (!t) return NULL;
    size_t len = strlen(t);
    char *out = malloc(strlen(VERT_PREFIX) + len + 1);
    if (!out) { free(t); return NULL; }
    strcpy(out, VERT_PREFIX);
    strcat(out, t);
    free(t);
    return out;
}

/* adapt legacy GLSL for the fragment stage */
static char *adapt_fragment(const char *src)
{
    char *s = str_replace_all(src, "varying", "in");
    if (!s) return NULL;
    char *t = str_replace_all(s, "gl_FragColor", "fragColor");
    free(s);
    if (!t) return NULL;
    char *u = str_replace_all(t, "texture2D", "texture");
    free(t);
    if (!u) return NULL;
    /* textureCube(lod) -> texture (GLSL 330 core: texture() dispatches on
     * the sampler type; samplerCube needs no lod variant here) */
    char *v = str_replace_all(u, "textureCube", "texture");
    free(u);
    if (!v) return NULL;
    size_t len = strlen(v);
    char *out = malloc(strlen(FRAG_PREFIX) + len + 1);
    if (!out) { free(v); return NULL; }
    strcpy(out, FRAG_PREFIX);
    strcat(out, v);
    free(v);
    return out;
}

/* ------------------------------------------------------------------ */

#define MAT_STACK_DEPTH 32

/* one captured vertex (interleaved as it goes into the VBO) */
typedef struct {
    float pos[3], color[4], tex[4], nrm[3];   /* 14 floats */
} GVertex;

struct pd_GLRenderer {
    int w, h;
    int fb_w, fb_h;           /* actual drawable pixels (Retina: 2x logical).
                                 Used for glViewport in render_to_default mode. */
    double fovy;
    int owns_ctx;            /* 1: created+owns an offscreen GL context (and
                               must destroy it); 0: caller owns the current
                               GL context (e.g. a GLFW window) */
    pd_GLOSCtx *osc;         /* offscreen context handle when owns_ctx==1 */
    int render_to_default;   /* 1: replay directly into the window's default
                               framebuffer (0) instead of the offscreen FBO —
                               used by the interactive viewer so no blit is
                               needed; 0 (default): replay into rd->fbo */

    GLuint program;
    GLuint u_mvp;
    GLuint vao, vbo;
    GLuint fbo, rbo_color, rbo_depth;

    /* immediate-mode state */
    double cur_color[4], cur_tex[4], cur_nrm[3];
    int mode;                 /* primitive type of the open glBegin, or -1 */
    GVertex *verts; size_t nverts, cap;

    /* matrices: stack of modelview; separate projection */
    double mvm_stack[MAT_STACK_DEPTH][16];
    int mvm_top;
    double proj[16];
    double mvp[16];
    float  mvp_f[16];         /* float copy for glUniformMatrix4fv */
    int matrix_mode;          /* 0 modelview, 1 projection */

    /* tessellation scratch buffer (per-renderer) */
    GVertex *tris; size_t ntri, captri;

    /* ---- draw-call batching ------------------------------------------
     * end_primitive() used to do a glBufferData + glDrawArrays for every
     * single glEnd(), which for scripts that emit thousands of tiny
     * primitives per frame (ken/balls.pss: 16k quads) meant 16k draw
     * calls/frame and made the GL replay the dominant cost.
     *
     * Instead tessellated triangles accumulate into `batch` and are
     * flushed with ONE draw call, either when a piece of state that the
     * draw depends on actually changes (MVP, blend/depth toggles,
     * program, texture binding, render target, ...) or at end of frame.
     * The MVP is folded into the vertices on the CPU at append time so a
     * matrix change alone does not have to break the batch. */
    GVertex *batch; size_t nbatch, batch_cap;
    int batch_prim;           /* GL_TRIANGLES / GL_LINES, or -1 when empty */
    float batch_mvp_f[16];    /* MVP the pending batch was built under */
    size_t stat_draws;        /* draw calls issued this frame (diagnostics) */

    /* getenv() was being called several times per primitive in the replay
     * hot path; resolve the debug switches once at creation instead */
    int dbg_gl;

    /* mvp_bake: when 1, the active vertex shader computes gl_Position purely
     * from ftransform() (u_mvp * a_vertex) and never references gl_Vertex as an
     * object-space value (e.g. for lighting/procedural UVs). In that case the
     * MVP can be folded into the vertex positions on the CPU, letting geometry
     * emitted under *different* MVP matrices share a single batched draw call
     * (the shader sees clip-space coords and a fixed identity MVP). This collapses
     * thousands of per-object-matrix draws (tree.pss: 3644, ...) into a few.
     * When 0, the legacy behaviour is kept: vertices stay in object space and a
     * matrix change flushes the batch (custom shaders may depend on gl_Vertex). */
    int mvp_bake;

    /* GL state mirrors */
    int depth_test_enabled;
    int blend_enabled;

    /* shader uniform-name table (filled by GLCMD_UNIFORMLOC, resolved
     * lazily against the current program on GLCMD_UNIFORM) */
#define REN_MAX_LOCS 64
    char *u_name[REN_MAX_LOCS];

    /* texture objects (unit 0..3) and the active texture unit */
#define REN_MAX_TEX 32
    GLuint tex_obj[REN_MAX_TEX];
    GLenum tex_tgt[REN_MAX_TEX];        /* GL_TEXTURE_2D / _CUBE_MAP */
    int active_unit;

    /* capture-to-texture scratch (FBO with a texture colour attachment) */
    GLuint cap_fbo, cap_tex;
    int cap_owned;       /* we created cap_fbo/cap_tex and must free them */
    int cap_texno;       /* target texture index for the in-flight capture */
    int cap_w, cap_h;
};

/* interleaved VBO layout offsets (floats) */
#define OFF_POS 0
#define OFF_COLOR 3
#define OFF_TEX 7
#define OFF_NRM 11
#define VFLOATS 14

static GLuint compile_shader(GLenum type, const char *src, const char *stage)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        fprintf(stderr, "gl_renderer: %s shader compile failed:\n%s\n", stage, log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint link_program(const char *vert_src, const char *frag_src)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vert_src, "vertex");
    if (!vs) return 0;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_src, "fragment");
    if (!fs) { glDeleteShader(vs); return 0; }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "gl_renderer: program link failed:\n%s\n", log);
        glDeleteProgram(prog);
        glDeleteShader(vs); glDeleteShader(fs);
        return 0;
    }
    glDeleteShader(vs); glDeleteShader(fs);
    return prog;
}

/* Public wrapper around the internal link_program, for callers (e.g. the
 * window viewer) that need their own tiny GL programs in the display context.
 * Returns a linked GL program object, or 0 on failure. */
GLuint pd_gl_link_program(const char *vert_src, const char *frag_src)
{
    return link_program(vert_src, frag_src);
}

static void bind_attributes(void)
{
    glEnableVertexAttribArray(0);  /* pos */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, VFLOATS * sizeof(float),
                          (void *)(OFF_POS * sizeof(float)));
    glEnableVertexAttribArray(1);  /* color */
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, VFLOATS * sizeof(float),
                          (void *)(OFF_COLOR * sizeof(float)));
    glEnableVertexAttribArray(2);  /* texcoord */
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, VFLOATS * sizeof(float),
                          (void *)(OFF_TEX * sizeof(float)));
    glEnableVertexAttribArray(3);  /* normal */
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, VFLOATS * sizeof(float),
                          (void *)(OFF_NRM * sizeof(float)));
}

/* push one vertex with the current sticky attrib state */
static void push_vertex(pd_GLRenderer *rd, double x, double y, double z, double w)
{
    (void)w;
    if (rd->nverts == rd->cap) {
        rd->cap = rd->cap ? rd->cap * 2 : 1024;
        rd->verts = realloc(rd->verts, rd->cap * sizeof(GVertex));
        if (!rd->verts) { rd->cap = rd->nverts = 0; return; }
    }
    GVertex *v = &rd->verts[rd->nverts++];
    v->pos[0] = (float)x; v->pos[1] = (float)y; v->pos[2] = (float)z;
    for (int i = 0; i < 4; i++) {
        v->color[i] = (float)rd->cur_color[i];
        v->tex[i] = (float)rd->cur_tex[i];
    }
    for (int i = 0; i < 3; i++) v->nrm[i] = (float)rd->cur_nrm[i];
}

/* convert the captured primitive into triangles, appending to `out`
 * (realloc'd as needed) and returning the new triangle count */
static GVertex *tessellate(GVertex *out, size_t *n_out, size_t *cap_out,
                           int mode, const GVertex *v, size_t nv)
{
    size_t nt = 0;
    switch (mode) {
    case PDGL_TRIANGLES:       nt = nv / 3 * 3; break;
    case PDGL_TRIANGLE_FAN:
    case PDGL_POLYGON:         nt = (nv >= 3) ? (nv - 2) * 3 : 0; break;
    case PDGL_TRIANGLE_STRIP:  nt = (nv >= 3) ? (nv - 2) * 3 : 0; break;
    case PDGL_QUADS:           nt = nv / 4 * 6; break;
    case PDGL_QUAD_STRIP:      nt = (nv >= 4) ? (nv / 2 - 1) * 6 : 0; break;
    default: return out;       /* points/lines: skipped (matches reference) */
    }
    if (nt == 0) return out;
    size_t need = *n_out + nt;
    if (need > *cap_out) {
        size_t nc = *cap_out ? *cap_out : 4096;
        while (nc < need) nc *= 2;
        out = realloc(out, nc * sizeof(GVertex));
        *cap_out = nc;
    }
    GVertex *w = out + *n_out;
    switch (mode) {
    case PDGL_TRIANGLES:
        memcpy(w, v, nv / 3 * 3 * sizeof(GVertex));
        break;
    case PDGL_TRIANGLE_FAN:
    case PDGL_POLYGON:
        for (size_t k = 1; k + 1 < nv; k++) {
            w[0] = v[0]; w[1] = v[k]; w[2] = v[k + 1];
            w += 3;
        }
        break;
    case PDGL_TRIANGLE_STRIP:
        for (size_t k = 0; k + 2 < nv; k++) {
            w[0] = v[k]; w[1] = v[k + 1]; w[2] = v[k + 2];
            w += 3;
        }
        break;
    case PDGL_QUADS:
        for (size_t k = 0; k + 3 < nv; k += 4) {
            w[0] = v[k]; w[1] = v[k + 1]; w[2] = v[k + 2];
            w[3] = v[k]; w[4] = v[k + 2]; w[5] = v[k + 3];
            w += 6;
        }
        break;
    case PDGL_QUAD_STRIP:
        for (size_t k = 0; k + 3 < nv; k += 2) {
            w[0] = v[k]; w[1] = v[k + 1]; w[2] = v[k + 3];
            w[3] = v[k]; w[4] = v[k + 3]; w[5] = v[k + 2];
            w += 6;
        }
        break;
    }
    *n_out += nt;
    return out;
}

/* recompute mvp from proj*modelview and keep a float copy for the GPU */
static void update_mvp(pd_GLRenderer *rd)
{
    mat4_mul(rd->mvp, rd->proj, rd->mvm_stack[rd->mvm_top]);
    for (int i = 0; i < 16; i++)
        rd->mvp_f[i] = (float)rd->mvp[i];
}

/* Issue the accumulated batch as a single draw call.
 *
 * The batch is built under a fixed MVP (batch_mvp): vertex positions stay in
 * OBJECT space and the matrix is still passed as the u_mvp uniform, because
 * custom shaders are free to use a_vertex/gl_Vertex as an object-space value
 * (lighting, procedural texturing). Pre-transforming on the CPU would change
 * what those shaders see, so instead a matrix change simply ends the batch. */
static void flush_batch(pd_GLRenderer *rd)
{
    if (rd->nbatch == 0) return;
    size_t n = rd->nbatch;
    rd->nbatch = 0;              /* clear first: errors must not re-enter */

    glUseProgram(rd->program);
    /* Some scripts (e.g. gears/funky post-process) draw a quad that samples
     * a texture left bound by glcaptureend without an explicit glbindtexture;
     * the replay only issues BINDTEX when the host calls it. Make sure the
     * texture for the active unit is actually bound before drawing so the
     * sampler sees it (otherwise the quad samples an unbound/zero texture). */
    if (rd->tex_obj[rd->active_unit]) {
        glActiveTexture(GL_TEXTURE0 + rd->active_unit);
        glBindTexture(rd->tex_tgt[rd->active_unit], rd->tex_obj[rd->active_unit]);
    }
    glBindBuffer(GL_ARRAY_BUFFER, rd->vbo);
    /* orphan-then-fill: lets the driver hand us a fresh block instead of
     * stalling until the previous draw from this VBO has retired */
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(n * sizeof(GVertex)), NULL,
                 GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(n * sizeof(GVertex)),
                    rd->batch);
    glBindVertexArray(rd->vao);
    /* In mvp_bake mode the vertex positions are already in clip space (the MVP
     * was folded into each vertex on the CPU in batch_append), so the shader
     * must see an identity matrix. Otherwise pass the batch's MVP. */
    if (rd->mvp_bake) {
        static const float I[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        glUniformMatrix4fv(rd->u_mvp, 1, GL_FALSE, I);
    } else {
        glUniformMatrix4fv(rd->u_mvp, 1, GL_FALSE, rd->batch_mvp_f);
    }
    glDrawArrays((GLenum)rd->batch_prim, 0, (GLsizei)n);
    rd->stat_draws++;
    rd->batch_prim = -1;
}

size_t pd_gl_renderer_draw_calls(const pd_GLRenderer *rd)
{
    return rd ? rd->stat_draws : 0;
}

/* Append `n` vertices to the batch under primitive type `prim`.
 * Flushes first if the primitive type differs (a draw call has one mode) or
 * if the MVP changed since the batch was started.
 *
 * In mvp_bake mode the MVP is instead folded into every vertex position on the
 * CPU, so a matrix change does NOT flush; all geometry emitted under differing
 * MVPs collapses into the same batch (the shader receives an identity matrix).
 * This is what collapses tree.pss / similar scenes from thousands of draw calls
 * to a handful. */
static void batch_append(pd_GLRenderer *rd, int prim, const GVertex *src, size_t n)
{
    if (n == 0) return;
    if (rd->batch_prim != prim) {
        flush_batch(rd);
        rd->batch_prim = prim;
    }
    if (!rd->mvp_bake &&
        memcmp(rd->batch_mvp_f, rd->mvp_f, sizeof(rd->mvp_f)) != 0) {
        flush_batch(rd);
        memcpy(rd->batch_mvp_f, rd->mvp_f, sizeof(rd->mvp_f));
    }
    if (rd->nbatch + n > rd->batch_cap) {
        size_t nc = rd->batch_cap ? rd->batch_cap * 2 : 4096;
        while (nc < rd->nbatch + n) nc *= 2;
        GVertex *nb = (GVertex *)realloc(rd->batch, nc * sizeof(GVertex));
        if (!nb) { flush_batch(rd); return; }
        rd->batch = nb; rd->batch_cap = nc;
    }
    if (rd->mvp_bake) {
        /* fold MVP into positions: out = mvp * pos. m is column-major. */
        const float *m = rd->mvp_f;
        GVertex *dst = rd->batch + rd->nbatch;
        for (size_t i = 0; i < n; i++) {
            const GVertex *s = &src[i];
            float x = s->pos[0], y = s->pos[1], z = s->pos[2];
            float o[4];
            o[0] = m[0]*x + m[4]*y + m[8] *z + m[12];
            o[1] = m[1]*x + m[5]*y + m[9] *z + m[13];
            o[2] = m[2]*x + m[6]*y + m[10]*z + m[14];
            o[3] = m[3]*x + m[7]*y + m[11]*z + m[15];
            dst[i] = *s;
            if (o[3] != 0.0f && o[3] != 1.0f) {
                dst[i].pos[0] = o[0] / o[3];
                dst[i].pos[1] = o[1] / o[3];
                dst[i].pos[2] = o[2] / o[3];
            } else {
                dst[i].pos[0] = o[0];
                dst[i].pos[1] = o[1];
                dst[i].pos[2] = o[2];
            }
        }
    } else {
        memcpy(rd->batch + rd->nbatch, src, n * sizeof(GVertex));
    }
    rd->nbatch += n;
}

/* end the open primitive: tessellate and append to the pending batch */
static void end_primitive(pd_GLRenderer *rd)
{
    if (rd->mode < 0 || rd->nverts == 0) {
        rd->mode = -1;
        rd->nverts = 0;
        return;
    }
    /* Lines/line-strips are drawn as real GL lines (not tessellated), to
     * match the reference. LINE_STRIP is expanded to independent LINES so
     * consecutive strips can share one batched draw call. */
    if (rd->mode == PDGL_LINES || rd->mode == PDGL_LINE_STRIP) {
        int strip = (rd->mode == PDGL_LINE_STRIP);
        size_t n = rd->nverts;
        rd->mode = -1;
        rd->nverts = 0;
        if (n == 0) return;
        if (!strip) {
            batch_append(rd, GL_LINES, rd->verts, n & ~(size_t)1);
        } else {
            for (size_t k = 0; k + 1 < n; k++)
                batch_append(rd, GL_LINES, rd->verts + k, 2);
        }
        return;
    }

    /* tessellate into the per-renderer scratch, then hand to the batch */
    rd->ntri = 0;
    rd->tris = tessellate(rd->tris, &rd->ntri, &rd->captri,
                          rd->mode, rd->verts, rd->nverts);
    rd->mode = -1;
    rd->nverts = 0;
    if (rd->ntri == 0) return;

    batch_append(rd, GL_TRIANGLES, rd->tris, rd->ntri);

    if (rd->dbg_gl) {
        GLint u0 = 0; glGetIntegerv(GL_ACTIVE_TEXTURE, &u0);
        GLint bound = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
        GLint vp[4] = {0,0,0,0}; glGetIntegerv(GL_VIEWPORT, vp);
        GLint fbd = 0; glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &fbd);
        fprintf(stderr, "DRAW ntri=%zu prog=%u active_unit=%d tex2d=%d vp=[%d,%d,%d,%d] fbo_att=%d mvp0=[%.4g %.4g %.4g %.4g]\n",
                rd->ntri, rd->program, u0 - GL_TEXTURE0, bound, vp[0], vp[1], vp[2], vp[3], fbd,
                rd->mvp[0], rd->mvp[1], rd->mvp[2], rd->mvp[3]);
        if (getenv("PD_DEBUG_FULL_MVP"))
            fprintf(stderr, "  MVP = [%s]\n", mvp_to_str(rd->mvp));
        if (getenv("PD_DEBUG_MODELVIEW")) {
            char bmv[256], bpr[256];
            snprintf(bmv, sizeof(bmv), "%s", mvp_to_str(rd->mvm_stack[rd->mvm_top]));
            snprintf(bpr, sizeof(bpr), "%s", mvp_to_str(rd->proj));
            fprintf(stderr, "  MV = [%s]\n  PROJ = [%s]\n", bmv, bpr);
        }
        if (getenv("PD_DEBUG_TRI"))
            fprintf(stderr, "  TRIS first8: p=[%.3f %.3f %.3f] c=[%.3f %.3f %.3f %.3f]\n",
                    rd->tris[0].pos[0], rd->tris[0].pos[1], rd->tris[0].pos[2],
                    rd->tris[0].color[0], rd->tris[0].color[1], rd->tris[0].color[2], rd->tris[0].color[3]);
        if (rd->ntri > 0)
            fprintf(stderr, "  v0=(%.3f,%.3f,%.3f) v1=(%.3f,%.3f,%.3f)\n",
                    rd->tris[0].pos[0], rd->tris[0].pos[1], rd->tris[0].pos[2],
                    rd->tris[1].pos[0], rd->tris[1].pos[1], rd->tris[1].pos[2]);
        if (getenv("PD_DEBUG_VERTS")) {
            fprintf(stderr, "  all captured verts (%zu):\n", rd->nverts);
            for (size_t q = 0; q < rd->nverts && q < 64; q++)
                fprintf(stderr, "    v%zu=(%.4f,%.4f,%.4f)\n", q,
                        rd->verts[q].pos[0], rd->verts[q].pos[1], rd->verts[q].pos[2]);
        }
    }
}

/* fullscreen quad (GLCMD_QUAD): emits 2 triangles in NDC space.
 * Must bypass the projection (it is defined in screen space), so we use an
 * identity matrix — matching the reference which does not project QUADs. */
static void draw_quad(pd_GLRenderer *rd)
{
    /* draws immediately with its own identity MVP: preserve draw order by
     * emitting any queued geometry first */
    flush_batch(rd);
    GVertex q[4];
    float qpos[4][3] = {{-1,-1,0},{1,-1,0},{1,1,0},{-1,1,0}};
    float qtex[4][4] = {{0,0,0,1},{1,0,0,1},{1,1,0,1},{0,1,0,1}};
    for (int i = 0; i < 4; i++) {
        memset(&q[i], 0, sizeof(GVertex));
        memcpy(q[i].pos, qpos[i], sizeof(qpos[i]));
        memcpy(q[i].tex, qtex[i], sizeof(qtex[i]));
        for (int k = 0; k < 4; k++) q[i].color[k] = (float)rd->cur_color[k];
        for (int k = 0; k < 3; k++) q[i].nrm[k] = (float)rd->cur_nrm[k];
    }
    GVertex tris[6];
    tris[0] = q[0]; tris[1] = q[1]; tris[2] = q[2];
    tris[3] = q[0]; tris[4] = q[2]; tris[5] = q[3];
    static const GLfloat ident_f[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    glUseProgram(rd->program);
    /* Bind the texture for the active unit (post-process quads sample the
     * capture texture left bound by glcaptureend; the replay issues BINDTEX
     * only when the host calls it, so bind defensively here). */
    if (rd->tex_obj[rd->active_unit]) {
        glActiveTexture(GL_TEXTURE0 + rd->active_unit);
        glBindTexture(rd->tex_tgt[rd->active_unit], rd->tex_obj[rd->active_unit]);
    }
    glBindBuffer(GL_ARRAY_BUFFER, rd->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tris), tris, GL_STREAM_DRAW);
    glBindVertexArray(rd->vao);
    glUniformMatrix4fv(rd->u_mvp, 1, GL_FALSE, ident_f);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

/* ------------------------------------------------------------------ */

pd_GLRenderer *pd_gl_renderer_create(int w, int h, double fovy)
{
    return pd_gl_renderer_create_ex(w, h, fovy, 1);
}

pd_GLRenderer *pd_gl_renderer_create_ex(int w, int h, double fovy, int own_offscreen)
{
    pd_GLOSCtx *osc = NULL;
    if (own_offscreen) {
        osc = pd_gl_offscreen_create();
        if (!osc) {
            fprintf(stderr, "gl_renderer: could not create offscreen context\n");
            return NULL;
        }
    }
    pd_GLRenderer *rd = calloc(1, sizeof(*rd));
    if (!rd) { if (osc) pd_gl_offscreen_destroy(osc); return NULL; }
    rd->w = w; rd->h = h; rd->fb_w = w; rd->fb_h = h; rd->fovy = fovy; rd->owns_ctx = own_offscreen ? 1 : 0;
    rd->osc = osc;
    rd->mode = -1;
    rd->batch_prim = -1;
    rd->mvp_bake = 0;
    rd->dbg_gl = getenv("PD_DEBUG_GL") ? 1 : 0;
    rd->mvm_top = 0;
    mat4_identity(rd->mvm_stack[0]);
    mat4_perspective(rd->proj, fovy, (double)w / h, 0.1, 1000.0);
    memcpy(rd->mvp, rd->proj, sizeof(rd->proj));
    rd->matrix_mode = 0;
    for (int i = 0; i < 4; i++) {
        rd->cur_color[i] = 1.0;
        rd->cur_tex[i] = (i == 3) ? 1.0 : 0.0;
    }
    rd->cur_nrm[2] = 1.0;

    /* FBO + renderbuffers */
    glGenFramebuffers(1, &rd->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rd->fbo);
    glGenRenderbuffers(1, &rd->rbo_color);
    glBindRenderbuffer(GL_RENDERBUFFER, rd->rbo_color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, rd->rbo_color);
    glGenRenderbuffers(1, &rd->rbo_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, rd->rbo_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, rd->rbo_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gl_renderer: FBO incomplete\n");
        pd_gl_renderer_destroy(rd);
        return NULL;
    }
    glViewport(0, 0, w, h);

    /* VBO + VAO */
    glGenBuffers(1, &rd->vbo);
    glGenVertexArrays(1, &rd->vao);
    glBindVertexArray(rd->vao);
    glBindBuffer(GL_ARRAY_BUFFER, rd->vbo);
    bind_attributes();
    glBindVertexArray(0);

    /* default passthrough program (color only) */
    static const char *PVERT =
        "#version 330 core\n"
        "uniform mat4 u_mvp;\n"
        "layout(location=0) in vec4 a_vertex;\n"
        "layout(location=1) in vec4 a_color;\n"
        "layout(location=2) in vec4 a_texcoord;\n"
        "layout(location=3) in vec3 a_normal;\n"
        "out vec4 c;\n"
        "void main() {\n"
        "   gl_Position = u_mvp * a_vertex;\n"
        "   c = a_color;\n"
        "}\n";
    static const char *PFRAG =
        "#version 330 core\n"
        "in vec4 c;\n"
        "out vec4 fragColor;\n"
        "void main() { fragColor = c; }\n";
    rd->program = link_program(PVERT, PFRAG);
    if (!rd->program) {
        fprintf(stderr, "gl_renderer: default program failed\n");
        pd_gl_renderer_destroy(rd);
        return NULL;
    }
    rd->u_mvp = glGetUniformLocation(rd->program, "u_mvp");

    /* alpha semantics identical to the reference: RGB overwrite (src=ONE,
     * dst=ZERO), alpha accumulates as max(src,dst) — that is blend
     * equation ADD for RGB and MAX for alpha */
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX);
    glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ONE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    return rd;
}

void pd_gl_renderer_set_shaders(pd_GLRenderer *rd,
                                const char *vert_src, const char *frag_src)
{
    if (!vert_src && !frag_src) return;
    if (getenv("PD_NO_CUSTOM"))
        fprintf(stderr, "PD_NO_CUSTOM: skip custom shader\n");
    if (getenv("PD_NO_CUSTOM")) return;
    char *va = vert_src ? adapt_vertex(vert_src) : NULL;
    char *fa = frag_src ? adapt_fragment(frag_src) : NULL;
    if (getenv("PD_DEBUG_SHADER")) {
        fprintf(stderr, "=== ADAPTED VERT {\n%s\n}\n", va ? va : "(null)");
        fprintf(stderr, "=== ADAPTED FRAG {\n%s\n}\n", fa ? fa : "(null)");
    }
    if (va && fa) {
        GLuint prog = link_program(va, fa);
        if (prog) {
            glDeleteProgram(rd->program);
            rd->program = prog;
            rd->u_mvp = glGetUniformLocation(rd->program, "u_mvp");
        }
    }
    /* Enable CPU MVP-baking only for shaders that never touch gl_Vertex as an
     * object-space value. Such shaders compute gl_Position purely via
     * ftransform() (adapted to u_mvp * a_vertex), so folding the MVP into the
     * vertex positions on the CPU is bit-exact. Shaders that reference
     * gl_Vertex for lighting/UVs must keep object-space vertices. */
    rd->mvp_bake = (vert_src && strstr(vert_src, "gl_Vertex") == NULL) ? 1 : 0;
    if (rd->mvp_bake && getenv("PD_NO_MVP_BAKE")) rd->mvp_bake = 0;
    free(va);
    free(fa);
}

void pd_gl_renderer_render(pd_GLRenderer *rd, const GLCmdBuf *buf)
{
    glBindFramebuffer(GL_FRAMEBUFFER, rd->render_to_default ? 0 : rd->fbo);
    /* In render_to_default mode the drawable is the window's real framebuffer,
     * which on a HiDPI display is larger than the logical w/h (e.g. 2x). Use
     * the actual pixel size so the image fills the window instead of sitting
     * in a quarter of it. For the offscreen FBO, fb_w==w. */
    if (rd->render_to_default) glViewport(0, 0, rd->fb_w, rd->fb_h);
    else                        glViewport(0, 0, rd->w, rd->h);
    if (rd->dbg_gl)
        fprintf(stderr, "RENDER buf=%zu cmds\n", buf->n);
    rd->stat_draws = 0;

    /* every frame starts from the reference host defaults (polydraw.c:3578):
     * projection = perspective(fovy, aspect, 0.1, 1000), modelview = identity */
    rd->matrix_mode = 0;
    rd->mvm_top = 0;
    mat4_identity(rd->mvm_stack[0]);
    mat4_perspective(rd->proj, rd->fovy, (double)rd->w / rd->h, 0.1, 1000.0);
    update_mvp(rd);

    /* matrices may change during replay; recompute mvp before each draw */
    update_mvp(rd);

    for (size_t i = 0; i < buf->n; i++) {
        const GLCmd *c = &buf->cmds[i];
        static const char *OPNAMES[] = {
            "CLEAR","BEGIN","END","VERTEX","COLOR","TEXCOORD","NORMAL",
            "PUSH","POP","TRANSLATE","ROTATE","SCALE","MATRIXMODE","LOADIDENTITY",
            "PERSPECTIVE","ORTHO","VIEWPORT","QUAD","ENABLE","DISABLE",
            "BLENDFUNC","CULLFACE","LINEWIDTH","SETTEXDATA","BINDTEX","ACTIVETEX",
            "CAPTURE","CAPTUREEND","SETFOV","SETSHADER","UNIFORMLOC","UNIFORM","MULTMATRIX"
        };
        if (rd->dbg_gl && c->op >= 0 && (size_t)c->op < sizeof(OPNAMES)/sizeof(OPNAMES[0]))
            fprintf(stderr, "  %s\n", OPNAMES[c->op]);
        switch (c->op) {
        case GLCMD_CLEAR: {
            flush_batch(rd);   /* pending geometry must land before the clear */
            double clr[4] = {c->a, c->b, c->c, c->d};
            GLbitfield bits = GL_COLOR_BUFFER_BIT;
            glClearColor((GLfloat)clr[0], (GLfloat)clr[1],
                         (GLfloat)clr[2], (GLfloat)clr[3]);
            if (rd->depth_test_enabled) bits |= GL_DEPTH_BUFFER_BIT;
            glClear(bits);
            break;
        }
        case GLCMD_BEGIN:
            rd->mode = c->mode;
            rd->nverts = 0;
            break;
        case GLCMD_END:
            end_primitive(rd);
            break;
        case GLCMD_VERTEX:
            push_vertex(rd, c->a, c->b, c->c, c->d);
            break;
        case GLCMD_COLOR:
            rd->cur_color[0] = c->a; rd->cur_color[1] = c->b;
            rd->cur_color[2] = c->c; rd->cur_color[3] = c->d;
            break;
        case GLCMD_TEXCOORD:
            rd->cur_tex[0] = c->a; rd->cur_tex[1] = c->b;
            rd->cur_tex[2] = c->c; rd->cur_tex[3] = c->d;
            break;
        case GLCMD_NORMAL:
            rd->cur_nrm[0] = c->a; rd->cur_nrm[1] = c->b; rd->cur_nrm[2] = c->c;
            break;
        case GLCMD_PUSHMATRIX:
            if (rd->mvm_top + 1 < MAT_STACK_DEPTH) {
                memcpy(rd->mvm_stack[rd->mvm_top + 1],
                       rd->mvm_stack[rd->mvm_top], 16 * sizeof(double));
                rd->mvm_top++;
            }
            update_mvp(rd);
            break;
        case GLCMD_POPMATRIX:
            if (rd->mvm_top > 0) rd->mvm_top--;
            update_mvp(rd);
            break;
        case GLCMD_TRANSLATE:
            if (rd->matrix_mode == 0)
                mat4_translate(rd->mvm_stack[rd->mvm_top], c->a, c->b, c->c);
            update_mvp(rd);
            if (getenv("PD_DEBUG_TRANSLATE"))
                fprintf(stderr, "  TRANSLATE(%.3f,%.3f,%.3f) mode=%d mvm_top=%d mvm0={%.3f %.3f %.3f %.3f} mvp0={%.3f %.3f %.3f %.3f}\n",
                        c->a, c->b, c->c, rd->matrix_mode, rd->mvm_top,
                        rd->mvm_stack[rd->mvm_top][0], rd->mvm_stack[rd->mvm_top][1],
                        rd->mvm_stack[rd->mvm_top][2], rd->mvm_stack[rd->mvm_top][3],
                        rd->mvp[0], rd->mvp[1], rd->mvp[2], rd->mvp[3]);
            break;
        case GLCMD_ROTATE:
            if (rd->matrix_mode == 0)
                mat4_rotate(rd->mvm_stack[rd->mvm_top], c->a, c->b, c->c, c->d);
            update_mvp(rd);
            if (getenv("PD_DEBUG_ROTATE"))
                fprintf(stderr, "  ROTATE(deg=%.3f, (%.3f,%.3f,%.3f)) mode=%d mvm_top=%d col0={%.3f %.3f %.3f %.3f}\n",
                        c->a, c->b, c->c, c->d, rd->matrix_mode, rd->mvm_top,
                        rd->mvm_stack[rd->mvm_top][0], rd->mvm_stack[rd->mvm_top][1],
                        rd->mvm_stack[rd->mvm_top][2], rd->mvm_stack[rd->mvm_top][3]);
            break;
        case GLCMD_SCALE:
            if (rd->matrix_mode == 0)
                mat4_scale(rd->mvm_stack[rd->mvm_top], c->a, c->b, c->c);
            update_mvp(rd);
            break;
        case GLCMD_MATRIXMODE:
            rd->matrix_mode = (c->mode == 1) ? 1 : 0;
            break;
        case GLCMD_LOADIDENTITY:
            if (rd->matrix_mode == 0)
                mat4_identity(rd->mvm_stack[rd->mvm_top]);
            else
                mat4_identity(rd->proj);
            update_mvp(rd);
            break;
        case GLCMD_PERSPECTIVE:
            if (rd->matrix_mode == 1)
                mat4_perspective(rd->proj, c->a, c->b, c->c, c->d);
            update_mvp(rd);
            break;
        case GLCMD_ORTHO:
            if (rd->matrix_mode == 1)
                mat4_ortho(rd->proj, c->a, c->b, c->c, c->d, -1, 1);
            update_mvp(rd);
            break;
        case GLCMD_VIEWPORT:
            break; /* renderer size fixed by the caller */
        case GLCMD_QUAD:
            draw_quad(rd);
            break;
        case GLCMD_ENABLE:
            if (c->mode == 0x0B71) { /* GL_DEPTH_TEST */
                if (!rd->depth_test_enabled) flush_batch(rd);
                glEnable(GL_DEPTH_TEST);
                rd->depth_test_enabled = 1;
            }
            break;
        case GLCMD_DISABLE:
            if (c->mode == 0x0B71) {
                if (rd->depth_test_enabled) flush_batch(rd);
                glDisable(GL_DEPTH_TEST);
                rd->depth_test_enabled = 0;
            }
            break;
        case GLCMD_BLENDFUNC: {
            int sf = (c->mode >> 16) & 0xFFFF;
            int df = c->mode & 0xFFFF;
            flush_batch(rd);
            glBlendFunc((GLenum)sf, (GLenum)df);
            break;
        }
        case GLCMD_CULLFACE:
            flush_batch(rd);
            glEnable(GL_CULL_FACE);
            glCullFace((GLenum)c->mode);
            break;
        case GLCMD_LINEWIDTH:
            flush_batch(rd);
            glLineWidth((GLfloat)c->a);
            break;

        /* ---- shaders, uniforms, matrices, textures, capture ---- */

        /* setfov(fov) — recompute the projection at the next frame start
         * (applied immediately here, since replay is per-frame). */
        case GLCMD_SETFOV:
            rd->fovy = c->a;
            mat4_perspective(rd->proj, rd->fovy, (double)rd->w / rd->h, 0.1, 1000.0);
            update_mvp(rd);
            break;

        /* glsetshader: replay the recorded GLSL source pointers (already
         * bit-cast into a/b by the host) through the adapt+link path. */
        case GLCMD_SETSHADER: {
            flush_batch(rd);   /* pending geometry belongs to the old program */
            const char *vs = (const char *)(uintptr_t)(unsigned long long)c->a;
            const char *fs = (const char *)(uintptr_t)(unsigned long long)c->b;
            pd_gl_renderer_set_shaders(rd, vs, fs);
            break;
        }

        /* glGetUniformLoc(name): remember the name under the returned id so
         * GLCMD_UNIFORM can resolve it against the (possibly new) program. */
        case GLCMD_UNIFORMLOC: {
            int id = (int)c->a;
            if (id >= 0 && id < REN_MAX_LOCS && c->s) {
                free(rd->u_name[id]);
                rd->u_name[id] = strdup(c->s);
            }
            break;
        }

        /* glUniformNf/Ni / glUniformNfv: resolve the uniform by name and set
         * it on the current program. */
        case GLCMD_UNIFORM: {
            int id = (int)c->a;
            int kind = (c->mode >> 24) & 0xFF;     /* 0 = float, 1 = int */
            int comps = (c->mode >> 16) & 0xFF;    /* components per vector */
            int nelem = c->mode & 0xFFFF;          /* element count */
            GLint loc = -1;
            /* name resolves from the persistent host loc table (survives the
             * per-frame buffer reset); fall back to the per-replay table. */
            const char *uname = pd_polyhost_get_locname(id);
            if (!uname && id >= 0 && id < REN_MAX_LOCS) uname = rd->u_name[id];
            if (uname)
                loc = glGetUniformLocation(rd->program, uname);
            if (getenv("PD_DEBUG_GL")) {
                const char *kid = (kind==0) ? "float" : "int";
                fprintf(stderr, "  UNIFORM %s comps=%d nelem=%d loc=%d ptr=%d\n",
                        uname?uname:"?", comps, nelem, (int)loc, c->s!=NULL);
                if (c->s && getenv("PD_DEBUG_LUT")) {
                    const float *fa = (const float*)c->s;
                    fprintf(stderr, "    [");
                    int npr = nelem * comps;
                    if (npr > 96) npr = 96;
                    for (int q = 0; q < npr; q++)
                        fprintf(stderr, "%s%g", q?"," : "", fa[q]);
                    fprintf(stderr, "]\n");
                }
            }
            if (loc < 0) break;
            /* geometry already queued was meant to be drawn with the OLD
             * uniform value — emit it before overwriting the uniform */
            flush_batch(rd);
            glUseProgram(rd->program);   /* uniforms target the current program */
            if (c->s == NULL) {
                /* scalar form: values packed in b,c,d (k>=3 shares d) */
                float fv[4]; int iv[4];
                for (int k = 0; k < nelem; k++) {
                    double v;
                    if (k == 0) v = c->b;
                    else if (k == 1) v = c->c;
                    else v = c->d;
                    fv[k] = (float)v; iv[k] = (int)v;
                }
                if (kind == 0) {
                    if (nelem == 1) glUniform1f(loc, fv[0]);
                    else if (nelem == 2) glUniform2f(loc, fv[0], fv[1]);
                    else if (nelem == 3) glUniform3f(loc, fv[0], fv[1], fv[2]);
                    else glUniform4f(loc, fv[0], fv[1], fv[2], fv[3]);
                } else {
                    if (nelem == 1) glUniform1i(loc, iv[0]);
                    else if (nelem == 2) glUniform2i(loc, iv[0], iv[1]);
                    else if (nelem == 3) glUniform3i(loc, iv[0], iv[1], iv[2]);
                    else glUniform4i(loc, iv[0], iv[1], iv[2], iv[3]);
                }
            } else {
                /* array (V) form: nelem vectors of `comps` components; upload
                 * with the matching glUniformNfv so the driver maps it onto
                 * the vecN[ ] uniform correctly. */
                const void *fa = c->s;
                if (kind == 0) {
                    if (comps == 1) glUniform1fv(loc, nelem, (const GLfloat*)fa);
                    else if (comps == 2) glUniform2fv(loc, nelem, (const GLfloat*)fa);
                    else if (comps == 3) glUniform3fv(loc, nelem, (const GLfloat*)fa);
                    else glUniform4fv(loc, nelem, (const GLfloat*)fa);
                } else {
                    if (comps == 1) glUniform1iv(loc, nelem, (const GLint*)fa);
                    else if (comps == 2) glUniform2iv(loc, nelem, (const GLint*)fa);
                    else if (comps == 3) glUniform3iv(loc, nelem, (const GLint*)fa);
                    else glUniform4iv(loc, nelem, (const GLint*)fa);
                }
            }
            break;
        }

        /* glMultMatrix(&m) / gluLookAt: multiply the current matrix (modelview
         * or projection per matrix_mode) by the supplied column-major 4x4. */
        case GLCMD_MULTMATRIX: {
            const double *m = (const double *)c->s;
            if (!m) break;
            if (getenv("PD_DEBUG_FULL_MVP"))
                fprintf(stderr, "  MULTMATRIX mode=%d\n    in=[%s]\n    pre=[%s]\n",
                        rd->matrix_mode, mvp_to_str(m), mvp_to_str(rd->mvm_stack[rd->mvm_top]));
            if (rd->matrix_mode == 0)
                mat4_mul(rd->mvm_stack[rd->mvm_top],
                         rd->mvm_stack[rd->mvm_top], m);
            else
                mat4_mul(rd->proj, rd->proj, m);
            if (getenv("PD_DEBUG_FULL_MVP"))
                fprintf(stderr, "    post=[%s]\n", mvp_to_str(rd->mvm_stack[rd->mvm_top]));
            update_mvp(rd);
            break;
        }

        /* glsettex: upload a BGRA32 double snapshot into a GL texture. */
        case GLCMD_SETTEXDATA: {
            flush_batch(rd);   /* re-uploading a texture changes what samples */
            int tex = (int)c->a;
            int w = (int)c->b, h = (int)c->c;
            if (getenv("PD_DEBUG_GL")) {
                fprintf(stderr, "SETTEXDATA tex=%d %dx%d mode=%d unit=%d\n",
                        tex, w, h, c->mode, rd->active_unit);
            }
            const double *px = (const double *)c->s;
            if (tex < 0 || tex >= REN_MAX_TEX || w < 1 || h < 1 || !px) break;
            if (!rd->tex_obj[tex]) glGenTextures(1, &rd->tex_obj[tex]);
            glActiveTexture(GL_TEXTURE0 + rd->active_unit);
            /* A vertical strip whose height is exactly 6× its width is a
             * cubemap (matches the reference: kglsettex detects xs*6==ys and
             * switches to GL_TEXTURE_CUBE_MAP). Otherwise plain 2D. */
            int cube = (w * 6 == h);
            GLenum target = cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
            rd->tex_tgt[tex] = target;
            glBindTexture(target, rd->tex_obj[tex]);
            /* packed 0xAABBGGRR → GL RGBA8 byte order */
            int nf = cube ? 6 : 1;
            int fw = w, fh = cube ? h / 6 : h;
            unsigned char *bytes = malloc((size_t)fw * fh * 4);
            if (!bytes) break;
            for (int f = 0; f < nf; f++) {
                if (cube) {
                    /* faces are stored bottom-to-top in the strip */
                    int row = (5 - f) * fh;
                    for (int y = 0; y < fh; y++)
                        for (int x = 0; x < fw; x++) {
                            size_t src = (size_t)(row + y) * w + x;
                            unsigned int v = (unsigned int)(unsigned long long)px[src];
                            size_t d = ((size_t)y * fw + x) * 4;
                            bytes[d+0] = (unsigned char)(v & 0xFF);
                            bytes[d+1] = (unsigned char)((v >> 8) & 0xFF);
                            bytes[d+2] = (unsigned char)((v >> 16) & 0xFF);
                            bytes[d+3] = (unsigned char)((v >> 24) & 0xFF);
                        }
                } else {
                    for (size_t p = 0; p < (size_t)fw * fh; p++) {
                        unsigned int v = (unsigned int)(unsigned long long)px[p];
                        bytes[p*4+0] = (unsigned char)(v & 0xFF);
                        bytes[p*4+1] = (unsigned char)((v >> 8) & 0xFF);
                        bytes[p*4+2] = (unsigned char)((v >> 16) & 0xFF);
                        bytes[p*4+3] = (unsigned char)((v >> 24) & 0xFF);
                    }
                }
                if (cube) {
                    static const GLenum faces[6] = {
                        GL_TEXTURE_CUBE_MAP_POSITIVE_X, GL_TEXTURE_CUBE_MAP_NEGATIVE_X,
                        GL_TEXTURE_CUBE_MAP_POSITIVE_Y, GL_TEXTURE_CUBE_MAP_NEGATIVE_Y,
                        GL_TEXTURE_CUBE_MAP_POSITIVE_Z, GL_TEXTURE_CUBE_MAP_NEGATIVE_Z
                    };
                    glTexImage2D(faces[f], 0, GL_RGBA8, fw, fh, 0, GL_RGBA,
                                 GL_UNSIGNED_BYTE, bytes);
                } else {
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fw, fh, 0, GL_RGBA,
                                 GL_UNSIGNED_BYTE, bytes);
                }
            }
            free(bytes);
            /* filtering / wrap from the colmode bits */
            int colmode = c->mode;
            int filter = (colmode >> 4) & 0xF;
            int wrap = (colmode >> 8) & 0xF;
            GLint minf = GL_LINEAR, magf = GL_LINEAR;
            if (filter == 1) { minf = magf = GL_NEAREST; }
            else if (filter >= 2) { minf = GL_LINEAR_MIPMAP_LINEAR; magf = GL_LINEAR; }
            GLint wt = (wrap == 1) ? GL_MIRRORED_REPEAT : (wrap == 2) ? GL_CLAMP_TO_EDGE
                                  : (wrap == 3) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
            if (cube) wt = GL_CLAMP_TO_EDGE;
            glTexParameteri(target, GL_TEXTURE_MIN_FILTER, minf);
            glTexParameteri(target, GL_TEXTURE_MAG_FILTER, magf);
            glTexParameteri(target, GL_TEXTURE_WRAP_S, wt);
            glTexParameteri(target, GL_TEXTURE_WRAP_T, wt);
            if (filter >= 2) glGenerateMipmap(target);
            break;
        }

        /* glbindtexture(tex): bind the texture to the current active unit. */
        case GLCMD_BINDTEX: {
            int tex = (int)c->a;
            if (tex >= 0 && tex < REN_MAX_TEX && rd->tex_obj[tex]) {
                flush_batch(rd);   /* queued tris sample the OLD binding */
                glActiveTexture(GL_TEXTURE0 + rd->active_unit);
                glBindTexture(rd->tex_tgt[tex], rd->tex_obj[tex]);
            }
            break;
        }

        /* glactivetexture(unit): select the active texture unit. */
        case GLCMD_ACTIVETEX:
            if (rd->active_unit != ((int)c->a & 3)) flush_batch(rd);
            rd->active_unit = (int)c->a & 3;
            glActiveTexture(GL_TEXTURE0 + rd->active_unit);
            break;

        /* glcapture / glcaptureend — render-to-texture, matching the
         * reference (the scene is drawn into an offscreen texture that the
         * post-process shader samples). glcapture() switches the active
         * render target to a dedicated capture FBO whose colour attachment is
         * a texture, so the scene draws straight into that texture with no
         * pixel read-back. glcaptureend() just binds that texture as the
         * named sampler and restores the main render target.
         *
         * This avoids the fragile mid-frame glReadPixels path, which returned
         * stale/blank data on the offline (headless) GL context and made every
         * capture-based script (gears, funky, ...) come out blank/white. */
        case GLCMD_CAPTURE: {
            /* switching render target: anything still queued belongs to the
             * PREVIOUS target and must be drawn there first */
            flush_batch(rd);
            /* glcapture() (no args) and glcapture(tex,w,h,col) both begin a
             * screen capture: switch the active render target to the capture
             * texture so the scene is drawn into it directly. */
            if (c->a >= 0) {
                rd->cap_texno = (int)c->a;
                rd->cap_w = (int)c->b > 0 ? (int)c->b : rd->w;
                rd->cap_h = (int)c->c > 0 ? (int)c->c : rd->h;
            } else {
                /* no-arg form: capture the whole framebuffer */
                rd->cap_texno = 0;
                rd->cap_w = rd->render_to_default ? rd->fb_w : rd->w;
                rd->cap_h = rd->render_to_default ? rd->fb_h : rd->h;
            }
            if (!rd->cap_fbo) {
                glGenFramebuffers(1, &rd->cap_fbo);
                glGenTextures(1, &rd->cap_tex);
                rd->cap_owned = 1;
            }
            glBindTexture(GL_TEXTURE_2D, rd->cap_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rd->cap_w, rd->cap_h,
                         0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            /* Post-process shaders sample with coordinates intentionally
             * scaled >1 (e.g. clock's blur uses 3.0*uv); wrap must REPEAT so
             * those samples reference the drawn scene rather than clamping to
             * the (black) edges. */
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glBindFramebuffer(GL_FRAMEBUFFER, rd->cap_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_TEXTURE_2D, rd->cap_tex, 0);
            /* start the capture from a clean (black, opaque) background so
             * the post-process samples only the drawn scene */
            glViewport(0, 0, rd->cap_w, rd->cap_h);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            break;
        }
        case GLCMD_CAPTUREEND: {
            /* the captured scene must be fully drawn into cap_fbo before it
             * is copied out into the sampled texture */
            flush_batch(rd);
            int tex = (int)c->a;
            if (tex < 0 || tex >= REN_MAX_TEX) break;
            if (rd->cap_fbo) {
                /* The scene just rendered into rd->cap_tex (via the cap_fbo).
                 * Hand a STABLE, independent copy to tex_obj[tex] so that
                 * subsequent glcapture() calls (a script may capture several
                 * indexes per frame) don't clobber an earlier captured texture
                 * the post-process quad still needs to sample. Sharing the
                 * cap_tex directly would overwrite every index each time. */
                if (!rd->tex_obj[tex]) {
                    glGenTextures(1, &rd->tex_obj[tex]);
                    glBindTexture(GL_TEXTURE_2D, rd->tex_obj[tex]);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rd->cap_w, rd->cap_h,
                                 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }
                /* copy cap_fbo contents into tex_obj[tex] */
                glBindFramebuffer(GL_FRAMEBUFFER, rd->cap_fbo);
                glBindTexture(GL_TEXTURE_2D, rd->tex_obj[tex]);
                glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0,
                                    rd->cap_w, rd->cap_h);
                rd->tex_tgt[tex] = GL_TEXTURE_2D;
                /* keep it bound to unit 0 so the post-process quad samples it */
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, rd->tex_obj[tex]);
                /* restore the main render target + viewport */
                glBindFramebuffer(GL_FRAMEBUFFER, rd->render_to_default ? 0 : rd->fbo);
                if (rd->render_to_default) glViewport(0, 0, rd->fb_w, rd->fb_h);
                else                       glViewport(0, 0, rd->w, rd->h);
            }
            break;
        }
        }
    }
    /* end of the command stream: emit whatever is still queued */
    flush_batch(rd);
    glBindVertexArray(0);
}

void pd_gl_renderer_read_rgba(pd_GLRenderer *rd, unsigned char *out)
{
    /* a read must see the finished frame */
    flush_batch(rd);
    /* When rendering directly into the window's default framebuffer
     * (render_to_default), the pixels live in FB 0, not rd->fbo. Read from the
     * one we actually drew into. */
    glBindFramebuffer(GL_FRAMEBUFFER, rd->render_to_default ? 0 : rd->fbo);
    glReadPixels(0, 0, rd->w, rd->h, GL_RGBA, GL_UNSIGNED_BYTE, out);
}

void pd_gl_renderer_acquire(pd_GLRenderer *rd)
{
    if (rd && rd->owns_ctx && rd->osc) pd_gl_offscreen_make_current(rd->osc);
}
void pd_gl_renderer_release(pd_GLRenderer *rd)
{
    if (rd && rd->owns_ctx && rd->osc) pd_gl_offscreen_make_current(NULL);
}

unsigned int pd_gl_renderer_fbo(pd_GLRenderer *rd)
{
    return (unsigned int)(rd ? rd->fbo : 0);
}

void pd_gl_renderer_set_render_to_default(pd_GLRenderer *rd, int on)
{
    if (rd) rd->render_to_default = on ? 1 : 0;
}

void pd_gl_renderer_set_framebuffer_size(pd_GLRenderer *rd, int fbw, int fbh)
{
    if (rd) { rd->fb_w = fbw; rd->fb_h = fbh; }
}

void pd_gl_renderer_get_framebuffer_size(const pd_GLRenderer *rd, int *fbw, int *fbh)
{
    if (rd) { *fbw = rd->fb_w; *fbh = rd->fb_h; }
    else { *fbw = 0; *fbh = 0; }
}

void pd_gl_renderer_destroy(pd_GLRenderer *rd)
{
    if (!rd) return;
    if (rd->program) glDeleteProgram(rd->program);
    if (rd->vao) glDeleteVertexArrays(1, &rd->vao);
    if (rd->vbo) glDeleteBuffers(1, &rd->vbo);
    if (rd->rbo_color) glDeleteRenderbuffers(1, &rd->rbo_color);
    if (rd->rbo_depth) glDeleteRenderbuffers(1, &rd->rbo_depth);
    if (rd->fbo) glDeleteFramebuffers(1, &rd->fbo);
    for (int i = 0; i < REN_MAX_TEX; i++)
        if (rd->tex_obj[i]) glDeleteTextures(1, &rd->tex_obj[i]);
    if (rd->cap_fbo) glDeleteFramebuffers(1, &rd->cap_fbo);
    if (rd->cap_tex) glDeleteTextures(1, &rd->cap_tex);
    for (int i = 0; i < REN_MAX_LOCS; i++) free(rd->u_name[i]);
    free(rd->verts);
    free(rd->tris);
    free(rd->batch);
    if (rd->owns_ctx && rd->osc) pd_gl_offscreen_destroy(rd->osc);
    free(rd);
}
