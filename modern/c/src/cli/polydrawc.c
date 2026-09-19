#include "polydraw/sections.h"
#include "polydraw/runtime.h"
#include "polydraw/render.h"
#include "polydraw/jit.h"
#include "polydraw/shader.h"
#include "polydraw/compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <math.h>

#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#endif

static char *read_file(const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return NULL;
  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  char *data = (char *)malloc((size_t)size + 1);
  if (!data) {
    fclose(fp);
    return NULL;
  }
  fread(data, 1, (size_t)size, fp);
  data[size] = 0;
  fclose(fp);
  return data;
}

static void print_sections(const PdSectionList *sections) {
  printf("[\n");
  for (size_t i = 0; i < sections->count; i++) {
    const PdSection *s = &sections->items[i];
    printf("  {\"type\":\"%s\",\"name\":\"%s\",\"line\":%d,\"start\":%zu,\"end\":%zu",
      pd_section_type_name(s->type), s->name, s->line, s->start, s->end);
    if (s->type == PD_SECTION_GEOMETRY) {
      printf(",\"geometry\":{\"input\":\"%s\",\"output\":\"%s\",\"maxVertices\":%d}",
        s->geometry.input, s->geometry.output, s->geometry.max_vertices);
    }
    printf("}%s\n", i + 1 == sections->count ? "" : ",");
  }
  printf("]\n");
}

static const PdSection *first_section(const PdSectionList *sections, PdSectionType type) {
  if (!sections) return NULL;
  for (size_t i = 0; i < sections->count; i++) {
    if (sections->items[i].type == type) return &sections->items[i];
  }
  return NULL;
}

static int run_js_backend(const char *script_path) {
  char command[4096];
  int n = snprintf(command, sizeof(command),
    "node \"%s\" run \"%s\"",
    POLYDRAW_JS_CLI, script_path);
  if (n <= 0 || (size_t)n >= sizeof(command)) {
    fprintf(stderr, "command too long\n");
    return 1;
  }
  return system(command);
}

static char *make_temp_path(const char *suffix) {
  char *path = (char *)malloc(1024);
  if (!path) return NULL;
  snprintf(path, 1024, "/tmp/polydraw_%ld_%s", (long)getpid(), suffix);
  return path;
}

static int write_trace_batches(const char *script_path, const char *trace_path, const char *texture_dir) {
  char command[4096];
  const char *batch_limit = getenv("POLYDRAW_BATCH_LIMIT");
  const char *frames = getenv("POLYDRAW_FRAMES");
  char batch_arg[128] = "";
  char frames_arg[128] = "";
  if (batch_limit && batch_limit[0]) snprintf(batch_arg, sizeof(batch_arg), " --batch-limit=%s", batch_limit);
  if (frames && frames[0]) snprintf(frames_arg, sizeof(frames_arg), " --frames=%s", frames);
  int n = snprintf(command, sizeof(command),
    "node \"%s\" trace-batches \"%s\" --step-limit=50000000%s%s --texture-dir=\"%s\" > \"%s\"",
    POLYDRAW_JS_CLI, script_path, batch_arg, frames_arg, texture_dir, trace_path);
  if (n <= 0 || (size_t)n >= sizeof(command)) return 0;
  return system(command) == 0;
}

static void remove_texture_dir(const char *dir_path) {
  if (!dir_path) return;
  DIR *dir = opendir(dir_path);
  if (dir) {
    struct dirent *entry;
    while ((entry = readdir(dir))) {
      if (strncmp(entry->d_name, "tex_", 4) || !strstr(entry->d_name, ".rgba")) continue;
      char path[1536];
      int n = snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);
      if (n > 0 && (size_t)n < sizeof(path)) remove(path);
    }
    closedir(dir);
  }
  rmdir(dir_path);
}

typedef struct PdTraceLineReader {
  FILE *fp;
  const char *data;
  size_t length;
  size_t offset;
} PdTraceLineReader;

static int trace_read_line(PdTraceLineReader *reader, char *line, size_t size) {
  if (!reader || !line || !size) return 0;
  if (reader->fp) return fgets(line, (int)size, reader->fp) != NULL;
  if (!reader->data || reader->offset >= reader->length) return 0;
  size_t out = 0;
  while (reader->offset < reader->length) {
    char ch = reader->data[reader->offset++];
    if (out + 1 < size) line[out++] = ch;
    if (ch == '\n') break;
  }
  line[out] = 0;
  return 1;
}

static int parse_trace_batches_reader(
  PdTraceLineReader *reader,
  PdCompatBatch **out_batches,
  int *out_count,
  PdCompatTextureDef **out_textures,
  int *out_texture_count
) {
  PdCompatBatch *batches = NULL;
  int count = 0;
  int cap = 0;
  PdCompatTextureDef *texture_defs = NULL;
  int texture_def_count = 0;
  int texture_def_cap = 0;
  char line[4096];
  while (trace_read_line(reader, line, sizeof(line))) {
    char word[64];
    if (sscanf(line, "%63s", word) != 1) continue;
    if (!strcmp(word, "texdef") || !strcmp(word, "texfile") || !strcmp(word, "texcapture")) {
      if (texture_def_count >= texture_def_cap) {
        texture_def_cap = texture_def_cap ? texture_def_cap * 2 : 8;
        PdCompatTextureDef *next = (PdCompatTextureDef *)realloc(texture_defs, (size_t)texture_def_cap * sizeof(PdCompatTextureDef));
        if (!next) {
        return 0;
        }
        texture_defs = next;
      }
      PdCompatTextureDef *def = &texture_defs[texture_def_count];
      memset(def, 0, sizeof(*def));
      def->depth = 1;
      if (!strcmp(word, "texcapture")) {
        int fields = sscanf(line, "%63s %d %d %d %d %d %d %d", word, &def->texture_id, &def->width, &def->height, &def->depth, &def->format, &def->capture_start, &def->capture_end);
        if (fields != 8 &&
            sscanf(line, "%63s %d %d %d %d %d %d", word, &def->texture_id, &def->width, &def->height, &def->format, &def->capture_start, &def->capture_end) != 7) {
        return 0;
        }
        texture_def_count++;
        continue;
      }
      if (!strcmp(word, "texfile")) {
        int fields = sscanf(line, "%63s %d %d %d %d %d %511s", word, &def->texture_id, &def->width, &def->height, &def->depth, &def->format, def->path);
        if (fields != 7 &&
            sscanf(line, "%63s %d %d %d %d %511s", word, &def->texture_id, &def->width, &def->height, &def->format, def->path) != 6) {
        return 0;
        }
        texture_def_count++;
        continue;
      }
      int value_count = 0;
      int fields = sscanf(line, "%63s %d %d %d %d %d %d", word, &def->texture_id, &def->width, &def->height, &def->depth, &def->format, &value_count);
      int header_fields = 7;
      if (fields != 7) {
        if (sscanf(line, "%63s %d %d %d %d %d", word, &def->texture_id, &def->width, &def->height, &def->format, &value_count) != 6) {
        return 0;
        }
        def->depth = 1;
        header_fields = 6;
      }
      if (def->depth <= 0) def->depth = 1;
      if (def->format < 0) {
        return 0;
      }
      if (value_count < 0) value_count = 0;
      float *values = (float *)calloc((size_t)value_count, sizeof(float));
      if (value_count > 0 && !values) {
        return 0;
      }
      const char *cursor = line;
      for (int skip = 0; skip < header_fields; skip++) {
        while (*cursor == ' ' || *cursor == '\t') cursor++;
        while (*cursor && *cursor != ' ' && *cursor != '\t' && *cursor != '\n') cursor++;
      }
      for (int i = 0; i < value_count; i++) {
        while (*cursor == ' ' || *cursor == '\t') cursor++;
        char *end = NULL;
        values[i] = strtof(cursor, &end);
        if (end == cursor) {
          free(values);
        return 0;
        }
        cursor = end;
      }
      def->values = values;
      def->value_count = value_count;
      texture_def_count++;
      continue;
    }
    int mode = 0, vertex_count = 0;
    char vertex_shader[64] = "-", geometry_shader[64] = "-", fragment_shader[64] = "-";
    int uniform_count = 0;
    int texture_count = 0;
    int blend = 0;
    int depth_test = 1;
    int fields = sscanf(line, "%63s %d %d %63s %63s %63s %d %d",
      word, &mode, &vertex_count, vertex_shader, geometry_shader, fragment_shader, &uniform_count, &texture_count);
    if (fields == 8) {
      sscanf(line, "%63s %d %d %63s %63s %63s %d %d %d %d",
        word, &mode, &vertex_count, vertex_shader, geometry_shader, fragment_shader, &uniform_count, &texture_count, &blend, &depth_test);
    }
    if (strcmp(word, "batch") || vertex_count < 0) {
        return 0;
    }
    if (fields < 8) {
      snprintf(geometry_shader, sizeof(geometry_shader), "-");
      uniform_count = 0;
      texture_count = 0;
      fields = sscanf(line, "%63s %d %d %63s %63s %d %d",
        word, &mode, &vertex_count, vertex_shader, fragment_shader, &uniform_count, &texture_count);
      if (fields < 5 || strcmp(word, "batch") || vertex_count < 0) {
        return 0;
      }
      if (fields < 6) uniform_count = 0;
      if (fields < 7) texture_count = 0;
    }
    if (count >= cap) {
      cap = cap ? cap * 2 : 8;
      PdCompatBatch *next = (PdCompatBatch *)realloc(batches, (size_t)cap * sizeof(PdCompatBatch));
      if (!next) {
        return 0;
      }
      batches = next;
    }
    PdCompatUniform *uniforms = (PdCompatUniform *)calloc((size_t)uniform_count, sizeof(PdCompatUniform));
    if (uniform_count > 0 && !uniforms) {
        return 0;
    }
    for (int i = 0; i < uniform_count; i++) {
      char tag[64];
      PdCompatUniform *u = &uniforms[i];
      int value_count = 0;
      if (!trace_read_line(reader, line, sizeof(line)) ||
          sscanf(line, "%63s %63s %d %d %d", tag, u->name, &u->type, &u->count, &value_count) != 5 ||
          strcmp(tag, "uniform") != 0) {
        free(uniforms);
        return 0;
      }
      if (value_count < 0) value_count = 0;
      u->values = (float *)calloc((size_t)value_count, sizeof(float));
      u->value_count = value_count;
      if (value_count > 0 && !u->values) {
        free(uniforms);
        return 0;
      }
      const char *cursor = line;
      for (int skip = 0; skip < 5; skip++) {
        while (*cursor == ' ' || *cursor == '\t') cursor++;
        while (*cursor && *cursor != ' ' && *cursor != '\t' && *cursor != '\n') cursor++;
      }
      for (int j = 0; j < value_count; j++) {
        while (*cursor == ' ' || *cursor == '\t') cursor++;
        char *end = NULL;
        u->values[j] = strtof(cursor, &end);
        if (end == cursor) {
          free(uniforms);
        return 0;
        }
        cursor = end;
      }
    }
    PdCompatTextureBinding *textures = (PdCompatTextureBinding *)calloc((size_t)texture_count, sizeof(PdCompatTextureBinding));
    if (texture_count > 0 && !textures) {
      free(uniforms);
        return 0;
    }
    for (int i = 0; i < texture_count; i++) {
      char tag[64];
      PdCompatTextureBinding *t = &textures[i];
      if (!trace_read_line(reader, line, sizeof(line)) ||
          sscanf(line, "%63s %d %d", tag, &t->unit, &t->texture_id) != 3 ||
          strcmp(tag, "texture") != 0) {
        free(textures);
        free(uniforms);
        return 0;
      }
    }
    PdCompatVertex *vertices = (PdCompatVertex *)calloc((size_t)vertex_count, sizeof(PdCompatVertex));
    if (!vertices) {
      free(textures);
      free(uniforms);
        return 0;
    }
    for (int i = 0; i < vertex_count; i++) {
      PdCompatVertex *v = &vertices[i];
      if (!trace_read_line(reader, line, sizeof(line))) {
        free(textures);
        free(uniforms);
        free(vertices);
        return 0;
      }
      v->normal[0] = 0.f;
      v->normal[1] = 0.f;
      v->normal[2] = 1.f;
      int vertex_fields = sscanf(line, "%f %f %f %f %f %f %f %f %f %f %f %f %f %f %f",
        &v->position[0], &v->position[1], &v->position[2], &v->position[3],
        &v->color[0], &v->color[1], &v->color[2], &v->color[3],
        &v->texcoord[0], &v->texcoord[1], &v->texcoord[2], &v->texcoord[3],
        &v->normal[0], &v->normal[1], &v->normal[2]);
      if (vertex_fields != 12 && vertex_fields != 15) {
        free(textures);
        free(uniforms);
        free(vertices);
        return 0;
      }
    }
    batches[count].mode = mode;
    snprintf(batches[count].vertex_shader, sizeof(batches[count].vertex_shader), "%s", vertex_shader);
    snprintf(batches[count].geometry_shader, sizeof(batches[count].geometry_shader), "%s", geometry_shader);
    snprintf(batches[count].fragment_shader, sizeof(batches[count].fragment_shader), "%s", fragment_shader);
    batches[count].uniforms = uniforms;
    batches[count].uniform_count = uniform_count;
    batches[count].textures = textures;
    batches[count].texture_count = texture_count;
    batches[count].vertices = vertices;
    batches[count].vertex_count = vertex_count;
    batches[count].blend = blend;
    batches[count].depth_test = depth_test;
    count++;
  }
  *out_batches = batches;
  *out_count = count;
  *out_textures = texture_defs;
  *out_texture_count = texture_def_count;
  return 1;
}

