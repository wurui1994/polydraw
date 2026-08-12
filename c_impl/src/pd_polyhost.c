/* pd_polyhost.c — default PolyDraw host functions.
 *
 * Implements the subset of polydraw host symbols that don't require a GPU:
 * printf, printg (no-op for now), klock, srand/sleep, rgb/rgba, and the
 * state variables xres/yres/mousx/mousy/bstatus/keystatus/numframes.
 * GPU functions (glBegin, glVertex, ...) are registered as no-op stubs so
 * host scripts compile; the graphics layer replaces them in a later phase.
 */
#include "pd_polyhost.h"
#include "eval/pd_interp.h"   /* pd_srand */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>

void pd_polystate_init(pd_PolyState *s) {
    memset(s, 0, sizeof(*s));
    s->xres = 640; s->yres = 480;
    s->mousx = 640 / 2; s->mousy = 480 / 2;   /* original starts the cursor at window center */
    s->startTime = (double)clock() / CLOCKS_PER_SEC;
}

/* ---- helper: log a formatted string to the state's log buffer or stdout ---- */
static void poly_log(pd_PolyState *s, const char *str, size_t len) {
    if (!s->logBuf) { fwrite(str, 1, len, stdout); return; }
    if (s->logLen + len + 1 > s->logCap) {
        size_t nc = s->logCap ? s->logCap * 2 : 1024;
        while (nc < s->logLen + len + 1) nc *= 2;
        s->logBuf = realloc(s->logBuf, nc);
        s->logCap = nc;
    }
    memcpy(s->logBuf + s->logLen, str, len);
    s->logLen += len;
    s->logBuf[s->logLen] = 0;
}

/* ---- host function implementations ----
 * Each takes (h, nargs, args[]) where h is the owning host (carries per-ctx
 * state) and args are raw doubles. For string args (printf format), the host
 * casts the double bits to char*. */

static double hf_printf(pd_Host *h, int n, const double *a) {
    /* args[0] = char* format (bit-cast), rest = doubles */
    if (n < 1) return 0;
    const char *fmt; memcpy(&fmt, &a[0], sizeof(void*));
    char buf[1024];
    /* simple printf: substitute %f/%g/%d with successive args */
    size_t bi = 0; int ai = 1;
    for (const char *p = fmt; *p && bi < sizeof(buf)-16; p++) {
        if (*p != '%') { buf[bi++] = *p; continue; }
        p++;
        if (*p == 0) break;
        char spec = *p;
        if (ai < n) {
            double v = a[ai++];
            if (spec == 'f') bi += snprintf(buf+bi, sizeof(buf)-bi, "%f", v);
            else if (spec == 'g' || spec == 'G') bi += snprintf(buf+bi, sizeof(buf)-bi, "%g", v);
            else if (spec == 'e' || spec == 'E') bi += snprintf(buf+bi, sizeof(buf)-bi, "%e", v);
            else if (spec == 'd') bi += snprintf(buf+bi, sizeof(buf)-bi, "%d", (int)v);
            else if (spec == 'x') bi += snprintf(buf+bi, sizeof(buf)-bi, "%x", (unsigned)(int)v);
            else if (spec == 'c') bi += snprintf(buf+bi, sizeof(buf)-bi, "%c", (int)v);
            else if (spec == 's') {
                /* arg is char* */
                const char *s; memcpy(&s, &v, sizeof(void*));
                bi += snprintf(buf+bi, sizeof(buf)-bi, "%s", s ? s : "(null)");
            }
            else if (spec == '%') { buf[bi++] = '%'; ai--; }
            else { buf[bi++] = '%'; buf[bi++] = spec; ai--; }
        } else {
            buf[bi++] = '%'; buf[bi++] = spec;
        }
    }
    buf[bi] = 0;
    poly_log(h->state, buf, bi);
    return 0;
}

static double hf_printg(pd_Host *h, int n, const double *a) { (void)h;(void)n; (void)a; return 0; } /* TODO: GPU text */
static double hf_klock(pd_Host *h, int n, const double *a) {
    /* Deterministic mode: if clockScale > 0, return numframes*clockScale
     * (so renders are reproducible across runs for golden diffing). */
    double now;
    if (h->state->clockScale > 0.0) now = h->state->numframes * h->state->clockScale;
    else                            now = (double)clock() / CLOCKS_PER_SEC - h->state->startTime;
    if (n >= 1 && a[0] != 0.0) return now;  /* date/time components: approx */
    return now;
}
static double hf_srand(pd_Host *h, int n, const double *a) { (void)h; if (n>=1) pd_srand((unsigned long)a[0]); return 0; }
static double hf_sleep(pd_Host *h, int n, const double *a) { (void)h;(void)n; (void)a; return 0; } /* no-op in headless */
static double hf_rgb(pd_Host *h, int n, const double *a) {
    (void)h;
    if (n < 3) return 0;
    int r = a[0]<0?0:a[0]>255?255:(int)a[0];
    int g = a[1]<0?0:a[1]>255?255:(int)a[1];
    int b = a[2]<0?0:a[2]>255?255:(int)a[2];
    return (double)((r<<16)|(g<<8)|b);
}
static double hf_rgba(pd_Host *h, int n, const double *a) {
    (void)h;
    if (n < 4) return 0;
    int r=a[0]<0?0:a[0]>255?255:(int)a[0];
    int g=a[1]<0?0:a[1]>255?255:(int)a[1];
    int b=a[2]<0?0:a[2]>255?255:(int)a[2];
    int al=a[3]<0?0:a[3]>255?255:(int)a[3];
    return (double)(((unsigned)al<<24)|(r<<16)|(g<<8)|b);
}
static double hf_noise(pd_Host *h, int n, const double *a) {
    (void)h;
    /* simple placeholder noise (not Tom's, but deterministic) */
    double s = 0;
    for (int i = 0; i < n; i++) s = s * 12.9898 + a[i] * 78.233;
    s = sin(s) * 43758.5453;
    return s - floor(s);
}
static double hf_playnote(pd_Host *h, int n, const double *a) { (void)h;(void)n; (void)a; return 0; } /* no audio in headless */

