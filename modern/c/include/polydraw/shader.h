#ifndef POLYDRAW_SHADER_H
#define POLYDRAW_SHADER_H

#include "polydraw/sections.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PdShaderCompileResult {
  int ok;
  char log[4096];
} PdShaderCompileResult;

PdShaderCompileResult pd_shader_validate_sections(const char *source, const PdSectionList *sections);
PdShaderCompileResult pd_shader_compile_sections(const char *source, const PdSectionList *sections);
PdShaderCompileResult pd_shader_render_first_pair_ppm(const char *source, const PdSectionList *sections, const char *out_path, int width, int height);
unsigned int pd_shader_link_sections(const char *source, const PdSection *vertex, const PdSection *fragment, PdShaderCompileResult *result);
unsigned int pd_shader_link_sections_with_geometry(const char *source, const PdSection *vertex, const PdSection *geometry, const PdSection *fragment, PdShaderCompileResult *result);

#ifdef __cplusplus
}
#endif

#endif