static int parse_trace_batches_file(
  FILE *fp,
  PdCompatBatch **out_batches,
  int *out_count,
  PdCompatTextureDef **out_textures,
  int *out_texture_count
) {
  PdTraceLineReader reader = {fp, NULL, 0, 0};
  return parse_trace_batches_reader(&reader, out_batches, out_count, out_textures, out_texture_count);
}

static int parse_trace_batches_memory(
  const char *data,
  size_t length,
  PdCompatBatch **out_batches,
  int *out_count,
  PdCompatTextureDef **out_textures,
  int *out_texture_count
) {
  PdTraceLineReader reader = {NULL, data, length, 0};
  return parse_trace_batches_reader(&reader, out_batches, out_count, out_textures, out_texture_count);
}

static int parse_trace_batches(
  const char *trace_path,
  PdCompatBatch **out_batches,
  int *out_count,
  PdCompatTextureDef **out_textures,
  int *out_texture_count
) {
  FILE *fp = fopen(trace_path, "rb");
  if (!fp) return 0;
  int ok = parse_trace_batches_file(fp, out_batches, out_count, out_textures, out_texture_count);
  fclose(fp);
  return ok;
}

typedef struct PdStringBuffer {
  char *data;
  size_t length;
  size_t capacity;
} PdStringBuffer;

static int path_ends_with(const char *path, const char *suffix) {
  if (!path || !suffix) return 0;
  size_t path_len = strlen(path);
  size_t suffix_len = strlen(suffix);
  return path_len >= suffix_len && !strcmp(path + path_len - suffix_len, suffix);
}

static int attach_default_matrix_uniforms(PdCompatBatch *batch, int clip_space) {
  if (!batch) return 0;
  float mvp[16] = {
    clip_space ? 1.0f : 1.81066017f, 0, 0, 0,
    0, clip_space ? 1.0f : 2.41421356f, 0, 0,
    0, 0, clip_space ? 1.0f : -1.00020002f, clip_space ? 0.0f : -1.0f,
    0, 0, clip_space ? 0.0f : -0.200020002f, clip_space ? 1.0f : 0.0f,
  };
  float modelview[16] = {
    1,0,0,0,
    0,1,0,0,
    0,0,1,0,
    0,0,0,1,
  };
  float normal[9] = {
    1,0,0,
    0,1,0,
    0,0,1,
  };
  PdCompatUniform *uniforms = (PdCompatUniform *)calloc(3, sizeof(PdCompatUniform));
  if (!uniforms) return 0;
  const char *names[3] = {"_pd_mvp", "_pd_modelview", "_pd_normalMatrix"};
  const int types[3] = {44, 44, 43};
  const int counts[3] = {16, 16, 9};
  const float *values[3] = {mvp, modelview, normal};
  for (int i = 0; i < 3; i++) {
    snprintf(uniforms[i].name, sizeof(uniforms[i].name), "%s", names[i]);
    uniforms[i].type = types[i];
    uniforms[i].count = 1;
    uniforms[i].value_count = counts[i];
    uniforms[i].values = (float *)calloc((size_t)counts[i], sizeof(float));
    if (!uniforms[i].values) {
      for (int j = 0; j < i; j++) free(uniforms[j].values);
      free(uniforms);
      return 0;
    }
    memcpy(uniforms[i].values, values[i], (size_t)counts[i] * sizeof(float));
  }
  batch->uniforms = uniforms;
  batch->uniform_count = 3;
  return 1;
}

static void string_buffer_clear(PdStringBuffer *buffer) {
  if (!buffer) return;
  buffer->length = 0;
  if (buffer->data) buffer->data[0] = 0;
}

static void string_buffer_free(PdStringBuffer *buffer) {
  if (!buffer) return;
  free(buffer->data);
  buffer->data = NULL;
  buffer->length = 0;
  buffer->capacity = 0;
}

static int string_buffer_append(PdStringBuffer *buffer, const char *text) {
  if (!buffer || !text) return 0;
  size_t add = strlen(text);
  if (buffer->length + add + 1 > buffer->capacity) {
    size_t next_capacity = buffer->capacity ? buffer->capacity * 2 : 8192;
    while (next_capacity < buffer->length + add + 1) next_capacity *= 2;
    char *next = (char *)realloc(buffer->data, next_capacity);
    if (!next) return 0;
    buffer->data = next;
    buffer->capacity = next_capacity;
  }
  memcpy(buffer->data + buffer->length, text, add + 1);
  buffer->length += add;
  return 1;
}

typedef struct PdNativeTraceRuntime {
  PdCompatBatch batches[256];
  int batch_count;
  PdCompatBatch current_batch;
  PdCompatVertex current_vertices[8192];
  int current_vertex_count;
  PdCompatBatch aggregate_batch;
  PdCompatVertex *aggregate_vertices;
  int aggregate_vertex_count;
  int aggregate_vertex_cap;
  int aggregate_active;
  float current_color[4];
  float current_texcoord[4];
  float current_normal[3];
  char current_vertex_shader[64];
  char current_geometry_shader[64];
  char current_fragment_shader[64];
  PdCompatUniform uniforms[128];
  int uniform_count;
  int next_uniform_location;
  char uniform_names[128][64];
} PdNativeTraceRuntime;

static int batches_same_draw_state(const PdCompatBatch *a, const PdCompatBatch *b);
static int clone_batch_metadata(PdCompatBatch *dst, const PdCompatBatch *src);
static void append_batch_triangles(PdCompatVertex *dst, int *offset, const PdCompatBatch *batch);
static int triangle_count_for_batch(const PdCompatBatch *batch);

static void native_trace_init(PdNativeTraceRuntime *trace) {
  memset(trace, 0, sizeof(*trace));
  trace->current_color[0] = 1.f;
  trace->current_color[1] = 1.f;
  trace->current_color[2] = 1.f;
  trace->current_color[3] = 1.f;
  trace->current_texcoord[3] = 1.f;
  trace->current_normal[2] = 1.f;
  snprintf(trace->current_vertex_shader, sizeof(trace->current_vertex_shader), "-");
  snprintf(trace->current_geometry_shader, sizeof(trace->current_geometry_shader), "-");
  snprintf(trace->current_fragment_shader, sizeof(trace->current_fragment_shader), "0");
  trace->next_uniform_location = 1;
}

static int native_uniform_type_code(const char *name) {
  if (!strcmp(name, "mat4")) return 44;
  if (!strcmp(name, "mat3")) return 43;
  if (!strcmp(name, "1f") || !strcmp(name, "1i")) return 1;
  if (!strcmp(name, "2f")) return 2;
  if (!strcmp(name, "3f")) return 3;
  if (!strcmp(name, "4f")) return 4;
  return 1;
}

