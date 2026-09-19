#include "polydraw/shader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

PdShaderCompileResult pd_shader_validate_sections(const char *source, const PdSectionList *sections) {
  PdShaderCompileResult result;
  memset(&result, 0, sizeof(result));
  result.ok = 1;
  int vertex_count = 0;
  int fragment_count = 0;
  for (size_t i = 0; i < sections->count; i++) {
    const PdSection *s = &sections->items[i];
    if (s->type == PD_SECTION_VERTEX) vertex_count++;
    if (s->type == PD_SECTION_FRAGMENT) fragment_count++;
    if ((s->type == PD_SECTION_VERTEX || s->type == PD_SECTION_FRAGMENT || s->type == PD_SECTION_GEOMETRY) && s->end <= s->start) {
      result.ok = 0;
      snprintf(result.log, sizeof(result.log), "empty shader section at line %d", s->line);
      return result;
    }
  }
  if (!vertex_count || !fragment_count) {
    result.ok = 0;
    snprintf(result.log, sizeof(result.log), "expected at least one vertex and one fragment shader section");
    return result;
  }
  (void)source;
  snprintf(result.log, sizeof(result.log), "shader sections validated: vertex=%d fragment=%d", vertex_count, fragment_count);
  return result;
}

static void replace_all(char *text, size_t cap, const char *from, const char *to) {
  char *pos = strstr(text, from);
  while (pos) {
    size_t before = (size_t)(pos - text);
    size_t from_len = strlen(from);
    size_t to_len = strlen(to);
    size_t tail_len = strlen(pos + from_len);
    if (before + to_len + tail_len + 1 >= cap) return;
    memmove(pos + to_len, pos + from_len, tail_len + 1);
    memcpy(pos, to, to_len);
    pos = strstr(pos + to_len, from);
  }
}

static int contains_any(const char *text, const char **needles, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (strstr(text, needles[i])) return 1;
  }
  return 0;
}

static void append_decl(char *out, size_t cap, const char *decl) {
  if (strlen(out) + strlen(decl) + 1 < cap) strcat(out, decl);
}

static void trim_inplace(char *s) {
  char *p = s;
  while (*p && isspace((unsigned char)*p)) p++;
  if (p != s) memmove(s, p, strlen(p) + 1);
  size_t len = strlen(s);
  while (len && isspace((unsigned char)s[len - 1])) s[--len] = 0;
}

static void strip_comment(char *s) {
  char *p = strchr(s, '#');
  if (p) *p = 0;
  p = strstr(s, "//");
  if (p) *p = 0;
}

static int starts_ci(const char *s, const char *prefix) {
  while (*prefix) {
    if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
    s++;
    prefix++;
  }
  return 1;
}

static int parse_arb_parts(char *line, char *op, size_t op_size, char *dst, size_t dst_size, char args[][256], int *arg_count) {
  trim_inplace(line);
  if (!line[0]) return 0;
  char *space = line;
  while (*space && !isspace((unsigned char)*space)) space++;
  size_t op_len = (size_t)(space - line);
  if (op_len >= op_size) op_len = op_size - 1;
  memcpy(op, line, op_len);
  op[op_len] = 0;
  while (*space && isspace((unsigned char)*space)) space++;
  char *comma = strchr(space, ',');
  if (!comma) {
    dst[0] = 0;
    *arg_count = 0;
    return 1;
  }
  size_t dst_len = (size_t)(comma - space);
  if (dst_len >= dst_size) dst_len = dst_size - 1;
  memcpy(dst, space, dst_len);
  dst[dst_len] = 0;
  trim_inplace(dst);
  char *p = comma + 1;
  *arg_count = 0;
  while (*p && *arg_count < 4) {
    while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
    if (!*p) break;
    int brace = 0;
    char *start = p;
    while (*p) {
      if (*p == '{') brace++;
      else if (*p == '}') brace--;
      else if (*p == ',' && brace == 0) break;
      p++;
    }
    size_t len = (size_t)(p - start);
    if (len > 255) len = 255;
    memcpy(args[*arg_count], start, len);
    args[*arg_count][len] = 0;
    trim_inplace(args[*arg_count]);
    (*arg_count)++;
    if (*p == ',') p++;
  }
  return 1;
}

static int arb_op_saturate(char *op) {
  char *sat = strstr(op, "_SAT");
  if (!sat) return 0;
  *sat = 0;
  return 1;
}

typedef struct ArbAlias {
  char name[64];
  char expr[192];
} ArbAlias;

typedef struct ArbAliasMap {
  ArbAlias items[96];
  int count;
} ArbAliasMap;

static const char *arb_alias_lookup(const ArbAliasMap *aliases, const char *name) {
  if (!aliases || !name || !name[0]) return NULL;
  for (int i = aliases->count - 1; i >= 0; i--) {
    if (!strcmp(aliases->items[i].name, name)) return aliases->items[i].expr;
  }
  return NULL;
}

static void arb_alias_add(ArbAliasMap *aliases, const char *name, const char *expr) {
  if (!aliases || !name || !name[0] || !expr || !expr[0]) return;
  for (int i = 0; i < aliases->count; i++) {
    if (!strcmp(aliases->items[i].name, name)) {
      snprintf(aliases->items[i].expr, sizeof(aliases->items[i].expr), "%s", expr);
      return;
    }
  }
  if (aliases->count >= (int)(sizeof(aliases->items) / sizeof(aliases->items[0]))) return;
  snprintf(aliases->items[aliases->count].name, sizeof(aliases->items[aliases->count].name), "%s", name);
  snprintf(aliases->items[aliases->count].expr, sizeof(aliases->items[aliases->count].expr), "%s", expr);
  aliases->count++;
}

