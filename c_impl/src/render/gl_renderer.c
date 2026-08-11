/* gl_renderer.c — replay a GLCmd stream through a real GL core-profile
 * context. See gl_renderer.h for the design notes. */
#include "gl_renderer.h"
#include "gl_offscreen.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#elif defined(__linux__)
#include "glad/glad.h"
#endif

/* ------------------------------------------------------------------ */
/* 4x4 column-major matrix helpers (double; matches the numpy reference) */

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

/* gluPerspective, written exactly like pyref's mat_perspective.
 * NOTE: the reference stores the matrix transposed in numpy (m[row][col]
 * = GL M[col][row]) and clips with m @ v, so the GL column-major array
 * must hold m[3,2] (the w coefficient, = 2fn/(n-f)) at index 11 and
 * m[2,3] (the z coefficient, = -1) at index 14 — the transpose of the
 * conventional gluPerspective layout. */
static void mat4_perspective(double m[16], double fovy_deg, double aspect,
                             double near, double far)
{
    double f = 1.0 / tan(fovy_deg * M_PI / 360.0);
    memset(m, 0, 16 * sizeof(double));
    m[0]  = f / aspect;
    m[5]  = f;
    m[10] = (far + near) / (near - far);
    m[11] = (2 * far * near) / (near - far);  /* w coefficient (m[3,2]) */
    m[14] = -1.0;                             /* z coefficient (m[2,3]) */
}

/* glOrtho with near=-1 far=1 default, transposed layout like mat_perspective
 * above (pyref mat_ortho): the translation lands in column 3 rows 0..2 for
 * x'/y'/z' and in row 3 of columns 0..2 for w' — matching the reference's
 * clip math. */
static void mat4_ortho(double m[16], double l, double r, double b, double t,
                       double n, double f)
{
    double tx = -(r + l) / (r - l);
    double ty = -(t + b) / (t - b);
    double tz = -(f + n) / (f - n);
    memset(m, 0, 16 * sizeof(double));
    m[0] = 2.0 / (r - l);  m[12] = tx;  m[3] = tx;
    m[5] = 2.0 / (t - b);  m[13] = ty;  m[7] = ty;
    m[10] = -2.0 / (f - n); m[14] = tz; m[11] = tz;
    m[15] = 1.0;
}

static void mat4_translate(double m[16], double x, double y, double z)
{
    double t[16]; mat4_identity(t);
    t[3] = x; t[7] = y; t[11] = z;
    double r[16]; mat4_mul(r, m, t); memcpy(m, r, sizeof(r));
}

static void mat4_rotate(double m[16], double ang_deg, double x, double y, double z)
{
    double a = ang_deg * M_PI / 180.0;
    double c = cos(a), s = sin(a);
    double len = sqrt(x * x + y * y + z * z);
    if (len == 0) return;
    x /= len; y /= len; z /= len;
    double t[16] = {
        c + x * x * (1 - c),      y * x * (1 - c) + z * s, z * x * (1 - c) - y * s, 0,
        x * y * (1 - c) - z * s,  c + y * y * (1 - c),     z * y * (1 - c) + x * s, 0,
        x * z * (1 - c) + y * s,  y * z * (1 - c) - x * s, c + z * z * (1 - c),     0,
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
    size_t len = strlen(u);
    char *out = malloc(strlen(FRAG_PREFIX) + len + 1);
    if (!out) { free(u); return NULL; }
    strcpy(out, FRAG_PREFIX);
    strcat(out, u);
    free(u);
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
    double fovy;

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

    /* GL state mirrors */
    int depth_test_enabled;
    int blend_enabled;
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

/* end the open primitive: tessellate + upload + draw */
static void end_primitive(pd_GLRenderer *rd)
{
    if (rd->mode < 0 || rd->nverts == 0) {
        rd->mode = -1;
        rd->nverts = 0;
        return;
    }
    /* single-primitive path: tessellate into the per-renderer scratch */
    rd->ntri = 0;
    rd->tris = tessellate(rd->tris, &rd->ntri, &rd->captri,
                          rd->mode, rd->verts, rd->nverts);
    rd->mode = -1;
    rd->nverts = 0;
    if (rd->ntri == 0) return;

    /* per-primitive VBO upload (immediate mode is inherently dynamic;
     * fine at this scale, mirrors the reference's per-tri loop) */
    glUseProgram(rd->program);
    glBindBuffer(GL_ARRAY_BUFFER, rd->vbo);
    glBufferData(GL_ARRAY_BUFFER, rd->ntri * sizeof(GVertex), rd->tris,
                 GL_STREAM_DRAW);
    glBindVertexArray(rd->vao);
    glUniformMatrix4fv(rd->u_mvp, 1, GL_FALSE, rd->mvp_f);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)rd->ntri);
    glBindVertexArray(0);
}