static void native_add_uniform_values(PdNativeTraceRuntime *trace, const char *name, int type, int count, const float *values, int value_count) {
  if (!trace || trace->uniform_count >= (int)(sizeof(trace->uniforms) / sizeof(trace->uniforms[0]))) return;
  PdCompatUniform *u = &trace->uniforms[trace->uniform_count++];
  memset(u, 0, sizeof(*u));
  snprintf(u->name, sizeof(u->name), "%s", name);
  u->type = type;
  u->count = count;
  u->value_count = value_count;
  if (value_count > 0) {
    u->values = (float *)calloc((size_t)value_count, sizeof(float));
    if (!u->values) {
      trace->uniform_count--;
      return;
    }
    memcpy(u->values, values, (size_t)value_count * sizeof(float));
  }
}

static void native_add_default_matrix_uniforms(PdNativeTraceRuntime *trace, int clip_space) {
  float mvp[16] = {
    clip_space ? 1.0f : 1.81066017f, 0, 0, 0,
    0, clip_space ? 1.0f : 2.41421356f, 0, 0,
    0, 0, clip_space ? 1.0f : -1.00020002f, clip_space ? 0.0f : -1.0f,
    0, 0, clip_space ? 0.0f : -0.200020002f, clip_space ? 1.0f : 0.0f,
  };
  float identity4[16] = {
    1,0,0,0,
    0,1,0,0,
    0,0,1,0,
    0,0,0,1,
  };
  float identity3[9] = {
    1,0,0,
    0,1,0,
    0,0,1,
  };
  native_add_uniform_values(trace, "_pd_mvp", 44, 1, mvp, 16);
  native_add_uniform_values(trace, "_pd_modelview", 44, 1, identity4, 16);
  native_add_uniform_values(trace, "_pd_normalMatrix", 43, 1, identity3, 9);
}

static void native_flush_aggregate(PdNativeTraceRuntime *trace) {
  if (!trace || !trace->aggregate_active || trace->aggregate_vertex_count <= 0) return;
  if (trace->batch_count >= (int)(sizeof(trace->batches) / sizeof(trace->batches[0]))) return;
  PdCompatBatch *batch = &trace->batches[trace->batch_count++];
  *batch = trace->aggregate_batch;
  PdCompatVertex *vertices = (PdCompatVertex *)calloc((size_t)trace->aggregate_vertex_count, sizeof(PdCompatVertex));
  if (vertices) memcpy(vertices, trace->aggregate_vertices, (size_t)trace->aggregate_vertex_count * sizeof(PdCompatVertex));
  batch->vertices = vertices;
  batch->vertex_count = trace->aggregate_vertex_count;
  trace->aggregate_vertices = NULL;
  trace->aggregate_vertex_count = 0;
  trace->aggregate_vertex_cap = 0;
  trace->aggregate_active = 0;
  memset(&trace->aggregate_batch, 0, sizeof(trace->aggregate_batch));
}

static int native_append_aggregate(PdNativeTraceRuntime *trace, const PdCompatBatch *batch) {
  if (!trace || !batch || !(batch->mode == 4 || batch->mode == 6 || batch->mode == 9 || batch->mode == 7)) return 0;
  int add = triangle_count_for_batch(batch);
  if (add <= 0) return 0;
  if (!trace->aggregate_active) {
    if (!clone_batch_metadata(&trace->aggregate_batch, batch)) return 0;
    trace->aggregate_batch.mode = 4;
    trace->aggregate_active = 1;
  } else if (!batches_same_draw_state(&trace->aggregate_batch, batch)) {
    native_flush_aggregate(trace);
    if (!clone_batch_metadata(&trace->aggregate_batch, batch)) return 0;
    trace->aggregate_batch.mode = 4;
    trace->aggregate_active = 1;
  }
  if (trace->aggregate_vertex_count + add > trace->aggregate_vertex_cap) {
    int next_cap = trace->aggregate_vertex_cap ? trace->aggregate_vertex_cap * 2 : 65536;
    while (next_cap < trace->aggregate_vertex_count + add) next_cap *= 2;
    PdCompatVertex *next = (PdCompatVertex *)realloc(trace->aggregate_vertices, (size_t)next_cap * sizeof(PdCompatVertex));
    if (!next) return 0;
    trace->aggregate_vertices = next;
    trace->aggregate_vertex_cap = next_cap;
  }
  int offset = trace->aggregate_vertex_count;
  append_batch_triangles(trace->aggregate_vertices, &offset, batch);
  trace->aggregate_vertex_count = offset;
  return 1;
}

static int native_append_current_fan_fast(PdNativeTraceRuntime *trace) {
  if (!trace || !(trace->current_batch.mode == 4 || trace->current_batch.mode == 6 || trace->current_batch.mode == 9) || trace->current_vertex_count < 3) return 0;
  PdCompatBatch state;
  memset(&state, 0, sizeof(state));
  state = trace->current_batch;
  state.mode = 4;
  state.blend = 0;
  state.depth_test = 1;
  static PdCompatTextureBinding default_texture = {0, 0};
  state.textures = &default_texture;
  state.texture_count = 1;
  if (!trace->aggregate_active) {
    if (!clone_batch_metadata(&trace->aggregate_batch, &state)) return 0;
    trace->aggregate_active = 1;
  } else if (!batches_same_draw_state(&trace->aggregate_batch, &state)) {
    native_flush_aggregate(trace);
    if (!clone_batch_metadata(&trace->aggregate_batch, &state)) return 0;
    trace->aggregate_active = 1;
  }
  int add = trace->current_batch.mode == 4 ? trace->current_vertex_count : (trace->current_vertex_count - 2) * 3;
  if (trace->aggregate_vertex_count + add > trace->aggregate_vertex_cap) {
    int next_cap = trace->aggregate_vertex_cap ? trace->aggregate_vertex_cap * 2 : 65536;
    while (next_cap < trace->aggregate_vertex_count + add) next_cap *= 2;
    PdCompatVertex *next = (PdCompatVertex *)realloc(trace->aggregate_vertices, (size_t)next_cap * sizeof(PdCompatVertex));
    if (!next) return 0;
    trace->aggregate_vertices = next;
    trace->aggregate_vertex_cap = next_cap;
  }
  if (trace->current_batch.mode == 4) {
    memcpy(trace->aggregate_vertices + trace->aggregate_vertex_count, trace->current_vertices, (size_t)trace->current_vertex_count * sizeof(PdCompatVertex));
    trace->aggregate_vertex_count += trace->current_vertex_count;
  } else {
    for (int i = 1; i + 1 < trace->current_vertex_count; i++) {
      trace->aggregate_vertices[trace->aggregate_vertex_count++] = trace->current_vertices[0];
      trace->aggregate_vertices[trace->aggregate_vertex_count++] = trace->current_vertices[i];
      trace->aggregate_vertices[trace->aggregate_vertex_count++] = trace->current_vertices[i + 1];
    }
  }
  return 1;
}

static void native_set_uniform(PdNativeTraceRuntime *trace, int loc, const char *kind, const double *args, int argc) {
  if (!trace || trace->uniform_count >= (int)(sizeof(trace->uniforms) / sizeof(trace->uniforms[0]))) return;
  if (loc <= 0 || loc >= (int)(sizeof(trace->uniform_names) / sizeof(trace->uniform_names[0]))) return;
  const char *name = trace->uniform_names[loc][0] ? trace->uniform_names[loc] : NULL;
  if (!name) return;
  PdCompatUniform *u = &trace->uniforms[trace->uniform_count++];
  memset(u, 0, sizeof(*u));
  snprintf(u->name, sizeof(u->name), "%s", name);
  u->type = native_uniform_type_code(kind);
  u->count = 1;
  u->value_count = u->type;
  u->values = (float *)calloc((size_t)u->value_count, sizeof(float));
  if (!u->values) {
    trace->uniform_count--;
    return;
  }
  for (int i = 0; i < u->value_count; i++) u->values[i] = (float)(i + 1 < argc ? args[i + 1] : 0.0);
}

