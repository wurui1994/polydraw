# PolyDraw C implementation — TDD build
CC      = cc
# POLYDRAW_JIT: llvm | sljit | off | auto
#   llvm  = LLVM backend only (core target; requires system LLVM)
#   sljit = sljit backend only (transitional)
#   off   = no JIT (interpreter only)
#   auto  = both, prefer LLVM when llvm-config found (default)
POLYDRAW_JIT ?= auto

LLVM_CONFIG ?= $(shell command -v llvm-config 2>/dev/null)
ifeq ($(LLVM_CONFIG),)
  LLVM_CONFIG := $(shell test -x /opt/homebrew/opt/llvm/bin/llvm-config && echo /opt/homebrew/opt/llvm/bin/llvm-config)
endif

ifeq ($(POLYDRAW_JIT),off)
  JIT_LLVM := 0
  JIT_SLJIT := 0
else ifeq ($(POLYDRAW_JIT),llvm)
  JIT_LLVM := 1
  JIT_SLJIT := 0
else ifeq ($(POLYDRAW_JIT),sljit)
  JIT_LLVM := 0
  JIT_SLJIT := 1
else
  JIT_LLVM := 0
  JIT_SLJIT := 1
  ifneq ($(LLVM_CONFIG),)
    JIT_LLVM := 1
  endif
endif

JITDEF =
SRC_JIT = src/eval/pd_jit.c
ifeq ($(JIT_SLJIT),1)
  SRC_JIT += src/eval/pd_jit_sljit.c third_party/sljit/sljitLir.c
  JITDEF += -DPD_HAVE_SLJIT
endif
ifeq ($(JIT_LLVM),1)
  SRC_JIT += src/eval/pd_jit_llvm.c
  JITDEF += -DPD_HAVE_LLVM
  LLVM_INC   = -I$(shell $(LLVM_CONFIG) --includedir)
  LLVM_LIBDIR = $(shell $(LLVM_CONFIG) --libdir)
  LLVM_LIBS   = -L$(LLVM_LIBDIR) -lLLVM-$(shell $(LLVM_CONFIG) --version | cut -d. -f1)
else
  LLVM_INC   =
  LLVM_LIBS  =
endif

CFLAGS  = -std=c11 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
          -O2 -g -Isrc -Isrc/eval -Itests -Ithird_party -Ithird_party/sljit \
          $(LLVM_INC) $(JITDEF)
LDFLAGS = -lm $(LLVM_LIBS)

SRC_EVAL = src/eval/pd_ir.c src/eval/pd_interp.c src/eval/pd_lexer.c \
           src/eval/pd_parser.c src/eval/parser-fold.c src/eval/pd_compile.c \
           src/eval/pd_host.c \
           src/eval/pd_section.c $(SRC_JIT)
SRC_POLY = src/pd_polyhost.c
SRC_RENDER = src/render/glcmd.c src/render/pd_polyhost_render.c src/render/pd_polyhost_tex.c src/render/pd_runlib.c
SRC_GL = src/render/gl_offscreen.c src/render/gl_renderer.c

TESTS = build/test_interp build/test_lexer build/test_compile build/test_host build/test_fold build/test_section build/test_render build/test_jit
TOOLS = build/polydraw-eval build/polydraw-render build/polydraw-view

# macOS uses .dylib, Linux .so
ifeq ($(shell uname -s),Darwin)
  SHLIB = build/libpolydraw_eval.dylib
  SHARED = -dynamiclib
  GLLIBS = -framework OpenGL
  GLFWLIB = -L/opt/homebrew/lib -lglfw
  GLFWINC = -I/opt/homebrew/include
else
  SHLIB = build/libpolydraw_eval.so
  SHARED = -shared
  GLLIBS = -lEGL -lGL
  GLFWLIB = -lglfw
  GLFWINC =
endif

all: $(TESTS) $(TOOLS) $(SHLIB)

build:
	mkdir -p build

build/test_interp: tests/test_interp.c src/eval/pd_ir.c src/eval/pd_interp.c \
                    src/eval/pd_ir.h src/eval/pd_interp.h tests/test_main.h | build
	$(CC) $(CFLAGS) -o $@ tests/test_interp.c src/eval/pd_ir.c src/eval/pd_interp.c $(LDFLAGS)

build/test_lexer: tests/test_lexer.c src/eval/pd_lexer.c src/eval/pd_ir.c \
                   src/eval/pd_lexer.h src/eval/pd_ir.h tests/test_main.h | build
	$(CC) $(CFLAGS) -o $@ tests/test_lexer.c src/eval/pd_lexer.c src/eval/pd_ir.c $(LDFLAGS)

build/test_compile: tests/test_compile.c $(SRC_EVAL) \
                    src/eval/pd_ir.h src/eval/pd_interp.h src/eval/pd_lexer.h \
                    src/eval/pd_parser.h src/eval/pd_compile.h src/eval/pd_host.h \
                    tests/test_main.h | build
	$(CC) $(CFLAGS) -o $@ tests/test_compile.c $(SRC_EVAL) $(LDFLAGS)

build/test_host: tests/test_host.c $(SRC_EVAL) \
                 src/eval/pd_compile.h src/eval/pd_host.h src/eval/pd_interp.h \
                 src/eval/pd_ir.h tests/test_main.h | build
	$(CC) $(CFLAGS) -o $@ tests/test_host.c $(SRC_EVAL) $(LDFLAGS)

build/test_fold: tests/test_fold.c $(SRC_EVAL) \
                 src/eval/pd_compile.h src/eval/pd_host.h src/eval/pd_interp.h \
                 src/eval/pd_ir.h tests/test_main.h | build
	$(CC) $(CFLAGS) -o $@ tests/test_fold.c $(SRC_EVAL) $(LDFLAGS)

