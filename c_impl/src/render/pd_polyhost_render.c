/* pd_polyhost_render.c — polyhost variant that RECORDS gl* calls into a GLCmdBuf.
 *
 * Reuses all the non-GPU host functions (printf/klock/srand/rgb/...) from
 * pd_polyhost.c by re-declaring them here is messy; instead we duplicate only
 * the install function and provide recording versions of the gl* stubs.
 *
 * The non-gl symbols are registered by calling the base install first, then
 * OVERWRITING the gl* entries with recording versions. (pd_host_add_fn
 * appends; a later install overwrites by name-match in pd_host_install's
 * overload threading — but to keep it simple we just add the gl* recorders
 * AFTER the base and rely on last-wins lookup. See pd_host_find_fn.)
 *
 * Also registers the GL_ constants (GL_POLYGON etc.) as host variables so
 * scripts like `glBegin(GL_POLYGON)` resolve the constant. */
#include "pd_polyhost.h"
#include "glcmd.h"

#include <string.h>

/* the active command buffer (single-threaded, set at install) */
static GLCmdBuf *g_glbuf = NULL;
static pd_PolyState *g_rstate = NULL;

/* current vertex attrib state (sticky between glVertex calls, like GL) */
static double cur_color[4] = {1,1,1,1};
static double cur_texcoord[4] = {0,0,0,1};
static double cur_normal[3] = {0,0,1};

/* ---- recording host functions ---- */
static double rh_glBegin(int n, const double *a) {
    (void)n;
    glcmd_begin(g_glbuf, (int)(n>=1 ? a[0] : 0));
    return 0;
}
static double rh_glEnd(int n, const double *a) { (void)n; (void)a; glcmd_end(g_glbuf); return 0; }

static double rh_glVertex(int n, const double *a) {
    double x = n>=1 ? a[0] : 0;
    double y = n>=2 ? a[1] : 0;
    double z = n>=3 ? a[2] : 0;
    double w = n>=4 ? a[3] : 1;
    glcmd_vertex(g_glbuf, x, y, z, w);
    return 0;
}
static double rh_glColor(int n, const double *a) {
    cur_color[0] = n>=1 ? a[0] : 0;
    cur_color[1] = n>=2 ? a[1] : 0;
    cur_color[2] = n>=3 ? a[2] : 0;
    cur_color[3] = n>=4 ? a[3] : 1;
    GLCmd *c=glcmd_push(g_glbuf); if(c){c->op=GLCMD_COLOR; c->a=cur_color[0]; c->b=cur_color[1]; c->c=cur_color[2]; c->d=cur_color[3];}
    return 0;
}
static double rh_glTexCoord(int n, const double *a) {
    cur_texcoord[0] = n>=1 ? a[0] : 0;
    cur_texcoord[1] = n>=2 ? a[1] : 0;
    cur_texcoord[2] = n>=3 ? a[2] : 0;
    cur_texcoord[3] = n>=4 ? a[3] : 1;
    GLCmd *c=glcmd_push(g_glbuf); if(c){c->op=GLCMD_TEXCOORD; c->a=cur_texcoord[0]; c->b=cur_texcoord[1]; c->c=cur_texcoord[2]; c->d=cur_texcoord[3];}
    return 0;
}
static double rh_glNormal(int n, const double *a) {
    cur_normal[0] = n>=1 ? a[0] : 0;
    cur_normal[1] = n>=2 ? a[1] : 0;
    cur_normal[2] = n>=3 ? a[2] : 0;
    GLCmd *c=glcmd_push(g_glbuf); if(c){c->op=GLCMD_NORMAL; c->a=cur_normal[0]; c->b=cur_normal[1]; c->c=cur_normal[2];}
    return 0;
}
static double rh_glClear(int n, const double *a) { (void)n; (void)a; glcmd_clear(g_glbuf); return 0; }