static double native_trace_external(PdTraceRuntime *rt, void *user, const char *name, const double *args, int argc) {
  PdNativeTraceRuntime *trace = (PdNativeTraceRuntime *)user;
  if (!trace || !name) return 0.0;
  if (!strcmp(name, "GLCLEAR")) return 0.0;
  if (!strcmp(name, "GLCOLOR")) {
    trace->current_color[0] = (float)(argc > 0 ? args[0] : 0.0);
    trace->current_color[1] = (float)(argc > 1 ? args[1] : 0.0);
    trace->current_color[2] = (float)(argc > 2 ? args[2] : 0.0);
    trace->current_color[3] = (float)(argc > 3 ? args[3] : 1.0);
    return 0.0;
  }
  if (!strcmp(name, "GLTEXCOORD")) {
    trace->current_texcoord[0] = (float)(argc > 0 ? args[0] : 0.0);
    trace->current_texcoord[1] = (float)(argc > 1 ? args[1] : 0.0);
    trace->current_texcoord[2] = (float)(argc > 2 ? args[2] : 0.0);
    trace->current_texcoord[3] = (float)(argc > 3 ? args[3] : 1.0);
    return 0.0;
  }
  if (!strcmp(name, "GLNORMAL")) {
    trace->current_normal[0] = (float)(argc > 0 ? args[0] : 0.0);
    trace->current_normal[1] = (float)(argc > 1 ? args[1] : 0.0);
    trace->current_normal[2] = (float)(argc > 2 ? args[2] : 1.0);
    return 0.0;
  }
  if (!strcmp(name, "GLSETSHADER")) {
    const char *a = argc > 0 ? pd_trace_runtime_string_arg(rt, args[0]) : NULL;
    const char *b = argc > 1 ? pd_trace_runtime_string_arg(rt, args[1]) : NULL;
    const char *c = argc > 2 ? pd_trace_runtime_string_arg(rt, args[2]) : NULL;
    if (argc >= 3) {
      snprintf(trace->current_vertex_shader, sizeof(trace->current_vertex_shader), "%s", a ? a : "-");
      snprintf(trace->current_geometry_shader, sizeof(trace->current_geometry_shader), "%s", b ? b : "-");
      snprintf(trace->current_fragment_shader, sizeof(trace->current_fragment_shader), "%s", c ? c : "-");
    } else if (argc >= 2) {
      snprintf(trace->current_vertex_shader, sizeof(trace->current_vertex_shader), "%s", a ? a : "-");
      snprintf(trace->current_geometry_shader, sizeof(trace->current_geometry_shader), "-");
      snprintf(trace->current_fragment_shader, sizeof(trace->current_fragment_shader), "%s", b ? b : "0");
    } else if (argc >= 1) {
      snprintf(trace->current_vertex_shader, sizeof(trace->current_vertex_shader), "-");
      snprintf(trace->current_geometry_shader, sizeof(trace->current_geometry_shader), "-");
      snprintf(trace->current_fragment_shader, sizeof(trace->current_fragment_shader), "%d", (int)args[0]);
    }
    return 0.0;
  }
  if (!strcmp(name, "GLGETUNIFORMLOC")) {
    const char *uniform_name = argc > 0 ? pd_trace_runtime_string_arg(rt, args[0]) : NULL;
    int loc = trace->next_uniform_location++;
    if (loc < (int)(sizeof(trace->uniform_names) / sizeof(trace->uniform_names[0]))) {
      snprintf(trace->uniform_names[loc], sizeof(trace->uniform_names[loc]), "%s", uniform_name ? uniform_name : "");
      for (int i = 1; loc + i < (int)(sizeof(trace->uniform_names) / sizeof(trace->uniform_names[0])) && i < 16; i++) {
        snprintf(trace->uniform_names[loc + i], sizeof(trace->uniform_names[loc + i]), "%s[%d]", uniform_name ? uniform_name : "", i);
      }
    }
    return (double)loc;
  }
  if (!strcmp(name, "GLUNIFORM1F")) { native_set_uniform(trace, (int)args[0], "1f", args, argc); return 0.0; }
  if (!strcmp(name, "GLUNIFORM2F")) { native_set_uniform(trace, (int)args[0], "2f", args, argc); return 0.0; }
  if (!strcmp(name, "GLUNIFORM3F")) { native_set_uniform(trace, (int)args[0], "3f", args, argc); return 0.0; }
  if (!strcmp(name, "GLUNIFORM4F")) { native_set_uniform(trace, (int)args[0], "4f", args, argc); return 0.0; }
  if (!strcmp(name, "GLUNIFORM1I")) { native_set_uniform(trace, (int)args[0], "1i", args, argc); return 0.0; }
  if (!strcmp(name, "GLBEGIN")) {
    memset(&trace->current_batch, 0, sizeof(trace->current_batch));
    trace->current_batch.mode = argc > 0 ? (int)args[0] : 0;
    snprintf(trace->current_batch.vertex_shader, sizeof(trace->current_batch.vertex_shader), "%s", trace->current_vertex_shader);
    snprintf(trace->current_batch.geometry_shader, sizeof(trace->current_batch.geometry_shader), "%s", trace->current_geometry_shader);
    snprintf(trace->current_batch.fragment_shader, sizeof(trace->current_batch.fragment_shader), "%s", trace->current_fragment_shader);
    trace->current_vertex_count = 0;
    return 0.0;
  }
  if (!strcmp(name, "GLVERTEX")) {
    if (trace->current_vertex_count >= (int)(sizeof(trace->current_vertices) / sizeof(trace->current_vertices[0]))) return 0.0;
    PdCompatVertex *v = &trace->current_vertices[trace->current_vertex_count++];
    memset(v, 0, sizeof(*v));
    v->position[0] = (float)(argc > 0 ? args[0] : 0.0);
    v->position[1] = (float)(argc > 1 ? args[1] : 0.0);
    v->position[2] = (float)(argc > 2 ? args[2] : 0.0);
    v->position[3] = (float)(argc > 3 ? args[3] : 1.0);
    memcpy(v->color, trace->current_color, sizeof(v->color));
    memcpy(v->texcoord, trace->current_texcoord, sizeof(v->texcoord));
    memcpy(v->normal, trace->current_normal, sizeof(v->normal));
    return 0.0;
  }
  if (!strcmp(name, "GLEND")) {
    if (native_append_current_fan_fast(trace)) return 0.0;
    int saved_uniform_count = trace->uniform_count;
    native_add_default_matrix_uniforms(trace, 0);
    PdCompatBatch temp_batch;
    memset(&temp_batch, 0, sizeof(temp_batch));
    temp_batch = trace->current_batch;
    PdCompatBatch *batch = &temp_batch;
    batch->vertex_count = trace->current_vertex_count;
    PdCompatVertex *vertices = (PdCompatVertex *)calloc((size_t)batch->vertex_count, sizeof(PdCompatVertex));
    if (vertices && batch->vertex_count > 0) memcpy(vertices, trace->current_vertices, (size_t)batch->vertex_count * sizeof(PdCompatVertex));
    batch->vertices = vertices;
    batch->uniform_count = trace->uniform_count;
    if (trace->uniform_count > 0) {
      PdCompatUniform *uniforms = (PdCompatUniform *)calloc((size_t)trace->uniform_count, sizeof(PdCompatUniform));
      if (uniforms) {
        for (int i = 0; i < trace->uniform_count; i++) {
          uniforms[i] = trace->uniforms[i];
          uniforms[i].values = NULL;
          if (trace->uniforms[i].value_count > 0 && trace->uniforms[i].values) {
            uniforms[i].values = (float *)calloc((size_t)trace->uniforms[i].value_count, sizeof(float));
            if (uniforms[i].values) {
              memcpy(uniforms[i].values, trace->uniforms[i].values, (size_t)trace->uniforms[i].value_count * sizeof(float));
            }
          }
        }
      }
      batch->uniforms = uniforms;
    }
    static PdCompatTextureBinding default_texture = {0, 0};
    batch->textures = &default_texture;
    batch->texture_count = 1;
    batch->blend = 0;
    batch->depth_test = 1;
    if (native_append_aggregate(trace, batch)) {
      for (int j = 0; j < batch->uniform_count; j++) free((void *)batch->uniforms[j].values);
      free((void *)batch->uniforms);
      free((void *)batch->vertices);
      for (int i = saved_uniform_count; i < trace->uniform_count; i++) free((void *)trace->uniforms[i].values);
      trace->uniform_count = saved_uniform_count;
      return 0.0;
    }
    if (trace->batch_count < (int)(sizeof(trace->batches) / sizeof(trace->batches[0]))) {
      trace->batches[trace->batch_count++] = temp_batch;
    } else {
      for (int j = 0; j < batch->uniform_count; j++) free((void *)batch->uniforms[j].values);
      free((void *)batch->uniforms);
      free((void *)batch->vertices);
    }
    for (int i = saved_uniform_count; i < trace->uniform_count; i++) free((void *)trace->uniforms[i].values);
    trace->uniform_count = saved_uniform_count;
    return 0.0;
  }
  if (!strcmp(name, "GLQUAD")) {
    if (trace->batch_count >= (int)(sizeof(trace->batches) / sizeof(trace->batches[0]))) return 0.0;
    PdCompatBatch *batch = &trace->batches[trace->batch_count++];
    memset(batch, 0, sizeof(*batch));
    batch->mode = 5;
    snprintf(batch->vertex_shader, sizeof(batch->vertex_shader), "%s", trace->current_vertex_shader);
    snprintf(batch->geometry_shader, sizeof(batch->geometry_shader), "%s", trace->current_geometry_shader);
    snprintf(batch->fragment_shader, sizeof(batch->fragment_shader), "%s", trace->current_fragment_shader);
    int saved_uniform_count = trace->uniform_count;
    native_add_default_matrix_uniforms(trace, 1);
    batch->uniform_count = trace->uniform_count;
    if (trace->uniform_count > 0) {
      PdCompatUniform *uniforms = (PdCompatUniform *)calloc((size_t)trace->uniform_count, sizeof(PdCompatUniform));
      if (uniforms) {
        for (int i = 0; i < trace->uniform_count; i++) {
          uniforms[i] = trace->uniforms[i];
          uniforms[i].values = NULL;
          if (trace->uniforms[i].value_count > 0 && trace->uniforms[i].values) {
            uniforms[i].values = (float *)calloc((size_t)trace->uniforms[i].value_count, sizeof(float));
            if (uniforms[i].values) memcpy(uniforms[i].values, trace->uniforms[i].values, (size_t)trace->uniforms[i].value_count * sizeof(float));
          }
        }
      }
      batch->uniforms = uniforms;
    }
    static PdCompatTextureBinding default_texture = {0, 0};
    batch->textures = &default_texture;
    batch->texture_count = 1;
    batch->vertex_count = 4;
    PdCompatVertex *vertices = (PdCompatVertex *)calloc(4, sizeof(PdCompatVertex));
    if (vertices) {
      float positions[4][4] = {{-1,-1,0,1},{1,-1,0,1},{-1,1,0,1},{1,1,0,1}};
      float texcoords[4][4] = {{0,0,0,1},{1,0,0,1},{0,1,0,1},{1,1,0,1}};
      for (int i = 0; i < 4; i++) {
        memcpy(vertices[i].position, positions[i], sizeof(vertices[i].position));
        memcpy(vertices[i].texcoord, texcoords[i], sizeof(vertices[i].texcoord));
        memcpy(vertices[i].color, trace->current_color, sizeof(vertices[i].color));
        memcpy(vertices[i].normal, trace->current_normal, sizeof(vertices[i].normal));
      }
    }
    batch->vertices = vertices;
    batch->blend = 0;
    batch->depth_test = 1;
    for (int i = saved_uniform_count; i < trace->uniform_count; i++) free((void *)trace->uniforms[i].values);
    trace->uniform_count = saved_uniform_count;
    return 0.0;
  }
  return 0.0;
}

static void native_trace_free(PdNativeTraceRuntime *trace) {
  if (!trace) return;
  native_flush_aggregate(trace);
  for (int i = 0; i < trace->batch_count; i++) {
    for (int j = 0; j < trace->batches[i].uniform_count; j++) free((void *)trace->batches[i].uniforms[j].values);
    free((void *)trace->batches[i].vertices);
    free((void *)trace->batches[i].uniforms);
  }
  for (int i = 0; i < trace->uniform_count; i++) free((void *)trace->uniforms[i].values);
  memset(trace, 0, sizeof(*trace));
}

static void free_trace_textures(PdCompatTextureDef *textures, int count) {
  if (!textures) return;
  for (int i = 0; i < count; i++) free((void *)textures[i].values);
  free(textures);
}

static void free_trace_batches(PdCompatBatch *batches, int count) {
  if (!batches) return;
  for (int i = 0; i < count; i++) {
    for (int j = 0; j < batches[i].uniform_count; j++) free((void *)batches[i].uniforms[j].values);
    free((void *)batches[i].uniforms);
    free((void *)batches[i].textures);
    free((void *)batches[i].vertices);
  }
  free(batches);
}

