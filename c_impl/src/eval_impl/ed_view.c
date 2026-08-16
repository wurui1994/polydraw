/* evaldraw-view — interactive window viewer for .kc scripts.
 *
 * Uses the same evaldraw software-rendering pipeline as `evaldraw`, but
 * drives it from a live GLFW window instead of a one-shot PNG:
 *   .kc -> ed_compile -> per-frame ed_run_frame -> blit framebuffer to window
 *
 * Controls:
 *   space      pause / resume
 *   r          restart from frame 0
 *   left/right step one frame (while paused)
 *   esc / q    quit
 */
#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif
#include <GLFW/glfw3.h>

#include "eval_impl/ed_runlib.h"
#include "eval/pd_jit.h"

#include "stb_image_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path, size_t *outLen) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    if (outLen)
        *outLen = rd;
    return buf;
}

/* ---- Texture-based framebuffer blit (modern GL, no deprecated fixed-fn) ---- */

static GLuint g_tex = 0;
static GLuint g_vao = 0;
static GLuint g_shader = 0;
static int g_tex_w = 0, g_tex_h = 0;

static const char *blit_vert_src =
    "#version 330 core\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID == 2) ? 3.0 : -1.0,\n"
    "                  (gl_VertexID == 1) ? 3.0 : -1.0);\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "    v_uv = p * 0.5 + 0.5;\n"
    "    v_uv.y = 1.0 - v_uv.y;\n" /* flip Y: framebuffer is top-to-bottom */
    "}\n";

static const char *blit_frag_src =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 frag_color;\n"
    "uniform sampler2D tex;\n"
    "void main() {\n"
    "    frag_color = texture(tex, v_uv);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        fprintf(stderr, "shader compile error: %s\n", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static void ed_blit_init(int fw, int fh) {
    if (g_tex && g_tex_w == fw && g_tex_h == fh)
        return;
    if (g_tex)
        glDeleteTextures(1, &g_tex);
    if (!g_shader) {
        GLuint vs = compile_shader(GL_VERTEX_SHADER, blit_vert_src);
        GLuint fs = compile_shader(GL_FRAGMENT_SHADER, blit_frag_src);
        g_shader = glCreateProgram();
        glAttachShader(g_shader, vs);
        glAttachShader(g_shader, fs);
        glLinkProgram(g_shader);
        glDeleteShader(vs);
        glDeleteShader(fs);
    }
    if (!g_vao)
        glGenVertexArrays(1, &g_vao);
    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fw, fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_tex_w = fw;
    g_tex_h = fh;
}

