/* balls_ref — standalone best-performance C port of ken/balls.pss.
 *
 * Purpose: performance CEILING reference for the polydraw pipeline. Same
 * simulation (16384 particles, 3-gon sprites, same RNG-free motion), same
 * GLSL shaders (the @v/@f sections adapted to core profile exactly the way
 * gl_renderer.c adapts them), but written directly in C with:
 *   - plain double arrays for particle state
 *   - one interleaved float VBO rebuilt per frame, ONE glDrawArrays call
 *   - no EVAL, no command buffer, no replay, no per-call getenv
 *
 * Usage:
 *   balls_ref [--frames N] [--win]        # default: headless timing, print ms/fps
 * The headless mode uses the same CGL offscreen context as polydraw-render,
 * so its numbers are directly comparable to framebench/phaseprof.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

/* ---- constants from balls.pss ---- */
#define N  16384
#define NS 3

/* ---- simulation state (same layout as the script's statics) ---- */
static double px[N], py[N], pvx[N], pvy[N];
static double pr[N], pg[N], pb[N], prad[N];
static double clut[NS], slut[NS];
static double tim, hx, hy, hz, rhz;

/* RNG: exact copy of the original krand/nrnd so frame-0 state matches the
 * script's (only used at init here, but keep sequences identical). */
static unsigned long g_holdrand = 1;
static unsigned long krand(void) {
    uint32_t v = (uint32_t)g_holdrand;
    v = (uint32_t)((v * (214013u * 2u) + 2531011u * 2u) >> 1);
    g_holdrand = v;
    return v;
}
static double rnd(void) { return ((double)krand()) * (1.0 / 2147483648.0); }
static int g_normstat = 0; static double g_srand2;
static double nrnd(void) {
    double x, y, r;
    static const double o2_31 = 1.0 / 2147483648.0;
    if (g_normstat) { g_normstat = 0; return g_srand2; }
    do {
        x = ((double)(krand() - 1073741824u)) * (o2_31 * 2.0);
        y = ((double)(krand() - 1073741824u)) * (o2_31 * 2.0);
        r = x * x + y * y;
    } while (r >= 1);
    g_normstat = 1;
    r = sqrt(-2.0 * log(r) / r);
    g_srand2 = x * r;
    return y * r;
}

/* ---- GL objects ---- */
static GLuint program, vao, vbo, fbo, rbo_color;
static int xres = 320, yres = 320;

/* same vertex layout as gl_renderer.c's GVertex (14 floats) */
typedef struct { float pos[3], color[4], tex[4], nrm[3]; } GVertex;
#define VFLOATS 14

/* the @v/@f shaders from balls.pss, adapted exactly like gl_renderer.c */
static const char *VS =
    "#version 330 core\n"
    "uniform mat4 u_mvp;\n"
    "layout(location=0) in vec4 a_vertex;\n"
    "layout(location=1) in vec4 a_color;\n"
    "layout(location=2) in vec4 a_texcoord;\n"
    "layout(location=3) in vec3 a_normal;\n"
    "out vec4 p;\nout vec4 c;\nout vec4 t;\n"
    "void main ()\n"
    "{\n"
    "   gl_Position = u_mvp * a_vertex;\n"
    "   p = a_vertex;\n"
    "   c = a_color;\n"
    "   t = a_texcoord;\n"
    "}\n";
static const char *FS =
    "#version 330 core\n"
    "in vec4 p;\nin vec4 c;\nin vec4 t;\n"
    "out vec4 fragColor;\n"
    "void main ()\n"
    "{\n"
    "   float d = length(t.xy); if (d > 1.0) discard;\n"
    "   fragColor = (1.0-d*.25)*c;\n"
    "}\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(sh, sizeof log, NULL, log);
               fprintf(stderr, "shader: %s", log); exit(1); }
    return sh;
}

static void init_gl(void) {
    GLuint vs = compile(GL_VERTEX_SHADER, VS);
    GLuint fs = compile(GL_FRAGMENT_SHADER, FS);
    program = glCreateProgram();
    glAttachShader(program, vs); glAttachShader(program, fs);
    glLinkProgram(program);
    GLint ok; glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; glGetProgramInfoLog(program, sizeof log, NULL, log);
               fprintf(stderr, "link: %s", log); exit(1); }

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)N * 3 * sizeof(GVertex), NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, VFLOATS * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, VFLOATS * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, VFLOATS * sizeof(float), (void *)(7 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, VFLOATS * sizeof(float), (void *)(11 * sizeof(float)));
    glBindVertexArray(0);

    /* offscreen FBO like gl_renderer (for headless timing parity) */
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &rbo_color);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo_color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, xres, yres);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo_color);

    /* match corrected gl_renderer.c defaults (original polydraw main loop):
     * depth test ON, standard alpha blending. */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_CULL_FACE);
    glViewport(0, 0, xres, yres);
}