static void parse_arb_alias_line(ArbAliasMap *aliases, char *line) {
  char *p = line;
  while (*p && !isspace((unsigned char)*p)) p++;
  while (*p && isspace((unsigned char)*p)) p++;
  char *eq = strchr(p, '=');
  if (!eq) return;
  char *semi = strchr(eq, ';');
  if (semi) *semi = 0;
  *eq = 0;
  char *name = p;
  trim_inplace(name);
  char *expr = eq + 1;
  trim_inplace(expr);
  char *bracket = strchr(name, '[');
  if (bracket) *bracket = 0;
  trim_inplace(name);
  if (name[0] && expr[0]) arb_alias_add(aliases, name, expr);
}

static void split_arb_swizzle(char *buf, char *swizzle, size_t swizzle_size) {
  swizzle[0] = 0;
  char *dot = strrchr(buf, '.');
  if (!dot || !dot[1]) return;
  for (char *p = dot + 1; *p; p++) {
    char c = (char)tolower((unsigned char)*p);
    if (!strchr("xyzwrgba stpq", c) || c == ' ') return;
  }
  snprintf(swizzle, swizzle_size, "%s", dot);
  *dot = 0;
}

static void arb_register_expr_alias(const char *in, char *out, size_t cap, const ArbAliasMap *aliases);

static void arb_register_expr(const char *in, char *out, size_t cap) {
  arb_register_expr_alias(in, out, cap, NULL);
}

static void arb_register_expr_alias(const char *in, char *out, size_t cap, const ArbAliasMap *aliases) {
  char buf[256];
  snprintf(buf, sizeof(buf), "%s", in ? in : "");
  trim_inplace(buf);
  int neg = 0;
  if (buf[0] == '-') {
    neg = 1;
    memmove(buf, buf + 1, strlen(buf));
    trim_inplace(buf);
  }
  char swizzle[32] = "";
  split_arb_swizzle(buf, swizzle, sizeof(swizzle));
  const char *alias = arb_alias_lookup(aliases, buf);
  if (alias) {
    char resolved[256];
    arb_register_expr_alias(alias, resolved, sizeof(resolved), aliases);
    snprintf(out, cap, "%s%s%s", neg ? "-" : "", resolved, swizzle);
    return;
  }
  char base[192];
  if (buf[0] == '{') {
    float values[4] = {0, 0, 0, 0};
    int count = 0;
    const char *p = buf + 1;
    while (*p && *p != '}' && count < 4) {
      while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
      if (!*p || *p == '}') break;
      char *end = NULL;
      values[count++] = strtof(p, &end);
      if (end == p) break;
      p = end;
    }
    if (count == 1) values[1] = values[2] = values[3] = values[0];
    else if (count == 2) values[2] = values[3] = 0;
    else if (count == 3) values[3] = 0;
    snprintf(base, sizeof(base), "vec4(%g,%g,%g,%g)", values[0], values[1], values[2], values[3]);
  } else if (starts_ci(buf, "vertex.position")) {
    snprintf(base, sizeof(base), "_pd_position");
  } else if (starts_ci(buf, "vertex.color")) {
    snprintf(base, sizeof(base), "_pd_color");
  } else if (starts_ci(buf, "vertex.texcoord[0]") || starts_ci(buf, "vertex.texcoord")) {
    snprintf(base, sizeof(base), "_pd_texcoord");
  } else if (starts_ci(buf, "fragment.color")) {
    snprintf(base, sizeof(base), "_pd_front_color");
  } else if (starts_ci(buf, "fragment.texcoord[0]") || starts_ci(buf, "fragment.texcoord")) {
    snprintf(base, sizeof(base), "_pd_texcoord0");
  } else if (starts_ci(buf, "texture[0]")) {
    snprintf(base, sizeof(base), "tex0");
  } else if (starts_ci(buf, "program.env[")) {
    int index = atoi(buf + strlen("program.env["));
    snprintf(base, sizeof(base), "_pd_programEnv[%d]", index);
  } else if (starts_ci(buf, "program.local[")) {
    int index = atoi(buf + strlen("program.local["));
    snprintf(base, sizeof(base), "_pd_programLocal[%d]", index);
  } else if (starts_ci(buf, "ModelViewProj[")) {
    int index = atoi(buf + strlen("ModelViewProj["));
    snprintf(base, sizeof(base), "_pd_mvpRow%d()", index);
  } else if (starts_ci(buf, "result.position")) {
    snprintf(base, sizeof(base), "gl_Position");
  } else if (starts_ci(buf, "result.color")) {
    snprintf(base, sizeof(base), "fragColor");
  } else if (starts_ci(buf, "result.depth")) {
    snprintf(base, sizeof(base), "gl_FragDepth");
  } else {
    snprintf(base, sizeof(base), "%s", buf);
  }
  snprintf(out, cap, "%s%s%s", neg ? "-" : "", base, swizzle);
}

