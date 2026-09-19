#ifndef POLYDRAW_JIT_H
#define POLYDRAW_JIT_H

#ifdef __cplusplus
extern "C" {
#endif

int pd_jit_available(void);
const char *pd_jit_status(void);
int pd_jit_smoke_value(void);
int pd_jit_run_subset(const char *source, double *out_return, char *output, int output_size);

#ifdef __cplusplus
}
#endif

#endif
