/* gl_renderer.h — replay a GLCmd stream through a real GL (core profile).
 *
 * Mirrors the Python reference renderer (pyref/software_renderer.py) but on
 * actual GL hardware, exactly the way moderngl would drive it:
 *   - GLCmd stream → interleaved vertex buffer (pos/color/texcoord/normal)
 *     → VBO/VAO → glDrawArrays(GL_TRIANGLES)
 *   - legacy GLSL (@v/@f blocks) adapted to GLSL 330 core
 *   - gluPerspective projection (modelview identity by default, full
 *     immediate-mode matrix stack supported)
 *   - render into an FBO, glReadPixels back
 *
 * The alpha semantics match the reference: RGB is overwritten (src=ONE,
 * dst=ZERO) and alpha accumulates as max(src,dst), replicating the numpy
 * reference's fb write rule.
 */
#ifndef PD_GL_RENDERER_H
#define PD_GL_RENDERER_H

#include "glcmd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pd_GLRenderer pd_GLRenderer;

#ifdef __APPLE__
#  include <OpenGL/gl.h>
#else
#  include <GL/gl.h>
#endif


/* Create a renderer bound to an offscreen context, w*h framebuffer,
 * perspective fovy (degrees; use 73.74 for setfov(90) parity with the
 * reference), near/far default 0.1/1000. Returns NULL on failure.
 *
 * own_offscreen != 0 (default for polydraw-render / headless view): create and
 * own a standalone offscreen GL context (CGL/EGL). own_offscreen == 0: assume
 * the caller has ALREADY made a GL context current (e.g. a GLFW window) and
 * build all GL objects into it — used by the interactive window viewer, where
 * the same GLFW context must both own the FBO and receive the blit to the
 * visible framebuffer. Mixing two contexts (offscreen + window) silently
 * yields a black window, which is why the window path must not create its own. */
pd_GLRenderer *pd_gl_renderer_create(int w, int h, double fovy);
pd_GLRenderer *pd_gl_renderer_create_ex(int w, int h, double fovy, int own_offscreen);

/* Install the script's legacy GLSL @v/@f sources (may be NULL for the
 * default passthrough program). Sources are adapted and compiled; on
 * compile failure the previous program stays active and an error is
 * printed to stderr. */
void pd_gl_renderer_set_shaders(pd_GLRenderer *rd,
                                const char *vert_src, const char *frag_src);

/* Replay directly into the window's default framebuffer (0) instead of the
 * offscreen FBO. Used by the interactive viewer so the scene lands on the
 * visible surface with no blit. No-op for readback-based (headless) users. */
void pd_gl_renderer_set_render_to_default(pd_GLRenderer *rd, int on);

/* Set the real drawable pixel size (for HiDPI/Retina the default-framebuffer
 * is larger than the logical w/h). Used for the viewport in render_to_default
 * mode so the image fills the window. */
void pd_gl_renderer_set_framebuffer_size(pd_GLRenderer *rd, int fbw, int fbh);
void pd_gl_renderer_get_framebuffer_size(const pd_GLRenderer *rd, int *fbw, int *fbh);

/* Replay one frame's worth of commands into the FBO (the stream must
 * contain the CLEAR command the script issued). */
void pd_gl_renderer_render(pd_GLRenderer *rd, const GLCmdBuf *buf);

/* Read the framebuffer as RGBA8, w*h*4 bytes, bottom-left origin
 * (GL convention). Caller flips for image output. */
void pd_gl_renderer_read_rgba(pd_GLRenderer *rd, unsigned char *out);

/* Compile + link a GLSL program (used by the window viewer for its display
 * quad). Returns a GL program object, or 0 on failure. */
GLuint pd_gl_link_program(const char *vert_src, const char *frag_src);

/* Context ownership helpers: when the renderer owns its GL context
 * (own_offscreen != 0), make it current / release it. Used by the window
 * viewer, which renders in this context then flips to a GLFW window context
 * to blit the read-back pixels. No-op when the caller owns the context. */
void pd_gl_renderer_acquire(pd_GLRenderer *rd);
void pd_gl_renderer_release(pd_GLRenderer *rd);

/* Return the internal offscreen FBO the renderer draws into, so an external
 * windowing layer (e.g. GLFW) can blit it to the visible framebuffer. */
unsigned int pd_gl_renderer_fbo(pd_GLRenderer *rd);

void pd_gl_renderer_destroy(pd_GLRenderer *rd);

#ifdef __cplusplus
}
#endif
#endif
