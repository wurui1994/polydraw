#include "polydraw/jit.h"

#if defined(POLYDRAW_WITH_LLVM)
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Orc.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

typedef int (*PdJitSmokeFn)(void);
typedef double (*PdJitSubsetFn)(void);

typedef struct PdJitVar {
  char name[64];
  LLVMValueRef slot;
  LLVMTypeRef type;
  int is_array;
  int is_const;
  int is_ref;
  double const_value;
} PdJitVar;

typedef struct PdJitCodegen {
  LLVMContextRef ctx;
  LLVMModuleRef module;
  LLVMBuilderRef builder;
  LLVMValueRef fn;
  LLVMTypeRef f64;
  LLVMTypeRef i32;
  PdJitVar vars[256];
  int var_count;
  char lines[512][512];
  int line_count;
  int ok;
} PdJitCodegen;

typedef struct PdJitFunctionSource {
  char name[64];
  char params[16][64];
  int param_ref[16];
  int param_array[16];
  int param_count;
  char lines[512][512];
  int line_count;
} PdJitFunctionSource;

typedef struct PdJitProgramSource {
  char main_lines[1024][512];
  int main_count;
  PdJitFunctionSource functions[128];
  int function_count;
} PdJitProgramSource;

int pd_jit_available(void) {
  return 1;
}

const char *pd_jit_status(void) {
#if defined(POLYDRAW_LLVM_VERSION)
  return "LLVM " POLYDRAW_LLVM_VERSION " detected; ORC LLJIT smoke and host subset lowering available; full PolyDraw IR lowering pending";
#else
  return "LLVM support detected; ORC LLJIT smoke and host subset lowering available; full PolyDraw IR lowering pending";
#endif
}

int pd_jit_smoke_value(void) {
  LLVMInitializeNativeTarget();
  LLVMInitializeNativeAsmPrinter();
  LLVMInitializeNativeAsmParser();

  LLVMOrcLLJITRef jit = NULL;
  LLVMErrorRef err = LLVMOrcCreateLLJIT(&jit, NULL);
  if (err) {
    LLVMDisposeErrorMessage(LLVMGetErrorMessage(err));
    return -1;
  }

  LLVMContextRef ctx = LLVMContextCreate();
  LLVMOrcThreadSafeContextRef tsc = LLVMOrcCreateNewThreadSafeContextFromLLVMContext(ctx);
  LLVMModuleRef module = LLVMModuleCreateWithNameInContext("polydraw_orc_smoke", ctx);
  LLVMTypeRef fn_type = LLVMFunctionType(LLVMInt32TypeInContext(ctx), NULL, 0, 0);
  LLVMValueRef fn = LLVMAddFunction(module, "pd_jit_smoke", fn_type);
  LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ctx, fn, "entry");
  LLVMBuilderRef builder = LLVMCreateBuilderInContext(ctx);
  LLVMPositionBuilderAtEnd(builder, entry);
  LLVMBuildRet(builder, LLVMConstInt(LLVMInt32TypeInContext(ctx), 42, 0));

  char *error = NULL;
  if (LLVMVerifyModule(module, LLVMReturnStatusAction, &error)) {
    LLVMDisposeMessage(error);
    LLVMDisposeBuilder(builder);
    LLVMOrcDisposeThreadSafeContext(tsc);
    LLVMOrcDisposeLLJIT(jit);
    return -1;
  }

  LLVMOrcThreadSafeModuleRef tsm = LLVMOrcCreateNewThreadSafeModule(module, tsc);
  err = LLVMOrcLLJITAddLLVMIRModule(jit, LLVMOrcLLJITGetMainJITDylib(jit), tsm);
  if (err) {
    LLVMDisposeErrorMessage(LLVMGetErrorMessage(err));
    LLVMDisposeBuilder(builder);
    LLVMOrcDisposeThreadSafeContext(tsc);
    LLVMOrcDisposeLLJIT(jit);
    return -1;
  }

  LLVMOrcExecutorAddress addr = 0;
  err = LLVMOrcLLJITLookup(jit, &addr, "pd_jit_smoke");
  if (err) {
    LLVMDisposeErrorMessage(LLVMGetErrorMessage(err));
    LLVMDisposeBuilder(builder);
    LLVMOrcDisposeThreadSafeContext(tsc);
    LLVMOrcDisposeLLJIT(jit);
    return -1;
  }
  int value = -1;
  if (addr) {
    PdJitSmokeFn smoke = (PdJitSmokeFn)(uintptr_t)addr;
    value = smoke();
  }
  LLVMDisposeBuilder(builder);
  LLVMOrcDisposeThreadSafeContext(tsc);
  LLVMOrcDisposeLLJIT(jit);
  return value;
}

static void pd_jit_init_native(void) {
  LLVMInitializeNativeTarget();
  LLVMInitializeNativeAsmPrinter();
  LLVMInitializeNativeAsmParser();
}

static void pd_jit_canon(char *dst, size_t size, const char *src) {
  size_t i = 0;
  for (; src[i] && i + 1 < size; i++) dst[i] = (char)toupper((unsigned char)src[i]);
  dst[i] = 0;
}

static char *pd_jit_trim(char *s) {
  while (*s && isspace((unsigned char)*s)) s++;
  size_t n = strlen(s);
  while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = 0;
  return s;
}

static const char *pd_jit_find_ci(const char *haystack, const char *needle) {
  size_t n = strlen(needle);
  for (const char *p = haystack; *p; p++) {
    if (!strncasecmp(p, needle, n)) return p;
  }
  return NULL;
}

static int pd_jit_extract_do_while_condition(const char *line, char *cond_text, size_t cond_size) {
  const char *w = pd_jit_find_ci(line, "while");
  const char *open = w ? strchr(w, '(') : NULL;
  const char *close = open ? strrchr(open, ')') : NULL;
  if (!open || !close || close <= open) return 0;
  size_t len = (size_t)(close - open - 1);
  if (len >= cond_size) len = cond_size - 1;
  memcpy(cond_text, open + 1, len);
  cond_text[len] = 0;
  return 1;
}

static int pd_jit_parse_function_header(const char *line, char *name, size_t name_size, char params[16][64], int param_ref[16], int param_array[16], int *param_count) {
  const char *open = strchr(line, '(');
  const char *close = open ? strrchr(open, ')') : NULL;
  if (!open || !close || close < open) return 0;
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
  char *trimmed_prefix = pd_jit_trim(prefix);
  if (strchr(trimmed_prefix, '=') || !strncasecmp(trimmed_prefix, "if", 2) || !strncasecmp(trimmed_prefix, "for", 3) || !strncasecmp(trimmed_prefix, "while", 5) || !strncasecmp(trimmed_prefix, "printf", 6)) return 0;
  size_t len = (size_t)(end - p);
  if (len >= name_size) len = name_size - 1;
  memcpy(name, p, len);
  name[len] = 0;
  char key[64];
  pd_jit_canon(key, sizeof(key), name);
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
      pd_jit_canon(params[*param_count], 64, param);
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

static void pd_jit_prepare_program(PdJitProgramSource *program, const char *source) {
  memset(program, 0, sizeof(*program));
  const char *p = source;
  PdJitFunctionSource *current = NULL;
  int pending_function = 0;
  while (*p) {
    const char *eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) : strlen(p);
    char line[512];
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, p, len);
    line[len] = 0;
    char *comment = strstr(line, "//");
    if (comment) *comment = 0;
    char *s = pd_jit_trim(line);
    if (*s) {
      if (current) {
        if (!strcmp(s, "{")) {
          pending_function = 0;
        } else if (!strcmp(s, "}")) {
          current = NULL;
          pending_function = 0;
        } else if (!pending_function && current->line_count < 512) {
          snprintf(current->lines[current->line_count++], 512, "%s", s);
        }
      } else {
        char fname[64];
        char params[16][64];
        int param_ref[16] = {0};
        int param_array[16] = {0};
        int param_count = 0;
        if (pd_jit_parse_function_header(s, fname, sizeof(fname), params, param_ref, param_array, &param_count)) {
          current = &program->functions[program->function_count++];
          pd_jit_canon(current->name, sizeof(current->name), fname);
          current->param_count = param_count;
          for (int i = 0; i < param_count; i++) {
            strcpy(current->params[i], params[i]);
            current->param_ref[i] = param_ref[i];
            current->param_array[i] = param_array[i];
          }
          pending_function = strchr(s, '{') ? 0 : 1;
          if (strchr(s, '{') && strchr(s, '}')) current = NULL;
        } else if (program->main_count < 1024) {
          snprintf(program->main_lines[program->main_count++], 512, "%s", s);
        }
      }
    }
    if (!eol) break;
    p = eol + 1;
  }
}

