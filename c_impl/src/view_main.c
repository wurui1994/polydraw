/* polydraw-view — interactive window viewer for .pss scripts (M8: window GUI).
 *
 * Reuses the exact same EVAL + GL renderer pipeline as polydraw-render, but
 * drives it from a live GLFW window instead of a one-shot PNG:
 *   .pss → section split → pdrl_compile → per-frame pdrl_run_frame
 *       → pd_gl_renderer_render (into its offscreen FBO)
 *       → glBlitFramebuffer(FBO → window) → glfwSwapBuffers
 *
 * The window therefore shows the SAME pixels as the offscreen render
 * (success criterion #3: "窗口模式与 offscreen 像素一致").
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

#include "render/pd_runlib.h"
#include "render/gl_renderer.h"
#include "render/pd_polyhost_tex.h"
#include "eval/pd_section.h"
#include "eval/pd_jit.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path, size_t *outLen) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f); fclose(f);
    buf[rd] = 0; if (outLen) *outLen = rd; return buf;
}

/* Minimal INI: read an int/double/string under [section]/key. Returns 1 if
 * found. Satisfies M8 config persistence (polydraw.ini). */
static int ini_get(const char *path, const char *sec, const char *key,
                   char *out, size_t outsz) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512]; int in_sec = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') { in_sec = strncmp(p + 1, sec, strlen(sec)) == 0 &&
                            (p[1 + strlen(sec)] == ']' || p[1 + strlen(sec)] == ' '); continue; }
        if (!in_sec) continue;
        if (*p == '#' || *p == ';' || *p == '\n' || *p == 0) continue;
        char *eq = strchr(p, '='); if (!eq) continue;
        *eq = 0; char *k = p; while (*k && *k != ' ' && *k != '\t') k++;
        *k = 0; char *val = eq + 1; while (*val == ' ' || *val == '\t') val++;
        size_t kl = strlen(p);
        if (kl && strncmp(p, key, kl) == 0 && p[kl] == 0) {
            size_t vl = strlen(val); if (vl && val[vl-1] == '\n') val[vl-1] = 0;
            snprintf(out, outsz, "%s", val); fclose(f); return 1;
        }
    }
    fclose(f); return 0;
}

/* Write (or update) `key = value` under [section] in path, preserving other
 * sections/keys. Creates the file/section if missing. Satisfies M8 config
 * persistence (polydraw.ini write-back). */
static void ini_set(const char *path, const char *sec, const char *key,
                    const char *val) {
    /* load existing file (if any) into memory */
    char *cur = NULL; size_t curlen = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        cur = malloc((size_t)sz + 1);
        if (cur) { curlen = (size_t)fread(cur, 1, (size_t)sz, f); cur[curlen] = 0; }
        fclose(f);
    }
    /* buffer for the rewritten contents */
    size_t cap = curlen + strlen(sec) + strlen(key) + strlen(val) + 64;
    char *out = malloc(cap);
    if (!out) { free(cur); return; }
    size_t olen = 0;
    int in_sec = 0, wrote = 0;
    if (cur) {
        char *p = cur, *e = cur + curlen;
        while (p < e) {
            char *nl = memchr(p, '\n', (size_t)(e - p));
            size_t ll = nl ? (size_t)(nl - p) : (size_t)(e - p);
            int is_sec = (ll > 0 && p[0] == '[');
            if (is_sec) {
                char sname[256]; size_t sn = 0;
                for (size_t i = 1; i < ll && p[i] != ']' && sn < sizeof sname - 1; i++)
                    sname[sn++] = p[i];
                sname[sn] = 0;
                int match = (strcmp(sname, sec) == 0);
                /* close previous target section before a new one */
                if (in_sec && !wrote) {
                    olen += (size_t)snprintf(out + olen, cap - olen, "%s = %s\n", key, val);
                    wrote = 1;
                }
                in_sec = match;
            } else if (in_sec && ll > 0) {
                /* skip an existing key matching ours (we replace it) */
                char k[256]; size_t kl = 0;
                while (kl < ll && p[kl] != '=' && p[kl] != ' ' && p[kl] != '\t' && kl < sizeof k - 1) k[kl++] = p[kl];
                k[kl] = 0;
                if (strcmp(k, key) == 0) { p = nl ? nl + 1 : e; continue; }
            }
            memcpy(out + olen, p, ll); olen += ll;
            if (nl) { out[olen++] = '\n'; p = nl + 1; } else { p = e; }
        }
    }
    if (in_sec && !wrote) {
        olen += (size_t)snprintf(out + olen, cap - olen, "%s = %s\n", key, val);
        wrote = 1;
    }
    if (!wrote) {
        if (olen > 0 && out[olen-1] != '\n') out[olen++] = '\n';
        olen += (size_t)snprintf(out + olen, cap - olen, "[%s]\n%s = %s\n", sec, key, val);
        wrote = 1;
    }
    f = fopen(path, "wb");
    if (f) { fwrite(out, 1, olen, f); fclose(f); }
    free(out); free(cur);
}