/* GPU stubs — all no-ops returning 0 */
static double hf_gl_noop(pd_Host *h, int n, const double *a) { (void)h;(void)n; (void)a; return 0; }

void pd_polyhost_install(pd_Host *h, pd_PolyState *s) {
    pd_host_init(h);   /* zeros fns/vars/state/glbuf/cur_* — safe to reset */
    h->state = s;      /* re-bind state AFTER init so it survives the memset */

    /* state variables (host reads them like globals) */
    pd_host_add_var(h, "XRES",      &s->xres);
    pd_host_add_var(h, "YRES",      &s->yres);
    pd_host_add_var(h, "MOUSX",     &s->mousx);
    pd_host_add_var(h, "MOUSY",     &s->mousy);
    pd_host_add_var(h, "BSTATUS",   &s->bstatus);
    pd_host_add_var(h, "NUMFRAMES", &s->numframes);
    /* keystatus is an array — register first element; array access via host
     * would need special handling. For now scripts using keystatus[i] won't
     * work until we add host-array support. */

    /* utility functions */
    pd_host_add_fn(h, "PRINTF($,.)",   hf_printf,  1);
    pd_host_add_fn(h, "PRINTG(,,,$,.)",hf_printg,  1);
    pd_host_add_fn(h, "KLOCK()",       hf_klock,   1); /* variadic: 0 or 1 arg */
    pd_host_add_fn(h, "SRAND()",       hf_srand,   0);
    pd_host_add_fn(h, "SLEEP()",       hf_sleep,   0);
    pd_host_add_fn(h, "RGB(,,)",       hf_rgb,     0);
    pd_host_add_fn(h, "RGBA(,,,)",     hf_rgba,    0);
    pd_host_add_fn(h, "NOISE()",       hf_noise,   1);
    pd_host_add_fn(h, "NOISE(,)",      hf_noise,   1);
    pd_host_add_fn(h, "NOISE(,,)",     hf_noise,   1);
    pd_host_add_fn(h, "PLAYNOTE(,,)",  hf_playnote,0);

    /* GL constant-like no-arg symbols (treated as 0 until graphics wired) */
    /* GL begin/end/draw */
    pd_host_add_fn(h, "GLBEGIN()",         hf_gl_noop, 0);
    pd_host_add_fn(h, "GLEND()",           hf_gl_noop, 0);
    pd_host_add_fn(h, "GLVERTEX()",        hf_gl_noop, 0);
    pd_host_add_fn(h, "GLVERTEX(,)",       hf_gl_noop, 0);
    pd_host_add_fn(h, "GLVERTEX(,,)",      hf_gl_noop, 0);
    pd_host_add_fn(h, "GLVERTEX(,,,)",     hf_gl_noop, 0);
    pd_host_add_fn(h, "GLTEXCOORD(,)",     hf_gl_noop, 0);
    pd_host_add_fn(h, "GLTEXCOORD(,,)",    hf_gl_noop, 0);
    pd_host_add_fn(h, "GLTEXCOORD(,,,)",   hf_gl_noop, 0);
    pd_host_add_fn(h, "GLCOLOR(,,)",       hf_gl_noop, 0);
    pd_host_add_fn(h, "GLCOLOR(,,,)",      hf_gl_noop, 0);
    pd_host_add_fn(h, "GLNORMAL(,,)",      hf_gl_noop, 0);
    pd_host_add_fn(h, "GLPUSHMATRIX()",    hf_gl_noop, 0);
    pd_host_add_fn(h, "GLPOPMATRIX()",     hf_gl_noop, 0);
    pd_host_add_fn(h, "GLTRANSLATE(,,)",   hf_gl_noop, 0);
    pd_host_add_fn(h, "GLROTATE(,,,)",     hf_gl_noop, 0);
    pd_host_add_fn(h, "GLSCALE(,,)",       hf_gl_noop, 0);
    pd_host_add_fn(h, "GLCLEAR()",         hf_gl_noop, 0);
    pd_host_add_fn(h, "GLQUAD()",          hf_gl_noop, 0);
    pd_host_add_fn(h, "GLENABLE()",        hf_gl_noop, 0);
    pd_host_add_fn(h, "GLDISABLE()",       hf_gl_noop, 0);
    pd_host_add_fn(h, "GLCLEAR()",         hf_gl_noop, 0);
}