static void arb_dst_expr(const char *in, char *out, size_t cap, GLenum shader_type) {
  char buf[256];
  snprintf(buf, sizeof(buf), "%s", in ? in : "");
  trim_inplace(buf);
  char swizzle[32] = "";
  split_arb_swizzle(buf, swizzle, sizeof(swizzle));
  if (starts_ci(buf, "result.position")) snprintf(out, cap, "gl_Position%s", swizzle);
  else if (starts_ci(buf, "result.color")) snprintf(out, cap, "%s%s", shader_type == GL_VERTEX_SHADER ? "_pd_front_color" : "fragColor", swizzle);
  else if (starts_ci(buf, "result.depth")) snprintf(out, cap, "gl_FragDepth%s", swizzle);
  else if (starts_ci(buf, "result.texcoord[0]") || starts_ci(buf, "result.texcoord")) snprintf(out, cap, "_pd_texcoord0%s", swizzle);
  else snprintf(out, cap, "%s%s", buf, swizzle);
}

static void emit_arb_assignment(char *out, size_t cap, const char *dst, const char *expr, GLenum shader_type) {
  char d[256];
  arb_dst_expr(dst, d, sizeof(d), shader_type);
  int mask_len = 0;
  const char *dot = strrchr(d, '.');
  if (dot) {
    mask_len = (int)strlen(dot + 1);
    if (mask_len < 1 || mask_len > 4) mask_len = 0;
  }
  append_decl(out, cap, "  ");
  append_decl(out, cap, d);
  append_decl(out, cap, " = ");
  int scalar_target = mask_len == 1 || !strcmp(d, "gl_FragDepth");
  int vector_target = !scalar_target && !strchr(d, '.');
  if (scalar_target) append_decl(out, cap, "_pd_scalar(");
  else if (vector_target) append_decl(out, cap, "_pd_vec4(");
  append_decl(out, cap, expr);
  if (scalar_target || vector_target) append_decl(out, cap, ")");
  else if (mask_len == 2) append_decl(out, cap, ".xy");
  else if (mask_len == 3) append_decl(out, cap, ".xyz");
  append_decl(out, cap, ";\n");
}

static void emit_arb_assignment_sat(char *out, size_t cap, const char *dst, const char *expr, GLenum shader_type, int saturate) {
  char wrapped[900];
  if (saturate) snprintf(wrapped, sizeof(wrapped), "clamp(%s, 0.0, 1.0)", expr);
  else snprintf(wrapped, sizeof(wrapped), "%s", expr);
  emit_arb_assignment(out, cap, dst, wrapped, shader_type);
}