static void ed_blit_to_window(ed_Ctx *ctx, int win_w, int win_h) {
    int fw = ctx->state.xres;
    int fh = ctx->state.yres;
    ed_blit_init(fw, fh);

    /* Upload framebuffer data (flip Y: fb is top-to-bottom, GL is bottom-to-top). */
    unsigned char *rgba = (unsigned char *)malloc((size_t)fw * fh * 4);
    if (!rgba)
        return;
    for (int y = 0; y < fh; y++) {
        int src_y = fh - 1 - y;
        for (int x = 0; x < fw; x++) {
            uint32_t col = ctx->state.fb[src_y * fw + x];
            int idx = (y * fw + x) * 4;
            rgba[idx + 0] = (unsigned char)((col >> 16) & 0xFF);
            rgba[idx + 1] = (unsigned char)((col >> 8) & 0xFF);
            rgba[idx + 2] = (unsigned char)(col & 0xFF);
            rgba[idx + 3] = 255;
        }
    }

    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba);

    glViewport(0, 0, win_w, win_h);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(g_shader);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    const char *script = NULL;
    const char *outpath = NULL;
    int w = 640, h = 480;
    int headless = 0;
    double headless_frame = 30;
    int jit_mode = 2;

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--w") == 0 && i + 1 < argc)      w = atoi(argv[++i]);
        else if (strcmp(argv[i], "--h") == 0 && i + 1 < argc)      h = atoi(argv[++i]);
        else if (strcmp(argv[i], "--once") == 0 && i + 1 < argc)   { headless = 1; headless_frame = atof(argv[++i]); }
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)       outpath = argv[++i];
        else if (strcmp(argv[i], "--jit") == 0)                    jit_mode = 1;
        else if (strcmp(argv[i], "--no-jit") == 0)                 jit_mode = 0;
        else if (argv[i][0] != '-')                                script = argv[i];
    }

    if (!script) {
        fprintf(stderr,
            "evaldraw-view — interactive .kc viewer\n"
            "Usage:\n"
            "  evaldraw-view file.kc [--w W] [--h H]\n"
            "              [--once FRAME -o out.png]  (headless, no window)\n");
        return 1;
    }

    size_t len = 0;
    char *src = read_file(script, &len);
    if (!src) {
        fprintf(stderr, "cannot read %s\n", script);
        return 2;
    }

    ed_Ctx *ctx = ed_compile(src, w, h);
    if (!ctx) {
        free(src);
        return 1;
    }
    free(src);

    ed_set_clock_scale(ctx, 1.0 / 60.0);

    int use_jit = (jit_mode == 1) ? 1 : (jit_mode == 2 ? pd_jit_available() : 0);
    double (*run_frame)(ed_Ctx *, double) =
        use_jit ? ed_run_frame_jit : ed_run_frame;
    if (use_jit)
        fprintf(stderr, "evaldraw-view: using JIT backend (%s)\n",
                pd_jit_backend_name());

    /* Headless one-shot mode: render to framebuffer, save PNG, exit. */
    if (headless) {
        for (int f = 0; f <= (int)headless_frame; f++)
            run_frame(ctx, (double)f);

        char outbuf[512];
        if (!outpath) {
            snprintf(outbuf, sizeof(outbuf), "%s_f%d.png", script, (int)headless_frame);
            outpath = outbuf;
        }
        if (ed_save_png(ctx, outpath) != 0) {
            fprintf(stderr, "cannot write %s\n", outpath);
            ed_free(ctx);
            return 1;
        }
        printf("wrote %s (%dx%d, frame %d)\n", outpath, w, h, (int)headless_frame);
        ed_free(ctx);
        return 0;
    }

    /* Interactive window mode. */
    if (!glfwInit()) {
        fprintf(stderr, "glfwInit failed\n");
        ed_free(ctx);
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    GLFWwindow *win = glfwCreateWindow(w, h, script, NULL, NULL);
    if (!win) {
        fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        ed_free(ctx);
        return 1;
    }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    /* Run frame 0 and display it immediately. */
    run_frame(ctx, 0.0);
    {
        int fbw, fbh;
        glfwGetFramebufferSize(win, &fbw, &fbh);
        ed_blit_to_window(ctx, fbw, fbh);
    }
    glfwSwapBuffers(win);

    int paused = 0, quit = 0, step = 0, restart = 0;
    double frame = 0.0;
    long last_rendered = 0;
    const double SEC_PER_FRAME = 1.0 / 60.0;
    double wall_prev = glfwGetTime();
    double acc = 0.0;
    int advanced = 0;
    int fps_frames = 0;
    double fps_t0 = wall_prev;
    int last_key_space = 0;

    while (!glfwWindowShouldClose(win) && !quit) {
        if (restart) {
            frame = 0;
            acc = 0;
            restart = 0;
            last_rendered = -1;
        }

        double wall = glfwGetTime();
        double dt = wall - wall_prev;
        if (dt > 0.25)
            dt = 0.25;
        wall_prev = wall;

        if (paused) {
            if (step) {
                frame += 1.0;
                step = 0;
                advanced = 1;
            }
        } else {
            acc += dt;
            if (acc >= SEC_PER_FRAME) {
                frame += 1.0;
                acc -= SEC_PER_FRAME;
                advanced = 1;
            } else {
                advanced = 0;
            }
        }

        if (advanced) {
            long fc = (long)frame;
            if (fc <= last_rendered) {
                for (long f = 0; f <= fc; f++)
                    run_frame(ctx, (double)f);
            } else {
                for (long f = last_rendered + 1; f <= fc; f++)
                    run_frame(ctx, (double)f);
            }
            last_rendered = fc;

            int fbw, fbh;
            glfwGetFramebufferSize(win, &fbw, &fbh);
            ed_blit_to_window(ctx, fbw, fbh);
            glfwSwapBuffers(win);
            advanced = 0;
            fps_frames++;
        } else {
            glfwSwapBuffers(win);
        }

        /* FPS display in title bar. */
        if (wall - fps_t0 >= 0.5) {
            double fps = fps_frames / (wall - fps_t0);
            char title[256];
            snprintf(title, sizeof(title), "%s  —  %.1f fps  (frame %.0f)%s",
                     script, fps, frame, paused ? "  [paused]" : "");
            glfwSetWindowTitle(win, title);
            fps_frames = 0;
            fps_t0 = wall;
        }

        /* Key handling. */
        int sp = glfwGetKey(win, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (sp && !last_key_space)
            paused ^= 1;
        last_key_space = sp;
        if (glfwGetKey(win, GLFW_KEY_ESCAPE) == GLFW_PRESS ||
            glfwGetKey(win, GLFW_KEY_Q) == GLFW_PRESS)
            quit = 1;
        if (glfwGetKey(win, GLFW_KEY_R) == GLFW_PRESS)
            restart = 1;
        if (paused && glfwGetKey(win, GLFW_KEY_RIGHT) == GLFW_PRESS)
            step = 1;
        if (paused && glfwGetKey(win, GLFW_KEY_LEFT) == GLFW_PRESS) {
            frame -= 1.0;
            if (frame < 0)
                frame = 0;
        }

        glfwPollEvents();
    }

    glfwDestroyWindow(win);
    glfwTerminate();
    ed_free(ctx);
    return 0;
}
