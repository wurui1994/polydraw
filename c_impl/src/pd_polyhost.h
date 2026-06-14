/* pd_polyhost.h — default PolyDraw host functions (printf, klock, xres, etc).
 *
 * Provides a pd_Host pre-populated with the host symbols a polydraw .pss
 * host script expects (see polydraw.txt). GPU functions (glBegin etc.) are
 * stubs that compile but do nothing until the graphics layer is wired.
 */
#ifndef PD_POLYHOST_H
#define PD_POLYHOST_H

#include "eval/pd_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/* State the host functions read/write. The host application updates these
 * each frame (xres/yres from window size, mousx/mousy from cursor, etc). */
typedef struct {
    double xres, yres;
    double mousx, mousy;
    double bstatus;
    double numframes;
    double keystatus[256];   /* indexed by scancode */
    /* accumulated time since compile (seconds) */
    double startTime;
    /* printf output buffer (or NULL = stdout) */
    char  *logBuf;
    size_t logLen, logCap;
} pd_PolyState;

void pd_polystate_init(pd_PolyState *s);

/* Populate a host table with all default polydraw symbols. The state's
 * lifetime must outlive the host table. */
void pd_polyhost_install(pd_Host *h, pd_PolyState *s);

#ifdef __cplusplus
}
#endif
#endif
