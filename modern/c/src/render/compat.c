#include "polydraw/compat.h"
#include "polydraw/render.h"
#include "polydraw/shader.h"

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#if defined(POLYDRAW_WITH_JPEG)
#include <jpeglib.h>
#endif

#if defined(POLYDRAW_WITH_PNG)
#include <png.h>
#endif

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

typedef struct PdCompatTextureEntry {
  int texture_id;
  GLuint texture;
  GLenum target;
} PdCompatTextureEntry;

typedef struct PdCompatProgramEntry {
  char vertex_shader[64];
  char geometry_shader[64];
  char fragment_shader[64];
  GLuint program;
} PdCompatProgramEntry;

struct PdCompatRenderer {
  GLuint fallback_program;
  GLuint vao;
  GLuint vbo;
  GLuint white_textures[8];
  GLuint white_cubemaps[8];
  GLuint white_textures3d[8];
  PdCompatTextureEntry texture_cache[64];
  int texture_cache_count;
  PdCompatProgramEntry program_cache[64];
  int program_cache_count;
};

static GLuint compile_shader(GLenum type, const char *src, PdCompatRenderResult *result) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &src, NULL);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    glGetShaderInfoLog(shader, (GLsizei)sizeof(result->log), NULL, result->log);
    result->ok = 0;
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

static GLuint make_program(PdCompatRenderResult *result) {
  const char *vs_src =
    "#version 150\n"
    "in vec4 position;\n"
    "in vec4 color;\n"
    "in vec4 texcoord;\n"
    "out vec4 v_color;\n"
    "out vec4 v_texcoord;\n"
    "void main(){ v_color=color; v_texcoord=texcoord; gl_Position=position; }\n";
  const char *fs_src =
    "#version 150\n"
    "in vec4 v_color;\n"
    "in vec4 v_texcoord;\n"
    "out vec4 fragColor;\n"
    "void main(){ fragColor=v_color; }\n";
  GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src, result);
  if (!vs) return 0;
  GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src, result);
  if (!fs) {
    glDeleteShader(vs);
    return 0;
  }
  GLuint program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    glGetProgramInfoLog(program, (GLsizei)sizeof(result->log), NULL, result->log);
    result->ok = 0;
    glDeleteProgram(program);
    return 0;
  }
  return program;
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

static GLenum map_mode(int mode) {
  switch (mode) {
    case 0: return GL_POINTS;
    case 1: return GL_LINES;
    case 2: return GL_LINE_LOOP;
    case 3: return GL_LINE_STRIP;
    case 4: return GL_TRIANGLES;
    case 5: return GL_TRIANGLE_STRIP;
    case 6: return GL_TRIANGLE_FAN;
    case 7: return GL_TRIANGLES;
    case 9: return GL_TRIANGLE_FAN;
    default: return GL_TRIANGLE_STRIP;
  }
}

static const PdSection *find_section_by_key(const PdSectionList *sections, PdSectionType type, const char *key) {
  if (!sections) return NULL;
  int index = 0;
  int want_index = 0;
  int key_is_index = 1;
  if (key && key[0] && strcmp(key, "-")) {
    char *end = NULL;
    long parsed = strtol(key, &end, 10);
    key_is_index = end && *end == 0;
    if (key_is_index) want_index = (int)parsed;
  }
  for (size_t i = 0; i < sections->count; i++) {
    const PdSection *section = &sections->items[i];
    if (section->type != type) continue;
    if (!key || !key[0] || !strcmp(key, "-")) return section;
    if (key_is_index && index == want_index) return section;
    if (!key_is_index && !strcmp(section->name, key)) return section;
    index++;
  }
  return NULL;
}

static GLuint shader_program_for_batch(
  const char *source,
  const PdSectionList *sections,
  const PdCompatBatch *batch,
  PdCompatRenderResult *result
) {
  if (!source || !sections || !batch) return 0;
  const PdSection *vertex = find_section_by_key(sections, PD_SECTION_VERTEX, batch->vertex_shader);
  const PdSection *geometry = NULL;
  if (batch->geometry_shader[0] && strcmp(batch->geometry_shader, "-")) {
    geometry = find_section_by_key(sections, PD_SECTION_GEOMETRY, batch->geometry_shader);
  }
  const PdSection *fragment = find_section_by_key(sections, PD_SECTION_FRAGMENT, batch->fragment_shader);
  if (!vertex || !fragment) return 0;
  PdShaderCompileResult shader_result;
  GLuint program = (GLuint)pd_shader_link_sections_with_geometry(source, vertex, geometry, fragment, &shader_result);
  if (!program) {
    result->ok = 0;
    snprintf(result->log, sizeof(result->log), "shader link failed for batch shader %s/%s/%s: %s",
      batch->vertex_shader, batch->geometry_shader, batch->fragment_shader, shader_result.log);
  }
  return program;
}