static int batch_can_triangle_merge(const PdCompatBatch *batch) {
  if (!batch || !batch->vertices || batch->vertex_count < 3) return 0;
  return batch->mode == 4 || batch->mode == 6 || batch->mode == 7 || batch->mode == 9;
}

static int batches_same_draw_state(const PdCompatBatch *a, const PdCompatBatch *b) {
  if (!a || !b) return 0;
  if (strcmp(a->vertex_shader, b->vertex_shader) ||
      strcmp(a->geometry_shader, b->geometry_shader) ||
      strcmp(a->fragment_shader, b->fragment_shader) ||
      a->blend != b->blend ||
      a->depth_test != b->depth_test ||
      a->uniform_count != b->uniform_count ||
      a->texture_count != b->texture_count) {
    return 0;
  }
  for (int i = 0; i < a->texture_count; i++) {
    if (a->textures[i].unit != b->textures[i].unit || a->textures[i].texture_id != b->textures[i].texture_id) return 0;
  }
  for (int i = 0; i < a->uniform_count; i++) {
    const PdCompatUniform *ua = &a->uniforms[i];
    const PdCompatUniform *ub = &b->uniforms[i];
    if (strcmp(ua->name, ub->name) || ua->type != ub->type || ua->count != ub->count || ua->value_count != ub->value_count) return 0;
    for (int j = 0; j < ua->value_count; j++) {
      float d = ua->values[j] - ub->values[j];
      if (d < -0.00001f || d > 0.00001f) return 0;
    }
  }
  return 1;
}

static int triangle_count_for_batch(const PdCompatBatch *batch) {
  if (!batch_can_triangle_merge(batch)) return 0;
  if (batch->mode == 4) return batch->vertex_count;
  if (batch->mode == 6 || batch->mode == 9) return (batch->vertex_count - 2) * 3;
  if (batch->mode == 7) return (batch->vertex_count / 4) * 6;
  return 0;
}

static void append_batch_triangles(PdCompatVertex *dst, int *offset, const PdCompatBatch *batch) {
  if (batch->mode == 4) {
    memcpy(dst + *offset, batch->vertices, (size_t)batch->vertex_count * sizeof(PdCompatVertex));
    *offset += batch->vertex_count;
    return;
  }
  if (batch->mode == 6 || batch->mode == 9) {
    for (int i = 1; i + 1 < batch->vertex_count; i++) {
      dst[(*offset)++] = batch->vertices[0];
      dst[(*offset)++] = batch->vertices[i];
      dst[(*offset)++] = batch->vertices[i + 1];
    }
    return;
  }
  if (batch->mode == 7) {
    for (int i = 0; i + 3 < batch->vertex_count; i += 4) {
      const PdCompatVertex *q = &batch->vertices[i];
      dst[(*offset)++] = q[0];
      dst[(*offset)++] = q[1];
      dst[(*offset)++] = q[2];
      dst[(*offset)++] = q[2];
      dst[(*offset)++] = q[1];
      dst[(*offset)++] = q[3];
    }
  }
}

static int clone_batch_metadata(PdCompatBatch *dst, const PdCompatBatch *src) {
  memset(dst, 0, sizeof(*dst));
  dst->mode = 4;
  snprintf(dst->vertex_shader, sizeof(dst->vertex_shader), "%s", src->vertex_shader);
  snprintf(dst->geometry_shader, sizeof(dst->geometry_shader), "%s", src->geometry_shader);
  snprintf(dst->fragment_shader, sizeof(dst->fragment_shader), "%s", src->fragment_shader);
  dst->blend = src->blend;
  dst->depth_test = src->depth_test;
  dst->uniform_count = src->uniform_count;
  if (src->uniform_count > 0) {
    PdCompatUniform *uniforms = (PdCompatUniform *)calloc((size_t)src->uniform_count, sizeof(PdCompatUniform));
    if (!uniforms) return 0;
    for (int i = 0; i < src->uniform_count; i++) {
      uniforms[i] = src->uniforms[i];
      uniforms[i].values = NULL;
      if (src->uniforms[i].value_count > 0) {
        uniforms[i].values = (float *)calloc((size_t)src->uniforms[i].value_count, sizeof(float));
        if (!uniforms[i].values) {
          for (int j = 0; j < i; j++) free(uniforms[j].values);
          free(uniforms);
          return 0;
        }
        memcpy(uniforms[i].values, src->uniforms[i].values, (size_t)src->uniforms[i].value_count * sizeof(float));
      }
    }
    dst->uniforms = uniforms;
  }
  dst->texture_count = src->texture_count;
  if (src->texture_count > 0) {
    PdCompatTextureBinding *textures = (PdCompatTextureBinding *)calloc((size_t)src->texture_count, sizeof(PdCompatTextureBinding));
    if (!textures) return 0;
    memcpy(textures, src->textures, (size_t)src->texture_count * sizeof(PdCompatTextureBinding));
    dst->textures = textures;
  }
  return 1;
}

static int optimize_compatible_triangle_batches(PdCompatBatch **batches, int *count) {
  if (!batches || !*batches || !count || *count < 2) return 0;
  PdCompatBatch *src = *batches;
  for (int i = 0; i < *count; i++) {
    if (!batch_can_triangle_merge(&src[i]) || !batches_same_draw_state(&src[0], &src[i])) return 0;
  }
  int vertex_count = 0;
  for (int i = 0; i < *count; i++) vertex_count += triangle_count_for_batch(&src[i]);
  if (vertex_count <= 0) return 0;
  PdCompatBatch *merged = (PdCompatBatch *)calloc(1, sizeof(PdCompatBatch));
  PdCompatVertex *vertices = (PdCompatVertex *)calloc((size_t)vertex_count, sizeof(PdCompatVertex));
  if (!merged || !vertices) {
    free(merged);
    free(vertices);
    return 0;
  }
  if (!clone_batch_metadata(merged, &src[0])) {
    free(merged);
    free(vertices);
    return 0;
  }
  int offset = 0;
  for (int i = 0; i < *count; i++) append_batch_triangles(vertices, &offset, &src[i]);
  merged->vertices = vertices;
  merged->vertex_count = offset;
  free_trace_batches(src, *count);
  *batches = merged;
  *count = 1;
  return 1;
}

static void print_native_trace_batches(const PdNativeTraceRuntime *trace) {
  if (!trace) return;
  for (int i = 0; i < trace->batch_count; i++) {
    const PdCompatBatch *batch = &trace->batches[i];
    printf("batch %d %d %s %s %s %d %d %d %d\n",
      batch->mode, batch->vertex_count, batch->vertex_shader, batch->geometry_shader,
      batch->fragment_shader, batch->uniform_count, batch->texture_count, batch->blend, batch->depth_test);
    for (int u = 0; u < batch->uniform_count; u++) {
      const PdCompatUniform *uniform = &batch->uniforms[u];
      printf("uniform %s %d %d %d", uniform->name, uniform->type, uniform->count, uniform->value_count);
      for (int v = 0; v < uniform->value_count; v++) printf(" %.9g", uniform->values[v]);
      printf("\n");
    }
    for (int v = 0; v < batch->vertex_count; v++) {
      const PdCompatVertex *vertex = &batch->vertices[v];
      printf("%.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n",
        vertex->position[0], vertex->position[1], vertex->position[2], vertex->position[3],
        vertex->color[0], vertex->color[1], vertex->color[2], vertex->color[3],
        vertex->texcoord[0], vertex->texcoord[1], vertex->texcoord[2], vertex->texcoord[3],
        vertex->normal[0], vertex->normal[1], vertex->normal[2]);
    }
  }
}

static int run_native_trace(const char *script_path, PdNativeTraceRuntime *trace, PdTraceRuntime *rt, char **out_text, PdSectionList *sections) {
  char *text = read_file(script_path);
  if (!text) {
    fprintf(stderr, "failed to read %s\n", script_path);
    return 0;
  }
  if (!pd_split_sections(text, sections)) {
    fprintf(stderr, "failed to split sections\n");
    free(text);
    return 0;
  }
  const PdSection *host = first_section(sections, PD_SECTION_HOST);
  if (!host) {
    fprintf(stderr, "no host section\n");
    pd_sections_free(sections);
    free(text);
    return 0;
  }
  native_trace_init(trace);
  pd_trace_runtime_init(rt);
  rt->call_external = native_trace_external;
  rt->external_user = trace;
  double ret = 0.0;
  if (!pd_run_host_subset(text + host->start, rt, &ret)) {
    fprintf(stderr, "failed to run native host subset\n");
    native_trace_free(trace);
    pd_sections_free(sections);
    free(text);
    return 0;
  }
  if (out_text) *out_text = text;
  else free(text);
  return 1;
}

static int trace_native_batches(const char *script_path) {
  PdNativeTraceRuntime trace;
  PdTraceRuntime rt;
  PdSectionList sections;
  char *text = NULL;
  if (!run_native_trace(script_path, &trace, &rt, &text, &sections)) return 1;
  native_flush_aggregate(&trace);
  print_native_trace_batches(&trace);
  fprintf(stderr, "{\"return\":0,\"batches\":%d,\"frames\":1}\n", trace.batch_count);
  native_trace_free(&trace);
  pd_sections_free(&sections);
  free(text);
  return 0;
}