/* glVertex in immediate mode must stamp the current color/texcoord/normal
 * AT THE TIME OF THE VERTEX (GL semantics). But our buffer records events in
 * order; the replayer applies color/texcoord state as it walks. So we don't
 * need to snapshot here — the replayer tracks sticky state. Good.
 *
 * HOWEVER: the replayer needs to know each vertex's color. Since glColor can
 * appear between glBegin and glEnd (per-vertex color), the replayer must
 * process COLOR records inline. That works because we emit them in order. */

static double rh_glPushMatrix(int n, const double *a) { (void)n;(void)a;
    GLCmd *c=glcmd_push(g_glbuf); if(c){c->op=GLCMD_PUSHMATRIX;} return 0; }
static double rh_glPopMatrix(int n, const double *a) { (void)n;(void)a;
    GLCmd *c=glcmd_push(g_glbuf); if(c){c->op=GLCMD_POPMATRIX;} return 0; }
static double rh_glTranslate(int n, const double *a) { (void)n;
    GLCmd *c=glcmd_push(g_glbuf); if(c){c->op=GLCMD_TRANSLATE; c->a=n>=1?a[0]:0; c->b=n>=2?a[1]:0; c->c=n>=3?a[2]:0;} return 0; }
static double rh_glRotate(int n, const double *a) { (void)n;
    GLCmd *c=glcmd_push(g_glbuf); if(c){c->op=GLCMD_ROTATE; c->a=n>=1?a[0]:0; c->b=n>=2?a[1]:0; c->c=n>=3?a[2]:0; c->d=n>=4?a[3]:0;} return 0; }
static double rh_glScale(int n, const double *a) { (void)n;
    GLCmd *c=glcmd_push(g_glbuf); if(c){c->op=GLCMD_SCALE; c->a=n>=1?a[0]:1; c->b=n>=2?a[1]:1; c->c=n>=3?a[2]:1;} return 0; }
static double rh_glEnable(int n, const double *a) { (void)n;
    GLCmd *c=glcmd_push(g_glbuf); if(c){c->op=GLCMD_ENABLE; c->mode=(int)(n>=1?a[0]:0);} return 0; }
static double rh_glDisable(int n, const double *a) { (void)n;
    GLCmd *c=glcmd_push(g_glbuf); if(c){c->op=GLCMD_DISABLE; c->mode=(int)(n>=1?a[0]:0);} return 0; }
static double rh_glQuad(int n, const double *a) { (void)n;
    GLCmd *c=glcmd_push(g_glbuf); if(c){c->op=GLCMD_QUAD; c->a=n>=1?a[0]:0;} return 0; }
static double rh_glLineWidth(int n, const double *a) { (void)n;
    GLCmd *c=glcmd_push(g_glbuf); if(c){c->op=GLCMD_LINEWIDTH; c->a=n>=1?a[0]:1;} return 0; }
static double rh_glCullFace(int n, const double *a) { (void)n;
    GLCmd *c=glcmd_push(g_glbuf); if(c){c->op=GLCMD_CULLFACE; c->mode=(int)(n>=1?a[0]:0);} return 0; }

/* GL_ constants as static doubles (registered as host vars). */
static double c_GL_POINTS = PDGL_POINTS;
static double c_GL_LINES = PDGL_LINES;
static double c_GL_LINE_LOOP = PDGL_LINE_LOOP;
static double c_GL_LINE_STRIP = PDGL_LINE_STRIP;
static double c_GL_TRIANGLES = PDGL_TRIANGLES;
static double c_GL_TRIANGLE_STRIP = PDGL_TRIANGLE_STRIP;
static double c_GL_TRIANGLE_FAN = PDGL_TRIANGLE_FAN;
static double c_GL_QUADS = PDGL_QUADS;
static double c_GL_QUAD_STRIP = PDGL_QUAD_STRIP;
static double c_GL_POLYGON = PDGL_POLYGON;
static double c_GL_DEPTH_TEST = 0x0B71;
static double c_GL_NONE = 0;
static double c_GL_FRONT = 0x0404;
static double c_GL_BACK = 0x0405;
static double c_GL_FRONT_AND_BACK = 0x0408;