/* frame-0 init: exactly the script's `if (numframes == 0)` block */
static void init_sim(void) {
    for (int i = 0; i < N; i++) {
        px[i] = xres * rnd(); pvx[i] = nrnd() * 64;
        py[i] = yres * rnd(); pvy[i] = nrnd() * 64;
        pr[i] = ((int)(64 * rnd()) + 0x60) / 256.0;
        pg[i] = ((int)(64 * rnd()) + 0x60) / 256.0;
        pb[i] = ((int)(64 * rnd()) + 0x60) / 256.0;
        prad[i] = .02 * rnd() + .01;
    }
    double r = 1.0 / cos(M_PI / NS);
    for (int i = 0; i < NS; i++) {
        clut[i] = cos((i + .5) * M_PI * 2 / NS - M_PI / 2) * r;
        slut[i] = sin((i + .5) * M_PI * 2 / NS - M_PI / 2) * r;
    }
    hx = xres / 2.0; hy = yres / 2.0; hz = hx; rhz = 1.0 / hz;
}

/* per-frame: build the batch (tessellated 3-gons) and draw in ONE call */
static GVertex *batch;        /* N*3 vertices */
static void frame(double dtim) {
    GVertex *w = batch;
    for (int i = 0; i < N; i++) {
        double r = prad[i];
        double x = (px[i] - hx) * rhz;
        double y = (py[i] - hy) * rhz;
        float cr = (float)pr[i], cg = (float)pg[i], cb2 = (float)pb[i];
        /* GL_POLYGON with NS verts -> NS-2 = 1 triangle (fan) */
        for (int j = 0; j < NS; j++) {
            double tc = clut[j], ts = slut[j];
            GVertex *v = &w[j];
            v->pos[0] = (float)(tc * r + x); v->pos[1] = (float)(ts * r + y); v->pos[2] = -1.0f;
            v->color[0] = cr; v->color[1] = cg; v->color[2] = cb2; v->color[3] = 1.0f;
            v->tex[0] = (float)tc; v->tex[1] = (float)ts; v->tex[2] = 0; v->tex[3] = 1;
            v->nrm[0] = 0; v->nrm[1] = 0; v->nrm[2] = 1;
        }
        w += NS - 2 + 1;   /* NS vertices emitted (NS=3: exactly one triangle) */

        px[i] += pvx[i] * dtim;
        py[i] += pvy[i] * dtim;
        if (px[i] < r)            pvx[i] = fabs(pvx[i]);
        if (py[i] < r)            pvy[i] = fabs(pvy[i]);
        if (px[i] >= xres - r)    pvx[i] = -fabs(pvx[i]);
        if (py[i] >= yres - r)    pvy[i] = -fabs(pvy[i]);
    }

    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)N * sizeof(GVertex), batch, GL_STREAM_DRAW);
    glBindVertexArray(vao);
    static const GLfloat ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    glUniformMatrix4fv(glGetUniformLocation(program, "u_mvp"), 1, GL_FALSE, ident);
    glDrawArrays(GL_TRIANGLES, 0, N);
    glBindVertexArray(0);
}

#if defined(__APPLE__)
#include <OpenGL/OpenGL.h>
static CGLContextObj setup_offscreen(void) {
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL3_Core,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        kCGLPFADepthSize, (CGLPixelFormatAttribute)24,
        kCGLPFAAllowOfflineRenderers, (CGLPixelFormatAttribute)0 };
    CGLPixelFormatObj pf; GLint npv = 0;
    if (CGLChoosePixelFormat(attrs, &pf, &npv) != kCGLNoError) { fprintf(stderr, "CGL pf fail\n"); exit(1); }
    CGLContextObj ctx;
    if (CGLCreateContext(pf, NULL, &ctx) != kCGLNoError) { fprintf(stderr, "CGL ctx fail\n"); exit(1); }
    CGLSetCurrentContext(ctx);
    CGLLockContext(ctx);
    return ctx;
}
#endif

static double now_sec(void) {
#if defined(__APPLE__)
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
#endif
}

int main(int argc, char **argv) {
    int nframes = 300;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) nframes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--res") && i + 1 < argc) { xres = yres = atoi(argv[++i]); }

    setup_offscreen();
    init_gl();
    init_sim();
    batch = malloc((size_t)N * NS * sizeof(GVertex));

    /* warmup + timing: dtim = 1/60 per frame (matches pdrl clock scale) */
    double dtim = 1.0 / 60.0;
    for (int f = 0; f < 10; f++) frame(dtim);
    glFinish();
    double t0 = now_sec();
    for (int f = 0; f < nframes; f++) frame(dtim);
    glFinish();
    double dt = now_sec() - t0;
    printf("balls_ref  frames=%d res=%dx%d : %.3f ms/frame  (%.0f fps theoretical)\n",
           nframes, xres, yres, dt * 1e3 / nframes, nframes / dt);
    return 0;
}
