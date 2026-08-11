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

/* Create a renderer bound to an offscreen context, w*h framebuffer,
 * perspective fovy (degrees; use 73.74 for setfov(90) parity with the
 * reference), near/far default 0.1/1000. Returns NULL on failure. */
pd_GLRenderer *pd_gl_renderer_create(int w, int h, double fovy);

/* Install the script's legacy GLSL @v/@f sources (may be NULL for the
 * default passthrough program). Sources are adapted and compiled; on
 * compile failure the previous program stays active and an error is
 * printed to stderr. */
void pd_gl_renderer_set_shaders(pd_GLRenderer *rd,
                                const char *vert_src, const char *frag_src);

/* Replay one frame's worth of commands into the FBO (the stream must
 * contain the CLEAR command the script issued). */
void pd_gl_renderer_render(pd_GLRenderer *rd, const GLCmdBuf *buf);

/* Read the framebuffer as RGBA8, w*h*4 bytes, bottom-left origin
 * (GL convention). Caller flips for image output. */
void pd_gl_renderer_read_rgba(pd_GLRenderer *rd, unsigned char *out);

void pd_gl_renderer_destroy(pd_GLRenderer *rd);

#ifdef __cplusplus
}
#endif
#endif
