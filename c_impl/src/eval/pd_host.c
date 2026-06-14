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
static int parse_proto(const char *proto, char *nameOut, size_t nameCap, int *nParamsOut) {
    size_t i = 0, ni = 0;
    while (proto[i] && proto[i] != '(' && ni < nameCap - 1) nameOut[ni++] = proto[i++];
    nameOut[ni] = 0;
    /* uppercase */
    for (size_t k = 0; k < ni; k++) if (nameOut[k] >= 'a' && nameOut[k] <= 'z') nameOut[k] -= 32;
    int n = 0;
    if (proto[i] == '(') {
        i++;
        if (proto[i] == ')') { n = 0; }
        else {
            n = 1;
            while (proto[i] && proto[i] != ')') { if (proto[i] == ',') n++; i++; }
        }
    }
    *nParamsOut = n;
    return 0;
}

int pd_host_add_fn(pd_Host *h, const char *proto, pd_HostFn fn, int variadic) {
    if (h->nFns >= PD_MAX_HOST_FNS) return -1;
    char name[40]; int n;
    parse_proto(proto, name, sizeof(name), &n);
    pd_HostFunc *f = &h->fns[h->nFns];
    strncpy(f->name, name, sizeof(f->name)-1);
    f->name[sizeof(f->name)-1] = 0;
    f->nParams = n;
    f->fn = fn;
    f->variadic = variadic;
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
    /* host variables: store double* bit-cast in a const slot, fam=EXT */
    for (int i = 0; i < h->nVars; i++) {
        pd_Sym *s = pd_parser_sym_add(p, h->vars[i].name, PD_SYM_EXT_VAR);
        if (!s) continue;
        double dptr; void *pp = (void*)h->vars[i].pVal;
        memcpy(&dptr, &pp, sizeof(void*));
        s->reg = pd_new_const(p->b, dptr);
        s->reg.fam = PD_FAM_EXT;
    }
    /* host functions: register as EXT_FUNC with funcIdx = host fn index */
    for (int i = 0; i < h->nFns; i++) {
        pd_Sym *prev = pd_parser_sym_find(p, h->fns[i].name);
        pd_Sym *s = pd_parser_sym_add(p, h->fns[i].name, PD_SYM_EXT_FUNC);
        if (!s) continue;
        s->nParams = h->fns[i].nParams;
        s->funcIdx = i;
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