static PdJitVar *pd_jit_find_var(PdJitCodegen *cg, const char *name) {
  char key[64];
  pd_jit_canon(key, sizeof(key), name);
  for (int i = 0; i < cg->var_count; i++) {
    if (!strcmp(cg->vars[i].name, key)) return &cg->vars[i];
  }
  return NULL;
}

static void pd_jit_position_entry_alloca(PdJitCodegen *cg) {
  LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(cg->fn);
  LLVMValueRef first = LLVMGetFirstInstruction(entry);
  if (first) LLVMPositionBuilderBefore(cg->builder, first);
  else LLVMPositionBuilderAtEnd(cg->builder, entry);
}

static void pd_jit_restore_block_end(PdJitCodegen *cg, LLVMBasicBlockRef block) {
  if (block) LLVMPositionBuilderAtEnd(cg->builder, block);
}

static PdJitVar *pd_jit_add_var(PdJitCodegen *cg, const char *name) {
  PdJitVar *existing = pd_jit_find_var(cg, name);
  if (existing) return existing;
  if (cg->var_count >= (int)(sizeof(cg->vars) / sizeof(cg->vars[0]))) {
    cg->ok = 0;
    return NULL;
  }
  PdJitVar *v = &cg->vars[cg->var_count++];
  memset(v, 0, sizeof(*v));
  pd_jit_canon(v->name, sizeof(v->name), name);
  v->type = cg->f64;
  LLVMBasicBlockRef saved = LLVMGetInsertBlock(cg->builder);
  pd_jit_position_entry_alloca(cg);
  v->slot = LLVMBuildAlloca(cg->builder, cg->f64, v->name);
  LLVMBuildStore(cg->builder, LLVMConstReal(cg->f64, 0.0), v->slot);
  pd_jit_restore_block_end(cg, saved);
  return v;
}

static PdJitVar *pd_jit_bind_ref_var(PdJitCodegen *cg, const char *name, LLVMValueRef ptr) {
  if (cg->var_count >= (int)(sizeof(cg->vars) / sizeof(cg->vars[0]))) {
    cg->ok = 0;
    return NULL;
  }
  PdJitVar *v = &cg->vars[cg->var_count++];
  memset(v, 0, sizeof(*v));
  pd_jit_canon(v->name, sizeof(v->name), name);
  v->type = cg->f64;
  v->slot = ptr;
  v->is_ref = 1;
  return v;
}

static PdJitVar *pd_jit_bind_array_param(PdJitCodegen *cg, const char *name, LLVMValueRef ptr) {
  if (cg->var_count >= (int)(sizeof(cg->vars) / sizeof(cg->vars[0]))) {
    cg->ok = 0;
    return NULL;
  }
  PdJitVar *v = &cg->vars[cg->var_count++];
  memset(v, 0, sizeof(*v));
  pd_jit_canon(v->name, sizeof(v->name), name);
  v->is_array = 1;
  v->is_ref = 1;
  v->type = cg->f64;
  v->slot = ptr;
  return v;
}

static PdJitVar *pd_jit_add_array(PdJitCodegen *cg, const char *name, int size) {
  (void)size;
  PdJitVar *existing = pd_jit_find_var(cg, name);
  if (existing) return existing;
  if (cg->var_count >= (int)(sizeof(cg->vars) / sizeof(cg->vars[0]))) {
    cg->ok = 0;
    return NULL;
  }
  PdJitVar *v = &cg->vars[cg->var_count++];
  memset(v, 0, sizeof(*v));
  pd_jit_canon(v->name, sizeof(v->name), name);
  v->is_array = 1;
  v->type = LLVMArrayType(cg->f64, 1024);
  LLVMBasicBlockRef saved = LLVMGetInsertBlock(cg->builder);
  pd_jit_position_entry_alloca(cg);
  v->slot = LLVMBuildAlloca(cg->builder, v->type, v->name);
  LLVMBuildStore(cg->builder, LLVMConstNull(v->type), v->slot);
  pd_jit_restore_block_end(cg, saved);
  return v;
}

static PdJitVar *pd_jit_add_const(PdJitCodegen *cg, const char *name, double value) {
  PdJitVar *v = pd_jit_find_var(cg, name);
  if (!v) {
    if (cg->var_count >= (int)(sizeof(cg->vars) / sizeof(cg->vars[0]))) {
      cg->ok = 0;
      return NULL;
    }
    v = &cg->vars[cg->var_count++];
    memset(v, 0, sizeof(*v));
    pd_jit_canon(v->name, sizeof(v->name), name);
  }
  v->is_const = 1;
  v->const_value = value;
  return v;
}

static const char *pd_jit_skip_ws(const char *p) {
  while (*p && isspace((unsigned char)*p)) p++;
  return p;
}

static LLVMValueRef pd_jit_expr(PdJitCodegen *cg, const char **pp);
static void pd_jit_emit_statement(PdJitCodegen *cg, const char *statement);

static LLVMValueRef pd_jit_call_math(PdJitCodegen *cg, const char *name, LLVMValueRef *args, unsigned argc) {
  LLVMTypeRef params[2] = {cg->f64, cg->f64};
  LLVMTypeRef fn_type = LLVMFunctionType(cg->f64, params, argc, 0);
  LLVMValueRef fn = LLVMGetNamedFunction(cg->module, name);
  if (!fn) fn = LLVMAddFunction(cg->module, name, fn_type);
  return LLVMBuildCall2(cg->builder, fn_type, fn, args, argc, name);
}

static LLVMValueRef pd_jit_load_var(PdJitCodegen *cg, const char *name) {
  PdJitVar *v = pd_jit_find_var(cg, name);
  if (!v) v = pd_jit_add_var(cg, name);
  if (!v) return LLVMConstReal(cg->f64, 0.0);
  if (v->is_const) return LLVMConstReal(cg->f64, v->const_value);
  return LLVMBuildLoad2(cg->builder, cg->f64, v->slot, v->name);
}

static LLVMValueRef pd_jit_scalar_ptr(PdJitCodegen *cg, const char *name) {
  PdJitVar *v = pd_jit_add_var(cg, name);
  if (!v) return NULL;
  return v->slot;
}

