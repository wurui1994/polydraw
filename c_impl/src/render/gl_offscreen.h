/* gl_offscreen.h — platform offscreen GL context (mirrors moderngl's
 * create_standalone_context). No window, no display server needed:
 *   - macOS:  CGL (kCGLOGLPVersion_3_2_Core profile, no drawable)
 *   - Linux:  EGL surfaceless (EGL_MESA_platform_surfaceless / pbuffer)
 *   - Windows: WGL (hidden window) — TODO
 * GL functions come from the platform headers directly (macOS <OpenGL/gl3.h>
 * exposes the core-profile API; EGL path uses GLAD).
 */
#ifndef PD_GL_OFFSCREEN_H
#define PD_GL_OFFSCREEN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pd_GLOSCtx pd_GLOSCtx;

/* Create an offscreen GL context (core profile 3.2+, matching moderngl).
 * Returns NULL on failure. */
pd_GLOSCtx *pd_gl_offscreen_create(void);

/* Make the context current on this thread (required before GL calls). */
void pd_gl_offscreen_make_current(pd_GLOSCtx *ctx);

/* Destroy the context. */
void pd_gl_offscreen_destroy(pd_GLOSCtx *ctx);

/* Convenience: read the current framebuffer (bottom-left origin) into
 * an RGBA8 buffer of w*h*4 bytes. Returns 0 on success. */
int pd_gl_read_pixels_rgba(int w, int h, unsigned char *out);

#ifdef __cplusplus
}
#endif
#endif