/* Keep the renderer's viewport aligned with the window's real drawable size
 * (HiDPI: framebuffer != logical window size). Called on resize and at start. */
static void on_framebuffer_resize(GLFWwindow *win, int fbw, int fbh) {
    (void)win;
    pd_GLRenderer *rd = (pd_GLRenderer *)glfwGetWindowUserPointer(win);
    if (rd && fbw > 0 && fbh > 0)
        pd_gl_renderer_set_framebuffer_size(rd, fbw, fbh);
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL); setbuf(stderr, NULL);
    const char *script = NULL;
    int w = 640, h = 480; double fovy = 73.74;
    int headless = 0; double headless_frame = 30;
    int jit_mode = 2;   /* 2=auto, 1=force on, 0=force off */
    const char *outpath = NULL;
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--w") == 0 && i+1 < argc)     w = atoi(argv[++i]);
        else if (strcmp(argv[i], "--h") == 0 && i+1 < argc)     h = atoi(argv[++i]);
        else if (strcmp(argv[i], "--fovy") == 0 && i+1 < argc)  fovy = atof(argv[++i]);
        else if (strcmp(argv[i], "--once") == 0 && i+1 < argc)  { headless = 1; headless_frame = atof(argv[++i]); }
        else if (strcmp(argv[i], "-o") == 0 && i+1 < argc)      outpath = argv[++i];
        else if (strcmp(argv[i], "--jit") == 0)                 jit_mode = 1;
        else if (strcmp(argv[i], "--no-jit") == 0)              jit_mode = 0;
        else if (argv[i][0] != '-')                             script = argv[i];
    }
    int use_jit = (jit_mode == 1) ? 1 : (jit_mode == 2 ? pd_jit_available() : 0);
    double (*run_frame)(pdrl_Ctx *, double) =
        use_jit ? pdrl_run_frame_jit : pdrl_run_frame;
    if (!script) {
        /* no script on the command line: try the last-opened one from INI */
        char last[1024]; int have_ini = ini_get("polydraw.ini", "last", "script", last, sizeof last);
        if (have_ini && last[0]) script = strdup(last);
    }
    if (!script) {
        fprintf(stderr,
            "polydraw-view — interactive .pss viewer\n"
            "Usage:\n"
            "  polydraw-view file.pss [--w W] [--h H] [--fovy DEG]\n"
            "              [--once FRAME -o out.png]  (headless, no window)\n");
        return 1;
    }

    /* apply INI defaults only when the command line did not override them */
    char tmp[64];
    if (w == 640 && ini_get("polydraw.ini", "window", "w", tmp, sizeof tmp)) w = atoi(tmp);
    if (h == 480 && ini_get("polydraw.ini", "window", "h", tmp, sizeof tmp)) h = atoi(tmp);
    if (fovy == 73.74 && ini_get("polydraw.ini", "window", "fovy", tmp, sizeof tmp)) fovy = atof(tmp);

    /* write-back: remember the last script and window settings for next launch */
    {
        char sbuf[64];
        ini_set("polydraw.ini", "last", "script", script);
        snprintf(sbuf, sizeof sbuf, "%d", w);     ini_set("polydraw.ini", "window", "w", sbuf);
        snprintf(sbuf, sizeof sbuf, "%d", h);     ini_set("polydraw.ini", "window", "h", sbuf);
        snprintf(sbuf, sizeof sbuf, "%.2f", fovy); ini_set("polydraw.ini", "window", "fovy", sbuf);
    }

    size_t len = 0;
    char *src = read_file(script, &len);
    if (!src) { fprintf(stderr, "cannot read %s\n", script); return 2; }
    pd_SectionList sl;
    if (!pd_section_parse(&sl, src)) {
        fprintf(stderr, "section parse error: %s\n", sl.err);
        free(src); return 1;
    }
    const pd_Section *hs = pd_section_host(&sl);
    if (!hs) { fprintf(stderr, "no host block in %s\n", script); free(src); return 1; }

    char *host_buf = NULL, *vert_buf = NULL, *frag_buf = NULL;
    const pd_Section *vs = pd_section_find(&sl, PD_SEC_VERTEX, NULL);
    const pd_Section *fs = pd_section_find(&sl, PD_SEC_FRAGMENT, NULL);
    const pd_Section *blocks[3] = {hs, vs, fs};
    char **bufs[3] = {&host_buf, &vert_buf, &frag_buf};
    for (int i = 0; i < 3; i++) {
        if (!blocks[i]) continue;
        size_t n = blocks[i]->end - blocks[i]->start;
        *bufs[i] = malloc(n + 1);
        memcpy(*bufs[i], src + blocks[i]->start, n);
        (*bufs[i])[n] = 0;
    }

    pdrl_Block *gblocks = calloc((size_t)sl.nSecs, sizeof(pdrl_Block));
    int gnb = 0; int idxByType[4] = {0, 0, 0, 0};
    for (int i = 0; i < sl.nSecs; i++) {
        const pd_Section *sec = &sl.secs[i];
        size_t n = sec->end - sec->start;
        char *b = malloc(n + 1);
        memcpy(b, src + sec->start, n); b[n] = 0;
        pdrl_Block *bk = &gblocks[gnb++];
        bk->src = b;
        strncpy(bk->name, sec->name, sizeof(bk->name) - 1);
        bk->type = (int)sec->type;
        bk->index = idxByType[sec->type]++;
    }
    pdrl_install_tex_blocks(gblocks, gnb);

    char err[256];
    pdrl_Ctx *ctx = pdrl_compile(host_buf, w, h, err, sizeof(err));
    if (!ctx) { fprintf(stderr, "compile error: %s\n", err); free(src); return 1; }
    pdrl_set_clock_scale(ctx, 1.0 / 60.0);

    if (!glfwInit()) { fprintf(stderr, "glfwInit failed\n"); pdrl_free(ctx); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    GLFWwindow *win = glfwCreateWindow(w, h, script, NULL, NULL);
    if (!win) { fprintf(stderr, "glfwCreateWindow failed\n"); glfwTerminate(); pdrl_free(ctx); return 1; }
    glfwMakeContextCurrent(win);
    if (getenv("PD_VIEW_NOSYNC")) glfwSwapInterval(0);   /* benchmark: uncap vsync */

    /* Renderer strategy:
     *   - Live window: render DIRECTLY into the GLFW window's own GL context
     *     (own_offscreen=0, render_to_default=1). One context, no per-frame
     *     GPU->CPU->GPU pixel round-trip — this is what makes the viewer fast
     *     and is exactly how the original app worked. The earlier "black
     *     window" was NOT a GLFW-context problem: it was two real EVAL bugs
     *     (frame-0 init skipped; texture uploads discarded by glcmd_reset)
     *     that are now fixed, so direct rendering is correct.
     *   - Headless --once: render in the proven CGL offscreen FBO and read it
     *     back to CPU for the PNG (bit-identical to polydraw-render). */
    pd_GLRenderer *rd;
    if (headless) {
        rd = pd_gl_renderer_create(w, h, fovy);   /* CGL offscreen FBO */
    } else {
        rd = pd_gl_renderer_create_ex(w, h, fovy, 0);
        pd_gl_renderer_set_render_to_default(rd, 1);
    }
    if (!rd) { fprintf(stderr, "GL renderer failed\n"); glfwDestroyWindow(win); glfwTerminate(); pdrl_free(ctx); return 1; }
    pd_gl_renderer_set_shaders(rd, vert_buf, frag_buf);

    /* HiDPI: the window's GL drawable is larger than the logical size, so feed
     * the real pixel size to the renderer for the viewport (otherwise the scene
     * occupies only a quarter of the window). */
    if (!headless) {
        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(win, &fbw, &fbh);
        if (fbw > 0 && fbh > 0) pd_gl_renderer_set_framebuffer_size(rd, fbw, fbh);
        glfwSetWindowUserPointer(win, rd);
        glfwSetFramebufferSizeCallback(win, on_framebuffer_resize);
    }
    fprintf(stderr, "[view-setup] GL_VERSION=%s direct_window=%d\n",
            (const char*)glGetString(GL_VERSION), headless ? 0 : 1);

    /* Headless one-shot mode: render to the FBO (same path as the window),
     * read it back and write a PNG. Reuses the exact render pipeline so it is
     * bit-identical to polydraw-render output, and is CI/automation friendly. */
    if (headless) {
        pd_gl_renderer_acquire(rd);
        for (int f = 0; f <= (int)headless_frame; f++) {
            run_frame(ctx, (double)f);
            pd_gl_renderer_render(rd, pdrl_glbuf(ctx));
        }
        unsigned char *rgba = malloc((size_t)w * h * 4);
        unsigned char *rgb  = malloc((size_t)w * h * 3);
        if (!rgba || !rgb) { fprintf(stderr, "out of memory\n"); return 1; }
        pd_gl_renderer_read_rgba(rd, rgba);
        pd_gl_renderer_release(rd);
        for (int y = 0; y < h; y++) {
            const unsigned char *row = rgba + (size_t)(h - 1 - y) * w * 4;
            unsigned char *dst = rgb + (size_t)y * w * 3;
            for (int x = 0; x < w; x++) {
                dst[x*3+0] = row[x*4+0];
                dst[x*3+1] = row[x*4+1];
                dst[x*3+2] = row[x*4+2];
            }
        }
        char outbuf[512];
        if (!outpath) { snprintf(outbuf, sizeof(outbuf), "%s_f%d.png", script, (int)headless_frame); outpath = outbuf; }
        int ok = stbi_write_png(outpath, w, h, 3, rgb, w * 3);
        printf("wrote %s (%dx%d, frame %d)\n", outpath, w, h, (int)headless_frame);
        free(rgb); free(rgba);
        pd_gl_renderer_destroy(rd);
        pdrl_free(ctx);
        for (int i = 0; i < gnb; i++) free((void *)gblocks[i].src);
        free(gblocks);
        free(host_buf); free(vert_buf); free(frag_buf);
        free(src);
        return ok ? 0 : 1;
    }

    int paused = 0, quit = 0, step = 0, restart = 0;
    double frame = 0;
    double last_rendered = -1.0;   /* last frame actually run (incremental playback) */
    /* Scripts seed their one-time state (particle positions, palettes,
     * uploaded textures) inside `if (numframes == 0)`. The render loop below
     * skips frame 0 on its very first iteration (dt>0 advances `frame` before
     * the first render). So we explicitly RUN frame 0 AND RENDER it here: the
     * run records the upload commands (SETTEXDATA), and the render EXECUTES
     * them into GL — without the render, the next run(frame) would glcmd_reset
     * the buffer and discard the upload, leaving textures empty (black).
     * In GLFW-direct mode acquire/release are no-ops, so this runs directly in
     * the window's GL context. */
    {
        run_frame(ctx, 0.0);
        pd_gl_renderer_render(rd, pdrl_glbuf(ctx));
        last_rendered = 0.0;
    }
    /* Fixed real-time playback: the script advances at its native rate
     * (pdrl_set_clock_scale(1/60) above ⇒ klock() advances 1/60 s per script
     * frame). We accumulate wall-clock time and step the script one frame at
     * a time so animation plays at the correct speed regardless of how fast
     * the machine renders, while never skipping more than one script frame
     * per displayed frame (avoids time-jumps on a slow GPU). */
    const double SEC_PER_FRAME = 1.0 / 60.0;
    double wall_prev = glfwGetTime();
    double acc = 0.0;
    int advanced = 1;            /* whether 'frame' changed since last render */
    /* fps accounting */
    int fps_frames = 0; double fps_t0 = wall_prev, last_key_space = 0;

    while (!glfwWindowShouldClose(win) && !quit) {
        if (restart) { frame = 0; acc = 0; restart = 0; last_rendered = -1.0; }

        double wall = glfwGetTime();
        double dt = wall - wall_prev;
        if (dt > 0.25) dt = 0.25;        /* clamp after a stall */
        wall_prev = wall;

        if (paused) {
            if (step) { frame += 1.0; step = 0; advanced = 1; }
        } else {
            acc += dt;
            /* Advance at most ONE script frame per displayed frame so playback
             * speed is correct and we never fast-forward through animation when
             * the machine is slow (we just fall behind in real time). */
            if (acc >= SEC_PER_FRAME) {
                frame += 1.0; acc -= SEC_PER_FRAME; advanced = 1;
            } else {
                advanced = 0;
            }
        }

        if (advanced) {
            /* Render directly into the window's own GL context (the renderer
             * was created with render_to_default=1, so pd_gl_renderer_render
             * draws into framebuffer 0) and present it. No per-frame GPU→CPU→GPU
             * pixel round-trip — this is what makes the viewer fast and matches
             * how the original app worked. The GLFW context is already current.
             *
             * Incremental playback: globals/arrays persist across run_frame
             * calls (pd_run reuses prog.globals; only the draw buffer is reset),
             * so we only need to run the NEW frames since last_rendered. A full
             * replay from 0 is required only when seeking backwards or after a
             * restart (last_rendered > frame). This avoids the O(N^2) cost of
             * replaying 0..frame every displayed frame. */
            double tA = getenv("PD_VIEW_TIMING") ? glfwGetTime() : 0;
            if (frame <= last_rendered) {
                for (int f = 0; f <= (int)frame; f++)
                    run_frame(ctx, (double)f);
                if (getenv("PD_VIEW_TIMING")) fprintf(stderr, "[run-count] full replay 0..%.0f\n", frame);
            } else {
                if (getenv("PD_VIEW_TIMING"))
                    fprintf(stderr, "[run-range] %.0f..%.0f (%d frames) acc=%.2f\n",
                            last_rendered+1, frame, (int)frame - (int)last_rendered, acc);
                for (int f = (int)last_rendered + 1; f <= (int)frame; f++)
                    run_frame(ctx, (double)f);
            }
            last_rendered = frame;
            pd_gl_renderer_render(rd, pdrl_glbuf(ctx));
            if (getenv("PD_VIEW_TIMING")) {
                double tB = glfwGetTime();
                fprintf(stderr, "[adv-frame] frame %.0f: run+render %.1f ms\n",
                        frame, (tB - tA) * 1000.0);
            }
            if (getenv("PD_VIEW_DIAG")) {
                int fbw = 0, fbh = 0;
                pd_gl_renderer_get_framebuffer_size(rd, &fbw, &fbh);
                if (fbw <= 0) fbw = w; if (fbh <= 0) fbh = h;
                unsigned char *winpx = malloc((size_t)fbw * fbh * 4);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                glReadPixels(0, 0, fbw, fbh, GL_RGBA, GL_UNSIGNED_BYTE, winpx);
                long nb = 0;
                for (int i = 0; i < fbw*fbh; i++)
                    if (winpx[i*4] || winpx[i*4+1] || winpx[i*4+2]) nb++;
                fprintf(stderr, "[view-diag] frame=%.0f fb=%dx%d win_nonblack=%ld/%d\n",
                        frame, fbw, fbh, nb, fbw*fbh);
                /* write a PNG (flipped) for quadrant inspection */
                unsigned char *rgb = malloc((size_t)fbw * fbh * 3);
                for (int y = 0; y < fbh; y++) {
                    const unsigned char *row = winpx + (size_t)(fbh-1-y)*fbw*4;
                    unsigned char *dst = rgb + (size_t)y*fbw*3;
                    for (int x = 0; x < fbw; x++) {
                        dst[x*3]=row[x*4]; dst[x*3+1]=row[x*4+1]; dst[x*3+2]=row[x*4+2];
                    }
                }
                stbi_write_png("/tmp/view_window_diag.png", fbw, fbh, 3, rgb, fbw*3);
                free(rgb); free(winpx);
            }
            advanced = 0;
            glfwSwapBuffers(win);
            fps_frames++;
        } else {
            glfwSwapBuffers(win);   /* keep the event loop alive at vsync */
        }

        /* show fps roughly twice per second */
        if (wall - fps_t0 >= 0.5) {
            double fps = fps_frames / (wall - fps_t0);
            char title[256];
            snprintf(title, sizeof(title), "%s  —  %.1f fps  (frame %.0f)%s",
                     script, fps, frame, paused ? "  [paused]" : "");
            glfwSetWindowTitle(win, title);
            if (getenv("PD_VIEW_FPS"))
                fprintf(stderr, "[view-fps] %.1f fps (frame %.0f)\n", fps, frame);
            fps_frames = 0; fps_t0 = wall;
        }

        /* edge-triggered key handling (avoid key-repeat auto-repeat) */
        int sp = glfwGetKey(win, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (sp && !last_key_space) paused ^= 1;
        last_key_space = sp;
        if (glfwGetKey(win, GLFW_KEY_ESCAPE) == GLFW_PRESS ||
            glfwGetKey(win, GLFW_KEY_Q) == GLFW_PRESS) quit = 1;
        if (glfwGetKey(win, GLFW_KEY_R) == GLFW_PRESS) restart = 1;
        if (paused && glfwGetKey(win, GLFW_KEY_RIGHT) == GLFW_PRESS) step = 1;
        if (paused && glfwGetKey(win, GLFW_KEY_LEFT) == GLFW_PRESS) { frame -= 1.0; if (frame < 0) frame = 0; }

        glfwPollEvents();
    }

    pd_gl_renderer_destroy(rd);
    glfwDestroyWindow(win);
    glfwTerminate();
    pdrl_free(ctx);
    for (int i = 0; i < gnb; i++) free((void *)gblocks[i].src);
    free(gblocks);
    free(host_buf); free(vert_buf); free(frag_buf);
    free(src);
    return 0;
}