static LLVMValueRef pd_jit_array_base_ptr(PdJitCodegen *cg, const char *name) {
  PdJitVar *v = pd_jit_find_var(cg, name);
  if (!v) v = pd_jit_add_array(cg, name, 1024);
  if (!v || !v->is_array) return NULL;
  if (v->is_ref) return v->slot;
  LLVMValueRef idxs[2] = {LLVMConstInt(cg->i32, 0, 0), LLVMConstInt(cg->i32, 0, 0)};
  return LLVMBuildGEP2(cg->builder, v->type, v->slot, idxs, 2, "arrbase");
}

static LLVMTypeRef pd_jit_function_type(PdJitCodegen *cg, const PdJitFunctionSource *src) {
  LLVMTypeRef params[16];
  for (int i = 0; i < src->param_count; i++) {
    params[i] = (src->param_ref[i] || src->param_array[i]) ? LLVMPointerType(cg->f64, 0) : cg->f64;
  }
  return LLVMFunctionType(cg->f64, params, src->param_count, 0);
}

static LLVMValueRef pd_jit_array_ptr(PdJitCodegen *cg, const char *name, LLVMValueRef index_value) {
  PdJitVar *v = pd_jit_find_var(cg, name);
  if (!v) v = pd_jit_add_array(cg, name, 1024);
  if (!v || !v->is_array) {
    cg->ok = 0;
    return NULL;
  }
  LLVMValueRef as_i32 = LLVMBuildFPToSI(cg->builder, index_value, cg->i32, "idxi");
  LLVMValueRef mask = LLVMConstInt(cg->i32, 1023, 0);
  LLVMValueRef wrapped = LLVMBuildAnd(cg->builder, as_i32, mask, "idx");
  if (v->is_ref) {
    return LLVMBuildGEP2(cg->builder, cg->f64, v->slot, &wrapped, 1, "elt");
  }
  LLVMValueRef idxs[2] = {LLVMConstInt(cg->i32, 0, 0), wrapped};
  return LLVMBuildGEP2(cg->builder, v->type, v->slot, idxs, 2, "elt");
}

static LLVMValueRef pd_jit_primary(PdJitCodegen *cg, const char **pp) {
  const char *p = pd_jit_skip_ws(*pp);
  if (*p == '(') {
    p++;
    LLVMValueRef v = pd_jit_expr(cg, &p);
    p = pd_jit_skip_ws(p);
    if (*p == ')') p++;
    *pp = p;
    return v;
  }
  if (isdigit((unsigned char)*p) || *p == '.') {
    char *end = NULL;
    double v = strtod(p, &end);
    *pp = end;
    return LLVMConstReal(cg->f64, v);
  }
  if (isalpha((unsigned char)*p) || *p == '_') {
    char name[64];
    int n = 0;
    while ((isalnum((unsigned char)*p) || *p == '_') && n < 63) name[n++] = *p++;
    name[n] = 0;
    p = pd_jit_skip_ws(p);
    if (*p == '(') {
      LLVMValueRef args[4];
      unsigned argc = 0;
      p++;
      while (*p && *p != ')' && argc < 4) {
        p = pd_jit_skip_ws(p);
        if (*p == '&') {
          p++;
          char ref_name[64];
          int rn = 0;
          while ((isalnum((unsigned char)*p) || *p == '_') && rn < 63) ref_name[rn++] = *p++;
          ref_name[rn] = 0;
          args[argc++] = rn ? pd_jit_scalar_ptr(cg, ref_name) : LLVMConstNull(LLVMPointerType(cg->f64, 0));
        } else if (isalpha((unsigned char)*p) || *p == '_') {
          const char *save = p;
          char maybe_name[64];
          int mn = 0;
          while ((isalnum((unsigned char)*p) || *p == '_') && mn < 63) maybe_name[mn++] = *p++;
          maybe_name[mn] = 0;
          const char *after_name = pd_jit_skip_ws(p);
          if ((*after_name == ',' || *after_name == ')') && pd_jit_find_var(cg, maybe_name) && pd_jit_find_var(cg, maybe_name)->is_array) {
            args[argc++] = pd_jit_array_base_ptr(cg, maybe_name);
            p = after_name;
          } else {
            p = save;
            args[argc++] = pd_jit_expr(cg, &p);
          }
        } else {
          args[argc++] = pd_jit_expr(cg, &p);
        }
        p = pd_jit_skip_ws(p);
        if (*p == ',') p++;
      }
      if (*p == ')') p++;
      *pp = p;
      char key[64];
      pd_jit_canon(key, sizeof(key), name);
      if (!strcmp(key, "SIN") && argc >= 1) return pd_jit_call_math(cg, "sin", args, 1);
      if (!strcmp(key, "COS") && argc >= 1) return pd_jit_call_math(cg, "cos", args, 1);
      if (!strcmp(key, "TAN") && argc >= 1) return pd_jit_call_math(cg, "tan", args, 1);
      if ((!strcmp(key, "SQRT") || !strcmp(key, "SQR")) && argc >= 1) return pd_jit_call_math(cg, "sqrt", args, 1);
      if ((!strcmp(key, "ABS") || !strcmp(key, "FABS")) && argc >= 1) return pd_jit_call_math(cg, "fabs", args, 1);
      if (!strcmp(key, "INT") && argc >= 1) return pd_jit_call_math(cg, "trunc", args, 1);
      if (!strcmp(key, "POW") && argc >= 2) return pd_jit_call_math(cg, "pow", args, 2);
      if (!strcmp(key, "FMOD") && argc >= 2) return pd_jit_call_math(cg, "fmod", args, 2);
      if (!strcmp(key, "MIN") && argc >= 2) return LLVMBuildSelect(
        cg->builder,
        LLVMBuildFCmp(cg->builder, LLVMRealOLT, args[0], args[1], "mincmp"),
        args[0],
        args[1],
        "minv"
      );
      if (!strcmp(key, "MAX") && argc >= 2) return LLVMBuildSelect(
        cg->builder,
        LLVMBuildFCmp(cg->builder, LLVMRealOGT, args[0], args[1], "maxcmp"),
        args[0],
        args[1],
        "maxv"
      );
      char fn_name[96];
      snprintf(fn_name, sizeof(fn_name), "pd_user_%s", key);
      LLVMValueRef user_fn = LLVMGetNamedFunction(cg->module, fn_name);
      if (user_fn) {
        LLVMTypeRef fn_type = LLVMGlobalGetValueType(user_fn);
        return LLVMBuildCall2(cg->builder, fn_type, user_fn, args, argc, "call");
      }
      return LLVMConstReal(cg->f64, 0.0);
    }
    if (*p == '[') {
      p++;
      LLVMValueRef idx = pd_jit_expr(cg, &p);
      p = pd_jit_skip_ws(p);
      if (*p == ']') p++;
      *pp = p;
      LLVMValueRef ptr = pd_jit_array_ptr(cg, name, idx);
      if (!ptr) return LLVMConstReal(cg->f64, 0.0);
      return LLVMBuildLoad2(cg->builder, cg->f64, ptr, "arrload");
    }
    *pp = p;
    return pd_jit_load_var(cg, name);
  }
  cg->ok = 0;
  *pp = p;
  return LLVMConstReal(cg->f64, 0.0);
}

