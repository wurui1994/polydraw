#include "render_backend.h"

#if defined(POLYDRAW_WITH_CGL)
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <stdlib.h>
#include <string.h>

typedef struct CglBackend {
  CGLContextObj ctx;
  CGLContextObj previous;
  int width;
  int height;
} CglBackend;

static void cgl_destroy(void *impl);
static int cgl_should_close(void *impl);
static void cgl_begin_frame(void *impl);
static void cgl_end_frame(void *impl);
static void cgl_framebuffer_size(void *impl, int *width, int *height);
static void *cgl_native_window(void *impl);

PdRenderBackend pd_render_create_cgl_backend(const PdRenderOptions *options) {
  PdRenderBackend backend = {0};
  if (options && options->backend && strcmp(options->backend, "offscreen") && strcmp(options->backend, "cgl")) return backend;

  CGLPixelFormatObj pixelformat = NULL;
  GLint num_pixelformats = 0;

  CGLPixelFormatAttribute attrs4[] = {
    kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,
    (CGLPixelFormatAttribute)0,
  };
  CGLChoosePixelFormat(attrs4, &pixelformat, &num_pixelformats);

  if (!pixelformat) {
    CGLPixelFormatAttribute attrs3[] = {
      kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL3_Core,
      (CGLPixelFormatAttribute)0,
    };
    CGLChoosePixelFormat(attrs3, &pixelformat, &num_pixelformats);
  }

  if (!pixelformat) {
    CGLPixelFormatAttribute attrs_compat[] = {
      (CGLPixelFormatAttribute)0,
    };
    CGLChoosePixelFormat(attrs_compat, &pixelformat, &num_pixelformats);
  }

  if (!pixelformat) return backend;

  CGLContextObj ctx = NULL;
  CGLError err = CGLCreateContext(pixelformat, NULL, &ctx);
  CGLDestroyPixelFormat(pixelformat);
  if (err != kCGLNoError || !ctx) return backend;

  CglBackend *render = (CglBackend *)calloc(1, sizeof(CglBackend));
  if (!render) {
    CGLDestroyContext(ctx);
    return backend;
  }
  render->ctx = ctx;
  render->previous = CGLGetCurrentContext();
  render->width = options && options->width ? options->width : 640;
  render->height = options && options->height ? options->height : 480;
  CGLSetCurrentContext(ctx);
  backend.impl = render;
  backend.destroy = cgl_destroy;
  backend.should_close = cgl_should_close;
  backend.begin_frame = cgl_begin_frame;
  backend.end_frame = cgl_end_frame;
  backend.framebuffer_size = cgl_framebuffer_size;
  backend.native_window = cgl_native_window;
  return backend;
}

static void cgl_destroy(void *impl) {
  CglBackend *ctx = (CglBackend *)impl;
  if (!ctx) return;
  CGLSetCurrentContext(ctx->previous);
  if (ctx->ctx) CGLDestroyContext(ctx->ctx);
  free(ctx);
}

static int cgl_should_close(void *impl) {
  (void)impl;
  return 0;
}

static void cgl_begin_frame(void *impl) {
  (void)impl;
}

static void cgl_end_frame(void *impl) {
  (void)impl;
}

static void *cgl_native_window(void *impl) {
  CglBackend *ctx = (CglBackend *)impl;
  return ctx ? ctx->ctx : NULL;
}

static void cgl_framebuffer_size(void *impl, int *width, int *height) {
  CglBackend *ctx = (CglBackend *)impl;
  if (!ctx) return;
  if (width) *width = ctx->width;
  if (height) *height = ctx->height;
}
#else
PdRenderBackend pd_render_create_cgl_backend(const PdRenderOptions *options) {
  (void)options;
  PdRenderBackend backend = {0};
  return backend;
}
#endif
