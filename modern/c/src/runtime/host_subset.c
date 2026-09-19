#include "polydraw/runtime.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct Var {
  char name[64];
  double value;
  struct Var *ref;
} Var;

typedef struct Env {
  Var vars[512];
  int count;
  double *arrays[64];
  double *array_aliases[64];
  char array_names[64][64];
  int array_sizes[64];
  int array_owned[64];
  int array_count;
  unsigned int rng_state;
  PdTraceRuntime *rt;
} Env;

typedef struct Function {
  char name[64];
  char params[16][64];
  int param_ref[16];
  int param_array[16];
  int param_count;
  char lines[512][512];
  int line_count;
} Function;

typedef struct Program {
  char main_lines[1024][512];
  int main_count;
  Function functions[128];
  int function_count;
} Program;

typedef struct ExecCtx {
  Program *program;
  Env *global;
} ExecCtx;

struct PdHostProgram {
  Program *program;
  Env env;
};

static ExecCtx *g_exec_ctx = NULL;

static double monotonic_seconds(void) {
#if defined(CLOCK_MONOTONIC)
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
#else
  return (double)clock() / (double)CLOCKS_PER_SEC;
#endif
}

static int wall_milliseconds(void) {
#if defined(CLOCK_REALTIME)
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (int)(ts.tv_nsec / 1000000);
#else
  return 0;
#endif
}

static double klock_value(PdTraceRuntime *rt, double arg) {
  int mode = (int)arg;
  double now_seconds = monotonic_seconds();
  if (!mode) return now_seconds - rt->start_seconds;
  if (mode <= -10 || mode >= 10) return 0.0;
  time_t now = time(NULL);
  struct tm tmv;
  if (mode < 0) {
    mode = -mode;
#if defined(_WIN32)
    gmtime_s(&tmv, &now);
#else
    gmtime_r(&now, &tmv);
#endif
  } else {
#if defined(_WIN32)
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
  }
  switch (mode) {
    case 1:
      return ((double)(tmv.tm_year + 1900) * 10000000000000.0 +
        (double)(tmv.tm_mon + 1) * 100000000000.0 +
        (double)tmv.tm_mday * 1000000000.0 +
        (double)tmv.tm_hour * 10000000.0 +
        (double)tmv.tm_min * 100000.0 +
        (double)tmv.tm_sec * 1000.0 +
        (double)wall_milliseconds()) * 0.001;
    case 2: return (double)(tmv.tm_year + 1900);
    case 3: return (double)(tmv.tm_mon + 1);
    case 4: return (double)tmv.tm_wday;
    case 5: return (double)tmv.tm_mday;
    case 6: return (double)tmv.tm_hour;
    case 7: return (double)tmv.tm_min;
    case 8: return (double)tmv.tm_sec;
    case 9: return (double)wall_milliseconds();
    default: return 0.0;
  }
}

static double glklock_start(PdTraceRuntime *rt) {
  rt->glklock_start_seconds = monotonic_seconds();
  rt->glklock_active = 1;
  return 0.0;
}

static double glklock_elapsed(PdTraceRuntime *rt) {
  if (!rt->glklock_active) return -2.0;
  rt->glklock_active = 0;
  return monotonic_seconds() - rt->glklock_start_seconds;
}

static void canon(char *dst, size_t size, const char *src) {
  size_t i = 0;
  for (; src[i] && i + 1 < size; i++) dst[i] = (char)toupper((unsigned char)src[i]);
  dst[i] = 0;
}

static Var *var_get(Env *env, const char *name) {
  char key[64];
  canon(key, sizeof(key), name);
  if (!strcmp(key, "NUMFRAMES")) {
    static Var v;
    strcpy(v.name, "NUMFRAMES");
    v.value = env->rt->numframes;
    return &v;
  }
  if (!strcmp(key, "XRES")) {
    static Var v;
    strcpy(v.name, "XRES");
    v.value = env->rt->xres;
    return &v;
  }
  if (!strcmp(key, "YRES")) {
    static Var v;
    strcpy(v.name, "YRES");
    v.value = env->rt->yres;
    return &v;
  }
  if (!strcmp(key, "PI")) {
    static Var v;
    strcpy(v.name, "PI");
    v.value = 3.14159265358979323846;
    return &v;
  }
  if (!strcmp(key, "GL_POLYGON")) {
    static Var v;
    strcpy(v.name, "GL_POLYGON");
    v.value = 9.0;
    return &v;
  }
  if (!strcmp(key, "GL_TRIANGLE_FAN")) {
    static Var v;
    strcpy(v.name, "GL_TRIANGLE_FAN");
    v.value = 6.0;
    return &v;
  }
  for (int i = 0; i < env->count; i++) if (!strcmp(env->vars[i].name, key)) return env->vars[i].ref ? env->vars[i].ref : &env->vars[i];
  Var *v = &env->vars[env->count++];
  strcpy(v->name, key);
  v->value = 0.0;
  return v;
}