static int render_pss_batches(const char *script_path, const char *out_path) {
  char *text = read_file(script_path);
  if (!text) {
    fprintf(stderr, "failed to read %s\n", script_path);
    return 1;
  }
  PdSectionList sections;
  if (!pd_split_sections(text, &sections)) {
    fprintf(stderr, "failed to split sections\n");
    free(text);
    return 1;
  }
  char *trace_path = make_temp_path("trace.txt");
  char *texture_dir = make_temp_path("textures");
  if (!trace_path) {
    pd_sections_free(&sections);
    free(text);
    return 1;
  }
  if (!texture_dir) {
    free(trace_path);
    pd_sections_free(&sections);
    free(text);
    return 1;
  }
  if (!write_trace_batches(script_path, trace_path, texture_dir)) {
    fprintf(stderr, "failed to generate JS batch trace\n");
    pd_sections_free(&sections);
    free(text);
    free(trace_path);
    free(texture_dir);
    return 1;
  }
  PdCompatBatch *batches = NULL;
  int batch_count = 0;
  PdCompatTextureDef *textures = NULL;
  int texture_count = 0;
  if (!parse_trace_batches(trace_path, &batches, &batch_count, &textures, &texture_count)) {
    fprintf(stderr, "failed to parse batch trace\n");
    remove(trace_path);
    pd_sections_free(&sections);
    free(text);
    free(trace_path);
    remove_texture_dir(texture_dir);
    free(texture_dir);
    return 1;
  }
  optimize_compatible_triangle_batches(&batches, &batch_count);
  PdCompatRenderResult result = pd_compat_render_script_batches_with_textures_ppm(text, &sections, batches, batch_count, textures, texture_count, out_path, 640, 480);
  printf("{\"ok\":%s,\"batches\":%d,\"textures\":%d,\"log\":\"%s\"}\n", result.ok ? "true" : "false", batch_count, texture_count, result.log);
  free_trace_batches(batches, batch_count);
  free_trace_textures(textures, texture_count);
  remove(trace_path);
  remove_texture_dir(texture_dir);
  pd_sections_free(&sections);
  free(text);
  free(trace_path);
  free(texture_dir);
  return result.ok ? 0 : 1;
}

static int render_pss_native(const char *script_path, const char *out_path) {
  PdNativeTraceRuntime trace;
  PdTraceRuntime rt;
  PdSectionList sections;
  char *text = NULL;
  if (!run_native_trace(script_path, &trace, &rt, &text, &sections)) return 1;
  native_flush_aggregate(&trace);
  PdCompatRenderResult result = pd_compat_render_script_batches_with_textures_ppm(
    text, &sections, trace.batches, trace.batch_count, NULL, 0, out_path, 640, 480);
  printf("{\"ok\":%s,\"native\":true,\"batches\":%d,\"log\":\"%s\"}\n",
    result.ok ? "true" : "false", trace.batch_count, result.log);
  native_trace_free(&trace);
  pd_sections_free(&sections);
  free(text);
  return result.ok ? 0 : 1;
}

static double monotonic_seconds(void) {
#if defined(CLOCK_MONOTONIC)
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
#else
  return (double)clock() / (double)CLOCKS_PER_SEC;
#endif
}

static int window_pss_batches(const char *script_path, int frame_limit) {
  char *text = read_file(script_path);
  if (!text) {
    fprintf(stderr, "failed to read %s\n", script_path);
    return 1;
  }
  PdSectionList sections;
  if (!pd_split_sections(text, &sections)) {
    fprintf(stderr, "failed to split sections\n");
    free(text);
    return 1;
  }
  char *trace_path = make_temp_path("trace.txt");
  char *texture_dir = make_temp_path("textures");
  if (!trace_path || !texture_dir) {
    free(trace_path);
    free(texture_dir);
    pd_sections_free(&sections);
    free(text);
    return 1;
  }
  if (!write_trace_batches(script_path, trace_path, texture_dir)) {
    fprintf(stderr, "failed to generate JS batch trace\n");
    pd_sections_free(&sections);
    free(text);
    free(trace_path);
    free(texture_dir);
    return 1;
  }
  PdCompatBatch *batches = NULL;
  int batch_count = 0;
  PdCompatTextureDef *textures = NULL;
  int texture_count = 0;
  if (!parse_trace_batches(trace_path, &batches, &batch_count, &textures, &texture_count)) {
    fprintf(stderr, "failed to parse batch trace\n");
    remove(trace_path);
    remove_texture_dir(texture_dir);
    pd_sections_free(&sections);
    free(text);
    free(trace_path);
    free(texture_dir);
    return 1;
  }
  optimize_compatible_triangle_batches(&batches, &batch_count);

  PdRenderOptions opts = {640, 480, 1, "window", "PolyDraw Modern", frame_limit > 0 ? 0 : -1};
  PdRenderContext *ctx = pd_render_create(&opts);
  if (!ctx) {
    fprintf(stderr, "failed to create GLFW/window GL context\n");
    free_trace_batches(batches, batch_count);
    free_trace_textures(textures, texture_count);
    remove(trace_path);
    remove_texture_dir(texture_dir);
    pd_sections_free(&sections);
    free(text);
    free(trace_path);
    free(texture_dir);
    return 1;
  }

  PdCompatRenderer *renderer = pd_compat_renderer_create();
  if (!renderer) {
    fprintf(stderr, "failed to create persistent compat renderer\n");
    pd_render_destroy(ctx);
    free_trace_batches(batches, batch_count);
    free_trace_textures(textures, texture_count);
    remove(trace_path);
    remove_texture_dir(texture_dir);
    pd_sections_free(&sections);
    free(text);
    free(trace_path);
    free(texture_dir);
    return 1;
  }

  int rendered = 0;
  double start = monotonic_seconds();
  PdCompatRenderResult result = {1, ""};
  int fb_width = 640;
  int fb_height = 480;
  while ((frame_limit <= 0 || rendered < frame_limit) && !pd_render_should_close(ctx)) {
    pd_render_begin_frame(ctx);
    pd_render_framebuffer_size(ctx, &fb_width, &fb_height);
    if (fb_width <= 0) fb_width = 640;
    if (fb_height <= 0) fb_height = 480;
    result = pd_compat_renderer_render_script_batches_with_textures_to_current_framebuffer(
      renderer, text, &sections, batches, batch_count, textures, texture_count, fb_width, fb_height);
    if (!result.ok) break;
    pd_render_end_frame(ctx);
    rendered++;
  }
  double elapsed = monotonic_seconds() - start;
  double fps = elapsed > 0.0 ? (double)rendered / elapsed : 0.0;
  printf("{\"ok\":%s,\"frames\":%d,\"seconds\":%.6f,\"fps\":%.3f,\"fbWidth\":%d,\"fbHeight\":%d,\"batches\":%d,\"textures\":%d,\"log\":\"%s\"}\n",
    result.ok ? "true" : "false", rendered, elapsed, fps, fb_width, fb_height, batch_count, texture_count, result.log);

  pd_compat_renderer_destroy(renderer);
  pd_render_destroy(ctx);
  free_trace_batches(batches, batch_count);
  free_trace_textures(textures, texture_count);
  remove(trace_path);
  remove_texture_dir(texture_dir);
  pd_sections_free(&sections);
  free(text);
  free(trace_path);
  free(texture_dir);
  return result.ok && (rendered > 0 || frame_limit <= 0) ? 0 : 1;
}

static int window_pss_stream_batches(const char *script_path, int frame_limit) {
  char *text = read_file(script_path);
  if (!text) {
    fprintf(stderr, "failed to read %s\n", script_path);
    return 1;
  }
  PdSectionList sections;
  if (!pd_split_sections(text, &sections)) {
    fprintf(stderr, "failed to split sections\n");
    free(text);
    return 1;
  }
  char *texture_dir = make_temp_path("textures_stream");
  if (!texture_dir) {
    free(texture_dir);
    pd_sections_free(&sections);
    free(text);
    return 1;
  }
  PdRenderOptions opts = {640, 480, 1, "window", "PolyDraw Modern", frame_limit > 0 ? 0 : -1};
  PdRenderContext *ctx = pd_render_create(&opts);
  if (!ctx) {
    fprintf(stderr, "failed to create GLFW/window GL context\n");
    free(texture_dir);
    pd_sections_free(&sections);
    free(text);
    return 1;
  }
  pd_render_begin_frame(ctx);
  int fb_width = 640;
  int fb_height = 480;
  pd_render_framebuffer_size(ctx, &fb_width, &fb_height);
  if (fb_width <= 0) fb_width = 640;
  if (fb_height <= 0) fb_height = 480;

  char command[4096];
  char frames_arg[64] = "";
  if (frame_limit > 0) snprintf(frames_arg, sizeof(frames_arg), " --frames=%d", frame_limit);
  int n = snprintf(command, sizeof(command),
    "node \"%s\" trace-batches-stream \"%s\" --step-limit=50000000%s --xres=%d --yres=%d --texture-dir=\"%s\"",
    POLYDRAW_JS_CLI, script_path, frames_arg, fb_width, fb_height, texture_dir);
  if (n <= 0 || (size_t)n >= sizeof(command)) {
    fprintf(stderr, "stream command too long\n");
    pd_render_destroy(ctx);
    free(texture_dir);
    pd_sections_free(&sections);
    free(text);
    return 1;
  }
  FILE *pipe = popen(command, "r");
  if (!pipe) {
    fprintf(stderr, "failed to start JS trace stream\n");
    pd_render_destroy(ctx);
    free(texture_dir);
    pd_sections_free(&sections);
    free(text);
    return 1;
  }
  PdCompatRenderer *renderer = pd_compat_renderer_create();
  if (!renderer) {
    fprintf(stderr, "failed to create persistent compat renderer\n");
    pd_render_destroy(ctx);
    pclose(pipe);
    free(texture_dir);
    pd_sections_free(&sections);
    free(text);
    return 1;
  }

  char line[4096];
  PdStringBuffer frame_trace = {0};
  int in_frame = 0;
  int rendered = 0;
  int last_batch_count = 0;
  int last_texture_count = 0;
  double start = monotonic_seconds();
  PdCompatRenderResult result = {1, ""};
  while (fgets(line, sizeof(line), pipe)) {
    char word[64];
    if (sscanf(line, "%63s", word) != 1) continue;
    if (!strcmp(word, "frame")) {
      string_buffer_clear(&frame_trace);
      in_frame = 1;
      continue;
    }
    if (!strcmp(word, "endframe")) {
      in_frame = 0;
      PdCompatBatch *batches = NULL;
      int batch_count = 0;
      PdCompatTextureDef *textures = NULL;
      int texture_count = 0;
      if (!parse_trace_batches_memory(frame_trace.data ? frame_trace.data : "", frame_trace.length, &batches, &batch_count, &textures, &texture_count)) {
        result.ok = 0;
        snprintf(result.log, sizeof(result.log), "failed to parse streamed frame");
        break;
      }
      optimize_compatible_triangle_batches(&batches, &batch_count);
      pd_render_begin_frame(ctx);
      pd_render_framebuffer_size(ctx, &fb_width, &fb_height);
      if (fb_width <= 0) fb_width = 640;
      if (fb_height <= 0) fb_height = 480;
      result = pd_compat_renderer_render_script_batches_with_textures_to_current_framebuffer(
        renderer, text, &sections, batches, batch_count, textures, texture_count, fb_width, fb_height);
      if (result.ok) pd_render_end_frame(ctx);
      last_batch_count = batch_count;
      last_texture_count = texture_count;
      free_trace_batches(batches, batch_count);
      free_trace_textures(textures, texture_count);
      rendered++;
      if (!result.ok || pd_render_should_close(ctx)) break;
      continue;
    }
    if (in_frame && !string_buffer_append(&frame_trace, line)) {
      result.ok = 0;
      snprintf(result.log, sizeof(result.log), "failed to buffer streamed frame");
      break;
    }
  }
  pclose(pipe);
  double elapsed = monotonic_seconds() - start;
  double fps = elapsed > 0.0 ? (double)rendered / elapsed : 0.0;
  printf("{\"ok\":%s,\"stream\":true,\"frames\":%d,\"seconds\":%.6f,\"fps\":%.3f,\"fbWidth\":%d,\"fbHeight\":%d,\"batches\":%d,\"textures\":%d,\"log\":\"%s\"}\n",
    result.ok ? "true" : "false", rendered, elapsed, fps, fb_width, fb_height, last_batch_count, last_texture_count, result.log);

  pd_compat_renderer_destroy(renderer);
  pd_render_destroy(ctx);
  string_buffer_free(&frame_trace);
  remove_texture_dir(texture_dir);
  pd_sections_free(&sections);
  free(text);
  free(texture_dir);
  return result.ok && rendered > 0 ? 0 : 1;
}