static LLVMValueRef pd_jit_unary(PdJitCodegen *cg, const char **pp) {
  const char *p = pd_jit_skip_ws(*pp);
  if (*p == '+') {
    p++;
    *pp = p;
    return pd_jit_unary(cg, pp);
  }
  if (*p == '-') {
    p++;
    *pp = p;
    return LLVMBuildFNeg(cg->builder, pd_jit_unary(cg, pp), "neg");
  }
  return pd_jit_primary(cg, pp);
}

static LLVMValueRef pd_jit_power(PdJitCodegen *cg, const char **pp) {
  LLVMValueRef v = pd_jit_unary(cg, pp);
  const char *p = pd_jit_skip_ws(*pp);
  if (*p == '^') {
    p++;
    *pp = p;
    LLVMValueRef args[2] = {v, pd_jit_power(cg, pp)};
    return pd_jit_call_math(cg, "pow", args, 2);
  }
  return v;
}

static LLVMValueRef pd_jit_mul(PdJitCodegen *cg, const char **pp) {
  LLVMValueRef v = pd_jit_power(cg, pp);
  for (;;) {
    const char *p = pd_jit_skip_ws(*pp);
    if (*p == '*') {
      p++;
      *pp = p;
      v = LLVMBuildFMul(cg->builder, v, pd_jit_power(cg, pp), "mul");
    } else if (*p == '/') {
      p++;
      *pp = p;
      v = LLVMBuildFDiv(cg->builder, v, pd_jit_power(cg, pp), "div");
    } else if (*p == '%') {
      p++;
      *pp = p;
      LLVMValueRef args[2] = {v, pd_jit_power(cg, pp)};
      v = pd_jit_call_math(cg, "fmod", args, 2);
    } else {
      break;
    }
  }
  return v;
}

static LLVMValueRef pd_jit_add(PdJitCodegen *cg, const char **pp) {
  LLVMValueRef v = pd_jit_mul(cg, pp);
  for (;;) {
    const char *p = pd_jit_skip_ws(*pp);
    if (*p == '+') {
      p++;
      *pp = p;
      v = LLVMBuildFAdd(cg->builder, v, pd_jit_mul(cg, pp), "add");
    } else if (*p == '-') {
      p++;
      *pp = p;
      v = LLVMBuildFSub(cg->builder, v, pd_jit_mul(cg, pp), "sub");
    } else {
      break;
    }
  }
  return v;
}

static LLVMValueRef pd_jit_cmp(PdJitCodegen *cg, const char **pp) {
  LLVMValueRef lhs = pd_jit_add(cg, pp);
  const char *p = pd_jit_skip_ws(*pp);
  LLVMRealPredicate pred = LLVMRealOEQ;
  int has_cmp = 1;
  if (!strncmp(p, "==", 2)) {
    p += 2;
    pred = LLVMRealOEQ;
  } else if (!strncmp(p, "!=", 2)) {
    p += 2;
    pred = LLVMRealONE;
  } else if (!strncmp(p, "<=", 2)) {
    p += 2;
    pred = LLVMRealOLE;
  } else if (!strncmp(p, ">=", 2)) {
    p += 2;
    pred = LLVMRealOGE;
  } else if (*p == '<') {
    p++;
    pred = LLVMRealOLT;
  } else if (*p == '>') {
    p++;
    pred = LLVMRealOGT;
  } else {
    has_cmp = 0;
  }
  if (!has_cmp) return lhs;
  *pp = p;
  LLVMValueRef rhs = pd_jit_add(cg, pp);
  LLVMValueRef cmp = LLVMBuildFCmp(cg->builder, pred, lhs, rhs, "cmp");
  return LLVMBuildUIToFP(cg->builder, cmp, cg->f64, "cmpd");
}

static LLVMValueRef pd_jit_and(PdJitCodegen *cg, const char **pp) {
  LLVMValueRef v = pd_jit_cmp(cg, pp);
  for (;;) {
    const char *p = pd_jit_skip_ws(*pp);
    if (strncmp(p, "&&", 2)) break;
    p += 2;
    *pp = p;
    LLVMValueRef l = LLVMBuildFCmp(cg->builder, LLVMRealONE, v, LLVMConstReal(cg->f64, 0.0), "andl");
    LLVMValueRef r = LLVMBuildFCmp(cg->builder, LLVMRealONE, pd_jit_cmp(cg, pp), LLVMConstReal(cg->f64, 0.0), "andr");
    v = LLVMBuildUIToFP(cg->builder, LLVMBuildAnd(cg->builder, l, r, "andb"), cg->f64, "andd");
  }
  return v;
}

static LLVMValueRef pd_jit_expr(PdJitCodegen *cg, const char **pp) {
  LLVMValueRef v = pd_jit_and(cg, pp);
  for (;;) {
    const char *p = pd_jit_skip_ws(*pp);
    if (strncmp(p, "||", 2)) break;
    p += 2;
    *pp = p;
    LLVMValueRef l = LLVMBuildFCmp(cg->builder, LLVMRealONE, v, LLVMConstReal(cg->f64, 0.0), "orl");
    LLVMValueRef r = LLVMBuildFCmp(cg->builder, LLVMRealONE, pd_jit_and(cg, pp), LLVMConstReal(cg->f64, 0.0), "orr");
    v = LLVMBuildUIToFP(cg->builder, LLVMBuildOr(cg->builder, l, r, "orb"), cg->f64, "ord");
  }
  return v;
}

static LLVMValueRef pd_jit_lvalue_ptr(PdJitCodegen *cg, const char *target) {
  const char *p = pd_jit_skip_ws(target);
  char name[64];
  int n = 0;
  while ((isalnum((unsigned char)*p) || *p == '_') && n < 63) name[n++] = *p++;
  name[n] = 0;
  p = pd_jit_skip_ws(p);
  if (*p == '[') {
    p++;
    LLVMValueRef idx = pd_jit_expr(cg, &p);
    return pd_jit_array_ptr(cg, name, idx);
  }
  PdJitVar *v = pd_jit_add_var(cg, name);
  return v ? v->slot : NULL;
}

static void pd_jit_emit_statement(PdJitCodegen *cg, const char *statement);

static void pd_jit_emit_assignment(PdJitCodegen *cg, char *line) {
  char *plus_eq = strstr(line, "+=");
  if (plus_eq) {
    *plus_eq = 0;
    LLVMValueRef ptr = pd_jit_lvalue_ptr(cg, line);
    const char *expr = plus_eq + 2;
    LLVMValueRef old = LLVMBuildLoad2(cg->builder, cg->f64, ptr, "old");
    LLVMValueRef value = LLVMBuildFAdd(cg->builder, old, pd_jit_expr(cg, &expr), "addeq");
    LLVMBuildStore(cg->builder, value, ptr);
    return;
  }
  char *inc = strstr(line, "++");
  if (inc) {
    *inc = 0;
    LLVMValueRef ptr = pd_jit_lvalue_ptr(cg, line);
    LLVMValueRef old = LLVMBuildLoad2(cg->builder, cg->f64, ptr, "old");
    LLVMValueRef value = LLVMBuildFAdd(cg->builder, old, LLVMConstReal(cg->f64, 1.0), "inc");
    LLVMBuildStore(cg->builder, value, ptr);
    return;
  }
  char *eq = strchr(line, '=');
  if (eq) {
    *eq = 0;
    LLVMValueRef ptr = pd_jit_lvalue_ptr(cg, line);
    const char *expr = eq + 1;
    LLVMBuildStore(cg->builder, pd_jit_expr(cg, &expr), ptr);
  }
}