static int array_find(Env *env, const char *name) {
  char key[64];
  canon(key, sizeof(key), name);
  for (int i = 0; i < env->array_count; i++) if (!strcmp(env->array_names[i], key)) return i;
  return -1;
}

static int array_create(Env *env, const char *name, int size) {
  if (env->array_count >= (int)(sizeof(env->array_names) / sizeof(env->array_names[0]))) return -1;
  int idx = env->array_count++;
  canon(env->array_names[idx], sizeof(env->array_names[idx]), name);
  if (size <= 0) size = 1;
  env->array_sizes[idx] = size;
  env->array_aliases[idx] = NULL;
  env->arrays[idx] = (double *)calloc((size_t)size, sizeof(double));
  env->array_owned[idx] = 1;
  return idx;
}

static double *array_data(Env *env, int idx) {
  return env->array_aliases[idx] ? env->array_aliases[idx] : env->arrays[idx];
}

static int array_bind_alias(Env *env, const char *name, double *data, int size) {
  if (env->array_count >= (int)(sizeof(env->array_names) / sizeof(env->array_names[0]))) return -1;
  int idx = env->array_count++;
  canon(env->array_names[idx], sizeof(env->array_names[idx]), name);
  env->array_sizes[idx] = size > 1024 ? 1024 : size;
  env->array_aliases[idx] = data;
  env->arrays[idx] = NULL;
  env->array_owned[idx] = 0;
  return idx;
}

static void env_free(Env *env) {
  if (!env) return;
  for (int i = 0; i < env->array_count; i++) {
    if (env->array_owned[i]) free(env->arrays[i]);
    env->arrays[i] = NULL;
    env->array_aliases[i] = NULL;
  }
  env->array_count = 0;
}

static double env_rand01(Env *env) {
  env->rng_state = env->rng_state * 1664525u + 1013904223u;
  return (double)(env->rng_state >> 8) / 16777216.0;
}

static void append_output(PdTraceRuntime *rt, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  int n = vsnprintf(rt->output + rt->output_len, sizeof(rt->output) - (size_t)rt->output_len, fmt, va);
  va_end(va);
  if (n > 0) rt->output_len += n;
}

static const char *skip_ws(const char *p) {
  while (*p && isspace((unsigned char)*p)) p++;
  return p;
}

static double parse_expr(Env *env, const char **pp);
static double call_function(ExecCtx *ctx, Env *caller, const char *name, double *args, Var **arg_refs, char arg_arrays[4][64], int argc);

static int truthy(double v) {
  return v != 0.0;
}