static GLuint renderer_program_for_batch(
  PdCompatRenderer *renderer,
  const char *source,
  const PdSectionList *sections,
  const PdCompatBatch *batch,
  PdCompatRenderResult *result
) {
  if (!renderer || !batch || !source || !sections) return 0;
  if ((!batch->vertex_shader[0] || !strcmp(batch->vertex_shader, "-")) &&
      (!batch->geometry_shader[0] || !strcmp(batch->geometry_shader, "-")) &&
      (!batch->fragment_shader[0] || !strcmp(batch->fragment_shader, "-"))) {
    return 0;
  }
  for (int i = 0; i < renderer->program_cache_count; i++) {
    PdCompatProgramEntry *entry = &renderer->program_cache[i];
    if (!strcmp(entry->vertex_shader, batch->vertex_shader) &&
        !strcmp(entry->geometry_shader, batch->geometry_shader) &&
        !strcmp(entry->fragment_shader, batch->fragment_shader)) {
      return entry->program;
    }
  }
  if (renderer->program_cache_count >= (int)(sizeof(renderer->program_cache) / sizeof(renderer->program_cache[0]))) {
    return shader_program_for_batch(source, sections, batch, result);
  }
  GLuint program = shader_program_for_batch(source, sections, batch, result);
  if (!program) return 0;
  PdCompatProgramEntry *entry = &renderer->program_cache[renderer->program_cache_count++];
  snprintf(entry->vertex_shader, sizeof(entry->vertex_shader), "%s", batch->vertex_shader);
  snprintf(entry->geometry_shader, sizeof(entry->geometry_shader), "%s", batch->geometry_shader);
  snprintf(entry->fragment_shader, sizeof(entry->fragment_shader), "%s", batch->fragment_shader);
  entry->program = program;
  return program;
}

static void bind_batch_attributes(GLuint program, GLuint vbo) {
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  GLint position = glGetAttribLocation(program, "_pd_position");
  if (position < 0) position = glGetAttribLocation(program, "position");
  if (position >= 0) {
    glEnableVertexAttribArray((GLuint)position);
    glVertexAttribPointer((GLuint)position, 4, GL_FLOAT, GL_FALSE, sizeof(PdCompatVertex), (void *)offsetof(PdCompatVertex, position));
  }
  GLint color = glGetAttribLocation(program, "color");
  if (color < 0) color = glGetAttribLocation(program, "_pd_color");
  if (color >= 0) {
    glEnableVertexAttribArray((GLuint)color);
    glVertexAttribPointer((GLuint)color, 4, GL_FLOAT, GL_FALSE, sizeof(PdCompatVertex), (void *)offsetof(PdCompatVertex, color));
  }
  GLint texcoord = glGetAttribLocation(program, "texcoord");
  if (texcoord < 0) texcoord = glGetAttribLocation(program, "_pd_texcoord");
  if (texcoord >= 0) {
    glEnableVertexAttribArray((GLuint)texcoord);
    glVertexAttribPointer((GLuint)texcoord, 4, GL_FLOAT, GL_FALSE, sizeof(PdCompatVertex), (void *)offsetof(PdCompatVertex, texcoord));
  }
  GLint normal = glGetAttribLocation(program, "normal");
  if (normal < 0) normal = glGetAttribLocation(program, "_pd_normal");
  if (normal >= 0) {
    glEnableVertexAttribArray((GLuint)normal);
    glVertexAttribPointer((GLuint)normal, 3, GL_FLOAT, GL_FALSE, sizeof(PdCompatVertex), (void *)offsetof(PdCompatVertex, normal));
  }
}

static void apply_batch_uniforms(GLuint program, const PdCompatBatch *batch) {
  if (!batch || !batch->uniforms) return;
  for (int i = 0; i < batch->uniform_count; i++) {
    const PdCompatUniform *u = &batch->uniforms[i];
    GLint loc = glGetUniformLocation(program, u->name);
    if (loc < 0 && u->count > 1) {
      char array_name[80];
      snprintf(array_name, sizeof(array_name), "%s[0]", u->name);
      loc = glGetUniformLocation(program, array_name);
    }
    if (loc < 0) continue;
    if (!u->values || u->value_count <= 0) continue;
    switch (u->type) {
      case 1:
        glUniform1f(loc, u->values[0]);
        break;
      case 2:
        glUniform2f(loc, u->values[0], u->values[1]);
        break;
      case 3:
        glUniform3f(loc, u->values[0], u->values[1], u->values[2]);
        break;
      case 4:
        glUniform4f(loc, u->values[0], u->values[1], u->values[2], u->values[3]);
        break;
      case 11:
        glUniform1i(loc, (GLint)u->values[0]);
        break;
      case 12:
        glUniform2i(loc, (GLint)u->values[0], (GLint)u->values[1]);
        break;
      case 13:
        glUniform3i(loc, (GLint)u->values[0], (GLint)u->values[1], (GLint)u->values[2]);
        break;
      case 14:
        glUniform4i(loc, (GLint)u->values[0], (GLint)u->values[1], (GLint)u->values[2], (GLint)u->values[3]);
        break;
      case 21:
        glUniform1fv(loc, (GLsizei)u->count, u->values);
        break;
      case 22:
        glUniform2fv(loc, (GLsizei)u->count, u->values);
        break;
      case 23:
        glUniform3fv(loc, (GLsizei)u->count, u->values);
        break;
      case 24:
        glUniform4fv(loc, (GLsizei)u->count, u->values);
        break;
      case 31: {
        GLint *iv = (GLint *)malloc(sizeof(GLint) * (size_t)u->value_count);
        if (!iv) break;
        for (int j = 0; j < u->value_count; j++) iv[j] = (GLint)u->values[j];
        glUniform1iv(loc, (GLsizei)u->count, iv);
        free(iv);
        break;
      }
      case 32: {
        GLint *iv = (GLint *)malloc(sizeof(GLint) * (size_t)u->value_count);
        if (!iv) break;
        for (int j = 0; j < u->value_count; j++) iv[j] = (GLint)u->values[j];
        glUniform2iv(loc, (GLsizei)u->count, iv);
        free(iv);
        break;
      }
      case 33: {
        GLint *iv = (GLint *)malloc(sizeof(GLint) * (size_t)u->value_count);
        if (!iv) break;
        for (int j = 0; j < u->value_count; j++) iv[j] = (GLint)u->values[j];
        glUniform3iv(loc, (GLsizei)u->count, iv);
        free(iv);
        break;
      }
      case 34: {
        GLint *iv = (GLint *)malloc(sizeof(GLint) * (size_t)u->value_count);
        if (!iv) break;
        for (int j = 0; j < u->value_count; j++) iv[j] = (GLint)u->values[j];
        glUniform4iv(loc, (GLsizei)u->count, iv);
        free(iv);
        break;
      }
      case 43:
        glUniformMatrix3fv(loc, 1, GL_FALSE, u->values);
        break;
      case 44:
        glUniformMatrix4fv(loc, 1, GL_FALSE, u->values);
        break;
      default:
        break;
    }
  }
}

