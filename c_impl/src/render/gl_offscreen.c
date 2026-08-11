/* gl_offscreen.c — platform offscreen GL context.
 *
 * Mirrors moderngl's create_standalone_context() so the C renderer needs no
 * window, no pyglet/GLFW, no display server:
 *   - macOS: CGL with kCGLOGLPVersion_3_2_Core profile, no drawable attached.
 *            Framebuffers are render-to-texture/renderbuffer, so a windowless
 *            context is fully sufficient (this is exactly what moderngl does).
 *   - Linux: EGL surfaceless (EGL_MESA_platform_surfaceless, else pbuffer).
 *            GL entry points are loaded via GLAD (generated, included).
 *   - Windows: TODO (WGL hidden window).
 */
#include "gl_offscreen.h"

#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#include <OpenGL/OpenGL.h>
#include <stdlib.h>

struct pd_GLOSCtx {
    CGLContextObj cgl;
};

pd_GLOSCtx *pd_gl_offscreen_create(void)
{
    /* moderngl requests a core 3.3 context; macOS caps core at 4.1, so we
     * ask for 3.2 core which macOS maps onto its 4.1 core implementation. */
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile,
        (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        kCGLPFADepthSize, (CGLPixelFormatAttribute)24,
        kCGLPFAStencilSize, (CGLPixelFormatAttribute)8,
        kCGLPFAAllowOfflineRenderers,
        (CGLPixelFormatAttribute)0
    };
    CGLPixelFormatObj pf = NULL;
    GLint npix = 0;
    if (CGLChoosePixelFormat(attrs, &pf, &npix) != kCGLNoError || !pf) {
        /* fall back: allow the default renderer */
        CGLPixelFormatAttribute attrs2[] = {
            kCGLPFAOpenGLProfile,
            (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
            kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
            (CGLPixelFormatAttribute)0
        };
        if (CGLChoosePixelFormat(attrs2, &pf, &npix) != kCGLNoError || !pf)
            return NULL;
    }
    pd_GLOSCtx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) { CGLDestroyPixelFormat(pf); return NULL; }
    CGLError err = CGLCreateContext(pf, NULL, &ctx->cgl);
    CGLDestroyPixelFormat(pf);
    if (err != kCGLNoError || !ctx->cgl) { free(ctx); return NULL; }
    /* No drawable is attached — this is fine for FBO rendering. */
    pd_gl_offscreen_make_current(ctx);
    return ctx;
}

void pd_gl_offscreen_make_current(pd_GLOSCtx *ctx)
{
    if (ctx && ctx->cgl)
        CGLSetCurrentContext(ctx->cgl);
}

void pd_gl_offscreen_destroy(pd_GLOSCtx *ctx)
{
    if (!ctx) return;
    if (ctx->cgl) {
        CGLSetCurrentContext(NULL);
        CGLDestroyContext(ctx->cgl);
    }
    free(ctx);
}

int pd_gl_read_pixels_rgba(int w, int h, unsigned char *out)
{
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out);
    return 0;
}

#elif defined(__linux__)

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdlib.h>
#include <string.h>

/* GL entry points: GLAD loader generated for GL 4.1 core (see
 * c_impl/third_party/glad — build step in Makefile). */
#include "glad/glad.h"

struct pd_GLOSCtx {
    EGLDisplay dpy;
    EGLContext egl;
};

static EGLContext create_egl_context(EGLDisplay dpy)
{
    EGLint attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    return eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
}

pd_GLOSCtx *pd_gl_offscreen_create(void)
{
    /* 1. try EGL_MESA_platform_surfaceless (headless, no X/Wayland) */
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = EGL_NO_DISPLAY;
    if (get_platform_display) {
        dpy = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                                   EGL_DEFAULT_DISPLAY, NULL);
    }
    /* 2. fall back to the default display (X11/Wayland, may still be headless
     * via EGL_KHR_surfaceless_context) */
    if (dpy == EGL_NO_DISPLAY)
        dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY)
        return NULL;
    if (!eglInitialize(dpy, NULL, NULL))
        return NULL;
    /* 3. surfaceless context */
    EGLContext egl_ctx = create_egl_context(dpy);
    if (egl_ctx == EGL_NO_CONTEXT)
        return NULL;
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_ctx)) {
        eglDestroyContext(dpy, egl_ctx);
        return NULL;
    }
    if (gladLoadGLLoader((GLADloadproc)eglGetProcAddress) == 0) {
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(dpy, egl_ctx);
        return NULL;
    }
    pd_GLOSCtx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) { eglDestroyContext(dpy, egl_ctx); return NULL; }
    ctx->dpy = dpy;
    ctx->egl = egl_ctx;
    return ctx;
}

void pd_gl_offscreen_make_current(pd_GLOSCtx *ctx)
{
    if (ctx && ctx->egl)
        eglMakeCurrent(ctx->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx->egl);
}

void pd_gl_offscreen_destroy(pd_GLOSCtx *ctx)
{
    if (!ctx) return;
    if (ctx->egl != EGL_NO_CONTEXT)
        eglDestroyContext(ctx->dpy, ctx->egl);
    if (ctx->dpy != EGL_NO_DISPLAY)
        eglTerminate(ctx->dpy);
    free(ctx);
}

int pd_gl_read_pixels_rgba(int w, int h, unsigned char *out)
{
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out);
    return 0;
}

#elif defined(_WIN32)
#error "WGL offscreen context: TODO"
#else
#error "pd_gl_offscreen: unsupported platform (need __APPLE__, __linux__ or _WIN32)"
#endif