static double parse_primary(Env *env, const char **pp) {
  const char *p = skip_ws(*pp);
  if (*p == '"') {
    p++;
    static char string_slots[64][256];
    int slot = env->rt->string_arg_count++ % 64;
    int n = 0;
    while (*p && *p != '"' && n < 255) {
      if (*p == '\\' && p[1]) p++;
      string_slots[slot][n++] = *p++;
    }
    string_slots[slot][n] = 0;
    if (*p == '"') p++;
    env->rt->string_args[slot] = string_slots[slot];
    *pp = p;
    return -1000000.0 - (double)slot;
  }
  if (*p == '(') {
    p++;
    double v = parse_expr(env, &p);
    p = skip_ws(p);
    if (*p == ')') p++;
    *pp = p;
    return v;
  }
  if (isdigit((unsigned char)*p) || *p == '.') {
    char *end = NULL;
    double v = strtod(p, &end);
    *pp = end;
    return v;
  }
  if (isalpha((unsigned char)*p) || *p == '_') {
    char name[64];
    int n = 0;
    while ((isalnum((unsigned char)*p) || *p == '_') && n < 63) name[n++] = *p++;
    name[n] = 0;
    p = skip_ws(p);
    if (*p == '(') {
      double args[4] = {0, 0, 0, 0};
      Var *arg_refs[4] = {NULL, NULL, NULL, NULL};
      char arg_arrays[4][64] = {{0}};
      int argc = 0;
      p++;
      while (*p && *p != ')' && argc < 4) {
        p = skip_ws(p);
        if (*p == '&') {
          p++;
          char ref_name[64];
          int rn = 0;
          while ((isalnum((unsigned char)*p) || *p == '_') && rn < 63) ref_name[rn++] = *p++;
          ref_name[rn] = 0;
          arg_refs[argc] = rn ? var_get(env, ref_name) : NULL;
          args[argc] = arg_refs[argc] ? arg_refs[argc]->value : 0.0;
          argc++;
        } else if (isalpha((unsigned char)*p) || *p == '_') {
          const char *save = p;
          char maybe_name[64];
          int mn = 0;
          while ((isalnum((unsigned char)*p) || *p == '_') && mn < 63) maybe_name[mn++] = *p++;
          maybe_name[mn] = 0;
          const char *after_name = skip_ws(p);
          if ((*after_name == ',' || *after_name == ')') && array_find(env, maybe_name) >= 0) {
            canon(arg_arrays[argc], sizeof(arg_arrays[argc]), maybe_name);
            args[argc] = 0.0;
            argc++;
            p = after_name;
          } else {
            p = save;
            args[argc++] = parse_expr(env, &p);
          }
        } else {
          args[argc++] = parse_expr(env, &p);
        }
        p = skip_ws(p);
        if (*p == ',') p++;
      }
      if (*p == ')') p++;
      *pp = p;
      char key[64];
      canon(key, sizeof(key), name);
      if (!strcmp(key, "SIN")) return sin(args[0]);
      if (!strcmp(key, "COS")) return cos(args[0]);
      if (!strcmp(key, "TAN")) return tan(args[0]);
      if (!strcmp(key, "SQRT") || !strcmp(key, "SQR")) return sqrt(args[0]);
      if (!strcmp(key, "ABS") || !strcmp(key, "FABS")) return fabs(args[0]);
      if (!strcmp(key, "INT")) return trunc(args[0]);
      if (!strcmp(key, "MIN")) return argc > 1 ? fmin(args[0], args[1]) : args[0];
      if (!strcmp(key, "MAX")) return argc > 1 ? fmax(args[0], args[1]) : args[0];
      if (!strcmp(key, "POW")) return argc > 1 ? pow(args[0], args[1]) : args[0];
      if (!strcmp(key, "FMOD")) return argc > 1 ? fmod(args[0], args[1]) : 0.0;
      if (!strcmp(key, "KLOCK")) return klock_value(env->rt, argc > 0 ? args[0] : 0.0);
      if (!strcmp(key, "GLKLOCKSTART")) return glklock_start(env->rt);
      if (!strcmp(key, "GLKLOCKELAPSED")) return glklock_elapsed(env->rt);
      if (env->rt->call_external) return env->rt->call_external(env->rt, env->rt->external_user, key, args, argc);
      if (g_exec_ctx) return call_function(g_exec_ctx, env, key, args, arg_refs, arg_arrays, argc);
      return 0.0;
    }
    if (*p == '[') {
      p++;
      int ai = array_find(env, name);
      int index = (int)parse_expr(env, &p);
      p = skip_ws(p);
      if (*p == ']') p++;
      *pp = p;
      if (ai < 0) return 0.0;
      int size = env->array_sizes[ai];
      if (size && (size & (size - 1)) == 0) index &= size - 1;
      else if (index < 0 || index >= size) index = 0;
      return array_data(env, ai)[index];
    }
    *pp = p;
    char key[64];
    canon(key, sizeof(key), name);
    if (!strcmp(key, "RND")) return env_rand01(env);
    if (!strcmp(key, "NRND")) return env_rand01(env) * 2.0 - 1.0;
    return var_get(env, name)->value;
  }
  *pp = p;
  return 0.0;
}

static double parse_unary(Env *env, const char **pp) {
  const char *p = skip_ws(*pp);
  if (*p == '+') { p++; *pp = p; return parse_unary(env, pp); }
  if (*p == '-') { p++; *pp = p; return -parse_unary(env, pp); }
  return parse_primary(env, pp);
}

static double parse_power(Env *env, const char **pp) {
  double v = parse_unary(env, pp);
  const char *p = skip_ws(*pp);
  if (*p == '^') {
    p++;
    *pp = p;
    v = pow(v, parse_power(env, pp));
  }
  return v;
}

static double parse_mul(Env *env, const char **pp) {
  double v = parse_power(env, pp);
  for (;;) {
    const char *p = skip_ws(*pp);
    if (*p == '*') { p++; *pp = p; v *= parse_power(env, pp); }
    else if (*p == '/') { p++; *pp = p; v /= parse_power(env, pp); }
    else if (*p == '%') { p++; *pp = p; v = fmod(v, parse_power(env, pp)); }
    else break;
  }
  return v;
}

static double parse_add(Env *env, const char **pp) {
  double v = parse_mul(env, pp);
  for (;;) {
    const char *p = skip_ws(*pp);
    if (*p == '+') { p++; *pp = p; v += parse_mul(env, pp); }
    else if (*p == '-') { p++; *pp = p; v -= parse_mul(env, pp); }
    else break;
  }
  return v;
}

static double parse_cmp(Env *env, const char **pp) {
  double v = parse_add(env, pp);
  const char *p = skip_ws(*pp);
  if (!strncmp(p, "==", 2)) { p += 2; *pp = p; return v == parse_add(env, pp); }
  if (!strncmp(p, "!=", 2)) { p += 2; *pp = p; return v != parse_add(env, pp); }
  if (!strncmp(p, "<=", 2)) { p += 2; *pp = p; return v <= parse_add(env, pp); }
  if (!strncmp(p, ">=", 2)) { p += 2; *pp = p; return v >= parse_add(env, pp); }
  if (*p == '<') { p++; *pp = p; return v < parse_add(env, pp); }
  if (*p == '>') { p++; *pp = p; return v > parse_add(env, pp); }
  return v;
}