static PdCompatVertex *expand_quads(const PdCompatVertex *vertices, int vertex_count, int *out_count) {
  int quad_count = vertex_count / 4;
  int tri_count = quad_count * 6;
  PdCompatVertex *expanded = (PdCompatVertex *)malloc(sizeof(PdCompatVertex) * (size_t)tri_count);
  if (!expanded) return NULL;
  for (int q = 0; q < quad_count; q++) {
    const PdCompatVertex *src = vertices + q * 4;
    PdCompatVertex *dst = expanded + q * 6;
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[2];
    dst[4] = src[1];
    dst[5] = src[3];
  }
  *out_count = tri_count;
  return expanded;
}

static int is_fullscreen_clip_quad(const PdCompatBatch *batch) {
  if (!batch || batch->mode != 5 || batch->vertex_count != 4 || !batch->vertices) return 0;
  const float expected[4][2] = {{-1.f, -1.f}, {1.f, -1.f}, {-1.f, 1.f}, {1.f, 1.f}};
  for (int i = 0; i < 4; i++) {
    float dx = batch->vertices[i].position[0] - expected[i][0];
    float dy = batch->vertices[i].position[1] - expected[i][1];
    float z = batch->vertices[i].position[2];
    if (dx < -0.0001f || dx > 0.0001f || dy < -0.0001f || dy > 0.0001f || z < -0.0001f || z > 0.0001f) return 0;
  }
  return 1;
}

static GLuint create_white_texture(void) {
  unsigned char white[4] = {255, 255, 255, 255};
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  return tex;
}

static GLuint create_white_cubemap(void) {
  unsigned char white[4] = {255, 255, 255, 255};
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
  for (int face = 0; face < 6; face++) {
    glTexImage2D((GLenum)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face), 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
  }
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
  return tex;
}

static GLuint create_white_texture3d(void) {
  unsigned char white[4] = {255, 255, 255, 255};
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_3D, tex);
  glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
  return tex;
}

static GLuint create_placeholder_texture(int texture_id) {
  unsigned char pixels[16];
  unsigned int seed = (unsigned int)(texture_id * 1103515245u + 12345u);
  for (int i = 0; i < 4; i++) {
    seed = seed * 1664525u + 1013904223u;
    pixels[i * 4 + 0] = (unsigned char)(32u + ((seed >> 16) & 191u));
    pixels[i * 4 + 1] = (unsigned char)(32u + ((seed >> 8) & 191u));
    pixels[i * 4 + 2] = (unsigned char)(32u + (seed & 191u));
    pixels[i * 4 + 3] = 255;
  }
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  return tex;
}

static const PdCompatTextureDef *find_texture_def(const PdCompatTextureDef *defs, int def_count, int texture_id) {
  for (int i = 0; i < def_count; i++) {
    if (defs[i].texture_id == texture_id) return &defs[i];
  }
  return NULL;
}

static const PdCompatTextureDef *find_capture_def_ending_at(const PdCompatTextureDef *defs, int def_count, int batch_end) {
  for (int i = 0; i < def_count; i++) {
    if (defs[i].capture_end == batch_end) return &defs[i];
  }
  return NULL;
}

static int ppm_next_token(FILE *fp, char *buf, size_t size) {
  int ch = 0;
  do {
    ch = fgetc(fp);
    if (ch == '#') {
      while (ch != EOF && ch != '\n') ch = fgetc(fp);
    }
  } while (ch != EOF && ch <= ' ');
  if (ch == EOF) return 0;
  size_t n = 0;
  while (ch != EOF && ch > ' ') {
    if (n + 1 < size) buf[n++] = (char)ch;
    ch = fgetc(fp);
  }
  buf[n] = 0;
  return n > 0;
}