static char *modernize_arb_assembly(const char *source, const PdSection *section, GLenum shader_type, GLint *out_len) {
  size_t len = section->end - section->start;
  size_t cap = len * 16 + 4096;
  char *out = (char *)calloc(1, cap);
  if (!out) return NULL;
  if (shader_type == GL_VERTEX_SHADER) {
    snprintf(out, cap,
      "#version 150\n"
      "in vec4 _pd_position;\n"
      "in vec4 _pd_color;\n"
      "in vec4 _pd_texcoord;\n"
      "uniform mat4 _pd_mvp;\n"
      "uniform vec4 _pd_programEnv[32];\n"
      "uniform vec4 _pd_programLocal[32];\n"
      "out vec4 _pd_front_color;\n"
      "out vec4 _pd_texcoord0;\n"
      "vec4 _pd_mvpRow0(){return vec4(_pd_mvp[0][0],_pd_mvp[1][0],_pd_mvp[2][0],_pd_mvp[3][0]);}\n"
      "vec4 _pd_mvpRow1(){return vec4(_pd_mvp[0][1],_pd_mvp[1][1],_pd_mvp[2][1],_pd_mvp[3][1]);}\n"
      "vec4 _pd_mvpRow2(){return vec4(_pd_mvp[0][2],_pd_mvp[1][2],_pd_mvp[2][2],_pd_mvp[3][2]);}\n"
      "vec4 _pd_mvpRow3(){return vec4(_pd_mvp[0][3],_pd_mvp[1][3],_pd_mvp[2][3],_pd_mvp[3][3]);}\n"
      "float _pd_scalar(float v){return v;}\n"
      "float _pd_scalar(vec2 v){return v.x;}\n"
      "float _pd_scalar(vec3 v){return v.x;}\n"
      "float _pd_scalar(vec4 v){return v.x;}\n"
      "vec4 _pd_vec4(float v){return vec4(v);}\n"
      "vec4 _pd_vec4(vec2 v){return vec4(v,0.0,0.0);}\n"
      "vec4 _pd_vec4(vec3 v){return vec4(v,0.0);}\n"
      "vec4 _pd_vec4(vec4 v){return v;}\n"
      "void main(){\n");
  } else {
    snprintf(out, cap,
      "#version 150\n"
      "uniform sampler2D tex0;\n"
      "uniform vec4 _pd_programEnv[32];\n"
      "uniform vec4 _pd_programLocal[32];\n"
      "in vec4 _pd_front_color;\n"
      "in vec4 _pd_texcoord0;\n"
      "out vec4 fragColor;\n"
      "float _pd_scalar(float v){return v;}\n"
      "float _pd_scalar(vec2 v){return v.x;}\n"
      "float _pd_scalar(vec3 v){return v.x;}\n"
      "float _pd_scalar(vec4 v){return v.x;}\n"
      "vec4 _pd_vec4(float v){return vec4(v);}\n"
      "vec4 _pd_vec4(vec2 v){return vec4(v,0.0,0.0);}\n"
      "vec4 _pd_vec4(vec3 v){return vec4(v,0.0);}\n"
      "vec4 _pd_vec4(vec4 v){return v;}\n"
      "void main(){\n");
  }

  char *body = (char *)calloc(1, len + 1);
  if (!body) {
    free(out);
    return NULL;
  }
  memcpy(body, source + section->start, len);
  ArbAliasMap aliases;
  memset(&aliases, 0, sizeof(aliases));
  char *save = NULL;
  for (char *line = strtok_r(body, "\n\r;", &save); line; line = strtok_r(NULL, "\n\r;", &save)) {
    strip_comment(line);
    trim_inplace(line);
    if (!line[0] || starts_ci(line, "!!ARB") || starts_ci(line, "END")) continue;
    if (starts_ci(line, "PARAM ") || starts_ci(line, "ATTRIB ") || starts_ci(line, "OUTPUT ") || starts_ci(line, "TEMP ")) {
      if (starts_ci(line, "PARAM ") || starts_ci(line, "ATTRIB ")) parse_arb_alias_line(&aliases, line);
      if (starts_ci(line, "TEMP ")) {
        char *p = line + 5;
        char *tok_save = NULL;
        for (char *tok = strtok_r(p, ",;", &tok_save); tok; tok = strtok_r(NULL, ",;", &tok_save)) {
          trim_inplace(tok);
          if (tok[0]) {
            append_decl(out, cap, "  vec4 ");
            append_decl(out, cap, tok);
            append_decl(out, cap, " = vec4(0.0);\n");
          }
        }
      }
      continue;
    }
    char op[32], dst[256], args[4][256];
    int argc = 0;
    if (!parse_arb_parts(line, op, sizeof(op), dst, sizeof(dst), args, &argc)) continue;
    int saturate = arb_op_saturate(op);
    char a[256], b[256], c[256], expr[768];
    if (starts_ci(op, "MOV") && argc >= 1) {
      arb_register_expr_alias(args[0], a, sizeof(a), &aliases);
      snprintf(expr, sizeof(expr), "%s", a);
      emit_arb_assignment_sat(out, cap, dst, expr, shader_type, saturate);
    } else if (starts_ci(op, "ADD") && argc >= 2) {
      arb_register_expr_alias(args[0], a, sizeof(a), &aliases); arb_register_expr_alias(args[1], b, sizeof(b), &aliases);
      snprintf(expr, sizeof(expr), "(%s + %s)", a, b);
      emit_arb_assignment_sat(out, cap, dst, expr, shader_type, saturate);
    } else if (starts_ci(op, "SUB") && argc >= 2) {
      arb_register_expr_alias(args[0], a, sizeof(a), &aliases); arb_register_expr_alias(args[1], b, sizeof(b), &aliases);
      snprintf(expr, sizeof(expr), "(%s - %s)", a, b);
      emit_arb_assignment_sat(out, cap, dst, expr, shader_type, saturate);
    } else if (starts_ci(op, "MUL") && argc >= 2) {
      arb_register_expr_alias(args[0], a, sizeof(a), &aliases); arb_register_expr_alias(args[1], b, sizeof(b), &aliases);
      snprintf(expr, sizeof(expr), "(%s * %s)", a, b);
      emit_arb_assignment_sat(out, cap, dst, expr, shader_type, saturate);
    } else if (starts_ci(op, "MAD") && argc >= 3) {
      arb_register_expr_alias(args[0], a, sizeof(a), &aliases); arb_register_expr_alias(args[1], b, sizeof(b), &aliases); arb_register_expr_alias(args[2], c, sizeof(c), &aliases);
      snprintf(expr, sizeof(expr), "(%s * %s + %s)", a, b, c);
      emit_arb_assignment_sat(out, cap, dst, expr, shader_type, saturate);
    } else if (starts_ci(op, "DP3") && argc >= 2) {
      arb_register_expr_alias(args[0], a, sizeof(a), &aliases); arb_register_expr_alias(args[1], b, sizeof(b), &aliases);
      snprintf(expr, sizeof(expr), strchr(dst, '.') ? "dot(%s.xyz, %s.xyz)" : "vec4(dot(%s.xyz, %s.xyz))", a, b);
      emit_arb_assignment_sat(out, cap, dst, expr, shader_type, saturate);
    } else if (starts_ci(op, "DP4") && argc >= 2) {
      arb_register_expr_alias(args[0], a, sizeof(a), &aliases); arb_register_expr_alias(args[1], b, sizeof(b), &aliases);
      snprintf(expr, sizeof(expr), strchr(dst, '.') ? "dot(%s, %s)" : "vec4(dot(%s, %s))", a, b);
      emit_arb_assignment_sat(out, cap, dst, expr, shader_type, saturate);
    } else if (starts_ci(op, "ABS") && argc >= 1) {
      arb_register_expr_alias(args[0], a, sizeof(a), &aliases);
      snprintf(expr, sizeof(expr), "abs(%s)", a);
      emit_arb_assignment_sat(out, cap, dst, expr, shader_type, saturate);
    } else if (starts_ci(op, "RCP") && argc >= 1) {
      arb_register_expr_alias(args[0], a, sizeof(a), &aliases);
      snprintf(expr, sizeof(expr), "(1.0 / %s)", a);
      emit_arb_assignment_sat(out, cap, dst, expr, shader_type, saturate);
    } else if (starts_ci(op, "RSQ") && argc >= 1) {
      arb_register_expr_alias(args[0], a, sizeof(a), &aliases);
      snprintf(expr, sizeof(expr), "inversesqrt(%s)", a);
      emit_arb_assignment_sat(out, cap, dst, expr, shader_type, saturate);
    } else if (starts_ci(op, "FRC") && argc >= 1) {
      arb_register_expr_alias(args[0], a, sizeof(a), &aliases);
      snprintf(expr, sizeof(expr), "fract(%s)", a);
      emit_arb_assignment_sat(out, cap, dst, expr, shader_type, saturate);
    } else if (starts_ci(op, "MIN") && argc >= 2) {
      arb_register_expr_alias(args[0], a, sizeof(a), &aliases); arb_register_expr_alias(args[1], b, sizeof(b), &aliases);
      snprintf(expr, sizeof(expr), "min(_pd_vec4(%s), _pd_vec4(%s))", a, b);
      emit_arb_assignment_sat(out, cap, dst, expr, shader_type, saturate);
    } else if (starts_ci(op, "MAX") && argc >= 2) {
      arb_register_expr_alias(args[0], a, sizeof(a), &aliases); arb_register_expr_alias(args[1], b, sizeof(b), &aliases);
      snprintf(expr, sizeof(expr), "max(_pd_vec4(%s), _pd_vec4(%s))", a, b);
      emit_arb_assignment_sat(out, cap, dst, expr, shader_type, saturate);
    } else if (starts_ci(op, "CMP") && argc >= 3) {
      arb_register_expr_alias(args[0], a, sizeof(a), &aliases); arb_register_expr_alias(args[1], b, sizeof(b), &aliases); arb_register_expr_alias(args[2], c, sizeof(c), &aliases);
      snprintf(expr, sizeof(expr), "mix(_pd_vec4(%s), _pd_vec4(%s), lessThan(_pd_vec4(%s), vec4(0.0)))", c, b, a);
      emit_arb_assignment_sat(out, cap, dst, expr, shader_type, saturate);
    } else if (starts_ci(op, "KIL") && argc >= 1) {
      arb_register_expr_alias(args[0], a, sizeof(a), &aliases);
      append_decl(out, cap, "  if (any(lessThan(_pd_vec4(");
      append_decl(out, cap, a);
      append_decl(out, cap, "), vec4(0.0)))) discard;\n");
    } else if (starts_ci(op, "TEX") && argc >= 3) {
      arb_register_expr_alias(args[0], b, sizeof(b), &aliases);
      char sampler[64] = "tex0";
      if (starts_ci(args[1], "texture[")) {
        int unit = atoi(args[1] + strlen("texture["));
        snprintf(sampler, sizeof(sampler), "tex%d", unit);
      }
      snprintf(expr, sizeof(expr), "texture(%s, %s.xy)", sampler, b);
      emit_arb_assignment_sat(out, cap, dst, expr, shader_type, saturate);
    }
  }
  append_decl(out, cap, "}\n");
  free(body);
  *out_len = (GLint)strlen(out);
  return out;
}