static double parse_and(Env *env, const char **pp) {
  double v = parse_cmp(env, pp);
  for (;;) {
    const char *p = skip_ws(*pp);
    if (strncmp(p, "&&", 2)) break;
    p += 2;
    *pp = p;
    v = truthy(v) && truthy(parse_cmp(env, pp)) ? 1.0 : 0.0;
  }
  return v;
}

static double parse_expr(Env *env, const char **pp) {
  double v = parse_and(env, pp);
  for (;;) {
    const char *p = skip_ws(*pp);
    if (strncmp(p, "||", 2)) break;
    p += 2;
    *pp = p;
    v = truthy(v) || truthy(parse_and(env, pp)) ? 1.0 : 0.0;
  }
  return v;
}

static void assign_target(Env *env, const char *target, double value) {
  char name[64];
  int n = 0;
  const char *p = skip_ws(target);
  while ((isalnum((unsigned char)*p) || *p == '_') && n < 63) name[n++] = *p++;
  name[n] = 0;
  p = skip_ws(p);
  if (*p == '[') {
    p++;
    int ai = array_find(env, name);
    if (ai < 0) ai = array_create(env, name, 1024);
    int index = (int)parse_expr(env, &p);
    int size = env->array_sizes[ai];
    if (size && (size & (size - 1)) == 0) index &= size - 1;
    else if (index < 0 || index >= size) index = 0;
    array_data(env, ai)[index] = value;
  } else {
    var_get(env, name)->value = value;
  }
}

static void exec_static_decl(Env *env, char *line) {
  char *s = line;
  while (*s && isspace((unsigned char)*s)) s++;
  if (strncmp(s, "static", 6)) return;
  s += 6;
  char *semi = strchr(s, ';');
  if (semi) *semi = 0;
  char *part = strtok(s, ",");
  while (part) {
    char *p = part;
    while (*p && isspace((unsigned char)*p)) p++;
    char name[64];
    int n = 0;
    while ((isalnum((unsigned char)*p) || *p == '_') && n < 63) name[n++] = *p++;
    name[n] = 0;
    p = (char *)skip_ws(p);
    if (name[0]) {
      if (*p == '[') {
        p++;
        int size = (int)parse_expr(env, (const char **)&p);
        if (size <= 0) size = 1024;
        if (array_find(env, name) < 0) array_create(env, name, size);
      } else {
        var_get(env, name);
      }
    }
    part = strtok(NULL, ",");
  }
}

static void exec_enum_decl(Env *env, char *line) {
  char *open = strchr(line, '{');
  char *close = open ? strchr(open, '}') : NULL;
  if (!open || !close) return;
  *close = 0;
  double value = 0.0;
  char *part = strtok(open + 1, ",");
  while (part) {
    char *p = part;
    while (*p && isspace((unsigned char)*p)) p++;
    char name[64];
    int n = 0;
    while ((isalnum((unsigned char)*p) || *p == '_') && n < 63) name[n++] = *p++;
    name[n] = 0;
    p = (char *)skip_ws(p);
    if (*p == '=') {
      p++;
      const char *expr = p;
      value = parse_expr(env, &expr);
    }
    if (name[0]) var_get(env, name)->value = value++;
    part = strtok(NULL, ",");
  }
}