static int load_ppm_rgba(const char *path, unsigned char **out_pixels, int *out_width, int *out_height) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return 0;
  char token[64];
  if (!ppm_next_token(fp, token, sizeof(token))) {
    fclose(fp);
    return 0;
  }
  int binary = !strcmp(token, "P6");
  if (!binary && strcmp(token, "P3")) {
    fclose(fp);
    return 0;
  }
  if (!ppm_next_token(fp, token, sizeof(token))) {
    fclose(fp);
    return 0;
  }
  int width = atoi(token);
  if (!ppm_next_token(fp, token, sizeof(token))) {
    fclose(fp);
    return 0;
  }
  int height = atoi(token);
  if (!ppm_next_token(fp, token, sizeof(token))) {
    fclose(fp);
    return 0;
  }
  int maxv = atoi(token);
  if (width <= 0 || height <= 0 || maxv <= 0) {
    fclose(fp);
    return 0;
  }
  size_t pixel_count = (size_t)width * (size_t)height;
  unsigned char *pixels = (unsigned char *)calloc(pixel_count * 4u, 1);
  if (!pixels) {
    fclose(fp);
    return 0;
  }
  if (binary) {
    for (size_t i = 0; i < pixel_count; i++) {
      int r = fgetc(fp);
      int g = fgetc(fp);
      int b = fgetc(fp);
      if (r == EOF || g == EOF || b == EOF) {
        free(pixels);
        fclose(fp);
        return 0;
      }
      pixels[i * 4 + 0] = (unsigned char)(r * 255 / maxv);
      pixels[i * 4 + 1] = (unsigned char)(g * 255 / maxv);
      pixels[i * 4 + 2] = (unsigned char)(b * 255 / maxv);
      pixels[i * 4 + 3] = 255;
    }
  } else {
    for (size_t i = 0; i < pixel_count; i++) {
      int rgb[3] = {0, 0, 0};
      for (int c = 0; c < 3; c++) {
        if (!ppm_next_token(fp, token, sizeof(token))) {
          free(pixels);
          fclose(fp);
          return 0;
        }
        rgb[c] = atoi(token);
      }
      pixels[i * 4 + 0] = (unsigned char)(rgb[0] * 255 / maxv);
      pixels[i * 4 + 1] = (unsigned char)(rgb[1] * 255 / maxv);
      pixels[i * 4 + 2] = (unsigned char)(rgb[2] * 255 / maxv);
      pixels[i * 4 + 3] = 255;
    }
  }
  fclose(fp);
  *out_pixels = pixels;
  *out_width = width;
  *out_height = height;
  return 1;
}

#if defined(POLYDRAW_WITH_JPEG)
typedef struct PdJpegError {
  struct jpeg_error_mgr pub;
  jmp_buf jump;
} PdJpegError;

static void pd_jpeg_error_exit(j_common_ptr cinfo) {
  PdJpegError *err = (PdJpegError *)cinfo->err;
  longjmp(err->jump, 1);
}

static void pd_jpeg_output_message(j_common_ptr cinfo) {
  (void)cinfo;
}

static int load_jpeg_rgba(const char *path, unsigned char **out_pixels, int *out_width, int *out_height) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return 0;
  struct jpeg_decompress_struct cinfo;
  PdJpegError jerr;
  memset(&cinfo, 0, sizeof(cinfo));
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = pd_jpeg_error_exit;
  jerr.pub.output_message = pd_jpeg_output_message;
  if (setjmp(jerr.jump)) {
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);
    return 0;
  }
  jpeg_create_decompress(&cinfo);
  jpeg_stdio_src(&cinfo, fp);
  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);
    return 0;
  }
  cinfo.out_color_space = JCS_RGB;
  if (!jpeg_start_decompress(&cinfo)) {
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);
    return 0;
  }
  int width = (int)cinfo.output_width;
  int height = (int)cinfo.output_height;
  int channels = (int)cinfo.output_components;
  if (width <= 0 || height <= 0 || channels < 3) {
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);
    return 0;
  }
  unsigned char *pixels = (unsigned char *)calloc((size_t)width * (size_t)height * 4u, 1);
  unsigned char *row = (unsigned char *)malloc((size_t)width * (size_t)channels);
  if (!pixels || !row) {
    free(row);
    free(pixels);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);
    return 0;
  }
  while (cinfo.output_scanline < cinfo.output_height) {
    JSAMPROW rowptr[1] = {row};
    JDIMENSION y = cinfo.output_scanline;
    jpeg_read_scanlines(&cinfo, rowptr, 1);
    for (int x = 0; x < width; x++) {
      size_t dst = ((size_t)y * (size_t)width + (size_t)x) * 4u;
      size_t src = (size_t)x * (size_t)channels;
      pixels[dst + 0] = row[src + 0];
      pixels[dst + 1] = row[src + 1];
      pixels[dst + 2] = row[src + 2];
      pixels[dst + 3] = 255;
    }
  }
  free(row);
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  fclose(fp);
  *out_pixels = pixels;
  *out_width = width;
  *out_height = height;
  return 1;
}
#endif