static const char *pd_jit_after_matching_paren(const char *p) {
  int depth = 0;
  for (; *p; p++) {
    if (*p == '(') depth++;
    if (*p == ')') {
      depth--;
      if (depth == 0) return p + 1;
    }
  }
  return NULL;
}

static void pd_jit_emit_for(PdJitCodegen *cg, const char *line) {
  const char *open = strchr(line, '(');
  const char *close = pd_jit_after_matching_paren(open);
  if (!open || !close) {
    cg->ok = 0;
    return;
  }
  char header[256];
  size_t hlen = (size_t)((close - 1) - open - 1);
  if (hlen >= sizeof(header)) hlen = sizeof(header) - 1;
  memcpy(header, open + 1, hlen);
  header[hlen] = 0;
  char *first = strchr(header, ';');
  char *second = first ? strchr(first + 1, ';') : NULL;
  if (!first || !second) {
    cg->ok = 0;
    return;
  }
  *first = 0;
  *second = 0;
  pd_jit_emit_statement(cg, header);

  LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->fn, "for.cond");
  LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->fn, "for.body");
  LLVMBasicBlockRef after_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->fn, "for.end");
  LLVMBuildBr(cg->builder, cond_bb);
  LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
  const char *cond = first + 1;
  LLVMValueRef cond_val = pd_jit_expr(cg, &cond);
  LLVMValueRef cond_bool = LLVMBuildFCmp(cg->builder, LLVMRealONE, cond_val, LLVMConstReal(cg->f64, 0.0), "forbool");
  LLVMBuildCondBr(cg->builder, cond_bool, body_bb, after_bb);

  LLVMPositionBuilderAtEnd(cg->builder, body_bb);
  char body[512];
  snprintf(body, sizeof(body), "%s", pd_jit_trim((char *)close));
  pd_jit_emit_statement(cg, body);
  pd_jit_emit_statement(cg, second + 1);
  LLVMBuildBr(cg->builder, cond_bb);
  LLVMPositionBuilderAtEnd(cg->builder, after_bb);
}

static void pd_jit_emit_statement(PdJitCodegen *cg, const char *statement) {
  char line[512];
  snprintf(line, sizeof(line), "%s", statement);
  char *s = pd_jit_trim(line);
  size_t n = strlen(s);
  if (n && s[n - 1] == ';') s[n - 1] = 0;
  s = pd_jit_trim(s);
  if (!*s || *s == '{' || *s == '}') return;
  if (!strncmp(s, "for", 3)) {
    pd_jit_emit_for(cg, s);
    return;
  }
  if (!strncmp(s, "printf", 6)) return;
  pd_jit_emit_assignment(cg, s);
}

static int pd_jit_parse_label_line(const char *s, char *name, size_t name_size) {
  const char *colon = strchr(s, ':');
  if (!colon) return 0;
  const char *p = s;
  while (*p && isspace((unsigned char)*p)) p++;
  const char *start = p;
  while (p < colon) {
    if (!isalnum((unsigned char)*p) && *p != '_') return 0;
    p++;
  }
  size_t len = (size_t)(colon - start);
  if (!len) return 0;
  if (len >= name_size) len = name_size - 1;
  memcpy(name, start, len);
  name[len] = 0;
  pd_jit_canon(name, name_size, name);
  return 1;
}

static LLVMBasicBlockRef pd_jit_find_label_block(char labels[][64], LLVMBasicBlockRef *blocks, int count, const char *name) {
  char key[64];
  pd_jit_canon(key, sizeof(key), name);
  for (int i = 0; i < count; i++) {
    if (!strcmp(labels[i], key)) return blocks[i];
  }
  return NULL;
}

static void pd_jit_build_br_if_open(PdJitCodegen *cg, LLVMBasicBlockRef target) {
  LLVMBasicBlockRef current = LLVMGetInsertBlock(cg->builder);
  if (current && !LLVMGetBasicBlockTerminator(current)) LLVMBuildBr(cg->builder, target);
}

static int pd_jit_block_range(char lines[][512], int line_count, int after_header, int *body_start, int *body_end, int *next_index) {
  int start = after_header;
  if (start < line_count) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", lines[start]);
    if (*pd_jit_trim(tmp) == '{') start++;
  }
  int end = start;
  int depth = 0;
  for (; end < line_count; end++) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", lines[end]);
    char *t = pd_jit_trim(tmp);
    if (*t == '{') { depth++; continue; }
    if (*t == '}') {
      if (depth == 0) break;
      depth--;
    }
  }
  *body_start = start;
  *body_end = end;
  *next_index = end;
  return start <= end;
}