static void exec_line(Env *env, const char *line, double *ret, int *has_ret) {
  char buf[512];
  strncpy(buf, line, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  char *comment = strstr(buf, "//");
  if (comment) *comment = 0;
  char *p = buf;
  while (*p && isspace((unsigned char)*p)) p++;
  if (!*p || *p == '{' || *p == '}') return;
  if (!strncmp(p, "static", 6)) { exec_static_decl(env, p); return; }
  if (!strncmp(p, "enum", 4)) { exec_enum_decl(env, p); return; }
  if (!strncmp(p, "if", 2)) return;
  char *colon = strchr(p, ':');
  if (colon) {
    char *q = p;
    int label = 1;
    while (q < colon) {
      if (!isalnum((unsigned char)*q) && *q != '_') { label = 0; break; }
      q++;
    }
    if (label) return;
  }
  if (!strncmp(p, "for", 3)) {
    char *open = strchr(p, '(');
    char *close = strrchr(p, ')');
    if (!open || !close || close < open) return;
    *close = 0;
    char *first = strchr(open + 1, ';');
    char *second = first ? strchr(first + 1, ';') : NULL;
    if (!first || !second) return;
    *first = 0;
    *second = 0;
    double inner_ret = 0.0;
    int inner_has_ret = 0;
    exec_line(env, open + 1, &inner_ret, &inner_has_ret);
    char *body = close + 1;
    while (*body && isspace((unsigned char)*body)) body++;
    int guard = 0;
    for (;;) {
      const char *cond = first + 1;
      if (!truthy(parse_expr(env, &cond))) break;
      exec_line(env, body, ret, has_ret);
      if (*has_ret) return;
      exec_line(env, second + 1, &inner_ret, &inner_has_ret);
      if (++guard > 1000000) break;
    }
    return;
  }
  if (!strncmp(p, "return", 6)) {
    const char *expr = p + 6;
    *ret = parse_expr(env, &expr);
    *has_ret = 1;
    return;
  }
  if (!strncasecmp(p, "printf", 6)) {
    const char *comma = strchr(p, ',');
    if (comma) {
      comma++;
      double v = parse_expr(env, &comma);
      append_output(env->rt, "%f\n", v);
    }
    return;
  }
  char *op = strstr(p, "+=");
  if (op) {
    *op = 0;
    const char *expr = op + 2;
    double old = parse_expr(env, (const char **)&p);
    assign_target(env, buf, old + parse_expr(env, &expr));
    return;
  }
  op = strstr(p, "++");
  if (op) {
    *op = 0;
    const char *target = buf;
    double old = parse_expr(env, &target);
    assign_target(env, buf, old + 1.0);
    return;
  }
  op = strstr(p, "--");
  if (op) {
    *op = 0;
    const char *target = buf;
    double old = parse_expr(env, &target);
    assign_target(env, buf, old - 1.0);
    return;
  }
  op = strchr(p, '=');
  if (op) {
    *op = 0;
    const char *expr = op + 1;
    assign_target(env, buf, parse_expr(env, &expr));
    return;
  }
  const char *expr = p;
  parse_expr(env, &expr);
}

static void trim_line(char *s) {
  char *comment = strstr(s, "//");
  if (comment) *comment = 0;
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
  char *p = s;
  while (*p && isspace((unsigned char)*p)) p++;
  if (p != s) memmove(s, p, strlen(p) + 1);
}

static int parse_function_header(const char *line, char *name, size_t name_size, char params[16][64], int param_ref[16], int param_array[16], int *param_count) {
  const char *open = strchr(line, '(');
  const char *close = open ? strrchr(open, ')') : NULL;
  if (!open || !close || close < open) return 0;
  const char *after_close = close + 1;
  while (*after_close && isspace((unsigned char)*after_close)) after_close++;
  if (*after_close == ';') return 0;
  const char *p = open;
  while (p > line && isspace((unsigned char)p[-1])) p--;
  const char *end = p;
  while (p > line && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) p--;
  if (p == end) return 0;
  char prefix[128];
  size_t prefix_len = (size_t)(p - line);
  if (prefix_len >= sizeof(prefix)) prefix_len = sizeof(prefix) - 1;
  memcpy(prefix, line, prefix_len);
  prefix[prefix_len] = 0;
  trim_line(prefix);
  if (strchr(prefix, '=') || !strncasecmp(prefix, "if", 2) || !strncasecmp(prefix, "for", 3) || !strncasecmp(prefix, "while", 5) || !strncasecmp(prefix, "printf", 6)) return 0;
  size_t len = (size_t)(end - p);
  if (len >= name_size) len = name_size - 1;
  memcpy(name, p, len);
  name[len] = 0;
  char key[64];
  canon(key, sizeof(key), name);
  if (!strcmp(key, "IF") || !strcmp(key, "FOR") || !strcmp(key, "WHILE") || !strcmp(key, "DO") || !strcmp(key, "RETURN") || !strcmp(key, "PRINTF")) return 0;
  *param_count = 0;
  const char *q = open + 1;
  while (q < close && *param_count < 16) {
    while (q < close && (isspace((unsigned char)*q) || *q == ',')) q++;
    int is_ref = 0;
    if (*q == '&') {
      is_ref = 1;
      q++;
      while (q < close && isspace((unsigned char)*q)) q++;
    }
    char param[64];
    int n = 0;
    while (q < close && (isalnum((unsigned char)*q) || *q == '_') && n < 63) param[n++] = *q++;
    param[n] = 0;
    if (n) {
      canon(params[*param_count], 64, param);
      param_ref[*param_count] = is_ref;
      const char *r = q;
      while (r < close && isspace((unsigned char)*r)) r++;
      param_array[*param_count] = (*r == '[');
      (*param_count)++;
    }
    while (q < close && *q != ',') q++;
  }
  return 1;
}

static void program_parse(Program *program, const char *source) {
  memset(program, 0, sizeof(*program));
  const char *p = source;
  Function *current = NULL;
  int pending_function = 0;
  while (*p) {
    const char *eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) : strlen(p);
    char line[512];
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, p, len);
    line[len] = 0;
    trim_line(line);
    if (line[0]) {
      if (current) {
        if (!strcmp(line, "{")) {
          pending_function = 0;
        } else if (!strcmp(line, "}")) {
          current = NULL;
          pending_function = 0;
        } else if (!pending_function && current->line_count < 512) {
          strncpy(current->lines[current->line_count++], line, 511);
        }
      } else {
        char fname[64];
        char params[16][64];
        int param_ref[16] = {0};
        int param_array[16] = {0};
        int param_count = 0;
        if (parse_function_header(line, fname, sizeof(fname), params, param_ref, param_array, &param_count)) {
          current = &program->functions[program->function_count++];
          canon(current->name, sizeof(current->name), fname);
          current->param_count = param_count;
          for (int i = 0; i < param_count; i++) {
            strcpy(current->params[i], params[i]);
            current->param_ref[i] = param_ref[i];
            current->param_array[i] = param_array[i];
          }
          pending_function = strchr(line, '{') ? 0 : 1;
          if (strchr(line, '{') && strchr(line, '}')) current = NULL;
        } else if (program->main_count < 1024) {
          strncpy(program->main_lines[program->main_count++], line, 511);
        }
      }
    }
    if (!eol) break;
    p = eol + 1;
  }
}

