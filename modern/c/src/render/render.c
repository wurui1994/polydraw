#include "polydraw/render.h"
#include "render_backend.h"

#include <stdlib.h>
#include <string.h>

struct PdRenderContext {
  PdRenderBackend backend;
};

static int backend_valid(PdRenderBackend backend) {
  return backend.impl && backend.destroy;
}

PdRenderContext *pd_render_create(const PdRenderOptions *options) {
  PdRenderBackend backend = {0};

  if (options && options->backend) {
    if (!strcmp(options->backend, "offscreen") || !strcmp(options->backend, "cgl")) {
      backend = pd_render_create_cgl_backend(options);
    } else if (!strcmp(options->backend, "glfw") || !strcmp(options->backend, "window")) {
      backend = pd_render_create_glfw_backend(options);
    }
  } else {
    backend = pd_render_create_cgl_backend(options);
    if (!backend_valid(backend)) backend = pd_render_create_glfw_backend(options);
  }

  if (!backend_valid(backend)) return NULL;
  PdRenderContext *ctx = (PdRenderContext *)calloc(1, sizeof(PdRenderContext));
  if (!ctx) {
    backend.destroy(backend.impl);
    return NULL;
  }
  ctx->backend = backend;
  return ctx;
}

void pd_render_destroy(PdRenderContext *ctx) {
  if (!ctx) return;
  if (ctx->backend.destroy) ctx->backend.destroy(ctx->backend.impl);
  free(ctx);
}

int pd_render_should_close(PdRenderContext *ctx) {
  if (!ctx || !ctx->backend.should_close) return 1;
  return ctx->backend.should_close(ctx->backend.impl);
}

void pd_render_begin_frame(PdRenderContext *ctx) {
  if (ctx && ctx->backend.begin_frame) ctx->backend.begin_frame(ctx->backend.impl);
}

void pd_render_end_frame(PdRenderContext *ctx) {
  if (ctx && ctx->backend.end_frame) ctx->backend.end_frame(ctx->backend.impl);
}

void pd_render_framebuffer_size(PdRenderContext *ctx, int *width, int *height) {
  if (width) *width = 0;
  if (height) *height = 0;
  if (ctx && ctx->backend.framebuffer_size) ctx->backend.framebuffer_size(ctx->backend.impl, width, height);
}

void *pd_render_native_window(PdRenderContext *ctx) {
  if (!ctx || !ctx->backend.native_window) return NULL;
  return ctx->backend.native_window(ctx->backend.impl);
}
