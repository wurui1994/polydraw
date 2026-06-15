/* glcmd.h — record immediate-mode GL calls into a flat command buffer.
 *
 * When the EVAL interpreter runs a .pss host block, the host's gl* extern
 * functions record their arguments here instead of calling a real GL. A
 * downstream renderer (Python vispy / C GL / JS WebGL2) replays the buffer.
 *
 * This is the cross-backend contract: Python reference renderer, C offscreen,
 * and JS WebGL2 all consume the same GLCmd stream.
 *
 * Primitive-type constants match OpenGL (see Plan/05_Graphics.md). */
#ifndef PD_GLCMD_H
#define PD_GLCMD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GL primitive-type constants (canonical OpenGL values). */
#define PDGL_POINTS          0x0000
#define PDGL_LINES           0x0001
#define PDGL_LINE_LOOP       0x0002
#define PDGL_LINE_STRIP      0x0003
#define PDGL_TRIANGLES       0x0004
#define PDGL_TRIANGLE_STRIP  0x0005
#define PDGL_TRIANGLE_FAN    0x0006
#define PDGL_QUADS           0x0007
#define PDGL_QUAD_STRIP      0x0008
#define PDGL_POLYGON         0x0009

/* command opcodes */
typedef enum {
    GLCMD_CLEAR = 0,     /* a=r,g=b, c=b (GL_COLOR_BUFFER_BIT only for now) */
    GLCMD_BEGIN,         /* mode = primitive type */
    GLCMD_END,
    GLCMD_VERTEX,        /* a,b,c,d = x,y,z,w (w defaults 1) */
    GLCMD_COLOR,         /* a,b,c,d = r,g,b,a (a defaults 1) */
    GLCMD_TEXCOORD,      /* a,b,c,d = s,t,p,q */
    GLCMD_NORMAL,        /* a,b,c */
    GLCMD_PUSHMATRIX,
    GLCMD_POPMATRIX,
    GLCMD_TRANSLATE,     /* a,b,c */
    GLCMD_ROTATE,        /* a=angle, b,c,d = x,y,z axis */
    GLCMD_SCALE,         /* a,b,c */
    GLCMD_MATRIXMODE,    /* mode = 0 modelview, 1 projection */
    GLCMD_LOADIDENTITY,
    GLCMD_PERSPECTIVE,   /* a=fovy, b=aspect, c=near, d=far */
    GLCMD_ORTHO,         /* a,b = left,right; c,d = bottom,top (near=−1,far=1) */
    GLCMD_VIEWPORT,      /* a,b = w,h (x=y=0) */
    GLCMD_QUAD,          /* fullscreen quad, a = mode (0 alpha,1 opaque) */
    GLCMD_ENABLE,        /* mode = cap (e.g. GL_DEPTH_TEST) */
    GLCMD_DISABLE,       /* mode = cap */
    GLCMD_BLENDFUNC,     /* mode = sf<<16|df (encoded) */
    GLCMD_CULLFACE,      /* mode = face */
    GLCMD_LINEWIDTH      /* a = width */
} GLCmdOp;

/* One recorded command. Fixed 48 bytes for easy ctypes mapping. */
typedef struct {
    int    op;       /* GLCmdOp */
    int    mode;     /* primitive type / cap / etc (op-dependent) */
    double a, b, c, d;
} GLCmd;

typedef struct GLCmdBuf {
    GLCmd  *cmds;
    size_t  n, cap;
} GLCmdBuf;

void glcmd_init(GLCmdBuf *buf);
void glcmd_free(GLCmdBuf *buf);
void glcmd_reset(GLCmdBuf *buf);
void glcmd_reserve(GLCmdBuf *buf, size_t need);

/* emit returns a pointer to the freshly pushed slot (caller fills fields) */
GLCmd *glcmd_push(GLCmdBuf *buf);

/* convenience emitters */
void glcmd_begin(GLCmdBuf *buf, int mode);
void glcmd_end(GLCmdBuf *buf);
void glcmd_vertex(GLCmdBuf *buf, double x, double y, double z, double w);
void glcmd_color(GLCmdBuf *buf, double r, double g, double b, double a);
void glcmd_clear(GLCmdBuf *buf);

#ifdef __cplusplus
}
#endif
#endif