static Function *find_function(Program *program, const char *name) {
  char key[64];
  canon(key, sizeof(key), name);
  for (int i = 0; i < program->function_count; i++) {
    if (!strcmp(program->functions[i].name, key)) return &program->functions[i];
  }
  return NULL;
}

static int exec_lines(ExecCtx *ctx, Env *env, char lines[][512], int line_count, double *ret, int *has_ret);

static int exec_inline_control(Env *env, char *stmt, double *ret, int *has_ret) {
  while (*stmt && isspace((unsigned char)*stmt)) stmt++;
  if (!strncasecmp(stmt, "break", 5)) return 2;
  if (!strncasecmp(stmt, "continue", 8)) return 3;
  exec_line(env, stmt, ret, has_ret);
  return *has_ret ? 1 : 0;
}

static int block_range(char lines[][512], int line_count, int after_header, int *body_start, int *body_end, int *next_ip) {
  int start = after_header;
  if (start < line_count) {
    char *b = lines[start];
    while (*b && isspace((unsigned char)*b)) b++;
    if (*b == '{') start++;
  }
  int end = start;
  int depth = 0;
  for (; end < line_count; end++) {
    char *b = lines[end];
    while (*b && isspace((unsigned char)*b)) b++;
    if (*b == '{') { depth++; continue; }
    if (*b == '}') {
      if (depth == 0) break;
      depth--;
    }
  }
  *body_start = start;
  *body_end = end;
  *next_ip = end;
  return start <= end;
}

static const char *find_ci(const char *haystack, const char *needle) {
  size_t n = strlen(needle);
  for (const char *p = haystack; *p; p++) {
    if (!strncasecmp(p, needle, n)) return p;
  }
  return NULL;
}

static int extract_do_while_condition(const char *line, char *cond_text, size_t cond_size) {
  const char *w = find_ci(line, "while");
  const char *open = w ? strchr(w, '(') : NULL;
  const char *close = open ? strrchr(open, ')') : NULL;
  if (!open || !close || close <= open) return 0;
  size_t len = (size_t)(close - open - 1);
  if (len >= cond_size) len = cond_size - 1;
  memcpy(cond_text, open + 1, len);
  cond_text[len] = 0;
  return 1;
}

