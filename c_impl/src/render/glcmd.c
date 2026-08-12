/* glcmd.c — see glcmd.h. A trivial growable array of GLCmd. */
#include "glcmd.h"
#include <stdlib.h>
#include <string.h>

void glcmd_init(GLCmdBuf *buf) {
    buf->cmds = NULL; buf->n = 0; buf->cap = 0;
}

void glcmd_free(GLCmdBuf *buf) {
    /* GLCMD_UNIFORM (array form) owns its float buffer (cmd->s); free it.
     * Other ops keep host-/EVAL-owned pointers in s and must not be freed. */
    for (size_t i = 0; i < buf->n; i++)
        if (buf->cmds[i].op == GLCMD_UNIFORM && buf->cmds[i].s)
            free((void *)buf->cmds[i].s);
    free(buf->cmds);
    buf->cmds = NULL; buf->n = 0; buf->cap = 0;
}

void glcmd_reset(GLCmdBuf *buf) {
    for (size_t i = 0; i < buf->n; i++)
        if (buf->cmds[i].op == GLCMD_UNIFORM && buf->cmds[i].s)
            free((void *)buf->cmds[i].s);
    buf->n = 0; /* keep allocation */
}

void glcmd_reserve(GLCmdBuf *buf, size_t need) {
    if (buf->cap >= need) return;
    size_t nc = buf->cap ? buf->cap : 256;
    while (nc < need) nc *= 2;
    GLCmd *nb = (GLCmd*)realloc(buf->cmds, nc * sizeof(GLCmd));
    if (!nb) return; /* out of memory: silently keep old; push will guard */
    buf->cmds = nb;
    buf->cap = nc;
}

GLCmd *glcmd_push(GLCmdBuf *buf) {
    glcmd_reserve(buf, buf->n + 1);
    if (buf->n >= buf->cap) return NULL;
    GLCmd *c = &buf->cmds[buf->n++];
    /* Fully initialise every field. `s` in particular MUST be cleared: slots
     * are recycled across frames by glcmd_reset (which keeps the allocation),
     * so a stale `s` from a previous frame would otherwise leak into commands
     * that never set it (VERTEX/COLOR/...). That both breaks bit-exact buffer
     * comparison and risks glcmd_reset/free treating foreign bytes as an owned
     * GLCMD_UNIFORM float array. */
    c->op = 0; c->mode = 0; c->a = c->b = c->c = c->d = 0; c->s = NULL;
    return c;
}

void glcmd_begin(GLCmdBuf *buf, int mode) {
    GLCmd *c = glcmd_push(buf); if (!c) return;
    c->op = GLCMD_BEGIN; c->mode = mode;
}

void glcmd_end(GLCmdBuf *buf) {
    GLCmd *c = glcmd_push(buf); if (!c) return;
    c->op = GLCMD_END;
}

void glcmd_vertex(GLCmdBuf *buf, double x, double y, double z, double w) {
    GLCmd *c = glcmd_push(buf); if (!c) return;
    c->op = GLCMD_VERTEX; c->a = x; c->b = y; c->c = z; c->d = w;
}

void glcmd_color(GLCmdBuf *buf, double r, double g, double b, double a) {
    GLCmd *c = glcmd_push(buf); if (!c) return;
    c->op = GLCMD_COLOR; c->a = r; c->b = g; c->c = b; c->d = a;
}

void glcmd_clear(GLCmdBuf *buf) {
    GLCmd *c = glcmd_push(buf); if (!c) return;
    c->op = GLCMD_CLEAR;
}