/* fullscreen quad (GLCMD_QUAD): emits 2 triangles in NDC space.
 * Must bypass the projection (it is defined in screen space), so we use an
 * identity matrix — matching the reference which does not project QUADs. */
static void draw_quad(pd_GLRenderer *rd)
{
    GVertex q[4];
    float qpos[4][3] = {{-1,-1,0},{1,-1,0},{1,1,0},{-1,1,0}};
    float qtex[4][4] = {{0,0,0,1},{1,0,0,1},{1,1,0,1},{0,1,0,1}};
    for (int i = 0; i < 4; i++) {
        memset(&q[i], 0, sizeof(GVertex));
        memcpy(q[i].pos, qpos[i], sizeof(qpos[i]));
        memcpy(q[i].tex, qtex[i], sizeof(qtex[i]));
        for (int k = 0; k < 4; k++) q[i].color[k] = (float)rd->cur_color[k];
    }
    GVertex tris[6];
    tris[0] = q[0]; tris[1] = q[1]; tris[2] = q[2];
    tris[3] = q[0]; tris[4] = q[2]; tris[5] = q[3];
    static const double ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    glUseProgram(rd->program);
    glBindBuffer(GL_ARRAY_BUFFER, rd->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tris), tris, GL_STREAM_DRAW);
    glBindVertexArray(rd->vao);
    glUniformMatrix4fv(rd->u_mvp, 1, GL_FALSE, (const GLfloat *)ident);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

/* ------------------------------------------------------------------ */

pd_GLRenderer *pd_gl_renderer_create(int w, int h, double fovy)
{
    pd_GLOSCtx *osc = pd_gl_offscreen_create();
    if (!osc) {
        fprintf(stderr, "gl_renderer: could not create offscreen context\n");
        return NULL;
    }
    pd_GLRenderer *rd = calloc(1, sizeof(*rd));
    if (!rd) { pd_gl_offscreen_destroy(osc); return NULL; }
    rd->w = w; rd->h = h; rd->fovy = fovy;
    rd->mode = -1;
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
        pd_gl_offscreen_destroy(osc);
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
        pd_gl_offscreen_destroy(osc);
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
    char *va = vert_src ? adapt_vertex(vert_src) : NULL;
    char *fa = frag_src ? adapt_fragment(frag_src) : NULL;
    if (va && fa) {
        GLuint prog = link_program(va, fa);
        if (prog) {
            glDeleteProgram(rd->program);
            rd->program = prog;
            rd->u_mvp = glGetUniformLocation(rd->program, "u_mvp");
        }
    }
    free(va);
    free(fa);
}

void pd_gl_renderer_render(pd_GLRenderer *rd, const GLCmdBuf *buf)
{
    glBindFramebuffer(GL_FRAMEBUFFER, rd->fbo);
    glViewport(0, 0, rd->w, rd->h);

    /* matrices may change during replay; recompute mvp before each draw */
    update_mvp(rd);

    for (size_t i = 0; i < buf->n; i++) {
        const GLCmd *c = &buf->cmds[i];
        switch (c->op) {
        case GLCMD_CLEAR: {
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
            break;
        case GLCMD_ROTATE:
            if (rd->matrix_mode == 0)
                mat4_rotate(rd->mvm_stack[rd->mvm_top], c->a, c->b, c->c, c->d);
            update_mvp(rd);
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
                glEnable(GL_DEPTH_TEST);
                rd->depth_test_enabled = 1;
            }
            break;
        case GLCMD_DISABLE:
            if (c->mode == 0x0B71) {
                glDisable(GL_DEPTH_TEST);
                rd->depth_test_enabled = 0;
            }
            break;
        case GLCMD_BLENDFUNC: {
            int sf = (c->mode >> 16) & 0xFFFF;
            int df = c->mode & 0xFFFF;
            glBlendFunc((GLenum)sf, (GLenum)df);
            break;
        }
        case GLCMD_CULLFACE:
            glEnable(GL_CULL_FACE);
            glCullFace((GLenum)c->mode);
            break;
        case GLCMD_LINEWIDTH:
            glLineWidth((GLfloat)c->a);
            break;
        }
    }
}

void pd_gl_renderer_read_rgba(pd_GLRenderer *rd, unsigned char *out)
{
    glBindFramebuffer(GL_FRAMEBUFFER, rd->fbo);
    glReadPixels(0, 0, rd->w, rd->h, GL_RGBA, GL_UNSIGNED_BYTE, out);
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
    free(rd->verts);
    free(rd->tris);
    free(rd);
}
