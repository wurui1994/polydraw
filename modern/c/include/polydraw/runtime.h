#ifndef POLYDRAW_RUNTIME_H
#define POLYDRAW_RUNTIME_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PdTraceRuntime PdTraceRuntime;
typedef double (*PdRuntimeExternalFn)(PdTraceRuntime *rt, void *user, const char *name, const double *args, int argc);
typedef struct PdHostProgram PdHostProgram;

typedef struct PdTraceRuntime {
  char output[4096];
  int output_len;
  double numframes;
  double xres;
  double yres;
  double start_seconds;
  double glklock_start_seconds;
  int glklock_active;
  PdRuntimeExternalFn call_external;
  void *external_user;
  const char *string_args[64];
  int string_arg_count;
} PdTraceRuntime;

const char *pd_trace_runtime_string_arg(PdTraceRuntime *rt, double value);

void pd_trace_runtime_init(PdTraceRuntime *rt);
int pd_run_host_subset(const char *source, PdTraceRuntime *rt, double *out_return);
PdHostProgram *pd_host_program_compile_subset(const char *source, PdTraceRuntime *rt);
void pd_host_program_free(PdHostProgram *host);
int pd_host_program_run_subset(PdHostProgram *host, PdTraceRuntime *rt, double *out_return);

#ifdef __cplusplus
}
#endif

#endif