static int exec_lines(ExecCtx *ctx, Env *env, char lines[][512], int line_count, double *ret, int *has_ret) {
  char labels[256][64];
  int label_ip[256];
  int label_count = 0;
  for (int i = 0; i < line_count; i++) {
    char *p = lines[i];
    while (*p && isspace((unsigned char)*p)) p++;
    char *colon = strchr(p, ':');
    if (!colon) continue;
    int ok = 1;
    for (char *q = p; q < colon; q++) {
      if (!isalnum((unsigned char)*q) && *q != '_') { ok = 0; break; }
    }
    if (ok && label_count < 256) {
      char name[64];
      size_t len = (size_t)(colon - p);
      if (len >= sizeof(name)) len = sizeof(name) - 1;
      memcpy(name, p, len);
      name[len] = 0;
      canon(labels[label_count], sizeof(labels[label_count]), name);
      label_ip[label_count++] = i;
    }
  }
  int guard = 0;
  for (int ip = 0; ip < line_count && !*has_ret; ip++) {
    char local[512];
    strncpy(local, lines[ip], sizeof(local) - 1);
    local[sizeof(local) - 1] = 0;
    char *p = local;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!strncasecmp(p, "for", 3)) {
      char *open = strchr(p, '(');
      char *close = open ? strrchr(open, ')') : NULL;
      if (!open || !close || close <= open) continue;
      char *inline_body = close + 1;
      while (*inline_body && isspace((unsigned char)*inline_body)) inline_body++;
      if (*inline_body && *inline_body != '{') {
        exec_line(env, lines[ip], ret, has_ret);
        if (*has_ret) return 1;
        continue;
      }
      *close = 0;
      char loop_text[384];
      snprintf(loop_text, sizeof(loop_text), "%s", open + 1);
      char *first = strchr(loop_text, ';');
      char *second = first ? strchr(first + 1, ';') : NULL;
      if (!first || !second) continue;
      *first = 0;
      *second = 0;
      char *init_text = loop_text;
      char *cond_text = first + 1;
      char *step_text = second + 1;
      int body_start = 0, body_end = 0, next_ip = 0;
      block_range(lines, line_count, ip + 1, &body_start, &body_end, &next_ip);
      double inner_ret = 0.0;
      int inner_has_ret = 0;
      exec_line(env, init_text, &inner_ret, &inner_has_ret);
      int loop_guard = 0;
      while (!*has_ret) {
        const char *cond = cond_text;
        if (!truthy(parse_expr(env, &cond))) break;
        int signal = exec_lines(ctx, env, &lines[body_start], body_end - body_start, ret, has_ret);
        if (signal == 1 || *has_ret) break;
        if (signal == 2) break;
        exec_line(env, step_text, &inner_ret, &inner_has_ret);
        if (++loop_guard > 1000000) return 0;
      }
      ip = next_ip;
    } else if (!strncasecmp(p, "do", 2)) {
      int body_start = 0, body_end = 0, next_ip = 0;
      block_range(lines, line_count, ip + 1, &body_start, &body_end, &next_ip);
      char cond_text[256];
      if (body_end >= line_count || !extract_do_while_condition(lines[body_end], cond_text, sizeof(cond_text))) continue;
      int loop_guard = 0;
      while (!*has_ret) {
        int signal = exec_lines(ctx, env, &lines[body_start], body_end - body_start, ret, has_ret);
        if (signal == 1 || *has_ret) break;
        if (signal == 2) break;
        const char *cond = cond_text;
        if (!truthy(parse_expr(env, &cond))) break;
        if (++loop_guard > 1000000) return 0;
      }
      ip = next_ip;
    } else if (!strncasecmp(p, "while", 5)) {
      char *open = strchr(p, '(');
      char *close = open ? strrchr(open, ')') : NULL;
      if (!open || !close || close <= open) continue;
      char cond_text[256];
      size_t clen = (size_t)(close - open - 1);
      if (clen >= sizeof(cond_text)) clen = sizeof(cond_text) - 1;
      memcpy(cond_text, open + 1, clen);
      cond_text[clen] = 0;
      int body_start = 0, body_end = 0, next_ip = 0;
      block_range(lines, line_count, ip + 1, &body_start, &body_end, &next_ip);
      int loop_guard = 0;
      while (!*has_ret) {
        const char *cond = cond_text;
        if (!truthy(parse_expr(env, &cond))) break;
        int signal = exec_lines(ctx, env, &lines[body_start], body_end - body_start, ret, has_ret);
        if (signal == 1 || *has_ret) break;
        if (signal == 2) break;
        if (++loop_guard > 1000000) return 0;
      }
      ip = next_ip;
    } else if (!strncasecmp(p, "if", 2)) {
      char *open = strchr(p, '(');
      char *close = open ? strrchr(open, ')') : NULL;
      if (open && close && close > open) {
        *close = 0;
        const char *cond = open + 1;
        char *stmt = close + 1;
        while (*stmt && isspace((unsigned char)*stmt)) stmt++;
        if (*stmt) {
          if (truthy(parse_expr(env, &cond))) {
            if (!strncasecmp(stmt, "goto", 4)) {
              char target[64];
              int n = 0;
              char *q = stmt + 4;
              while (*q && isspace((unsigned char)*q)) q++;
              while ((isalnum((unsigned char)*q) || *q == '_') && n < 63) target[n++] = *q++;
              target[n] = 0;
              canon(target, sizeof(target), target);
              for (int i = 0; i < label_count; i++) {
                if (!strcmp(labels[i], target)) { ip = label_ip[i]; break; }
              }
            } else {
              int signal = exec_inline_control(env, stmt, ret, has_ret);
              if (signal) return signal;
            }
          }
        } else {
          int then_start = 0, then_end = 0, then_next = 0;
          block_range(lines, line_count, ip + 1, &then_start, &then_end, &then_next);
          int else_start = 0, else_end = 0, else_next = then_next;
          int has_else = 0;
          int else_line = then_next + 1;
          if (else_line < line_count) {
            char *e = lines[else_line];
            while (*e && isspace((unsigned char)*e)) e++;
            if (!strncasecmp(e, "else", 4)) {
              has_else = 1;
              block_range(lines, line_count, else_line + 1, &else_start, &else_end, &else_next);
            }
          }
          int signal = 0;
          if (truthy(parse_expr(env, &cond))) {
            signal = exec_lines(ctx, env, &lines[then_start], then_end - then_start, ret, has_ret);
          } else if (has_else) {
            signal = exec_lines(ctx, env, &lines[else_start], else_end - else_start, ret, has_ret);
          }
          if (signal) return signal;
          ip = has_else ? else_next : then_next;
        }
      }
    } else if (!strncasecmp(p, "goto", 4)) {
      char target[64];
      int n = 0;
      char *q = p + 4;
      while (*q && isspace((unsigned char)*q)) q++;
      while ((isalnum((unsigned char)*q) || *q == '_') && n < 63) target[n++] = *q++;
      target[n] = 0;
      canon(target, sizeof(target), target);
      for (int i = 0; i < label_count; i++) {
        if (!strcmp(labels[i], target)) { ip = label_ip[i]; break; }
      }
    } else {
      exec_line(env, p, ret, has_ret);
    }
    if (*has_ret) return 1;
    if (++guard > 1000000) return 0;
  }
  (void)ctx;
  return 0;
}

