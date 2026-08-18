/* pd_host.c — host function registration & dispatch helpers. */
#include "pd_host.h"
#include "pd_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void pd_host_init(pd_Host *h) {
    memset(h, 0, sizeof(*h));
}

/* parse "NAME(,,)" → extract base name + param count. Returns 0 on success. */
static int parse_proto(const char *proto, char *nameOut, size_t nameCap, int *nParamsOut, int *refMaskOut) {
    size_t i = 0, ni = 0;
    while (proto[i] && proto[i] != '(' && ni < nameCap - 1) nameOut[ni++] = proto[i++];
    nameOut[ni] = 0;
    /* uppercase */
    for (size_t k = 0; k < ni; k++) if (nameOut[k] >= 'a' && nameOut[k] <= 'z') nameOut[k] -= 32;
    int n = 0, refMask = 0;
    if (proto[i] == '(') {
        i++;
        if (proto[i] == ')') { n = 0; }
        else {
            n = 1;
            while (proto[i] && proto[i] != ')') {
                if (proto[i] == ',') n++;
                else if (proto[i] == '$') refMask |= (1 << (n - 1));
                i++;
            }
        }
    }
    *nParamsOut = n;
    if (refMaskOut) *refMaskOut = refMask;
    return 0;
}

int pd_host_add_fn(pd_Host *h, const char *proto, pd_HostFn fn, int variadic) {
    if (h->nFns >= PD_MAX_HOST_FNS) return -1;
    char name[40]; int n, refMask = 0;
    parse_proto(proto, name, sizeof(name), &n, &refMask);
    pd_HostFunc *f = &h->fns[h->nFns];
    strncpy(f->name, name, sizeof(f->name)-1);
    f->name[sizeof(f->name)-1] = 0;
    f->nParams = n;
    f->fn = fn;
    f->variadic = variadic;
    f->isStub = 0;
    f->refMask = refMask;
    return h->nFns++;
}

int pd_host_add_var(pd_Host *h, const char *name, double *pVal) {
    if (h->nVars >= PD_MAX_HOST_VARS) return -1;
    pd_HostVar *v = &h->vars[h->nVars];
    size_t i = 0;
    for (; name[i] && i < sizeof(v->name)-1; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        v->name[i] = c;
    }
    v->name[i] = 0;
    v->pVal = pVal;
    return h->nVars++;
}

int pd_host_find_fn(const pd_Host *h, const char *name, int nargs) {
    /* exact name+arity; variadic fns match any nargs >= hint */
    for (int i = 0; i < h->nFns; i++) {
        if (strncmp(h->fns[i].name, name, sizeof(h->fns[i].name)) != 0) continue;
        if (h->fns[i].variadic) { if (nargs >= h->fns[i].nParams) return i; }
        else if (h->fns[i].nParams == nargs) return i;
    }
    /* fallback: name match ignoring arity */
    for (int i = 0; i < h->nFns; i++)
        if (strncmp(h->fns[i].name, name, sizeof(h->fns[i].name)) == 0) return i;
    return -1;
}

int pd_host_find_var(const pd_Host *h, const char *name) {
    for (int i = 0; i < h->nVars; i++)
        if (strncmp(h->vars[i].name, name, sizeof(h->vars[i].name)) == 0) return i;
    return -1;
}

void pd_host_install(const pd_Host *h, pd_Parser *p) {
    /* host variables: store double* bit-cast in a const slot, fam=EXT.
     * Variables are stored uppercased by pd_host_add_var, but scripts often
     * reference them in lowercase (e.g. `xres`/`yres`). Register both the
     * stored (uppercase) name and a lowercased alias pointing at the SAME
     * const slot, so lookups are case-insensitive. The alias SHARES the
     * uppercase symbol's EXT reg (no second const slot) to avoid the
     * pd_new_const dedupe corrupting the pointer into a numeric literal. */
    for (int i = 0; i < h->nVars; i++) {
        const char *nm = h->vars[i].name;
        pd_Sym *s = pd_parser_sym_add(p, nm, PD_SYM_EXT_VAR);
        if (!s) continue;
        double dptr; void *pp = (void*)h->vars[i].pVal;
        memcpy(&dptr, &pp, sizeof(void*));
        s->reg = pd_new_const_ptr(p->b, dptr);
        s->reg.fam = PD_FAM_EXT;
        /* lowercased alias shares the same reg */
        char low[64];
        int k = 0;
        for (; nm[k] && k < 63; k++)
            low[k] = (nm[k]>='A'&&nm[k]<='Z') ? nm[k]+32 : nm[k];
        low[k] = 0;
        if (strcmp(low, nm) != 0 && !pd_parser_sym_find(p, low)) {
            pd_Sym *sa = pd_parser_sym_add(p, low, PD_SYM_EXT_VAR);
            if (sa) sa->reg = s->reg;
        }
    }
    /* host functions: register as EXT_FUNC with funcIdx = host fn index */
    for (int i = 0; i < h->nFns; i++) {
        pd_Sym *prev = pd_parser_sym_find(p, h->fns[i].name);
        pd_Sym *s = pd_parser_sym_add(p, h->fns[i].name, PD_SYM_EXT_FUNC);
        if (!s) continue;
        s->nParams = h->fns[i].nParams;
        s->funcIdx = i;
        s->refMask = h->fns[i].refMask;
        s->reg = pdR(PD_FAM_EXT, 0);
        if (prev) {
            s->nextOverload = prev->nextOverload;
            prev->nextOverload = (int)(s - p->syms);
        }
    }
}

void pd_host_attach(pd_Program *prog, const pd_Host *h) {
    prog->host = h;
}

/* No-op host callback: a missing/unimplemented host function simply returns 0.
 * Used to gracefully compile scripts that call host APIs this software host
 * does not implement (e.g. PLAYSOUND, GLDISABLE, PIC) instead of emitting a
 * broken CALL that crashes at runtime. */
static double pd_host_stub_fn(pd_Host *h, int nargs, const double *args) {
    (void)h; (void)nargs; (void)args;
    return 0.0;
}

/* Register a missing host function as a no-op stub so call sites resolve to a
 * proper EXT_FUNC CALL (aux = -1000 - idx) that returns 0. Returns the host
 * fn index, or -1 if the table is full / name invalid. */
int pd_host_add_stub(pd_Host *h, const char *name, int nParams) {
    if (!h || !name || !*name) return -1;
    /* already present? reuse (keep a real impl if one was registered) */
    int existing = pd_host_find_fn(h, name, nParams);
    if (existing >= 0) {
        if (!h->fns[existing].isStub) return existing; /* real impl, keep it */
        return existing; /* already a stub, reuse */
    }
    char proto[48];
    int n = snprintf(proto, sizeof(proto), "%.*s(", 39, name);
    if (nParams > 0) { for (int i = 0; i < nParams; i++) proto[n++] = i + 1 < nParams ? ',' : ')'; }
    else proto[n++] = ')';
    proto[n] = 0;
    int idx = pd_host_add_fn((pd_Host*)h, proto, pd_host_stub_fn, 0);
    if (idx >= 0) {
        h->fns[idx].isStub = 1;
        /* Report once at registration (parse time) so a missing function is
         * visible even if the runtime call never executes (e.g. the script
         * crashes elsewhere first). A stub is only registered once per name,
         * so this is naturally deduplicated. */
        fprintf(stderr,
            "[pd_host] UNIMPLEMENTED host function '%s' referenced — stubbed as no-op (returns 0, no effect); needs real implementation\n",
            name);
    }
    return idx;
}
