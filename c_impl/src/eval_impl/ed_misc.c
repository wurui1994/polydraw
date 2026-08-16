/* ed_misc.c — evaldraw host: utility functions (klock, rgb, noise, etc). */
#include "ed_internal.h"
#include "eval/pd_interp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* klock([mode]) — return wall-clock time or calendar component. */
static double hf_klock(pd_Host *h, int n, const double *a) {
    ed_State *s = ST(h);

    if (n >= 1 && a[0] != 0.0) {
        int mode = (int)a[0];
        int abs_mode = mode < 0 ? -mode : mode;
        if (abs_mode >= 1 && abs_mode <= 9) {
            time_t t = time(NULL);
            struct tm *tmv = (mode < 0) ? gmtime(&t) : localtime(&t);
            if (!tmv)
                return 0;
            switch (abs_mode) {
                case 1:
                    return (double)((tmv->tm_year + 1900) * 10000000000LL +
                                   (tmv->tm_mon + 1) * 100000000 +
                                   tmv->tm_mday * 1000000 +
                                   tmv->tm_hour * 10000 +
                                   tmv->tm_min * 100 +
                                   tmv->tm_sec);
                case 2: return tmv->tm_year + 1900;
                case 3: return tmv->tm_mon + 1;
                case 4: return tmv->tm_wday;
                case 5: return tmv->tm_mday;
                case 6: return tmv->tm_hour;
                case 7: return tmv->tm_min;
                case 8: return tmv->tm_sec;
                case 9: return 0;
            }
        }
    }

    /* Default: elapsed seconds since start (or deterministic frame time). */
    if (s->clockScale > 0.0)
        return s->numframes * s->clockScale;
    return (double)clock() / CLOCKS_PER_SEC - s->startTime;
}

/* srand(seed) — seed the EVAL random number generator. */
static double hf_srand(pd_Host *h, int n, const double *a) {
    (void)h;
    if (n >= 1)
        pd_srand((unsigned long)a[0]);
    return 0;
}

/* sleep(seconds) — no-op in headless mode. */
static double hf_sleep(pd_Host *h, int n, const double *a) {
    (void)h; (void)n; (void)a;
    return 0;
}

/* refresh() — request a screen refresh. No-op in headless mode. */
static double hf_refresh(pd_Host *h, int n, const double *a) {
    ed_State *s = ST(h);
    s->refreshCount++;
    return 0;
}

/* rgb(r,g,b) — pack color components into a single integer. */
static double hf_rgb(pd_Host *h, int n, const double *a) {
    (void)h;
    if (n < 3)
        return 0;
    return (double)ed_pack_rgb((int)a[0], (int)a[1], (int)a[2]);
}

/* rgba(r,g,b,a) — pack color with alpha (stored in high byte). */
static double hf_rgba(pd_Host *h, int n, const double *a) {
    (void)h;
    if (n < 4)
        return 0;
    int r = ed_clamp((int)a[0], 0, 255);
    int g = ed_clamp((int)a[1], 0, 255);
    int b = ed_clamp((int)a[2], 0, 255);
    int alpha = ed_clamp((int)a[3], 0, 255);
    return (double)(((uint32_t)alpha << 24) | ((uint32_t)r << 16) |
                    ((uint32_t)g << 8) | (uint32_t)b);
}

/* noise(x[,y[,z]]) — simple value noise (placeholder: hash-based). */
static double hf_noise(pd_Host *h, int n, const double *a) {
    (void)h;
    if (n < 1)
        return 0;

    /* Simple hash-based noise. Not as good as Ken's, but functional. */
    double x = a[0];
    double y = (n >= 2) ? a[1] : 0;
    double z = (n >= 3) ? a[2] : 0;

    /* Use sin-based pseudo-noise for now. */
    double val = sin(x * 12.9898 + y * 78.233 + z * 37.719) * 43758.5453;
    double frac = val - floor(val);
    return frac * 2.0 - 1.0; /* range [-1, 1] */
}

/* printf(fmt, ...) — formatted output to log buffer or stdout. */
static double hf_printf(pd_Host *h, int n, const double *a) {
    ed_State *s = ST(h);
    if (n < 1)
        return 0;

    /* First argument is a format string (stored as raw pointer in double). */
    const char *fmt;
    memcpy(&fmt, &a[0], sizeof(void *));
    if (!fmt)
        return 0;

    char buf[1024];
    size_t bi = 0;
    int ai = 1;

    for (const char *p = fmt; *p && bi < sizeof(buf) - 16; p++) {
        if (*p != '%') {
            buf[bi++] = *p;
            continue;
        }
        p++;
        if (!*p)
            break;

        char spec = *p;
        if (ai < n) {
            double v = a[ai++];
            switch (spec) {
                case 'f': bi += snprintf(buf + bi, sizeof(buf) - bi, "%f", v); break;
                case 'g':
                case 'G': bi += snprintf(buf + bi, sizeof(buf) - bi, "%g", v); break;
                case 'e':
                case 'E': bi += snprintf(buf + bi, sizeof(buf) - bi, "%e", v); break;
                case 'd': bi += snprintf(buf + bi, sizeof(buf) - bi, "%d", (int)v); break;
                case 'x': bi += snprintf(buf + bi, sizeof(buf) - bi, "%x", (unsigned)(int)v); break;
                case 'c': bi += snprintf(buf + bi, sizeof(buf) - bi, "%c", (int)v); break;
                case 's': {
                    const char *sv;
                    memcpy(&sv, &v, sizeof(void *));
                    bi += snprintf(buf + bi, sizeof(buf) - bi, "%s", sv ? sv : "(null)");
                    break;
                }
                case '%':
                    buf[bi++] = '%';
                    ai--;
                    break;
                default:
                    buf[bi++] = '%';
                    buf[bi++] = spec;
                    ai--;
                    break;
            }
        } else {
            buf[bi++] = '%';
            buf[bi++] = spec;
        }
    }
    buf[bi] = '\0';
    ed_log(s, buf, bi);
    return 0;
}

/* setgrid(x0,y0,x1,y1) — set grid scale for 1D/2D graphing modes. */
static double hf_setgrid(pd_Host *h, int n, const double *a) {
    ed_State *s = ST(h);
    if (n >= 4) {
        s->gridX0 = a[0];
        s->gridY0 = a[1];
        s->gridX1 = a[2];
        s->gridY1 = a[3];
    }
    return 0;
}

/* ---- registration ---- */

typedef struct { const char *sig; pd_HostFn fn; int variadic; } ed_FnReg;

static const ed_FnReg ed_misc_fns[] = {
    { "KLOCK()",     hf_klock,   0 },
    { "SRAND()",     hf_srand,   0 },
    { "SLEEP()",     hf_sleep,   0 },
    { "REFRESH()",   hf_refresh, 0 },
    { "RGB(,,)",     hf_rgb,     0 },
    { "RGBA(,,)",    hf_rgba,    0 },
    { "NOISE(,,)",   hf_noise,   0 },
    { "PRINTF($,.)", hf_printf,  1 },
    { "SETGRID(,,)", hf_setgrid, 0 },
    { NULL, NULL, 0 }
};

const ed_FnReg *ed_get_misc_fns(void) {
    return ed_misc_fns;
}