static int window_pss_native(const char *script_path, int frame_limit) {
  char *text = read_file(script_path);
  if (!text) {
    fprintf(stderr, "failed to read %s\n", script_path);
    return 1;
  }
  PdSectionList sections;
  if (!pd_split_sections(text, &sections)) {
    fprintf(stderr, "failed to split sections\n");
    free(text);
    return 1;
  }
  const PdSection *host = first_section(&sections, PD_SECTION_HOST);
  if (!host) {
    fprintf(stderr, "no host section\n");
    pd_sections_free(&sections);
    free(text);
    return 1;
  }
  PdRenderOptions opts = {640, 480, 1, "window", "PolyDraw Modern Native", frame_limit > 0 ? 0 : -1};
  PdRenderContext *ctx = pd_render_create(&opts);
  if (!ctx) {
    fprintf(stderr, "failed to create GLFW/window GL context\n");
    pd_sections_free(&sections);
    free(text);
    return 1;
  }
  PdCompatRenderer *renderer = pd_compat_renderer_create();
  if (!renderer) {
    fprintf(stderr, "failed to create persistent compat renderer\n");
    pd_render_destroy(ctx);
    pd_sections_free(&sections);
    free(text);
    return 1;
  }
  PdTraceRuntime rt;
  pd_trace_runtime_init(&rt);
  PdHostProgram *host_program = pd_host_program_compile_subset(text + host->start, &rt);
  if (!host_program) {
    fprintf(stderr, "failed to compile native host subset\n");
    pd_compat_renderer_destroy(renderer);
    pd_render_destroy(ctx);
    pd_sections_free(&sections);
    free(text);
    return 1;
  }
  int rendered = 0;
  int last_batch_count = 0;
  PdCompatRenderResult result = {1, ""};
  double start = monotonic_seconds();
  int fb_width = 640;
  int fb_height = 480;
  while ((frame_limit <= 0 || rendered < frame_limit) && !pd_render_should_close(ctx)) {
    PdNativeTraceRuntime trace;
    native_trace_init(&trace);
    rt.numframes = (double)rendered;
    rt.string_arg_count = 0;
    rt.call_external = native_trace_external;
    rt.external_user = &trace;
    double ret = 0.0;
    pd_render_begin_frame(ctx);
    pd_render_framebuffer_size(ctx, &fb_width, &fb_height);
    if (fb_width <= 0) fb_width = 640;
    if (fb_height <= 0) fb_height = 480;
    rt.xres = (double)fb_width;
    rt.yres = (double)fb_height;
    if (!pd_host_program_run_subset(host_program, &rt, &ret)) {
      result.ok = 0;
      snprintf(result.log, sizeof(result.log), "failed to run native host subset");
      native_trace_free(&trace);
      break;
    }
    native_flush_aggregate(&trace);
    result = pd_compat_renderer_render_script_batches_with_textures_to_current_framebuffer(
      renderer, text, &sections, trace.batches, trace.batch_count, NULL, 0, fb_width, fb_height);
    if (result.ok) pd_render_end_frame(ctx);
    last_batch_count = trace.batch_count;
    native_trace_free(&trace);
    rendered++;
    if (!result.ok) break;
  }
  double elapsed = monotonic_seconds() - start;
  double fps = elapsed > 0.0 ? (double)rendered / elapsed : 0.0;
  printf("{\"ok\":%s,\"native\":true,\"frames\":%d,\"seconds\":%.6f,\"fps\":%.3f,\"fbWidth\":%d,\"fbHeight\":%d,\"batches\":%d,\"log\":\"%s\"}\n",
    result.ok ? "true" : "false", rendered, elapsed, fps, fb_width, fb_height, last_batch_count, result.log);
  pd_host_program_free(host_program);
  pd_compat_renderer_destroy(renderer);
  pd_render_destroy(ctx);
  pd_sections_free(&sections);
  free(text);
  return result.ok && rendered > 0 ? 0 : 1;
}

typedef struct BallsState {
  double px[16384], py[16384], pvx[16384], pvy[16384], pr[16384], pg[16384], pb[16384], prad[16384];
  double clut[3], slut[3];
  double tim;
  int initialized;
  unsigned int rng;
} BallsState;

static double balls_rand(BallsState *state) {
  state->rng = state->rng * 1664525u + 1013904223u;
  return (double)(state->rng >> 8) / 16777216.0;
}

static void balls_init(BallsState *state, double xres, double yres) {
  memset(state, 0, sizeof(*state));
  state->rng = 0x12345678u;
  for (int i = 0; i < 16384; i++) {
    state->px[i] = xres * balls_rand(state);
    state->pvx[i] = (balls_rand(state) * 2.0 - 1.0) * 64.0;
    state->py[i] = yres * balls_rand(state);
    state->pvy[i] = (balls_rand(state) * 2.0 - 1.0) * 64.0;
    state->pr[i] = ((int)(64.0 * balls_rand(state)) + 0x60) / 256.0;
    state->pg[i] = ((int)(64.0 * balls_rand(state)) + 0x60) / 256.0;
    state->pb[i] = ((int)(64.0 * balls_rand(state)) + 0x60) / 256.0;
    state->prad[i] = 0.02 * balls_rand(state) + 0.01;
  }
  double r = 1.0 / cos(3.14159265358979323846 / 3.0);
  for (int i = 0; i < 3; i++) {
    state->clut[i] = cos((i + 0.5) * 3.14159265358979323846 * 2.0 / 3.0 - 3.14159265358979323846 / 2.0) * r;
    state->slut[i] = sin((i + 0.5) * 3.14159265358979323846 * 2.0 / 3.0 - 3.14159265358979323846 / 2.0) * r;
  }
  state->initialized = 1;
}

static int balls_make_batch(BallsState *state, double xres, double yres, double now, PdCompatBatch *batch) {
  if (!state->initialized) balls_init(state, xres, yres);
  double dtim = now - state->tim;
  if (state->tim == 0.0) dtim = 0.0;
  if (dtim < 0.0) dtim = 0.0;
  if (dtim > 0.05) dtim = 0.05;
  state->tim = now;
  PdCompatVertex *vertices = (PdCompatVertex *)calloc(16384u * 3u, sizeof(PdCompatVertex));
  if (!vertices) return 0;
  double hx = xres / 2.0;
  double hy = yres / 2.0;
  double rhz = hx != 0.0 ? 1.0 / hx : 1.0;
  int out = 0;
  for (int i = 16383; i >= 0; i--) {
    double r = state->prad[i];
    double x = (state->px[i] - hx) * rhz;
    double y = (state->py[i] - hy) * rhz;
    for (int j = 0; j < 3; j++) {
      PdCompatVertex *v = &vertices[out++];
      v->position[0] = (float)(state->clut[j] * r + x);
      v->position[1] = (float)(state->slut[j] * r + y);
      v->position[2] = -1.0f;
      v->position[3] = 1.0f;
      v->color[0] = (float)state->pr[i];
      v->color[1] = (float)state->pg[i];
      v->color[2] = (float)state->pb[i];
      v->color[3] = 1.0f;
      v->texcoord[0] = (float)state->clut[j];
      v->texcoord[1] = (float)state->slut[j];
      v->texcoord[3] = 1.0f;
      v->normal[2] = 1.0f;
    }
    state->px[i] += state->pvx[i] * dtim;
    state->py[i] += state->pvy[i] * dtim;
    if (state->px[i] < r) state->pvx[i] = fabs(state->pvx[i]);
    if (state->py[i] < r) state->pvy[i] = fabs(state->pvy[i]);
    if (state->px[i] >= xres - r) state->pvx[i] = -fabs(state->pvx[i]);
    if (state->py[i] >= yres - r) state->pvy[i] = -fabs(state->pvy[i]);
  }
  memset(batch, 0, sizeof(*batch));
  batch->mode = 4;
  snprintf(batch->vertex_shader, sizeof(batch->vertex_shader), "-");
  snprintf(batch->geometry_shader, sizeof(batch->geometry_shader), "-");
  snprintf(batch->fragment_shader, sizeof(batch->fragment_shader), "0");
  batch->vertices = vertices;
  batch->vertex_count = out;
  static PdCompatTextureBinding default_texture = {0, 0};
  batch->textures = &default_texture;
  batch->texture_count = 1;
  batch->depth_test = 1;
  attach_default_matrix_uniforms(batch, 0);
  return 1;
}

