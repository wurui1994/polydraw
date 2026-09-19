#include "polydraw/sections.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int ensure_capacity(PdSectionList *list) {
  if (list->count < list->capacity) return 1;
  size_t next = list->capacity ? list->capacity * 2 : 8;
  PdSection *items = (PdSection *)realloc(list->items, next * sizeof(PdSection));
  if (!items) return 0;
  list->items = items;
  list->capacity = next;
  return 1;
}

static int starts_with_ci(const char *s, const char *prefix) {
  while (*prefix) {
    if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
    s++;
    prefix++;
  }
  return 1;
}

static void trim_copy(char *dst, size_t dst_size, const char *start, size_t len) {
  while (len && isspace((unsigned char)*start)) {
    start++;
    len--;
  }
  while (len && isspace((unsigned char)start[len - 1])) len--;
  if (len >= dst_size) len = dst_size - 1;
  memcpy(dst, start, len);
  dst[len] = 0;
}

static size_t skip_line(const char *text, size_t i) {
  while (text[i] && text[i] != '\n') i++;
  return i;
}

static void default_geometry(PdGeometryMeta *meta) {
  strcpy(meta->input, "GL_TRIANGLES");
  strcpy(meta->output, "GL_TRIANGLE_STRIP");
  meta->max_vertices = 8;
}

static void parse_geometry(PdGeometryMeta *meta, const char *body, size_t len) {
  default_geometry(meta);
  if (!len || body[0] != ',') return;
  const char *p = body + 1;
  const char *end = body + len;
  const char *colon = memchr(p, ':', (size_t)(end - p));
  if (colon) end = colon;
  const char *a = p;
  const char *b = memchr(a, ',', (size_t)(end - a));
  if (!b) return;
  trim_copy(meta->input, sizeof(meta->input), a, (size_t)(b - a));
  a = b + 1;
  b = memchr(a, ',', (size_t)(end - a));
  if (!b) return;
  trim_copy(meta->output, sizeof(meta->output), a, (size_t)(b - a));
  meta->max_vertices = atoi(b + 1);
}

static void parse_directive(const char *line, size_t len, PdSectionType previous, PdSectionType *type, char *name, size_t name_size, PdGeometryMeta *geometry) {
  *type = previous;
  name[0] = 0;
  default_geometry(geometry);
  if (len && line[0] == '@') {
    line++;
    len--;
  }
  while (len && isspace((unsigned char)*line)) {
    line++;
    len--;
  }

  if (len >= 13 && starts_with_ci(line, "vertex_shader")) {
    *type = PD_SECTION_VERTEX;
    line += 13;
    len -= 13;
  } else if (len >= 15 && starts_with_ci(line, "fragment_shader")) {
    *type = PD_SECTION_FRAGMENT;
    line += 15;
    len -= 15;
  } else if (len && tolower((unsigned char)line[0]) == 'h') {
    *type = PD_SECTION_HOST;
    line++;
    len--;
  } else if (len && tolower((unsigned char)line[0]) == 'v') {
    *type = PD_SECTION_VERTEX;
    line++;
    len--;
  } else if (len && tolower((unsigned char)line[0]) == 'g') {
    *type = PD_SECTION_GEOMETRY;
    line++;
    len--;
  } else if (len && tolower((unsigned char)line[0]) == 'f') {
    *type = PD_SECTION_FRAGMENT;
    line++;
    len--;
  }

  if (*type == PD_SECTION_GEOMETRY) parse_geometry(geometry, line, len);

  const char *colon = memchr(line, ':', len);
  if (colon) {
    const char *start = colon + 1;
    const char *end = line + len;
    const char *comment = strstr(start, "//");
    if (comment && comment < end) end = comment;
    trim_copy(name, name_size, start, (size_t)(end - start));
  }
}

static int push_section(PdSectionList *out, PdSectionType type, const char *name, size_t start, size_t end, int line, const PdGeometryMeta *geometry) {
  if (!ensure_capacity(out)) return 0;
  PdSection *section = &out->items[out->count++];
  section->type = type;
  trim_copy(section->name, sizeof(section->name), name, strlen(name));
  section->start = start;
  section->end = end;
  section->line = line;
  section->geometry = *geometry;
  return 1;
}

int pd_split_sections(const char *text, PdSectionList *out) {
  memset(out, 0, sizeof(*out));
  PdSectionType current_type = PD_SECTION_HOST;
  PdGeometryMeta current_geometry;
  default_geometry(&current_geometry);
  char current_name[128] = "";
  size_t current_start = 0;
  int current_line = 1;
  int line = 1;
  int in_block_comment = 0;

  for (size_t i = 0; text[i]; i++) {
    char ch = text[i];
    char next = text[i + 1];
    if (in_block_comment) {
      if (ch == '*' && next == '/') {
        in_block_comment = 0;
        i++;
      } else if (ch == '\n') {
        line++;
      }
      continue;
    }
    if (ch == '/' && next == '*') {
      in_block_comment = 1;
      i++;
      continue;
    }
    if (ch == '/' && next == '/') {
      i = skip_line(text, i + 2);
      if (text[i] == '\n') line++;
      continue;
    }
    if (ch == '\n') {
      line++;
      continue;
    }
    if (ch == '@' && (i == 0 || text[i - 1] == '\n' || text[i - 1] == '\r')) {
      size_t line_end = skip_line(text, i);
      if (!push_section(out, current_type, current_name, current_start, i, current_line, &current_geometry)) return 0;
      parse_directive(text + i, line_end - i, current_type, &current_type, current_name, sizeof(current_name), &current_geometry);
      current_start = line_end + (text[line_end] == '\n' ? 1 : 0);
      current_line = line + 1;
      i = line_end;
      if (text[i] == '\n') line++;
    }
  }

  if (!push_section(out, current_type, current_name, current_start, strlen(text), current_line, &current_geometry)) return 0;
  return 1;
}

void pd_sections_free(PdSectionList *list) {
  free(list->items);
  memset(list, 0, sizeof(*list));
}

const char *pd_section_type_name(PdSectionType type) {
  switch (type) {
    case PD_SECTION_HOST: return "host";
    case PD_SECTION_VERTEX: return "vertex";
    case PD_SECTION_GEOMETRY: return "geometry";
    case PD_SECTION_FRAGMENT: return "fragment";
  }
  return "unknown";
}
