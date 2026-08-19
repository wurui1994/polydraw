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

/* ---- noise3d: Ken Silverman style 3D value noise (evaldraw reference) ----
 * Interpolated value noise on an integer lattice: hash each lattice point to
 * a [0,1) value, then trilinearly interpolate with a smoothstep fade.
 * This matches the smooth organic look scripts like glwavy.kc expect
 * (noise3d(x*.21,y*.27,t*.33) displacing a height field). */

static unsigned ed_noise_rand(unsigned s) {
    /* Ken-style bit mixer: deterministic, decent distribution. */
    s ^= s >> 13; s *= 0x788a9ed9u; s ^= s >> 7; s *= 0x4a39b0d1u; s ^= s >> 11;
    return s;
}

static double ed_noise_val(int xi, int yi, int zi) {
    unsigned h = (unsigned)xi * 92837111u ^ (unsigned)yi * 689287499u ^
                 (unsigned)zi * 283923481u;
    return (double)(ed_noise_rand(h) & 0xFFFFFF) / (double)0xFFFFFF; /* [0,1] */
}

/* noise3d(x,y,z) — smooth interpolated 3D value noise, range [0,1]. */
static double hf_noise3d(pd_Host *h, int n, const double *a) {
    (void)h;
    if (n < 3)
        return 0;

    double x = a[0], y = a[1], z = a[2];
    int x0 = (int)floor(x), y0 = (int)floor(y), z0 = (int)floor(z);
    double fx = x - x0, fy = y - y0, fz = z - z0;

    /* smoothstep fade */
    double u = fx * fx * (3 - 2 * fx);
    double v = fy * fy * (3 - 2 * fy);
    double w = fz * fz * (3 - 2 * fz);

    /* trilinear interpolation of the 8 lattice values */
    double v000 = ed_noise_val(x0,     y0,     z0    );
    double v100 = ed_noise_val(x0 + 1, y0,     z0    );
    double v010 = ed_noise_val(x0,     y0 + 1, z0    );
    double v110 = ed_noise_val(x0 + 1, y0 + 1, z0    );
    double v001 = ed_noise_val(x0,     y0,     z0 + 1);
    double v101 = ed_noise_val(x0 + 1, y0,     z0 + 1);
    double v011 = ed_noise_val(x0,     y0 + 1, z0 + 1);
    double v111 = ed_noise_val(x0 + 1, y0 + 1, z0 + 1);

    double x00 = v000 + (v100 - v000) * u;
    double x10 = v010 + (v110 - v010) * u;
    double x01 = v001 + (v101 - v001) * u;
    double x11 = v011 + (v111 - v011) * u;

    double y0v = x00 + (x10 - x00) * v;
    double y1v = x01 + (x11 - x01) * v;
    return y0v + (y1v - y0v) * w;
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
    /* evaldraw printf() renders text at the cursor (console-style) using the
     * current font and color, and advances the cursor. \n moves to the next
     * line, \r returns to column 0, \t advances to the next 8-char stop. */
    {
        uint32_t col = ed_current_color(s);
        int fw = (int)(s->fontW > 0 ? s->fontW : 5);
        int fh = (int)(s->fontH > 0 ? s->fontH : 7);
        for (const char *p = buf; *p; p++) {
            switch (*p) {
                case '\n':
                    s->curY += fh;
                    break;
                case '\r':
                    s->curX = 0;
                    break;
                case '\t':
                    s->curX = ((int)(s->curX / fw) / 8 + 1) * 8 * fw;
                    break;
                default:
                    ed_draw_char(s, (int)s->curX, (int)s->curY, *p, col);
                    s->curX += fw;
                    break;
            }
        }
    }
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
    { "NOISE3D(,,)", hf_noise3d, 0 },
    { "PRINTF($,.)", hf_printf,  1 },
    { "SETGRID(,,)", hf_setgrid, 0 },
    { NULL, NULL, 0 }
};

const ed_FnReg *ed_get_misc_fns(void) {
    return ed_misc_fns;
}
