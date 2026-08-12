/* Reference GLCmd dumper for cross-backend differential testing (M7).
 * Loads a .pss, runs one frame via the C EVAL+host pipeline, and prints a
 * JSON object of GLCmd structural counts to stdout. The JS backend emits the
 * same counts; the differential test asserts equality. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "eval/pd_compile.h"
#include "eval/pd_interp.h"
#include "eval/pd_section.h"
#include "render/pd_runlib.h"
#include "render/glcmd.h"

static char *read_file(const char *path, long *out_sz) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  char *buf = malloc(sz + 1); if (!buf) { fclose(f); return NULL; }
  if (fread(buf, 1, sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
  buf[sz] = 0; fclose(f); *out_sz = sz; return buf;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s file.pss [frame] [out.json]\n", argv[0]); return 2; }
  long sz = 0; char *src = read_file(argv[1], &sz);
  if (!src) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
  int frame = argc > 2 ? atoi(argv[2]) : 30;
  FILE *jout = stderr;
  if (argc > 3) { jout = fopen(argv[3], "w"); if (!jout) jout = stderr; }
  pd_SectionList sl; pd_section_parse(&sl, src);
  const pd_Section *hs = pd_section_host(&sl);
  size_t n = hs->end - hs->start; char *host = malloc(n + 1);
  memcpy(host, src + hs->start, n); host[n] = 0;
  char err[256]; pdrl_Ctx *ctx = pdrl_compile(host, 640, 480, err, sizeof(err));
  if (!ctx) { fprintf(stderr, "compile error: %s\n", err); free(src); free(host); return 1; }
  pdrl_set_clock_scale(ctx, 1.0 / 60.0); pd_srand(1);
  pdrl_run_frame(ctx, frame);
  const GLCmdBuf *g = pdrl_glbuf(ctx);
  long nverts = 0, nbegin = 0, nend = 0, npush = 0, npop = 0, nsettex = 0, ncolor = 0, ntexc = 0, nnorm = 0;
  for (size_t i = 0; i < g->n; i++) {
    int op = g->cmds[i].op;
    if (op == 3) nverts++; else if (op == 1) nbegin++; else if (op == 2) nend++;
    else if (op == 7) npush++; else if (op == 8) npop++; else if (op == 23) nsettex++;
    else if (op == 4) ncolor++; else if (op == 5) ntexc++; else if (op == 6) nnorm++;
  }
  fprintf(jout, "{\"total\":%zu,\"begin\":%ld,\"end\":%ld,\"vert\":%ld,\"push\":%ld,\"pop\":%ld,\"settex\":%ld,\"color\":%ld,\"texcoord\":%ld,\"normal\":%ld,\"full\":{",
    g->n, nbegin, nend, nverts, npush, npop, nsettex, ncolor, ntexc, nnorm);
  int first = 1;
  for (int op = 0; op <= 40; op++) {
    long cnt = 0;
    for (size_t i = 0; i < g->n; i++) if (g->cmds[i].op == op) cnt++;
    if (cnt == 0) continue;
    if (!first) fprintf(jout, ",");
    fprintf(jout, "\"%d\":%ld", op, cnt);
    first = 0;
  }
  fprintf(jout, "}}\n");
  if (jout != stderr) fclose(jout);
  free(src); free(host); return 0;
}