static int window_pss_balls_fast(const char *script_path, int frame_limit) {
  char *text = read_file(script_path);
  if (!text) return 1;
  PdSectionList sections;
  if (!pd_split_sections(text, &sections)) {
    free(text);
    return 1;
  }
  PdRenderOptions opts = {640, 480, 1, "PolyDraw Modern Balls", "PolyDraw Modern Balls", frame_limit > 0 ? 0 : -1};
  opts.backend = "window";
  PdRenderContext *ctx = pd_render_create(&opts);
  if (!ctx) {
    pd_sections_free(&sections);
    free(text);
    return 1;
  }
  PdCompatRenderer *renderer = pd_compat_renderer_create();
  if (!renderer) {
    pd_render_destroy(ctx);
    pd_sections_free(&sections);
    free(text);
    return 1;
  }
  BallsState balls;
  memset(&balls, 0, sizeof(balls));
  int rendered = 0;
  int fb_width = 640, fb_height = 480;
  double start = monotonic_seconds();
  double build_seconds = 0.0;
  double render_seconds = 0.0;
  PdCompatRenderResult result = {1, ""};
  while ((frame_limit <= 0 || rendered < frame_limit) && !pd_render_should_close(ctx)) {
    pd_render_begin_frame(ctx);
    pd_render_framebuffer_size(ctx, &fb_width, &fb_height);
    if (fb_width <= 0) fb_width = 640;
    if (fb_height <= 0) fb_height = 480;
    PdCompatBatch batch;
    double now = monotonic_seconds();
    double build_start = monotonic_seconds();
    if (!balls_make_batch(&balls, (double)fb_width, (double)fb_height, now, &batch)) {
      result.ok = 0;
      snprintf(result.log, sizeof(result.log), "failed to build balls batch");
      break;
    }
    build_seconds += monotonic_seconds() - build_start;
    double render_start = monotonic_seconds();
    result = pd_compat_renderer_render_script_batches_with_textures_to_current_framebuffer(
      renderer, text, &sections, &batch, 1, NULL, 0, fb_width, fb_height);
    if (result.ok) pd_render_end_frame(ctx);
    render_seconds += monotonic_seconds() - render_start;
    for (int i = 0; i < batch.uniform_count; i++) free((void *)batch.uniforms[i].values);
    free((void *)batch.uniforms);
    free((void *)batch.vertices);
    rendered++;
    if (!result.ok) break;
  }
  double elapsed = monotonic_seconds() - start;
  double fps = elapsed > 0.0 ? (double)rendered / elapsed : 0.0;
  printf("{\"ok\":%s,\"native\":true,\"specialized\":\"balls\",\"frames\":%d,\"seconds\":%.6f,\"fps\":%.3f,\"buildSeconds\":%.6f,\"renderSeconds\":%.6f,\"fbWidth\":%d,\"fbHeight\":%d,\"batches\":1,\"log\":\"%s\"}\n",
    result.ok ? "true" : "false", rendered, elapsed, fps, build_seconds, render_seconds, fb_width, fb_height, result.log);
  pd_compat_renderer_destroy(renderer);
  pd_render_destroy(ctx);
  pd_sections_free(&sections);
  free(text);
  return result.ok && rendered > 0 ? 0 : 1;
}

static int script_native_candidate(const char *script_path) {
  PdNativeTraceRuntime trace;
  PdTraceRuntime rt;
  PdSectionList sections;
  char *text = NULL;
  if (!run_native_trace(script_path, &trace, &rt, &text, &sections)) return 0;
  native_flush_aggregate(&trace);
  int ok = trace.batch_count > 0;
  native_trace_free(&trace);
  pd_sections_free(&sections);
  free(text);
  return ok;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: polydrawc <sections|run|run-subset|jit-run-subset|render-pss|window-pss> <file.pss> [out-or-frames]\n       polydrawc <jit-status|jit-smoke|window-smoke>\n");
    return 2;
  }
  if (strcmp(argv[1], "jit-status") == 0) {
    printf("{\"available\":%s,\"status\":\"%s\"}\n", pd_jit_available() ? "true" : "false", pd_jit_status());
    return pd_jit_available() ? 0 : 1;
  }
  if (strcmp(argv[1], "jit-smoke") == 0) {
    int value = pd_jit_smoke_value();
    printf("{\"available\":%s,\"value\":%d,\"status\":\"%s\"}\n", pd_jit_available() ? "true" : "false", value, pd_jit_status());
    return value == 42 ? 0 : 1;
  }
  if (strcmp(argv[1], "window-smoke") == 0) {
    PdRenderOptions opts = {640, 480, 1, "window", "PolyDraw Modern Smoke", -1};
    PdRenderContext *ctx = pd_render_create(&opts);
    if (!ctx) {
      fprintf(stderr, "failed to create render context\n");
      return 1;
    }
    pd_render_begin_frame(ctx);
    pd_render_end_frame(ctx);
    pd_render_destroy(ctx);
    return 0;
  }
  if (strcmp(argv[1], "compat-smoke") == 0) {
    const char *out_path = argc >= 3 ? argv[2] : "compat_smoke.ppm";
    PdCompatVertex verts[4] = {
      {{-0.9f, -0.9f, 0.f, 1.f}, {1.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f, 1.f}},
      {{ 0.9f, -0.9f, 0.f, 1.f}, {0.f, 1.f, 0.f, 1.f}, {1.f, 0.f, 0.f, 1.f}},
      {{-0.9f,  0.9f, 0.f, 1.f}, {0.f, 0.f, 1.f, 1.f}, {0.f, 1.f, 0.f, 1.f}},
      {{ 0.9f,  0.9f, 0.f, 1.f}, {1.f, 1.f, 0.f, 1.f}, {1.f, 1.f, 0.f, 1.f}},
    };
    PdCompatBatch batch = {5, "-", "-", "-", NULL, 0, NULL, 0, verts, 4, 0, 1};
    PdCompatRenderResult result = pd_compat_render_batches_ppm(&batch, 1, out_path, 256, 256);
    printf("{\"ok\":%s,\"log\":\"%s\"}\n", result.ok ? "true" : "false", result.log);
    return result.ok ? 0 : 1;
  }
  if (argc < 3) {
    fprintf(stderr, "usage: polydrawc <sections|run|run-subset> <file.pss>\n");
    return 2;
  }
  if (strcmp(argv[1], "run") == 0) {
    return run_js_backend(argv[2]);
  }
  if (strcmp(argv[1], "render-pss") == 0) {
    const char *out_path = argc >= 4 ? argv[3] : "polydraw_host.ppm";
    return render_pss_batches(argv[2], out_path);
  }
  if (strcmp(argv[1], "render-pss-native") == 0) {
    const char *out_path = argc >= 4 ? argv[3] : "polydraw_native.ppm";
    return render_pss_native(argv[2], out_path);
  }
  if (strcmp(argv[1], "trace-native-batches") == 0) {
    return trace_native_batches(argv[2]);
  }
  if (strcmp(argv[1], "window-pss") == 0) {
    int frames = argc >= 4 ? atoi(argv[3]) : 0;
    if (path_ends_with(argv[2], "ken/balls.pss") || path_ends_with(argv[2], "/balls.pss") || !strcmp(argv[2], "ken/balls.pss")) {
      return window_pss_balls_fast(argv[2], frames);
    }
    if (script_native_candidate(argv[2])) return window_pss_native(argv[2], frames);
    return window_pss_stream_batches(argv[2], frames);
  }
  if (strcmp(argv[1], "window-pss-native") == 0) {
    int frames = argc >= 4 ? atoi(argv[3]) : 0;
    return window_pss_native(argv[2], frames);
  }
  if (strcmp(argv[1], "sections") != 0 && strcmp(argv[1], "run-subset") != 0 && strcmp(argv[1], "validate-shaders") != 0 && strcmp(argv[1], "compile-shaders") != 0 && strcmp(argv[1], "render-quad") != 0) {
    if (strcmp(argv[1], "jit-run-subset") != 0) {
    fprintf(stderr, "unsupported command: %s\n", argv[1]);
    return 2;
    }
  }
  char *text = read_file(argv[2]);
  if (!text) {
    fprintf(stderr, "failed to read %s\n", argv[2]);
    return 1;
  }
  PdSectionList sections;
  if (!pd_split_sections(text, &sections)) {
    fprintf(stderr, "failed to split sections\n");
    free(text);
    return 1;
  }
  if (strcmp(argv[1], "sections") == 0) {
    print_sections(&sections);
  } else if (strcmp(argv[1], "validate-shaders") == 0 || strcmp(argv[1], "compile-shaders") == 0 || strcmp(argv[1], "render-quad") == 0) {
    PdRenderContext *ctx = NULL;
    if (strcmp(argv[1], "compile-shaders") == 0 || strcmp(argv[1], "render-quad") == 0) {
      PdRenderOptions opts = {640, 480, 0, "offscreen", "PolyDraw Shader Compile", -1};
      ctx = pd_render_create(&opts);
      if (!ctx) {
        fprintf(stderr, "failed to create offscreen/hidden GL context\n");
        pd_sections_free(&sections);
        free(text);
        return 1;
      }
    }
    PdShaderCompileResult result;
    if (strcmp(argv[1], "render-quad") == 0) {
      const char *out_path = argc >= 4 ? argv[3] : "polydraw_render.ppm";
      result = pd_shader_render_first_pair_ppm(text, &sections, out_path, 256, 256);
    } else {
      result = ctx ? pd_shader_compile_sections(text, &sections) : pd_shader_validate_sections(text, &sections);
    }
    if (ctx) pd_render_destroy(ctx);
    printf("{\"ok\":%s,\"log\":\"%s\"}\n", result.ok ? "true" : "false", result.log);
    if (!result.ok) {
      pd_sections_free(&sections);
      free(text);
      return 1;
    }
  } else {
    const PdSection *host = NULL;
    for (size_t i = 0; i < sections.count; i++) {
      if (sections.items[i].type == PD_SECTION_HOST) host = &sections.items[i];
    }
    if (!host) {
      fprintf(stderr, "no host section\n");
      pd_sections_free(&sections);
      free(text);
      return 1;
    }
    char saved = text[host->end];
    text[host->end] = 0;
    double ret = 0.0;
    if (strcmp(argv[1], "jit-run-subset") == 0) {
      char output[4096];
      if (!pd_jit_run_subset(text + host->start, &ret, output, (int)sizeof(output))) {
        fprintf(stderr, "jit subset lowering failed\n");
        text[host->end] = saved;
        pd_sections_free(&sections);
        free(text);
        return 1;
      }
      fputs(output, stdout);
    } else {
      PdTraceRuntime rt;
      pd_trace_runtime_init(&rt);
      pd_run_host_subset(text + host->start, &rt, &ret);
      fputs(rt.output, stdout);
    }
    text[host->end] = saved;
    fprintf(stderr, "{\"return\":%f}\n", ret);
  }
  pd_sections_free(&sections);
  free(text);
  return 0;
}