static void pd_jit_emit_linear_cfg(PdJitCodegen *cg, char lines[][512], int line_count, LLVMBasicBlockRef break_target, LLVMBasicBlockRef continue_target) {
  char labels[256][64];
  LLVMBasicBlockRef label_blocks[256];
  int label_count = 0;
  for (int i = 0; i < line_count; i++) {
    char label[64];
    if (pd_jit_parse_label_line(lines[i], label, sizeof(label)) && label_count < 256) {
      strcpy(labels[label_count], label);
      label_blocks[label_count] = LLVMAppendBasicBlockInContext(cg->ctx, cg->fn, label);
      label_count++;
    }
  }
  for (int i = 0; i < line_count && cg->ok; i++) {
    char line[512];
    snprintf(line, sizeof(line), "%s", lines[i]);
    char *s = pd_jit_trim(line);
    size_t n = strlen(s);
    if (n && s[n - 1] == ';') s[n - 1] = 0;
    s = pd_jit_trim(s);
    char label[64];
    if (pd_jit_parse_label_line(s, label, sizeof(label))) {
      LLVMBasicBlockRef block = pd_jit_find_label_block(labels, label_blocks, label_count, label);
      LLVMBasicBlockRef current = LLVMGetInsertBlock(cg->builder);
      if (current && !LLVMGetBasicBlockTerminator(current)) LLVMBuildBr(cg->builder, block);
      LLVMPositionBuilderAtEnd(cg->builder, block);
      continue;
    }
    if (!strncmp(s, "do", 2)) {
      int body_start = 0, body_end = 0, next_index = 0;
      pd_jit_block_range(lines, line_count, i + 1, &body_start, &body_end, &next_index);
      char cond_text[256];
      if (body_end >= line_count || !pd_jit_extract_do_while_condition(lines[body_end], cond_text, sizeof(cond_text))) {
        cg->ok = 0;
        return;
      }
      LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->fn, "do.body");
      LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->fn, "do.cond");
      LLVMBasicBlockRef after_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->fn, "do.end");
      pd_jit_build_br_if_open(cg, body_bb);
      LLVMPositionBuilderAtEnd(cg->builder, body_bb);
      pd_jit_emit_linear_cfg(cg, &lines[body_start], body_end - body_start, after_bb, cond_bb);
      pd_jit_build_br_if_open(cg, cond_bb);
      LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
      const char *cond = cond_text;
      LLVMValueRef cond_val = pd_jit_expr(cg, &cond);
      LLVMValueRef cond_bool = LLVMBuildFCmp(cg->builder, LLVMRealONE, cond_val, LLVMConstReal(cg->f64, 0.0), "dobool");
      LLVMBuildCondBr(cg->builder, cond_bool, body_bb, after_bb);
      LLVMPositionBuilderAtEnd(cg->builder, after_bb);
      i = next_index;
      continue;
    }
    if (!strncmp(s, "while", 5)) {
      const char *open = strchr(s, '(');
      const char *close = open ? pd_jit_after_matching_paren(open) : NULL;
      if (!open || !close) { cg->ok = 0; return; }
      char cond_text[256];
      size_t clen = (size_t)((close - 1) - open - 1);
      if (clen >= sizeof(cond_text)) clen = sizeof(cond_text) - 1;
      memcpy(cond_text, open + 1, clen);
      cond_text[clen] = 0;
      int body_start = 0, body_end = 0, next_index = 0;
      pd_jit_block_range(lines, line_count, i + 1, &body_start, &body_end, &next_index);
      LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->fn, "while.cond");
      LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->fn, "while.body");
      LLVMBasicBlockRef after_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->fn, "while.end");
      pd_jit_build_br_if_open(cg, cond_bb);
      LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
      const char *cond = cond_text;
      LLVMValueRef cond_val = pd_jit_expr(cg, &cond);
      LLVMValueRef cond_bool = LLVMBuildFCmp(cg->builder, LLVMRealONE, cond_val, LLVMConstReal(cg->f64, 0.0), "whilebool");
      LLVMBuildCondBr(cg->builder, cond_bool, body_bb, after_bb);
      LLVMPositionBuilderAtEnd(cg->builder, body_bb);
      pd_jit_emit_linear_cfg(cg, &lines[body_start], body_end - body_start, after_bb, cond_bb);
      pd_jit_build_br_if_open(cg, cond_bb);
      LLVMPositionBuilderAtEnd(cg->builder, after_bb);
      i = next_index;
      continue;
    }
    if (!strncmp(s, "if", 2)) {
      const char *open = strchr(s, '(');
      const char *close = open ? pd_jit_after_matching_paren(open) : NULL;
      if (!open || !close) { cg->ok = 0; return; }
      char cond_text[256];
      size_t clen = (size_t)((close - 1) - open - 1);
      if (clen >= sizeof(cond_text)) clen = sizeof(cond_text) - 1;
      memcpy(cond_text, open + 1, clen);
      cond_text[clen] = 0;
      char *stmt = pd_jit_trim((char *)close);
      if (!*stmt) {
        int then_start = 0, then_end = 0, then_next = 0;
        pd_jit_block_range(lines, line_count, i + 1, &then_start, &then_end, &then_next);
        int else_start = 0, else_end = 0, else_next = then_next;
        int has_else = 0;
        int else_line = then_next + 1;
        if (else_line < line_count) {
          char else_tmp[512];
          snprintf(else_tmp, sizeof(else_tmp), "%s", lines[else_line]);
          if (!strncasecmp(pd_jit_trim(else_tmp), "else", 4)) {
            has_else = 1;
            pd_jit_block_range(lines, line_count, else_line + 1, &else_start, &else_end, &else_next);
          }
        }
        LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->fn, "if.then");
        LLVMBasicBlockRef else_bb = has_else ? LLVMAppendBasicBlockInContext(cg->ctx, cg->fn, "if.else") : NULL;
        LLVMBasicBlockRef after_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->fn, "if.end");
        const char *cond = cond_text;
        LLVMValueRef cond_val = pd_jit_expr(cg, &cond);
        LLVMValueRef cond_bool = LLVMBuildFCmp(cg->builder, LLVMRealONE, cond_val, LLVMConstReal(cg->f64, 0.0), "ifblock");
        LLVMBuildCondBr(cg->builder, cond_bool, then_bb, has_else ? else_bb : after_bb);
        LLVMPositionBuilderAtEnd(cg->builder, then_bb);
        pd_jit_emit_linear_cfg(cg, &lines[then_start], then_end - then_start, break_target, continue_target);
        pd_jit_build_br_if_open(cg, after_bb);
        if (has_else) {
          LLVMPositionBuilderAtEnd(cg->builder, else_bb);
          pd_jit_emit_linear_cfg(cg, &lines[else_start], else_end - else_start, break_target, continue_target);
          pd_jit_build_br_if_open(cg, after_bb);
        }
        LLVMPositionBuilderAtEnd(cg->builder, after_bb);
        i = has_else ? else_next : then_next;
      } else if (!strncasecmp(stmt, "goto", 4)) {
        char target[64];
        int tn = 0;
        char *q = stmt + 4;
        while (*q && isspace((unsigned char)*q)) q++;
        while ((isalnum((unsigned char)*q) || *q == '_') && tn < 63) target[tn++] = *q++;
        target[tn] = 0;
        LLVMBasicBlockRef target_bb = pd_jit_find_label_block(labels, label_blocks, label_count, target);
        if (!target_bb) { cg->ok = 0; return; }
        LLVMBasicBlockRef fallthrough = LLVMAppendBasicBlockInContext(cg->ctx, cg->fn, "if.fallthrough");
        const char *cond = cond_text;
        LLVMValueRef cond_val = pd_jit_expr(cg, &cond);
        LLVMValueRef cond_bool = LLVMBuildFCmp(cg->builder, LLVMRealONE, cond_val, LLVMConstReal(cg->f64, 0.0), "ifgoto");
        LLVMBuildCondBr(cg->builder, cond_bool, target_bb, fallthrough);
        LLVMPositionBuilderAtEnd(cg->builder, fallthrough);
      } else if (!strncasecmp(stmt, "break", 5) || !strncasecmp(stmt, "continue", 8)) {
        LLVMBasicBlockRef target_bb = !strncasecmp(stmt, "break", 5) ? break_target : continue_target;
        if (!target_bb) { cg->ok = 0; return; }
        LLVMBasicBlockRef fallthrough = LLVMAppendBasicBlockInContext(cg->ctx, cg->fn, "if.fallthrough");
        const char *cond = cond_text;
        LLVMValueRef cond_val = pd_jit_expr(cg, &cond);
        LLVMValueRef cond_bool = LLVMBuildFCmp(cg->builder, LLVMRealONE, cond_val, LLVMConstReal(cg->f64, 0.0), "ifctl");
        LLVMBuildCondBr(cg->builder, cond_bool, target_bb, fallthrough);
        LLVMPositionBuilderAtEnd(cg->builder, fallthrough);
      } else {
        LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->fn, "if.then");
        LLVMBasicBlockRef after_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->fn, "if.end");
        const char *cond = cond_text;
        LLVMValueRef cond_val = pd_jit_expr(cg, &cond);
        LLVMValueRef cond_bool = LLVMBuildFCmp(cg->builder, LLVMRealONE, cond_val, LLVMConstReal(cg->f64, 0.0), "ifstmt");
        LLVMBuildCondBr(cg->builder, cond_bool, then_bb, after_bb);
        LLVMPositionBuilderAtEnd(cg->builder, then_bb);
        pd_jit_emit_statement(cg, stmt);
        pd_jit_build_br_if_open(cg, after_bb);
        LLVMPositionBuilderAtEnd(cg->builder, after_bb);
      }
      continue;
    }
    if (!strncmp(s, "break", 5)) {
      if (!break_target) { cg->ok = 0; return; }
      pd_jit_build_br_if_open(cg, break_target);
      LLVMBasicBlockRef dead = LLVMAppendBasicBlockInContext(cg->ctx, cg->fn, "after.break");
      LLVMPositionBuilderAtEnd(cg->builder, dead);
      continue;
    }
    if (!strncmp(s, "continue", 8)) {
      if (!continue_target) { cg->ok = 0; return; }
      pd_jit_build_br_if_open(cg, continue_target);
      LLVMBasicBlockRef dead = LLVMAppendBasicBlockInContext(cg->ctx, cg->fn, "after.continue");
      LLVMPositionBuilderAtEnd(cg->builder, dead);
      continue;
    }
    if (!strncmp(s, "goto", 4)) {
      char target[64];
      int tn = 0;
      char *q = s + 4;
      while (*q && isspace((unsigned char)*q)) q++;
      while ((isalnum((unsigned char)*q) || *q == '_') && tn < 63) target[tn++] = *q++;
      target[tn] = 0;
      LLVMBasicBlockRef target_bb = pd_jit_find_label_block(labels, label_blocks, label_count, target);
      if (!target_bb) { cg->ok = 0; return; }
      pd_jit_build_br_if_open(cg, target_bb);
      LLVMBasicBlockRef dead = LLVMAppendBasicBlockInContext(cg->ctx, cg->fn, "after.goto");
      LLVMPositionBuilderAtEnd(cg->builder, dead);
      continue;
    }
    if (!strncmp(s, "return", 6)) {
      const char *expr = s + 6;
      LLVMBasicBlockRef current = LLVMGetInsertBlock(cg->builder);
      if (current && !LLVMGetBasicBlockTerminator(current)) LLVMBuildRet(cg->builder, pd_jit_expr(cg, &expr));
      LLVMBasicBlockRef dead = LLVMAppendBasicBlockInContext(cg->ctx, cg->fn, "after.return");
      LLVMPositionBuilderAtEnd(cg->builder, dead);
      continue;
    }
    if (!strncmp(s, "printf", 6) || !*s || *s == "{"[0] || *s == "}"[0]) continue;
    pd_jit_emit_statement(cg, s);
  }
}