void pd_polyhost_install_render(pd_Host *h, pd_PolyState *s, GLCmdBuf *glbuf) {
    g_rstate = s;
    g_glbuf = glbuf;
    /* start from the base (non-gl) install */
    pd_polyhost_install(h, s);
    /* Now OVERWRITE the gl* entries. pd_host_add_fn appends a new entry; the
     * parser's pd_host_install threads overloads so the LAST same-name entry
     * wins in sym_find. To be safe we overwrite in-place: find each gl* slot
     * and replace its fn. */
    for (int i = 0; i < h->nFns; i++) {
        const char *nm = h->fns[i].name;
        if      (strcmp(nm,"GLBEGIN")==0)        h->fns[i].fn = rh_glBegin;
        else if (strcmp(nm,"GLEND")==0)          h->fns[i].fn = rh_glEnd;
        else if (strcmp(nm,"GLVERTEX")==0)       h->fns[i].fn = rh_glVertex;
        else if (strcmp(nm,"GLCOLOR")==0)        h->fns[i].fn = rh_glColor;
        else if (strcmp(nm,"GLTEXCOORD")==0)     h->fns[i].fn = rh_glTexCoord;
        else if (strcmp(nm,"GLNORMAL")==0)       h->fns[i].fn = rh_glNormal;
        else if (strcmp(nm,"GLCLEAR")==0)        h->fns[i].fn = rh_glClear;
        else if (strcmp(nm,"GLPUSHMATRIX")==0)   h->fns[i].fn = rh_glPushMatrix;
        else if (strcmp(nm,"GLPOPMATRIX")==0)    h->fns[i].fn = rh_glPopMatrix;
        else if (strcmp(nm,"GLTRANSLATE")==0)    h->fns[i].fn = rh_glTranslate;
        else if (strcmp(nm,"GLROTATE")==0)       h->fns[i].fn = rh_glRotate;
        else if (strcmp(nm,"GLSCALE")==0)        h->fns[i].fn = rh_glScale;
        else if (strcmp(nm,"GLENABLE")==0)       h->fns[i].fn = rh_glEnable;
        else if (strcmp(nm,"GLDISABLE")==0)      h->fns[i].fn = rh_glDisable;
        else if (strcmp(nm,"GLQUAD")==0)         h->fns[i].fn = rh_glQuad;
        else if (strcmp(nm,"GLLINEWIDTH")==0)    h->fns[i].fn = rh_glLineWidth;
        else if (strcmp(nm,"GLCULLFACE")==0)     h->fns[i].fn = rh_glCullFace;
    }
    /* GL_ constants */
    pd_host_add_var(h, "GL_POINTS",         &c_GL_POINTS);
    pd_host_add_var(h, "GL_LINES",          &c_GL_LINES);
    pd_host_add_var(h, "GL_LINE_LOOP",      &c_GL_LINE_LOOP);
    pd_host_add_var(h, "GL_LINE_STRIP",     &c_GL_LINE_STRIP);
    pd_host_add_var(h, "GL_TRIANGLES",      &c_GL_TRIANGLES);
    pd_host_add_var(h, "GL_TRIANGLE_STRIP", &c_GL_TRIANGLE_STRIP);
    pd_host_add_var(h, "GL_TRIANGLE_FAN",   &c_GL_TRIANGLE_FAN);
    pd_host_add_var(h, "GL_QUADS",          &c_GL_QUADS);
    pd_host_add_var(h, "GL_QUAD_STRIP",     &c_GL_QUAD_STRIP);
    pd_host_add_var(h, "GL_POLYGON",        &c_GL_POLYGON);
    pd_host_add_var(h, "GL_DEPTH_TEST",     &c_GL_DEPTH_TEST);
    pd_host_add_var(h, "GL_NONE",           &c_GL_NONE);
    pd_host_add_var(h, "GL_FRONT",          &c_GL_FRONT);
    pd_host_add_var(h, "GL_BACK",           &c_GL_BACK);
    pd_host_add_var(h, "GL_FRONT_AND_BACK", &c_GL_FRONT_AND_BACK);
}