build/test_section: tests/test_section.c src/eval/pd_section.c \
                     src/eval/pd_section.h tests/test_main.h | build
	$(CC) $(CFLAGS) -o $@ tests/test_section.c src/eval/pd_section.c $(LDFLAGS)

build/test_render: tests/test_render.c $(SRC_EVAL) $(SRC_POLY) $(SRC_RENDER) \
                    src/render/glcmd.h src/render/pd_runlib.h src/pd_polyhost.h \
                    tests/test_main.h | build
	$(CC) $(CFLAGS) -o $@ tests/test_render.c $(SRC_EVAL) $(SRC_POLY) $(SRC_RENDER) $(LDFLAGS)

build/test_jit: tests/test_jit.c $(SRC_EVAL) $(SRC_POLY) $(SRC_RENDER) \
                 src/eval/pd_jit.h src/eval/pd_interp.h src/eval/pd_compile.h \
                 src/render/glcmd.h src/render/pd_runlib.h src/pd_polyhost.h \
                 tests/test_main.h | build
	$(CC) $(CFLAGS) -o $@ tests/test_jit.c $(SRC_EVAL) $(SRC_POLY) $(SRC_RENDER) $(LDFLAGS)

# Shared library for Python ctypes (and future C offscreen). Exposes pdrl_*.
$(SHLIB): $(SRC_EVAL) $(SRC_POLY) $(SRC_RENDER) | build
	$(CC) $(CFLAGS) $(SHARED) -o $@ $(SRC_EVAL) $(SRC_POLY) $(SRC_RENDER) $(LDFLAGS)

build/diag_disco: tests/diag_disco.c $(SRC_EVAL) $(SRC_POLY) $(SRC_RENDER) | build
	$(CC) $(CFLAGS) -o $@ tests/diag_disco.c $(SRC_EVAL) $(SRC_POLY) $(SRC_RENDER) $(LDFLAGS)

build/polydraw-eval: src/polydraw-eval.c $(SRC_EVAL) $(SRC_POLY) \
                     src/eval/pd_compile.h src/eval/pd_ir.h src/eval/pd_interp.h \
                     src/eval/pd_section.h src/pd_polyhost.h | build
	$(CC) $(CFLAGS) -o $@ src/polydraw-eval.c $(SRC_EVAL) $(SRC_POLY) $(LDFLAGS)

# Offscreen GL renderer: .pss → EVAL → GLCmd stream → CGL/EGL GL → PNG
build/polydraw-render: src/render_main.c $(SRC_EVAL) $(SRC_POLY) $(SRC_RENDER) $(SRC_GL) \
                       src/render/glcmd.h src/render/pd_runlib.h src/render/gl_renderer.h \
                       src/render/gl_offscreen.h src/eval/pd_section.h src/pd_polyhost.h \
                       third_party/stb_image_write.h | build
	$(CC) $(CFLAGS) -Isrc/render -Ithird_party -o $@ src/render_main.c \
	    $(SRC_EVAL) $(SRC_POLY) $(SRC_RENDER) $(SRC_GL) $(LDFLAGS) $(GLLIBS)

# Interactive GLFW window viewer: same pipeline as polydraw-render but
# driven live (M8: window GUI). Blits the offscreen FBO onto the window.
build/polydraw-view: src/view_main.c $(SRC_EVAL) $(SRC_POLY) $(SRC_RENDER) $(SRC_GL) \
                       src/render/glcmd.h src/render/pd_runlib.h src/render/gl_renderer.h \
                       src/render/gl_offscreen.h src/eval/pd_section.h src/pd_polyhost.h | build
	$(CC) $(CFLAGS) $(GLFWINC) -Isrc/render -Ithird_party -o $@ src/view_main.c \
	    $(SRC_EVAL) $(SRC_POLY) $(SRC_RENDER) $(SRC_GL) $(LDFLAGS) $(GLLIBS) $(GLFWLIB)

# Performance baseline (M8): interp vs LLVM vs sljit over N frames.
build/bench: tools/bench.c $(SRC_EVAL) $(SRC_POLY) $(SRC_RENDER) | build
	$(CC) $(CFLAGS) -Isrc -Isrc/eval -Isrc/render -o $@ tools/bench.c \
	    $(SRC_EVAL) $(SRC_POLY) $(SRC_RENDER) $(LDFLAGS)

# REAL end-to-end frame-rate baseline (M8): EVAL + GL replay + readback.
build/framebench: tools/framebench.c $(SRC_EVAL) $(SRC_POLY) $(SRC_RENDER) $(SRC_GL) | build
	$(CC) $(CFLAGS) $(GLFWINC) -Isrc -Isrc/eval -Isrc/render -o $@ tools/framebench.c \
	    $(SRC_EVAL) $(SRC_POLY) $(SRC_RENDER) $(SRC_GL) $(LDFLAGS) $(GLLIBS) $(GLFWLIB)

# REAL window probe: launch a GLFW window, render into its default FB, read
# back the on-screen pixels. Definitive black-window check (M8).
build/winprobe: tools/winprobe.c $(SRC_EVAL) $(SRC_POLY) $(SRC_RENDER) $(SRC_GL) | build
	$(CC) $(CFLAGS) $(GLFWINC) -Isrc -Isrc/eval -Isrc/render -o $@ tools/winprobe.c \
	    $(SRC_EVAL) $(SRC_POLY) $(SRC_RENDER) $(SRC_GL) $(LDFLAGS) $(GLLIBS) $(GLFWLIB)

test: $(TESTS)
	@for t in $(TESTS); do echo "=== $$t ==="; $$t || exit 1; done

clean:
	rm -rf build

.PHONY: all test clean