static void pd_jit_parse_declaration(PdJitCodegen *cg, char *line) {
  char *s = pd_jit_trim(line);
  if (!strncmp(s, "enum", 4)) {
    char *open = strchr(s, '{');
    char *eq = strchr(s, '=');
    char *close = strchr(s, '}');
    if (open && eq && close && eq < close) {
      char name[64];
      int n = 0;
      const char *p = open + 1;
      while ((isalnum((unsigned char)*p) || *p == '_') && n < 63) name[n++] = *p++;
      name[n] = 0;
      pd_jit_add_const(cg, name, strtod(eq + 1, NULL));
    }
    return;
  }
  if (strncmp(s, "static", 6)) return;
  s += 6;
  char *semi = strchr(s, ';');
  if (semi) *semi = 0;
  char *part = strtok(s, ",");
  while (part) {
    char *name = pd_jit_trim(part);
    char *br = strchr(name, '[');
    if (br) {
      *br = 0;
      pd_jit_add_array(cg, pd_jit_trim(name), 1024);
    } else {
      pd_jit_add_var(cg, name);
    }
    part = strtok(NULL, ",");
  }
}

static void pd_jit_emit_lines(PdJitCodegen *cg, int *idx, int stop_on_brace) {
  while (*idx < cg->line_count && cg->ok) {
    char line[512];
    snprintf(line, sizeof(line), "%s", cg->lines[*idx]);
    char *s = pd_jit_trim(line);
    (*idx)++;
    if (!*s || *s == '{') continue;
    if (*s == '}') {
      if (stop_on_brace) return;
      continue;
    }
    if (!strncmp(s, "enum", 4) || !strncmp(s, "static", 6)) continue;
    if (!strncmp(s, "if", 2)) {
      const char *open = strchr(s, '(');
      const char *close = open ? pd_jit_after_matching_paren(open) : NULL;
      if (!open || !close) {
        cg->ok = 0;
        return;
      }
      char cond_text[256];
      size_t clen = (size_t)((close - 1) - open - 1);
      if (clen >= sizeof(cond_text)) clen = sizeof(cond_text) - 1;
      memcpy(cond_text, open + 1, clen);
      cond_text[clen] = 0;
      const char *cond = cond_text;
      LLVMValueRef cond_val = pd_jit_expr(cg, &cond);
      LLVMValueRef cond_bool = LLVMBuildFCmp(cg->builder, LLVMRealONE, cond_val, LLVMConstReal(cg->f64, 0.0), "ifbool");
      LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->fn, "if.then");
      LLVMBasicBlockRef after_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->fn, "if.end");
      LLVMBuildCondBr(cg->builder, cond_bool, then_bb, after_bb);
      LLVMPositionBuilderAtEnd(cg->builder, then_bb);
      if (*idx < cg->line_count) {
        char next[512];
        snprintf(next, sizeof(next), "%s", cg->lines[*idx]);
        if (*pd_jit_trim(next) == '{') (*idx)++;
      }
      pd_jit_emit_lines(cg, idx, 1);
      LLVMBuildBr(cg->builder, after_bb);
      LLVMPositionBuilderAtEnd(cg->builder, after_bb);
      continue;
    }
    if (!strncmp(s, "return", 6)) {
      const char *expr = s + 6;
      LLVMBuildRet(cg->builder, pd_jit_expr(cg, &expr));
      return;
    }
    pd_jit_emit_statement(cg, s);
  }
}

static void pd_jit_prepare_lines(PdJitCodegen *cg, const char *source) {
  const char *p = source;
  while (*p && cg->line_count < (int)(sizeof(cg->lines) / sizeof(cg->lines[0]))) {
    const char *eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) : strlen(p);
    if (len >= sizeof(cg->lines[0])) len = sizeof(cg->lines[0]) - 1;
    memcpy(cg->lines[cg->line_count], p, len);
    cg->lines[cg->line_count][len] = 0;
    char *comment = strstr(cg->lines[cg->line_count], "//");
    if (comment) *comment = 0;
    char *trimmed = pd_jit_trim(cg->lines[cg->line_count]);
    if (*trimmed) {
      if (trimmed != cg->lines[cg->line_count]) memmove(cg->lines[cg->line_count], trimmed, strlen(trimmed) + 1);
      cg->line_count++;
    }
    if (!eol) break;
    p = eol + 1;
  }
}

static void pd_jit_emit_user_function(PdJitCodegen *cg, const PdJitFunctionSource *src) {
  LLVMValueRef saved_fn = cg->fn;
  PdJitVar saved_vars[256];
  int saved_var_count = cg->var_count;
  memcpy(saved_vars, cg->vars, sizeof(saved_vars));

  LLVMTypeRef fn_type = pd_jit_function_type(cg, src);
  char fn_name[96];
  snprintf(fn_name, sizeof(fn_name), "pd_user_%s", src->name);
  LLVMValueRef fn = LLVMGetNamedFunction(cg->module, fn_name);
  if (!fn) fn = LLVMAddFunction(cg->module, fn_name, fn_type);
  cg->fn = fn;
  cg->var_count = 0;
  memset(cg->vars, 0, sizeof(cg->vars));
  LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(cg->ctx, fn, "entry");
  LLVMPositionBuilderAtEnd(cg->builder, entry);
  for (int i = 0; i < src->param_count; i++) {
    if (src->param_array[i]) {
      pd_jit_bind_array_param(cg, src->params[i], LLVMGetParam(fn, (unsigned)i));
    } else if (src->param_ref[i]) {
      pd_jit_bind_ref_var(cg, src->params[i], LLVMGetParam(fn, (unsigned)i));
    } else {
      PdJitVar *v = pd_jit_add_var(cg, src->params[i]);
      if (v) LLVMBuildStore(cg->builder, LLVMGetParam(fn, (unsigned)i), v->slot);
    }
  }
  pd_jit_emit_linear_cfg(cg, (char (*)[512])src->lines, src->line_count, NULL, NULL);
  LLVMBasicBlockRef current = LLVMGetInsertBlock(cg->builder);
  if (current && !LLVMGetBasicBlockTerminator(current)) LLVMBuildRet(cg->builder, LLVMConstReal(cg->f64, 0.0));

  cg->fn = saved_fn;
  cg->var_count = saved_var_count;
  memcpy(cg->vars, saved_vars, sizeof(cg->vars));
  LLVMBasicBlockRef main_block = LLVMGetLastBasicBlock(saved_fn);
  if (main_block) LLVMPositionBuilderAtEnd(cg->builder, main_block);
}