#if defined(POLYDRAW_WITH_PNG)
static int load_png_rgba(const char *path, unsigned char **out_pixels, int *out_width, int *out_height) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return 0;
  unsigned char sig[8];
  if (fread(sig, 1, sizeof(sig), fp) != sizeof(sig) || png_sig_cmp(sig, 0, sizeof(sig))) {
    fclose(fp);
    return 0;
  }
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!png) {
    fclose(fp);
    return 0;
  }
  png_infop info = png_create_info_struct(png);
  if (!info) {
    png_destroy_read_struct(&png, NULL, NULL);
    fclose(fp);
    return 0;
  }
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    return 0;
  }
  png_init_io(png, fp);
  png_set_sig_bytes(png, 8);
  png_read_info(png, info);
  png_uint_32 width = png_get_image_width(png, info);
  png_uint_32 height = png_get_image_height(png, info);
  int color_type = png_get_color_type(png, info);
  int bit_depth = png_get_bit_depth(png, info);
  if (bit_depth == 16) png_set_strip_16(png);
  if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
  if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
  if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
  if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
  if (!(color_type & PNG_COLOR_MASK_ALPHA)) png_set_add_alpha(png, 255, PNG_FILLER_AFTER);
  png_read_update_info(png, info);
  if (width == 0 || height == 0) {
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    return 0;
  }
  unsigned char *pixels = (unsigned char *)malloc((size_t)width * (size_t)height * 4u);
  png_bytep *rows = (png_bytep *)malloc(sizeof(png_bytep) * (size_t)height);
  if (!pixels || !rows) {
    free(rows);
    free(pixels);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    return 0;
  }
  for (png_uint_32 y = 0; y < height; y++) rows[y] = pixels + (size_t)y * (size_t)width * 4u;
  png_read_image(png, rows);
  png_read_end(png, NULL);
  free(rows);
  png_destroy_read_struct(&png, &info, NULL);
  fclose(fp);
  *out_pixels = pixels;
  *out_width = (int)width;
  *out_height = (int)height;
  return 1;
}
#endif

static GLuint create_defined_texture(const PdCompatTextureDef *def, GLenum requested_target, GLenum *out_target) {
  if (out_target) *out_target = requested_target;
  int width = def->width > 0 ? def->width : 1;
  int height = def->height > 0 ? def->height : 1;
  int depth = def->depth > 0 ? def->depth : 1;
  size_t pixel_count = (size_t)width * (size_t)height * (size_t)depth;
  unsigned char *pixels = (unsigned char *)calloc(pixel_count * 4u, 1);
  if (!pixels) return 0;
  if (def->path[0]) {
    unsigned char *ppm_pixels = NULL;
    int ppm_width = 0;
    int ppm_height = 0;
    if (load_ppm_rgba(def->path, &ppm_pixels, &ppm_width, &ppm_height)) {
      free(pixels);
      pixels = ppm_pixels;
      width = ppm_width;
      height = ppm_height;
      depth = 1;
      pixel_count = (size_t)width * (size_t)height * (size_t)depth;
    }
#if defined(POLYDRAW_WITH_JPEG)
    else if (load_jpeg_rgba(def->path, &ppm_pixels, &ppm_width, &ppm_height)) {
      free(pixels);
      pixels = ppm_pixels;
      width = ppm_width;
      height = ppm_height;
      depth = 1;
      pixel_count = (size_t)width * (size_t)height * (size_t)depth;
    }
#endif
#if defined(POLYDRAW_WITH_PNG)
    else if (load_png_rgba(def->path, &ppm_pixels, &ppm_width, &ppm_height)) {
      free(pixels);
      pixels = ppm_pixels;
      width = ppm_width;
      height = ppm_height;
      depth = 1;
      pixel_count = (size_t)width * (size_t)height * (size_t)depth;
    }
#endif
    else {
      FILE *fp = fopen(def->path, "rb");
      if (!fp) {
        free(pixels);
        return 0;
      }
      size_t got = fread(pixels, 1, pixel_count * 4u, fp);
      fclose(fp);
      if (got != pixel_count * 4u) {
        free(pixels);
        return 0;
      }
    }
  } else if ((def->format & 15) == 4) {
    for (size_t i = 0; i < pixel_count; i++) {
      float v = i < (size_t)def->value_count ? def->values[i] : 0.f;
      if (v < 0.f) v = 0.f;
      if (v > 1.f) v = 1.f;
      unsigned char b = (unsigned char)(v * 255.f + 0.5f);
      pixels[i * 4 + 0] = b;
      pixels[i * 4 + 1] = b;
      pixels[i * 4 + 2] = b;
      pixels[i * 4 + 3] = 255;
    }
  } else {
    for (size_t i = 0; i < pixel_count; i++) {
      unsigned int packed = i < (size_t)def->value_count ? (unsigned int)def->values[i] : 0xffffffffu;
      pixels[i * 4 + 0] = (unsigned char)((packed >> 16) & 255u);
      pixels[i * 4 + 1] = (unsigned char)((packed >> 8) & 255u);
      pixels[i * 4 + 2] = (unsigned char)(packed & 255u);
      pixels[i * 4 + 3] = (unsigned char)((packed >> 24) & 255u);
    }
  }
  GLuint tex = 0;
  glGenTextures(1, &tex);
  int cube_face = 0;
  int cube_vertical = 0;
  if (height == width * 6) {
    cube_face = width;
    cube_vertical = 1;
  } else if (width == height * 6) {
    cube_face = height;
  }
  if (requested_target == GL_TEXTURE_3D) {
    if (out_target) *out_target = GL_TEXTURE_3D;
    glBindTexture(GL_TEXTURE_3D, tex);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, width, height, depth, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
  } else if (requested_target == GL_TEXTURE_CUBE_MAP && cube_face > 0) {
    if (out_target) *out_target = GL_TEXTURE_CUBE_MAP;
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
    unsigned char *face_pixels = (unsigned char *)malloc((size_t)cube_face * (size_t)cube_face * 4u);
    if (!face_pixels) {
      glDeleteTextures(1, &tex);
      free(pixels);
      return 0;
    }
    for (int face = 0; face < 6; face++) {
      for (int y = 0; y < cube_face; y++) {
        for (int x = 0; x < cube_face; x++) {
          int src_x = cube_vertical ? x : face * cube_face + x;
          int src_y = cube_vertical ? face * cube_face + y : y;
          memcpy(
            face_pixels + ((size_t)y * (size_t)cube_face + (size_t)x) * 4u,
            pixels + ((size_t)src_y * (size_t)width + (size_t)src_x) * 4u,
            4u
          );
        }
      }
      glTexImage2D((GLenum)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face), 0, GL_RGBA8, cube_face, cube_face, 0, GL_RGBA, GL_UNSIGNED_BYTE, face_pixels);
    }
    free(face_pixels);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
  } else {
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  }
  free(pixels);
  return tex;
}

