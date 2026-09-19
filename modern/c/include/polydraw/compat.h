#ifndef POLYDRAW_COMPAT_H
#define POLYDRAW_COMPAT_H

#include "polydraw/sections.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PdCompatVertex {
  float position[4];
  float color[4];
  float texcoord[4];
  float normal[3];
} PdCompatVertex;

typedef struct PdCompatUniform {
  char name[64];
  int type;
  int count;
  float *values;
  int value_count;
} PdCompatUniform;

typedef struct PdCompatTextureBinding {
  int unit;
  int texture_id;
} PdCompatTextureBinding;

typedef struct PdCompatTextureDef {
  int texture_id;
  int width;
  int height;
  int depth;
  int format;
  int capture_start;
  int capture_end;
  char path[512];
  const float *values;
  int value_count;
} PdCompatTextureDef;

typedef struct PdCompatBatch {
  int mode;
  char vertex_shader[64];
  char geometry_shader[64];
  char fragment_shader[64];
  const PdCompatUniform *uniforms;
  int uniform_count;
  const PdCompatTextureBinding *textures;
  int texture_count;
  const PdCompatVertex *vertices;
  int vertex_count;
  int blend;
  int depth_test;
} PdCompatBatch;

typedef struct PdCompatRenderResult {
  int ok;
  char log[4096];
} PdCompatRenderResult;

typedef struct PdCompatRenderer PdCompatRenderer;

PdCompatRenderResult pd_compat_render_batches_ppm(
  const PdCompatBatch *batches,
  int batch_count,
  const char *out_path,
  int width,
  int height
);

PdCompatRenderResult pd_compat_render_script_batches_ppm(
  const char *source,
  const PdSectionList *sections,
  const PdCompatBatch *batches,
  int batch_count,
  const char *out_path,
  int width,
  int height
);

PdCompatRenderResult pd_compat_render_script_batches_with_textures_ppm(
  const char *source,
  const PdSectionList *sections,
  const PdCompatBatch *batches,
  int batch_count,
  const PdCompatTextureDef *textures,
  int texture_count,
  const char *out_path,
  int width,
  int height
);

PdCompatRenderer *pd_compat_renderer_create(void);
void pd_compat_renderer_destroy(PdCompatRenderer *renderer);

PdCompatRenderResult pd_compat_renderer_render_script_batches_with_textures_to_current_framebuffer(
  PdCompatRenderer *renderer,
  const char *source,
  const PdSectionList *sections,
  const PdCompatBatch *batches,
  int batch_count,
  const PdCompatTextureDef *textures,
  int texture_count,
  int width,
  int height
);

PdCompatRenderResult pd_compat_render_script_batches_with_textures_to_current_framebuffer(
  const char *source,
  const PdSectionList *sections,
  const PdCompatBatch *batches,
  int batch_count,
  const PdCompatTextureDef *textures,
  int texture_count,
  int width,
  int height
);

#ifdef __cplusplus
}
#endif

#endif