static double call_function(ExecCtx *ctx, Env *caller, const char *name, double *args, Var **arg_refs, char arg_arrays[4][64], int argc) {
  Function *fn = find_function(ctx->program, name);
  if (!fn) return 0.0;
  Env frame;
  memset(&frame, 0, sizeof(frame));
  frame.rt = ctx->global->rt;
  for (int i = 0; i < fn->param_count && i < argc; i++) {
    if (fn->param_array[i]) {
      int ai = arg_arrays[i][0] ? array_find(caller, arg_arrays[i]) : -1;
      if (ai >= 0) array_bind_alias(&frame, fn->params[i], array_data(caller, ai), caller->array_sizes[ai]);
      else array_create(&frame, fn->params[i], 1024);
    } else if (fn->param_ref[i]) {
      Var *slot = var_get(&frame, fn->params[i]);
      slot->ref = arg_refs[i] ? arg_refs[i] : var_get(caller, fn->params[i]);
    } else {
      Var *slot = var_get(&frame, fn->params[i]);
      slot->value = args[i];
    }
  }
  double ret = 0.0;
  int has_ret = 0;
  ExecCtx saved_ctx = *ctx;
  ctx->global = &frame;
  exec_lines(ctx, &frame, fn->lines, fn->line_count, &ret, &has_ret);
  *ctx = saved_ctx;
  return ret;
}

void pd_trace_runtime_init(PdTraceRuntime *rt) {
  memset(rt, 0, sizeof(*rt));
  rt->xres = 640;
  rt->yres = 480;
  rt->start_seconds = monotonic_seconds();
}

const char *pd_trace_runtime_string_arg(PdTraceRuntime *rt, double value) {
  int index = (int)(-1000000.0 - value);
  if (!rt || index < 0 || index >= 64) return NULL;
  return rt->string_args[index];
}

int pd_run_host_subset(const char *source, PdTraceRuntime *rt, double *out_return) {
  Env env;
  memset(&env, 0, sizeof(env));
  env.rt = rt;
  env.rng_state = 0x12345678u;
  Program *program = (Program *)calloc(1, sizeof(Program));
  if (!program) return 0;
  program_parse(program, source);
  ExecCtx ctx;
  ctx.program = program;
  ctx.global = &env;
  g_exec_ctx = &ctx;
  double ret = 0.0;
  int has_ret = 0;

  exec_lines(&ctx, &env, program->main_lines, program->main_count, &ret, &has_ret);
  g_exec_ctx = NULL;
  free(program);
  *out_return = ret;
  rt->numframes += 1.0;
  env_free(&env);
  return 1;
}

PdHostProgram *pd_host_program_compile_subset(const char *source, PdTraceRuntime *rt) {
  PdHostProgram *host = (PdHostProgram *)calloc(1, sizeof(PdHostProgram));
  if (!host) return NULL;
  host->program = (Program *)calloc(1, sizeof(Program));
  if (!host->program) {
    free(host);
    return NULL;
  }
  memset(&host->env, 0, sizeof(host->env));
  host->env.rt = rt;
  host->env.rng_state = 0x12345678u;
  program_parse(host->program, source);
  return host;
}

void pd_host_program_free(PdHostProgram *host) {
  if (!host) return;
  env_free(&host->env);
  free(host->program);
  free(host);
}

int pd_host_program_run_subset(PdHostProgram *host, PdTraceRuntime *rt, double *out_return) {
  if (!host || !host->program || !rt) return 0;
  host->env.rt = rt;
  ExecCtx ctx;
  ctx.program = host->program;
  ctx.global = &host->env;
  g_exec_ctx = &ctx;
  double ret = 0.0;
  int has_ret = 0;
  exec_lines(&ctx, &host->env, host->program->main_lines, host->program->main_count, &ret, &has_ret);
  g_exec_ctx = NULL;
  if (out_return) *out_return = ret;
  rt->numframes += 1.0;
  return 1;
}
