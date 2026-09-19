#ifndef POLYDRAW_RENDER_BACKEND_H
#define POLYDRAW_RENDER_BACKEND_H

#include "polydraw/render.h"

typedef struct PdRenderBackend {
  void *impl;
  void (*destroy)(void *impl);
  int (*should_close)(void *impl);
  void (*begin_frame)(void *impl);
  void (*end_frame)(void *impl);
  void (*framebuffer_size)(void *impl, int *width, int *height);
  void *(*native_window)(void *impl);
} PdRenderBackend;

PdRenderBackend pd_render_create_glfw_backend(const PdRenderOptions *options);
PdRenderBackend pd_render_create_cgl_backend(const PdRenderOptions *options);

#endif