static char *modernize_glsl_with_options(const char *source, const PdSection *section, GLenum shader_type, int fragment_from_geometry, GLint *out_len) {
  size_t len = section->end - section->start;
  if (len >= 5 && strstr(source + section->start, "!!ARB")) {
    return modernize_arb_assembly(source, section, shader_type, out_len);
  }
  size_t cap = len * 4 + 2048;
  char *out = (char *)calloc(1, cap);
  if (!out) return NULL;
  const char *body = source + section->start;
  int has_version = len >= 8 && !strncmp(body, "#version", 8);
  int force_geometry_modernize = 0;
#if defined(GL_GEOMETRY_SHADER)
  force_geometry_modernize = shader_type == GL_GEOMETRY_SHADER;
#endif
  if (force_geometry_modernize) has_version = 0;
  if (!has_version) {
    if (shader_type == GL_VERTEX_SHADER) {
      snprintf(out, cap,
        "#version 150\n"
        "in vec4 _pd_position;\n"
        "in vec4 _pd_color;\n"
        "in vec4 _pd_texcoord;\n"
        "in vec3 _pd_normal;\n"
        "uniform mat4 _pd_mvp;\n"
        "uniform mat4 _pd_modelview;\n"
        "uniform mat3 _pd_normalMatrix;\n"
        "out vec4 _pd_front_color;\n"
        "out vec4 _pd_texcoord0;\n");
#if defined(GL_GEOMETRY_SHADER)
    } else if (shader_type == GL_GEOMETRY_SHADER) {
      snprintf(out, cap,
        "#version 150\n"
        "layout(triangles) in;\n"
        "layout(triangle_strip, max_vertices = 15) out;\n"
        "uniform mat4 _pd_mvp;\n"
        "in vec4 _pd_front_color[];\n"
        "in vec4 _pd_texcoord0[];\n"
        "out vec4 _pd_geom_front_color;\n"
        "out vec4 _pd_geom_texcoord0;\n");
#endif
    } else {
      if (fragment_from_geometry) {
        snprintf(out, cap,
          "#version 150\n"
          "in vec4 _pd_geom_front_color;\n"
          "in vec4 _pd_geom_texcoord0;\n"
          "out vec4 fragColor;\n");
      } else {
        snprintf(out, cap,
          "#version 150\n"
          "in vec4 _pd_front_color;\n"
          "in vec4 _pd_texcoord0;\n"
          "out vec4 fragColor;\n");
      }
    }
  }
  if (force_geometry_modernize) {
    const char *cursor = body;
    const char *line_end = strchr(cursor, '\n');
    if (line_end && !strncmp(cursor, "#version", 8)) cursor = line_end + 1;
    if (!strncmp(cursor, "#extension", 10)) {
      line_end = strchr(cursor, '\n');
      if (line_end) cursor = line_end + 1;
    }
    strncat(out, cursor, len - (size_t)(cursor - body));
  } else {
    strncat(out, body, len);
  }
  if (!has_version) {
    static const char *frag_needles[] = {"gl_FragColor.r", "gl_FragColor.g", "gl_FragColor.b", "gl_FragColor.a", "gl_FragColor["};
    if (shader_type != GL_VERTEX_SHADER && contains_any(out, frag_needles, sizeof(frag_needles) / sizeof(frag_needles[0]))) {
      char *insert = strstr(out, "void main");
      if (insert) {
        size_t prefix_len = (size_t)(insert - out);
        const char *decl = "vec4 _pd_fragColor;\n";
        if (strlen(out) + strlen(decl) + 1 < cap) {
          memmove(insert + strlen(decl), insert, strlen(insert) + 1);
          memcpy(insert, decl, strlen(decl));
        }
      }
      char *last_close = strrchr(out, '}');
      if (last_close) {
        const char *assign = "\nfragColor = _pd_fragColor;\n";
        if (strlen(out) + strlen(assign) + 1 < cap) {
          memmove(last_close + strlen(assign), last_close, strlen(last_close) + 1);
          memcpy(last_close, assign, strlen(assign));
        }
      }
      replace_all(out, cap, "gl_FragColor", "_pd_fragColor");
    }
    replace_all(out, cap, "attribute ", "in ");
    replace_all(out, cap, "varying ", shader_type == GL_VERTEX_SHADER ? "out " : "in ");
    replace_all(out, cap, "gl_FragColor", "fragColor");
    replace_all(out, cap, "texture2D", "texture");
    replace_all(out, cap, "texture3D", "texture");
    replace_all(out, cap, "textureCube", "texture");
    replace_all(out, cap, "texture2DLod", "textureLod");
    replace_all(out, cap, "ftransform()", "(_pd_mvp * _pd_position)");
    if (shader_type == GL_VERTEX_SHADER) {
      replace_all(out, cap, "gl_Vertex", "_pd_position");
      replace_all(out, cap, "gl_Color", "_pd_color");
      replace_all(out, cap, "gl_MultiTexCoord0", "_pd_texcoord");
      replace_all(out, cap, "gl_NormalMatrix", "_pd_normalMatrix");
      replace_all(out, cap, "gl_Normal", "_pd_normal");
      replace_all(out, cap, "gl_FrontColor", "_pd_front_color");
      replace_all(out, cap, "gl_TexCoord[0]", "_pd_texcoord0");
      replace_all(out, cap, "gl_ModelViewProjectionMatrix", "_pd_mvp");
      replace_all(out, cap, "gl_ModelViewMatrix", "_pd_modelview");
      replace_all(out, cap, "gl_ProjectionMatrix", "mat4(1.0)");
#if defined(GL_GEOMETRY_SHADER)
    } else if (shader_type == GL_GEOMETRY_SHADER) {
      replace_all(out, cap, "gl_TexCoordIn[i][0]", "_pd_texcoord0[i]");
      replace_all(out, cap, "gl_TexCoordIn[0][0]", "_pd_texcoord0[0]");
      replace_all(out, cap, "gl_TexCoordIn[1][0]", "_pd_texcoord0[1]");
      replace_all(out, cap, "gl_TexCoordIn[2][0]", "_pd_texcoord0[2]");
      replace_all(out, cap, "gl_PositionIn", "gl_in");
      replace_all(out, cap, "gl_in[i]", "gl_in[i].gl_Position");
      replace_all(out, cap, "gl_in[0]", "gl_in[0].gl_Position");
      replace_all(out, cap, "gl_in[1]", "gl_in[1].gl_Position");
      replace_all(out, cap, "gl_in[2]", "gl_in[2].gl_Position");
      replace_all(out, cap, "gl_VerticesIn", "3");
      replace_all(out, cap, "gl_FrontColorIn", "_pd_front_color");
      replace_all(out, cap, "gl_FrontColor", "_pd_geom_front_color");
      replace_all(out, cap, "gl_TexCoordIn", "_pd_texcoord0");
      replace_all(out, cap, "gl_TexCoord[0]", "_pd_geom_texcoord0");
      replace_all(out, cap, "gl_ModelViewProjectionMatrix", "_pd_mvp");
#endif
    } else {
      replace_all(out, cap, "gl_Color", fragment_from_geometry ? "_pd_geom_front_color" : "_pd_front_color");
      replace_all(out, cap, "gl_TexCoord[0]", fragment_from_geometry ? "_pd_geom_texcoord0" : "_pd_texcoord0");
    }
  }
  *out_len = (GLint)strlen(out);
  return out;
}

