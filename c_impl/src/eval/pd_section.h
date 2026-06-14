/* pd_section.h — split a .pss file into host/vertex/geometry/fragment blocks.
 *
 * Mirrors polydraw.c's txt2sec() (polydraw.c:1741). A .pss file contains
 * blocks marked by lines starting with @h/@v/@g/@f. The host block (default,
 * at top, or marked with @h) is compiled by the EVAL compiler; the others
 * are passed as GLSL to the GPU.
 */
#ifndef PD_SECTION_H
#define PD_SECTION_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PD_SEC_HOST = 0,
    PD_SEC_VERTEX,
    PD_SEC_GEOMETRY,
    PD_SEC_FRAGMENT
} pd_SecType;

typedef struct {
    pd_SecType type;
    char  name[32];     /* shader name (after :) or "" */
    size_t start;       /* byte offset into the source */
    size_t end;         /* exclusive end */
    int    line;        /* 1-based line number where block starts */
    /* geometry-shader-specific (parsed from @g,GL_IN,GL_OUT,N:name) */
    int    geo_in, geo_out, geo_nverts;
} pd_Section;

#define PD_MAX_SECTIONS 64

typedef struct {
    pd_Section secs[PD_MAX_SECTIONS];
    int        nSecs;
    char       err[128];
} pd_SectionList;

/* Parse a .pss source into sections. Returns 1 on success, 0 on error. */
int pd_section_parse(pd_SectionList *sl, const char *src);

/* Get the host block (the last @h or the default top block). Returns NULL
 * if none. The returned pointer is into sl->secs (valid while sl lives). */
const pd_Section *pd_section_host(const pd_SectionList *sl);

/* Find a shader block by type and name (NULL name = first of type). */
const pd_Section *pd_section_find(const pd_SectionList *sl, pd_SecType type, const char *name);

#ifdef __cplusplus
}
#endif
#endif
