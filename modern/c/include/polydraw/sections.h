#ifndef POLYDRAW_SECTIONS_H
#define POLYDRAW_SECTIONS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PdSectionType {
  PD_SECTION_HOST,
  PD_SECTION_VERTEX,
  PD_SECTION_GEOMETRY,
  PD_SECTION_FRAGMENT
} PdSectionType;

typedef struct PdGeometryMeta {
  char input[64];
  char output[64];
  int max_vertices;
} PdGeometryMeta;

typedef struct PdSection {
  PdSectionType type;
  char name[128];
  size_t start;
  size_t end;
  int line;
  PdGeometryMeta geometry;
} PdSection;

typedef struct PdSectionList {
  PdSection *items;
  size_t count;
  size_t capacity;
} PdSectionList;

int pd_split_sections(const char *text, PdSectionList *out);
void pd_sections_free(PdSectionList *list);
const char *pd_section_type_name(PdSectionType type);

#ifdef __cplusplus
}
#endif

#endif