static char *modernize_glsl(const char *source, const PdSection *section, GLenum shader_type, GLint *out_len) {
  return modernize_glsl_with_options(source, section, shader_type, 0, out_len);
}

static PdShaderCompileResult compile_one(const char *source, const PdSection *section, GLenum shader_type) {
  PdShaderCompileResult result;
  memset(&result, 0, sizeof(result));
  result.ok = 1;

  GLuint shader = glCreateShader(shader_type);
  if (!shader) {
    result.ok = 0;
    snprintf(result.log, sizeof(result.log), "glCreateShader failed for section line %d", section->line);
    return result;
  }

  GLint len = 0;
  char *modernized = modernize_glsl(source, section, shader_type, &len);
  if (!modernized) {
    result.ok = 0;
    snprintf(result.log, sizeof(result.log), "out of memory modernizing shader");
    glDeleteShader(shader);
    return result;
  }
  const GLchar *ptr = (const GLchar *)modernized;
  glShaderSource(shader, 1, &ptr, &len);
  glCompileShader(shader);

  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    GLsizei log_len = 0;
    glGetShaderInfoLog(shader, (GLsizei)sizeof(result.log), &log_len, result.log);
    result.ok = 0;
  } else {
    snprintf(result.log, sizeof(result.log), "compiled shader section at line %d", section->line);
  }
  free(modernized);
  glDeleteShader(shader);
  return result;
}