static PdCompatTextureEntry texture_for_id(
  int texture_id,
  GLenum requested_target,
  const PdCompatTextureDef *defs,
  int def_count,
  PdCompatTextureEntry *textures,
  int *texture_count,
  int max_textures
) {
  for (int i = 0; i < *texture_count; i++) {
    if (textures[i].texture_id == texture_id && textures[i].target == requested_target) return textures[i];
  }
  PdCompatTextureEntry empty = {texture_id, 0, requested_target};
  if (*texture_count >= max_textures) return empty;
  const PdCompatTextureDef *def = find_texture_def(defs, def_count, texture_id);
  if (def && def->capture_end > 0) return empty;
  GLenum target = requested_target;
  GLuint tex = def ? create_defined_texture(def, requested_target, &target) : 0;
  if (!tex) {
    target = requested_target;
    tex = requested_target == GL_TEXTURE_CUBE_MAP ? create_white_cubemap()
      : requested_target == GL_TEXTURE_3D ? create_white_texture3d()
      : create_placeholder_texture(texture_id);
  }
  textures[*texture_count].texture_id = texture_id;
  textures[*texture_count].texture = tex;
  textures[*texture_count].target = target;
  (*texture_count)++;
  PdCompatTextureEntry entry = {texture_id, tex, target};
  return entry;
}

static void cache_texture_id(
  int texture_id,
  GLuint tex,
  GLenum target,
  PdCompatTextureEntry *textures,
  int *texture_count,
  int max_textures
) {
  if (!tex) return;
  for (int i = 0; i < *texture_count; i++) {
    if (textures[i].texture_id == texture_id) {
      if (textures[i].texture) glDeleteTextures(1, &textures[i].texture);
      textures[i].texture = tex;
      textures[i].target = target;
      return;
    }
  }
  if (*texture_count >= max_textures) {
    glDeleteTextures(1, &tex);
    return;
  }
  textures[*texture_count].texture_id = texture_id;
  textures[*texture_count].texture = tex;
  textures[*texture_count].target = target;
  (*texture_count)++;
}

static GLuint create_capture_texture_from_framebuffer(const PdCompatTextureDef *def, int framebuffer_width, int framebuffer_height) {
  int width = def->width > 0 ? def->width : framebuffer_width;
  int height = def->height > 0 ? def->height : framebuffer_height;
  if (width <= 0) width = 1;
  if (height <= 0) height = 1;
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  int copy_width = width < framebuffer_width ? width : framebuffer_width;
  int copy_height = height < framebuffer_height ? height : framebuffer_height;
  glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 0, 0, copy_width, copy_height, 0);
  return tex;
}

static GLenum sampler_target_for_unit(GLuint program, int unit) {
  char uniform_name[16];
  snprintf(uniform_name, sizeof(uniform_name), "tex%d", unit);
  GLint loc = glGetUniformLocation(program, uniform_name);
  if (loc < 0) return GL_TEXTURE_2D;
  GLint count = 0;
  glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
  for (GLint i = 0; i < count; i++) {
    char name[128];
    GLsizei len = 0;
    GLint size = 0;
    GLenum type = 0;
    glGetActiveUniform(program, (GLuint)i, (GLsizei)sizeof(name), &len, &size, &type, name);
    if (!strcmp(name, uniform_name) || !strncmp(name, uniform_name, strlen(uniform_name))) {
      if (type == GL_SAMPLER_CUBE) return GL_TEXTURE_CUBE_MAP;
      if (type == GL_SAMPLER_3D) return GL_TEXTURE_3D;
      return GL_TEXTURE_2D;
    }
  }
  return GL_TEXTURE_2D;
}

