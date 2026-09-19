#ifndef POLYDRAW_RENDER_H
#define POLYDRAW_RENDER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PdRenderOptions {
  int width;
  int height;
  int visible;
  const char *backend;
  const char *title;
  int swap_interval;
} PdRenderOptions;

typedef struct PdRenderContext PdRenderContext;

PdRenderContext *pd_render_create(const PdRenderOptions *options);
void pd_render_destroy(PdRenderContext *ctx);
int pd_render_should_close(PdRenderContext *ctx);
void pd_render_begin_frame(PdRenderContext *ctx);
void pd_render_end_frame(PdRenderContext *ctx);
void pd_render_framebuffer_size(PdRenderContext *ctx, int *width, int *height);
void *pd_render_native_window(PdRenderContext *ctx);

#ifdef __cplusplus
}
#endif

#endif