static GLuint compile_shader_object_with_options(const char *source, const PdSection *section, GLenum shader_type, int fragment_from_geometry, PdShaderCompileResult *result) {
  GLuint shader = glCreateShader(shader_type);
  if (!shader) {
    result->ok = 0;
    snprintf(result->log, sizeof(result->log), "glCreateShader failed for section line %d", section->line);
    return 0;
  }

  GLint len = 0;
  char *modernized = modernize_glsl_with_options(source, section, shader_type, fragment_from_geometry, &len);
  if (!modernized) {
    result->ok = 0;
    snprintf(result->log, sizeof(result->log), "out of memory modernizing shader");
    glDeleteShader(shader);
    return 0;
  }
  if (getenv("POLYDRAW_DUMP_SHADER")) fprintf(stderr, "----- shader line %d -----\n%s\n", section->line, modernized);
  const GLchar *ptr = (const GLchar *)modernized;
  glShaderSource(shader, 1, &ptr, &len);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    GLsizei log_len = 0;
    glGetShaderInfoLog(shader, (GLsizei)sizeof(result->log), &log_len, result->log);
    result->ok = 0;
    free(modernized);
    glDeleteShader(shader);
    return 0;
  }
  free(modernized);
  return shader;
}

static GLuint compile_shader_object(const char *source, const PdSection *section, GLenum shader_type, PdShaderCompileResult *result) {
  return compile_shader_object_with_options(source, section, shader_type, 0, result);
}