static void bind_batch_textures(
  GLuint program,
  const PdCompatBatch *batch,
  const PdCompatTextureDef *defs,
  int def_count,
  PdCompatTextureEntry *textures,
  int *texture_count,
  int max_textures,
  GLuint *white_textures,
  GLuint *white_cubemaps,
  GLuint *white_textures3d
) {
  for (int unit = 0; unit < 8; unit++) {
    glActiveTexture((GLenum)(GL_TEXTURE0 + unit));
    glBindTexture(GL_TEXTURE_2D, white_textures[unit]);
    glBindTexture(GL_TEXTURE_CUBE_MAP, white_cubemaps[unit]);
    glBindTexture(GL_TEXTURE_3D, white_textures3d[unit]);
  }
  if (!batch || !batch->textures) return;
  for (int i = 0; i < batch->texture_count; i++) {
    int unit = batch->textures[i].unit;
    if (unit < 0 || unit >= 8) continue;
    GLenum target = sampler_target_for_unit(program, unit);
    PdCompatTextureEntry entry = texture_for_id(batch->textures[i].texture_id, target, defs, def_count, textures, texture_count, max_textures);
    if (!entry.texture) continue;
    glActiveTexture((GLenum)(GL_TEXTURE0 + unit));
    glBindTexture(entry.target, entry.texture);
  }
}

PdCompatRenderer *pd_compat_renderer_create(void) {
  PdCompatRenderer *renderer = (PdCompatRenderer *)calloc(1, sizeof(PdCompatRenderer));
  if (!renderer) return NULL;
  PdCompatRenderResult result;
  memset(&result, 0, sizeof(result));
  result.ok = 1;
  renderer->fallback_program = make_program(&result);
  if (!renderer->fallback_program) {
    free(renderer);
    return NULL;
  }
  glGenVertexArrays(1, &renderer->vao);
  glBindVertexArray(renderer->vao);
  glGenBuffers(1, &renderer->vbo);
  glBindBuffer(GL_ARRAY_BUFFER, renderer->vbo);
  for (int unit = 0; unit < 8; unit++) {
    renderer->white_textures[unit] = create_white_texture();
    renderer->white_cubemaps[unit] = create_white_cubemap();
    renderer->white_textures3d[unit] = create_white_texture3d();
  }
  return renderer;
}

void pd_compat_renderer_destroy(PdCompatRenderer *renderer) {
  if (!renderer) return;
  if (renderer->vbo) glDeleteBuffers(1, &renderer->vbo);
  if (renderer->vao) glDeleteVertexArrays(1, &renderer->vao);
  for (int unit = 0; unit < 8; unit++) {
    if (renderer->white_textures[unit]) glDeleteTextures(1, &renderer->white_textures[unit]);
    if (renderer->white_cubemaps[unit]) glDeleteTextures(1, &renderer->white_cubemaps[unit]);
    if (renderer->white_textures3d[unit]) glDeleteTextures(1, &renderer->white_textures3d[unit]);
  }
  for (int i = 0; i < renderer->texture_cache_count; i++) {
    if (renderer->texture_cache[i].texture) glDeleteTextures(1, &renderer->texture_cache[i].texture);
  }
  for (int i = 0; i < renderer->program_cache_count; i++) {
    if (renderer->program_cache[i].program) glDeleteProgram(renderer->program_cache[i].program);
  }
  if (renderer->fallback_program) glDeleteProgram(renderer->fallback_program);
  free(renderer);
}

PdCompatRenderResult pd_compat_renderer_render_script_batches_with_textures_to_current_framebuffer(
  PdCompatRenderer *renderer,
  const char *source,
  const PdSectionList *sections,
  const PdCompatBatch *batches,
  int batch_count,
  const PdCompatTextureDef *texture_defs,
  int texture_def_count,
  int width,
  int height
) {
  PdCompatRenderResult result;
  memset(&result, 0, sizeof(result));
  result.ok = 1;
  if (!renderer) {
    result.ok = 0;
    snprintf(result.log, sizeof(result.log), "missing compat renderer");
    return result;
  }

  glViewport(0, 0, width, height);
  glClearColor(0.f, 0.f, 0.f, 1.f);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glClearDepth(1.0);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  glBindVertexArray(renderer->vao);
  glBindBuffer(GL_ARRAY_BUFFER, renderer->vbo);

  for (int i = 0; i < batch_count; i++) {
    const PdCompatBatch *batch = &batches[i];
    if (!batch->vertices || batch->vertex_count <= 0) continue;
    GLuint program = renderer_program_for_batch(renderer, source, sections, batch, &result);
    int delete_program = 0;
    if (!program) {
      if (!result.ok) goto cleanup;
      program = renderer->fallback_program;
    }
    glUseProgram(program);
    bind_batch_textures(
      program, batch, texture_defs, texture_def_count,
      renderer->texture_cache, &renderer->texture_cache_count, 64,
      renderer->white_textures, renderer->white_cubemaps, renderer->white_textures3d
    );
    for (int unit = 0; unit < 8; unit++) {
      char uniform_name[16];
      snprintf(uniform_name, sizeof(uniform_name), "tex%d", unit);
      GLint loc = glGetUniformLocation(program, uniform_name);
      if (loc >= 0) glUniform1i(loc, unit);
    }
    apply_batch_uniforms(program, batch);
    bind_batch_attributes(program, renderer->vbo);

    const PdCompatVertex *draw_vertices = batch->vertices;
    int draw_count = batch->vertex_count;
    PdCompatVertex *expanded = NULL;
    GLenum draw_mode = map_mode(batch->mode);
    if (batch->mode == 7) {
      expanded = expand_quads(batch->vertices, batch->vertex_count, &draw_count);
      if (!expanded) {
        result.ok = 0;
        snprintf(result.log, sizeof(result.log), "out of memory expanding quads");
        if (delete_program) glDeleteProgram(program);
        goto cleanup;
      }
      draw_vertices = expanded;
    }

    int disable_depth_write = is_fullscreen_clip_quad(batch);
    if (batch->depth_test) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);
    if (batch->blend) {
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
      glDisable(GL_BLEND);
    }
    if (disable_depth_write) glDepthMask(GL_FALSE);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(PdCompatVertex) * (size_t)draw_count), draw_vertices, GL_STREAM_DRAW);
    glDrawArrays(draw_mode, 0, draw_count);
    if (disable_depth_write) glDepthMask(GL_TRUE);
    free(expanded);
    if (delete_program) glDeleteProgram(program);
    const PdCompatTextureDef *capture_def = find_capture_def_ending_at(texture_defs, texture_def_count, i + 1);
    if (capture_def) {
      GLuint capture_tex = create_capture_texture_from_framebuffer(capture_def, width, height);
      cache_texture_id(capture_def->texture_id, capture_tex, GL_TEXTURE_2D, renderer->texture_cache, &renderer->texture_cache_count, 64);
    }
  }

  snprintf(result.log, sizeof(result.log), "rendered %d batches to current framebuffer", batch_count);

