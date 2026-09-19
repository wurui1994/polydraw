#include "render_backend.h"

#if defined(POLYDRAW_WITH_GLFW)
#include <GLFW/glfw3.h>
#include <stdlib.h>
#include <string.h>

typedef struct GlfwBackend {
  GLFWwindow *window;
  int owns_glfw;
} GlfwBackend;

static void glfw_destroy(void *impl);
static int glfw_should_close(void *impl);
static void glfw_begin_frame(void *impl);
static void glfw_end_frame(void *impl);
static void glfw_framebuffer_size(void *impl, int *width, int *height);
static void *glfw_native_window(void *impl);

PdRenderBackend pd_render_create_glfw_backend(const PdRenderOptions *options) {
  PdRenderBackend backend = {0};
  if (options && options->backend && strcmp(options->backend, "glfw") && strcmp(options->backend, "window")) return backend;
  if (!glfwInit()) return backend;
  glfwWindowHint(GLFW_VISIBLE, options && !options->visible ? GLFW_FALSE : GLFW_TRUE);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
  int width = options && options->width ? options->width : 640;
  int height = options && options->height ? options->height : 480;
  const char *title = options && options->title ? options->title : "PolyDraw Modern";
  GLFWwindow *window = glfwCreateWindow(width, height, title, NULL, NULL);
  if (!window) {
    glfwTerminate();
    return backend;
  }
  GlfwBackend *ctx = (GlfwBackend *)calloc(1, sizeof(GlfwBackend));
  ctx->window = window;
  ctx->owns_glfw = 1;
  glfwMakeContextCurrent(window);
  if (options && options->swap_interval >= 0) glfwSwapInterval(options->swap_interval);
  backend.impl = ctx;
  backend.destroy = glfw_destroy;
  backend.should_close = glfw_should_close;
  backend.begin_frame = glfw_begin_frame;
  backend.end_frame = glfw_end_frame;
  backend.framebuffer_size = glfw_framebuffer_size;
  backend.native_window = glfw_native_window;
  return backend;
}

static void glfw_destroy(void *impl) {
  GlfwBackend *ctx = (GlfwBackend *)impl;
  if (!ctx) return;
  if (ctx->window) glfwDestroyWindow(ctx->window);
  if (ctx->owns_glfw) glfwTerminate();
  free(ctx);
}

static int glfw_should_close(void *impl) {
  GlfwBackend *ctx = (GlfwBackend *)impl;
  return !ctx || glfwWindowShouldClose(ctx->window);
}

static void glfw_begin_frame(void *impl) {
  GlfwBackend *ctx = (GlfwBackend *)impl;
  if (!ctx) return;
  glfwPollEvents();
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(ctx->window, &width, &height);
  if (width > 0 && height > 0) glViewport(0, 0, width, height);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

static void glfw_end_frame(void *impl) {
  GlfwBackend *ctx = (GlfwBackend *)impl;
  if (!ctx) return;
  glfwSwapBuffers(ctx->window);
}

static void *glfw_native_window(void *impl) {
  GlfwBackend *ctx = (GlfwBackend *)impl;
  return ctx ? ctx->window : NULL;
}

static void glfw_framebuffer_size(void *impl, int *width, int *height) {
  GlfwBackend *ctx = (GlfwBackend *)impl;
  if (!ctx || !ctx->window) return;
  glfwGetFramebufferSize(ctx->window, width, height);
}

#else
#include <stdlib.h>

PdRenderBackend pd_render_create_glfw_backend(const PdRenderOptions *options) {
  (void)options;
  PdRenderBackend backend = {0};
  return backend;
}
#endif