unsigned int pd_shader_link_sections_with_geometry(const char *source, const PdSection *vertex, const PdSection *geometry, const PdSection *fragment, PdShaderCompileResult *result) {
  memset(result, 0, sizeof(*result));
  result->ok = 1;
  if (!source || !vertex || !fragment) {
    result->ok = 0;
    snprintf(result->log, sizeof(result->log), "missing shader source or section");
    return 0;
  }

  GLuint vs = compile_shader_object(source, vertex, GL_VERTEX_SHADER, result);
  if (!vs) return 0;
  GLuint gs = 0;
#if defined(GL_GEOMETRY_SHADER)
  if (geometry) {
    gs = compile_shader_object(source, geometry, GL_GEOMETRY_SHADER, result);
    if (!gs) {
      glDeleteShader(vs);
      return 0;
    }
  }
#else
  if (geometry) {
    result->ok = 0;
    snprintf(result->log, sizeof(result->log), "geometry shaders are not available in this OpenGL header");
    glDeleteShader(vs);
    return 0;
  }
#endif
  GLuint fs = compile_shader_object_with_options(source, fragment, GL_FRAGMENT_SHADER, geometry != NULL, result);
  if (!fs) {
    if (gs) glDeleteShader(gs);
    glDeleteShader(vs);
    return 0;
  }

  GLuint program = glCreateProgram();
  glAttachShader(program, vs);
  if (gs) glAttachShader(program, gs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  glDeleteShader(vs);
  if (gs) glDeleteShader(gs);
  glDeleteShader(fs);
  GLint linked = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (!linked) {
    GLsizei log_len = 0;
    glGetProgramInfoLog(program, (GLsizei)sizeof(result->log), &log_len, result->log);
    result->ok = 0;
    glDeleteProgram(program);
    return 0;
  }
  if (geometry) snprintf(result->log, sizeof(result->log), "linked shader sections v:%d g:%d f:%d", vertex->line, geometry->line, fragment->line);
  else snprintf(result->log, sizeof(result->log), "linked shader sections v:%d f:%d", vertex->line, fragment->line);
  return (unsigned int)program;
}

unsigned int pd_shader_link_sections(const char *source, const PdSection *vertex, const PdSection *fragment, PdShaderCompileResult *result) {
  return pd_shader_link_sections_with_geometry(source, vertex, NULL, fragment, result);
}

static int write_ppm(const char *path, const unsigned char *rgba, int width, int height) {
  FILE *fp = fopen(path, "wb");
  if (!fp) return 0;
  fprintf(fp, "P6\n%d %d\n255\n", width, height);
  for (int y = height - 1; y >= 0; y--) {
    const unsigned char *row = rgba + (size_t)y * (size_t)width * 4u;
    for (int x = 0; x < width; x++) fwrite(row + x * 4, 1, 3, fp);
  }
  fclose(fp);
  return 1;
}

PdShaderCompileResult pd_shader_compile_sections(const char *source, const PdSectionList *sections) {
  PdShaderCompileResult preflight = pd_shader_validate_sections(source, sections);
  if (!preflight.ok) return preflight;

  int compiled = 0;
  for (size_t i = 0; i < sections->count; i++) {
    const PdSection *s = &sections->items[i];
    GLenum type = 0;
    if (s->type == PD_SECTION_VERTEX) type = GL_VERTEX_SHADER;
    else if (s->type == PD_SECTION_FRAGMENT) type = GL_FRAGMENT_SHADER;
#if defined(GL_GEOMETRY_SHADER)
    else if (s->type == PD_SECTION_GEOMETRY) type = GL_GEOMETRY_SHADER;
#endif
    if (!type) continue;

    PdShaderCompileResult result = compile_one(source, s, type);
    if (!result.ok) return result;
    compiled++;
  }

  PdShaderCompileResult result;
  memset(&result, 0, sizeof(result));
  result.ok = compiled > 0;
  snprintf(result.log, sizeof(result.log), "compiled %d shader sections", compiled);
  return result;
}

PdShaderCompileResult pd_shader_render_first_pair_ppm(const char *source, const PdSectionList *sections, const char *out_path, int width, int height) {
  PdShaderCompileResult result = pd_shader_validate_sections(source, sections);
  if (!result.ok) return result;
  memset(&result, 0, sizeof(result));
  result.ok = 1;

  const PdSection *vertex = NULL;
  const PdSection *fragment = NULL;
  for (size_t i = 0; i < sections->count; i++) {
    if (!vertex && sections->items[i].type == PD_SECTION_VERTEX) vertex = &sections->items[i];
    if (!fragment && sections->items[i].type == PD_SECTION_FRAGMENT) fragment = &sections->items[i];
  }
  if (!vertex || !fragment) {
    result.ok = 0;
    snprintf(result.log, sizeof(result.log), "missing vertex or fragment section");
    return result;
  }

  GLuint program = (GLuint)pd_shader_link_sections(source, vertex, fragment, &result);
  if (!program) return result;

  GLuint tex = 0, fbo = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    result.ok = 0;
    snprintf(result.log, sizeof(result.log), "framebuffer incomplete");
    goto cleanup;
  }

  glViewport(0, 0, width, height);
  glClearColor(0.f, 0.f, 0.f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(program);

  GLfloat verts[] = {
    /* position, color, texcoord */
    -1.f, -1.f, 1.f, 1.f, 1.f, 1.f, 0.f, 0.f, 0.f, 1.f,
     1.f, -1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f, 0.f, 1.f,
    -1.f,  1.f, 1.f, 1.f, 1.f, 1.f, 0.f, 1.f, 0.f, 1.f,
     1.f,  1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f, 1.f,
  };
  GLuint vao = 0, vbo = 0;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
  GLint pos = glGetAttribLocation(program, "_pd_position");
  if (pos < 0) pos = glGetAttribLocation(program, "position");
  if (pos < 0) pos = 0;
  glEnableVertexAttribArray((GLuint)pos);
  glVertexAttribPointer((GLuint)pos, 2, GL_FLOAT, GL_FALSE, 10 * (GLsizei)sizeof(GLfloat), (void *)0);
  GLint color_attr = glGetAttribLocation(program, "color");
  if (color_attr < 0) color_attr = glGetAttribLocation(program, "_pd_color");
  if (color_attr >= 0) {
    glEnableVertexAttribArray((GLuint)color_attr);
    glVertexAttribPointer((GLuint)color_attr, 4, GL_FLOAT, GL_FALSE, 10 * (GLsizei)sizeof(GLfloat), (void *)(2 * sizeof(GLfloat)));
  }
  GLint tex_attr = glGetAttribLocation(program, "texcoord");
  if (tex_attr < 0) tex_attr = glGetAttribLocation(program, "_pd_texcoord");
  if (tex_attr >= 0) {
    glEnableVertexAttribArray((GLuint)tex_attr);
    glVertexAttribPointer((GLuint)tex_attr, 4, GL_FLOAT, GL_FALSE, 10 * (GLsizei)sizeof(GLfloat), (void *)(6 * sizeof(GLfloat)));
  }
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  unsigned char *pixels = (unsigned char *)malloc((size_t)width * (size_t)height * 4u);
  if (!pixels) {
    result.ok = 0;
    snprintf(result.log, sizeof(result.log), "out of memory");
  } else {
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (!write_ppm(out_path, pixels, width, height)) {
      result.ok = 0;
      snprintf(result.log, sizeof(result.log), "failed to write %s", out_path);
    } else {
      snprintf(result.log, sizeof(result.log), "rendered %dx%d to %s", width, height, out_path);
    }
    free(pixels);
  }
  glDeleteBuffers(1, &vbo);
  glDeleteVertexArrays(1, &vao);

cleanup:
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (fbo) glDeleteFramebuffers(1, &fbo);
  if (tex) glDeleteTextures(1, &tex);
  glDeleteProgram(program);
  return result;
}
