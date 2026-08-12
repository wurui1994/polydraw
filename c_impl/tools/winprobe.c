/* winprobe — launch a REAL GLFW window, render one frame into the window's
 * default framebuffer (exactly the path the interactive viewer uses), then
 * glReadPixels from framebuffer 0 and write it to a PNG. This is the
 * definitive check for "is the window black": it exercises the same
 * pd_gl_renderer_create_ex(...,0) + render_to_default path as polydraw-view.
 *
 * Usage: winprobe file.pss [--w W] [--h H] [--fovy DEG] [--frame N] -o out.png
 */
#include "render/pd_runlib.h"
#include "render/gl_renderer.h"
#include "render/pd_polyhost_tex.h"
#include "eval/pd_section.h"

#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif
#include <GLFW/glfw3.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)sz + 1); size_t rd = fread(b, 1, (size_t)sz, f);
    fclose(f); b[rd] = 0; return b;
}

int main(int argc, char **argv) {
    const char *script = NULL, *outpath = NULL;
    int w = 640, h = 480; double fovy = 73.74; double frame = 30;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--w") == 0 && i+1 < argc) w = atoi(argv[++i]);
        else if (strcmp(argv[i], "--h") == 0 && i+1 < argc) h = atoi(argv[++i]);
        else if (strcmp(argv[i], "--fovy") == 0 && i+1 < argc) fovy = atof(argv[++i]);
        else if (strcmp(argv[i], "--frame") == 0 && i+1 < argc) frame = atof(argv[++i]);
        else if (strcmp(argv[i], "-o") == 0 && i+1 < argc) outpath = argv[++i];
        else if (argv[i][0] != '-') script = argv[i];
    }
    if (!script || !outpath) { fprintf(stderr, "usage: winprobe file.pss -o out.png [--w W --h H --frame N]\n"); return 1; }

    if (!glfwInit()) { fprintf(stderr, "glfwInit failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
    glfwWindowHint(GLFW_STENCIL_BITS, 8);
    GLFWwindow *win = glfwCreateWindow(w, h, "winprobe", NULL, NULL);
    if (!win) { fprintf(stderr, "glfwCreateWindow failed (no display?)\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    fprintf(stderr, "GL_VENDOR=%s\n", (const char*)glGetString(GL_VENDOR));
    fprintf(stderr, "GL_RENDERER=%s\n", (const char*)glGetString(GL_RENDERER));

    char *src = read_file(script);
    if (!src) { fprintf(stderr, "cannot read %s\n", script); glfwDestroyWindow(win); glfwTerminate(); return 2; }
    pd_SectionList sl;
    if (!pd_section_parse(&sl, src)) { fprintf(stderr, "section err: %s\n", sl.err); free(src); return 1; }
    const pd_Section *hs = pd_section_host(&sl);
    if (!hs) { fprintf(stderr, "no host block\n"); free(src); return 1; }
    size_t hl = hs->end - hs->start;
    char *host = malloc(hl + 1); memcpy(host, src + hs->start, hl); host[hl] = 0;

    char err[256];
    pdrl_Ctx *ctx = pdrl_compile(host, w, h, err, sizeof(err));
    if (!ctx) { fprintf(stderr, "compile err: %s\n", err); free(src); free(host); return 1; }
    pdrl_set_clock_scale(ctx, 1.0 / 60.0);

    /* Render in the PROVEN CGL offscreen context (pd_gl_renderer_create,
     * own_offscreen=1) — identical to polydraw-render, which is correct for
     * all scripts including array-texture / custom-shader ones (heightmap,
     * ballsk) that render BLACK inside a GLFW window context. We read the
     * pixels back to memory and upload them as a texture onto a fullscreen
     * quad in the GLFW window, so the window shows the correct image. */
    pd_GLRenderer *rd = pd_gl_renderer_create(w, h, fovy);
    if (!rd) { fprintf(stderr, "renderer failed\n"); glfwDestroyWindow(win); glfwTerminate(); return 1; }
    const pd_Section *vs = pd_section_find(&sl, PD_SEC_VERTEX, NULL);
    const pd_Section *fs = pd_section_find(&sl, PD_SEC_FRAGMENT, NULL);
    char *vb = NULL, *fb = NULL;
    if (vs) { size_t l = vs->end - vs->start; vb = malloc(l+1); memcpy(vb, src+vs->start, l); vb[l]=0; }
    if (fs) { size_t l = fs->end - fs->start; fb = malloc(l+1); memcpy(fb, src+fs->start, l); fb[l]=0; }
    pd_gl_renderer_set_shaders(rd, vb, fb);

    /* GLFW-side display quad (one RGBA texture, fullscreen). */
    glfwMakeContextCurrent(win);
    GLuint disp_tex = 0, disp_vao = 0, disp_prog = 0;
    {
        static const char *qvert =
            "#version 150 core\n"
            "in vec2 a_pos; out vec2 uv;\n"
            "void main(){ uv = vec2(a_pos.x*0.5+0.5, 1.0-(a_pos.y*0.5+0.5));"
            " gl_Position = vec4(a_pos,0.0,1.0); }\n";
        static const char *qfrag =
            "#version 150 core\n"
            "in vec2 uv; out vec4 o; uniform sampler2D tex;\n"
            "void main(){ o = texture(tex, uv); }\n";
        disp_prog = pd_gl_link_program(qvert, qfrag);
        glGenTextures(1, &disp_tex);
        glBindTexture(GL_TEXTURE_2D, disp_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        float quad[8] = { -1,-1, 1,-1, -1,1, 1,1 };
        glGenVertexArrays(1, &disp_vao);
        glBindVertexArray(disp_vao);
        GLuint vbo; glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        GLint al = glGetAttribLocation(disp_prog, "a_pos");
        glEnableVertexAttribArray((GLuint)al);
        glVertexAttribPointer((GLuint)al, 2, GL_FLOAT, GL_FALSE, 0, 0);
        glBindVertexArray(0);
    }

    /* replay frames 0..frame in the CGL context, render the final frame,
     * read it back, then present on the GLFW window. */
    for (int f = 0; f <= (int)frame; f++) {
        pd_gl_renderer_acquire(rd);
        pdrl_run_frame(ctx, (double)f);
        pd_gl_renderer_render(rd, pdrl_glbuf(ctx));
    }
    unsigned char *rgba = malloc((size_t)w * h * 4);
    pd_gl_renderer_read_rgba(rd, rgba);
    pd_gl_renderer_release(rd);
    GLenum ge = glGetError();
    fprintf(stderr, "glGetError after render+read_fbo: 0x%x\n", ge);

    glfwMakeContextCurrent(win);
    glViewport(0, 0, w, h);
    glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(disp_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, disp_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glUniform1i(glGetUniformLocation(disp_prog, "tex"), 0);
    glBindVertexArray(disp_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glfwSwapBuffers(win);

    unsigned char *rgb = malloc((size_t)w * h * 3);
    long nonblack = 0;
    for (int y = 0; y < h; y++) {
        const unsigned char *row = rgba + (size_t)(h - 1 - y) * w * 4;
        unsigned char *dst = rgb + (size_t)y * w * 3;
        for (int x = 0; x < w; x++) {
            dst[x*3+0] = row[x*4+0]; dst[x*3+1] = row[x*4+1]; dst[x*3+2] = row[x*4+2];
            if (row[x*4+0] || row[x*4+1] || row[x*4+2]) nonblack++;
        }
    }
    int ok = stbi_write_png(outpath, w, h, 3, rgb, w * 3);
    fprintf(stderr, "wrote %s  nonblack_pixels=%ld/%d  meanRGB=(%d,%d,%d)\n", outpath, nonblack, w*h,
            (int)(rgba[0]), (int)(rgba[1]), (int)(rgba[2]));
    free(rgb); free(rgba);
    pd_gl_renderer_destroy(rd);
    pdrl_free(ctx); free(vb); free(fb); free(host); free(src);
    glfwDestroyWindow(win); glfwTerminate();
    return ok ? 0 : 1;
}