cleanup:
  return result;
}

static PdCompatRenderResult render_batches_to_bound_framebuffer(
  const char *source,
  const PdSectionList *sections,
  const PdCompatBatch *batches,
  int batch_count,
  const PdCompatTextureDef *texture_defs,
  int texture_def_count,
  int width,
  int height
) {
  PdCompatRenderer *renderer = pd_compat_renderer_create();
  if (!renderer) {
    PdCompatRenderResult result;
    memset(&result, 0, sizeof(result));
    snprintf(result.log, sizeof(result.log), "failed to create compat renderer");
    return result;
  }
  PdCompatRenderResult result = pd_compat_renderer_render_script_batches_with_textures_to_current_framebuffer(
    renderer, source, sections, batches, batch_count, texture_defs, texture_def_count, width, height);
  pd_compat_renderer_destroy(renderer);
  return result;
}

PdCompatRenderResult pd_compat_render_script_batches_with_textures_to_current_framebuffer(
  const char *source,
  const PdSectionList *sections,
  const PdCompatBatch *batches,
  int batch_count,
  const PdCompatTextureDef *texture_defs,
  int texture_def_count,
  int width,
  int height
) {
  return render_batches_to_bound_framebuffer(source, sections, batches, batch_count, texture_defs, texture_def_count, width, height);
}

PdCompatRenderResult pd_compat_render_script_batches_with_textures_ppm(
  const char *source,
  const PdSectionList *sections,
  const PdCompatBatch *batches,
  int batch_count,
  const PdCompatTextureDef *texture_defs,
  int texture_def_count,
  const char *out_path,
  int width,
  int height
) {
  PdCompatRenderResult result;
  memset(&result, 0, sizeof(result));
  result.ok = 1;

  PdRenderOptions options = {width, height, 0, "offscreen", "PolyDraw Compat"};
  PdRenderContext *ctx = pd_render_create(&options);
  if (!ctx) {
    result.ok = 0;
    snprintf(result.log, sizeof(result.log), "failed to create offscreen GL context");
    return result;
  }

  GLuint tex = 0, depth = 0, fbo = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  glGenRenderbuffers(1, &depth);
  glBindRenderbuffer(GL_RENDERBUFFER, depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    result.ok = 0;
    snprintf(result.log, sizeof(result.log), "framebuffer incomplete");
    goto cleanup;
  }

  result = render_batches_to_bound_framebuffer(source, sections, batches, batch_count, texture_defs, texture_def_count, width, height);
  if (!result.ok) {
    goto cleanup;
  }

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
      snprintf(result.log, sizeof(result.log), "rendered %d batches to %s", batch_count, out_path);
    }
    free(pixels);
  }

cleanup:
  if (fbo) glDeleteFramebuffers(1, &fbo);
  if (depth) glDeleteRenderbuffers(1, &depth);
  if (tex) glDeleteTextures(1, &tex);
  pd_render_destroy(ctx);
  return result;
}

PdCompatRenderResult pd_compat_render_script_batches_ppm(
  const char *source,
  const PdSectionList *sections,
  const PdCompatBatch *batches,
  int batch_count,
  const char *out_path,
  int width,
  int height
) {
  return pd_compat_render_script_batches_with_textures_ppm(source, sections, batches, batch_count, NULL, 0, out_path, width, height);
}

PdCompatRenderResult pd_compat_render_batches_ppm(
  const PdCompatBatch *batches,
  int batch_count,
  const char *out_path,
  int width,
  int height
) {
  return pd_compat_render_script_batches_ppm(NULL, NULL, batches, batch_count, out_path, width, height);
}