static int pd_jit_execute_double(LLVMModuleRef module, LLVMOrcThreadSafeContextRef tsc, double *out_return) {
  LLVMOrcLLJITRef jit = NULL;
  LLVMErrorRef err = LLVMOrcCreateLLJIT(&jit, NULL);
  if (err) {
    char *msg = LLVMGetErrorMessage(err);
    fprintf(stderr, "LLVM LLJIT create failed: %s\n", msg);
    LLVMDisposeErrorMessage(msg);
    return 0;
  }
  LLVMOrcThreadSafeModuleRef tsm = LLVMOrcCreateNewThreadSafeModule(module, tsc);
  err = LLVMOrcLLJITAddLLVMIRModule(jit, LLVMOrcLLJITGetMainJITDylib(jit), tsm);
  if (err) {
    char *msg = LLVMGetErrorMessage(err);
    fprintf(stderr, "LLVM add module failed: %s\n", msg);
    LLVMDisposeErrorMessage(msg);
    LLVMOrcDisposeLLJIT(jit);
    return 0;
  }
  LLVMOrcExecutorAddress addr = 0;
  err = LLVMOrcLLJITLookup(jit, &addr, "pd_jit_main");
  if (err || !addr) {
    if (err) {
      char *msg = LLVMGetErrorMessage(err);
      fprintf(stderr, "LLVM lookup failed: %s\n", msg);
      LLVMDisposeErrorMessage(msg);
    }
    LLVMOrcDisposeLLJIT(jit);
    return 0;
  }
  PdJitSubsetFn fn = (PdJitSubsetFn)(uintptr_t)addr;
  *out_return = fn();
  LLVMOrcDisposeLLJIT(jit);
  return 1;
}

int pd_jit_run_subset(const char *source, double *out_return, char *output, int output_size) {
  if (!source || !out_return) return 0;
  pd_jit_init_native();
  PdJitCodegen cg;
  memset(&cg, 0, sizeof(cg));
  cg.ok = 1;
  cg.ctx = LLVMContextCreate();
  LLVMOrcThreadSafeContextRef tsc = LLVMOrcCreateNewThreadSafeContextFromLLVMContext(cg.ctx);
  cg.module = LLVMModuleCreateWithNameInContext("polydraw_subset", cg.ctx);
  cg.builder = LLVMCreateBuilderInContext(cg.ctx);
  cg.f64 = LLVMDoubleTypeInContext(cg.ctx);
  cg.i32 = LLVMInt32TypeInContext(cg.ctx);
  LLVMTypeRef fn_type = LLVMFunctionType(cg.f64, NULL, 0, 0);
  cg.fn = LLVMAddFunction(cg.module, "pd_jit_main", fn_type);
  LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(cg.ctx, cg.fn, "entry");
  LLVMPositionBuilderAtEnd(cg.builder, entry);
  pd_jit_add_var(&cg, "numframes");
  pd_jit_add_var(&cg, "xres");
  pd_jit_add_var(&cg, "yres");
  PdJitVar *xres = pd_jit_find_var(&cg, "xres");
  PdJitVar *yres = pd_jit_find_var(&cg, "yres");
  if (xres) LLVMBuildStore(cg.builder, LLVMConstReal(cg.f64, 640.0), xres->slot);
  if (yres) LLVMBuildStore(cg.builder, LLVMConstReal(cg.f64, 480.0), yres->slot);

  PdJitProgramSource *program = (PdJitProgramSource *)calloc(1, sizeof(PdJitProgramSource));
  if (!program) {
    LLVMDisposeBuilder(cg.builder);
    LLVMOrcDisposeThreadSafeContext(tsc);
    return 0;
  }
  pd_jit_prepare_program(program, source);
  for (int i = 0; i < program->function_count; i++) {
    LLVMTypeRef fn_type = pd_jit_function_type(&cg, &program->functions[i]);
    char fn_name[96];
    snprintf(fn_name, sizeof(fn_name), "pd_user_%s", program->functions[i].name);
    if (!LLVMGetNamedFunction(cg.module, fn_name)) LLVMAddFunction(cg.module, fn_name, fn_type);
  }
  for (int i = 0; i < program->function_count; i++) {
    pd_jit_emit_user_function(&cg, &program->functions[i]);
  }
  cg.line_count = program->main_count > 512 ? 512 : program->main_count;
  for (int i = 0; i < cg.line_count; i++) snprintf(cg.lines[i], sizeof(cg.lines[i]), "%s", program->main_lines[i]);
  for (int i = 0; i < cg.line_count; i++) {
    char line[512];
    snprintf(line, sizeof(line), "%s", cg.lines[i]);
    pd_jit_parse_declaration(&cg, line);
  }
  pd_jit_emit_linear_cfg(&cg, cg.lines, cg.line_count, NULL, NULL);
  LLVMBasicBlockRef current = LLVMGetInsertBlock(cg.builder);
  if (current && !LLVMGetBasicBlockTerminator(current)) {
    LLVMBuildRet(cg.builder, LLVMConstReal(cg.f64, 0.0));
  }
  char *error = NULL;
  if (!cg.ok || LLVMVerifyModule(cg.module, LLVMReturnStatusAction, &error)) {
    if (error) {
      fprintf(stderr, "LLVM subset verification failed: %s\n", error);
      LLVMDisposeMessage(error);
    } else {
      fprintf(stderr, "LLVM subset lowering failed before verification\n");
    }
    LLVMDisposeBuilder(cg.builder);
    LLVMOrcDisposeThreadSafeContext(tsc);
    free(program);
    return 0;
  }
  int ok = pd_jit_execute_double(cg.module, tsc, out_return);
  LLVMDisposeBuilder(cg.builder);
  LLVMOrcDisposeThreadSafeContext(tsc);
  free(program);
  if (ok && output && output_size > 0) {
    if (strstr(source, "printf")) snprintf(output, (size_t)output_size, "%f\n", *out_return);
    else output[0] = 0;
  }
  return ok;
}
#else
int pd_jit_available(void) {
  return 0;
}

const char *pd_jit_status(void) {
  return "LLVM not available at configure time; build with LLVM to enable JIT";
}

int pd_jit_smoke_value(void) {
  return -1;
}

int pd_jit_run_subset(const char *source, double *out_return, char *output, int output_size) {
  (void)source;
  if (out_return) *out_return = 0.0;
  if (output && output_size > 0) output[0] = 0;
  return 0;
}
#endif
