/* pd_section.c — .pss section splitter. */
#include "pd_section.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int pd_section_parse(pd_SectionList *sl, const char *src) {
    memset(sl, 0, sizeof(*sl));
    size_t i = 0;
    int line = 1;
    /* The first block (before any @v/@g/@f/@h) is the host by default.
     * Track its start; when we hit a section marker, we close it. */
    size_t curStart = 0;
    int curLine = 1;
    pd_SecType curType = PD_SEC_HOST;

    /* Name of the block currently being built (set when we see `@x:name`).
     * It is applied to the block that starts at curStart when we push it. */
    char pendingName[32];
    pendingName[0] = 0;

    /* Helper to push the current block (carrying pendingName). */
    #define PUSH_BLOCK(end_) do { \
        if (sl->nSecs >= PD_MAX_SECTIONS) { snprintf(sl->err,sizeof(sl->err),"too many sections"); return 0; } \
        pd_Section *s = &sl->secs[sl->nSecs++]; \
        memset(s, 0, sizeof(*s)); \
        s->type = curType; s->start = curStart; s->end = (end_); s->line = curLine; \
        strncpy(s->name, pendingName, sizeof(s->name) - 1); \
        pendingName[0] = 0; \
    } while(0)

    while (src[i]) {
        /* detect line start: are we at column 0 (or only whitespace so far)? */
        int atLineStart = (i == 0) || (src[i-1] == '\n');
        if (atLineStart) {
            /* skip leading whitespace */
            size_t j = i;
            while (src[j] == ' ' || src[j] == '\t') j++;
            if (src[j] == '@') {
                /* section marker. Close the current block (if non-empty),
                 * then start a new one carrying the name parsed below. */
                size_t blockEnd = i;
                while (blockEnd > curStart && (src[blockEnd-1] == '\n' || src[blockEnd-1] == '\r')) blockEnd--;
                if (blockEnd > curStart) {
                    PUSH_BLOCK(blockEnd);
                }
                /* parse marker: @ followed by h/v/g/f, optional args, optional :name */
                char m = src[j+1];
                pd_SecType nt = curType;
                if (m == 'h' || m == 'H') nt = PD_SEC_HOST;
                else if (m == 'v' || m == 'V') nt = PD_SEC_VERTEX;
                else if (m == 'g' || m == 'G') nt = PD_SEC_GEOMETRY;
                else if (m == 'f' || m == 'F') nt = PD_SEC_FRAGMENT;
                /* default @ with no letter = same type as previous */
                curType = nt;
                curLine = line;
                pendingName[0] = 0;
                /* skip the marker letter (+ optional geometry args up to ':' / EOL) */
                size_t k = j + 1;
                while (src[k] && src[k] != '\n' && src[k] != ':') k++;
                if (src[k] == ':') {
                    k++;
                    size_t ni = 0;
                    while (src[k] && src[k] != '\n' && src[k] != ' ' && src[k] != '\t' &&
                           ni < sizeof(pendingName) - 1) {
                        pendingName[ni++] = src[k]; k++;
                    }
                    pendingName[ni] = 0;
                }
                /* skip the rest of the marker line */
                while (src[k] && src[k] != '\n') k++;
                curStart = k;
                i = k;
                continue;
            }
        }
        if (src[i] == '\n') line++;
        i++;
    }
    /* push final block */
    {
        size_t blockEnd = i;
        while (blockEnd > curStart && (src[blockEnd-1] == '\n' || src[blockEnd-1] == '\r')) blockEnd--;
        if (blockEnd > curStart) {
            PUSH_BLOCK(blockEnd);
        } else if (sl->nSecs == 0) {
            /* empty source — still push an empty host block */
            PUSH_BLOCK(0);
        }
    }
    #undef PUSH_BLOCK
    return 1;
}

const pd_Section *pd_section_host(const pd_SectionList *sl) {
    /* return the LAST host block (polydraw.txt:170: only 1 host allowed,
     * whichever comes last) */
    const pd_Section *last = NULL;
    for (int i = 0; i < sl->nSecs; i++)
        if (sl->secs[i].type == PD_SEC_HOST) last = &sl->secs[i];
    return last;
}

const pd_Section *pd_section_find(const pd_SectionList *sl, pd_SecType type, const char *name) {
    for (int i = 0; i < sl->nSecs; i++) {
        if (sl->secs[i].type != type) continue;
        if (!name || name[0] == 0) return &sl->secs[i];
        if (strcmp(sl->secs[i].name, name) == 0) return &sl->secs[i];
    }
    return NULL;
}
