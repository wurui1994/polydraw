# PolyDraw Modern

This directory contains the new implementation. The original `polydraw_src/`
tree is not modified.

## Current Status

Implemented:

- Shared `.pss` fixtures and conformance manifest.
- JavaScript section splitter.
- JavaScript EVAL-style host parser.
- JavaScript host interpreter with:
  - doubles,
  - case-insensitive names,
  - `static`,
  - function-scoped `static` storage,
  - `enum`,
  - arrays,
  - user functions,
  - reference arguments,
  - loops,
  - `goto`,
  - common math/runtime calls,
  - trace GL runtime.
- JavaScript corpus parse tests for representative `ken/` and `tigrou/` scripts.
- C section splitter and CLI.
- C GLFW/OpenGL render context skeleton.
- CGL standalone offscreen context on macOS, based on the same approach used by
  `glcontext`'s Darwin backend. Offscreen shader tests no longer require GLFW
  on macOS.
- C shader section validation and hidden-context GLSL compilation for shader
  sections.
- C hidden-context shader link and offscreen render-to-PPM path for the first
  vertex/fragment pair (`render-quad`).
- C modern OpenGL compatibility batch renderer for legacy immediate-mode host
  output. It consumes collected position/color/texcoord batches and renders via
  VAO/VBO/shaders rather than `glBegin`/`glEnd`.
- C `render-pss` command that runs the JS host interpreter for one frame,
  exports legacy draw batches, and renders those batches through the modern C
  compatibility renderer to an offscreen PPM.
- JS/C batch trace carries `glsetshader` selection metadata and `glUniform*`
  state per batch.
- JS/C batch trace carries texture unit bindings. `glsettex(id,array,w,h,
  KGL_FLOAT/KGL_VEC4/BGRA32-like)` CPU texture snapshots can be embedded for
  small data or written to RGBA8 sidecar files for larger arrays. The C renderer
  uploads those sidecars as real 2D textures.
- Multidimensional host arrays support single-index linear access, matching
  scripts that fill 3D texture storage with `buf[i]` while declaring
  `buf[z][y][x]`.
- The C compatibility renderer chooses texture targets from shader sampler
  uniforms. It binds ordinary `sampler2D` textures as `GL_TEXTURE_2D`, uploads
  6-face vertical or horizontal atlas images as `GL_TEXTURE_CUBE_MAP` when the
  shader uses `samplerCube`, and uploads 3D array textures as `GL_TEXTURE_3D`
  for `sampler3D`, covering `ken/cubetex.pss` and `ken/texture3d.pss`.
- `glsettex(id,"file")` now exports referenced file paths through the JS/C batch
  trace. The C renderer can decode P3/P6 PPM files directly, optionally decodes
  JPEG/PNG through CMake-detected `JPEG::JPEG` and `PNG::PNG`, and keeps the
  existing raw RGBA sidecar path for generated texture snapshots.
- The Node CLI path decodes PPM/JPEG/PNG image textures for JS-side CPU
  snapshots. Scripts can call `glgettex` on a file texture and then build a
  derived host-array texture before the C renderer uploads the generated RGBA
  sidecar.
- JS runtime models `glgettex` for known texture snapshots and exports
  `glcapture/glcaptureend` metadata. The C renderer now turns that metadata into
  real FBO-to-texture copies with `glCopyTexImage2D`, so later batches can sample
  captured framebuffer contents without relying on JS-generated placeholder
  capture data.
- JS trace runtime tracks basic fixed-function matrix state
  (`glpushmatrix/glpopmatrix/gltranslate/glscale/glrotate/gluperspective`) and
  emits `_pd_mvp`, `_pd_modelview`, and `_pd_normalMatrix` uniforms for batches.
  C GLSL modernization maps `ftransform()`, `gl_ModelViewProjectionMatrix`,
  `gl_ModelViewMatrix`, and `gl_NormalMatrix` to those uniforms.
- C compatibility renderer dispatches batches to `.pss` vertex/fragment shader
  sections when shader keys are available, falls back to a modern compatibility
  shader otherwise, and uploads uniforms before drawing.
- JS/C batch trace carries fixed-function normals and vector/integer uniform
  arrays. The C renderer uploads `_pd_normal` as a real vertex attribute and
  handles `glUniform[1-4][fi]v`, including array-uniform location fallback via
  `name[0]`.
- Uniform locations returned by `glGetUniformLoc` model contiguous array slots,
  so scripts can update `env[1]` by calling `glUniform*(loc+1, ...)`; this is
  covered by `ken/geo_duptris.pss`.
- JS/C batch trace carries basic blend/depth state. `glAlphaEnable` maps to
  alpha blending with depth test disabled, and `glAlphaDisable` restores depth
  testing with blending disabled, matching the original helper behavior used by
  `ken/texture3d.pss`.
- C compatibility renderer now carries the geometry shader key through the
  JS/C trace, links vertex/geometry/fragment shader triples, and modernizes the
  common `GL_EXT_geometry_shader4` built-ins used by `ken/geo_test.pss`.
- C compatibility renderer uses modern VAO/VBO draw calls, expands legacy
  `GL_QUADS` to triangles, and renders to color plus depth attachments in an
  offscreen FBO.
- The JS trace runtime starts with a PolyDraw-like default perspective matrix
  for 3D scripts that rely on the original frame setup. The `glquad` helper
  remains clip-space and C renders fullscreen helper quads without writing
  depth, matching background-pass usage in samples such as `drawcone2.pss`.
- `drawcone2.pss` now reaches the modern batch renderer with non-empty
  `GL_TRIANGLE_FAN` geometry after fixing reference-parameter readback in the
  JS host interpreter. The C suite keeps a limited 64-batch offscreen regression
  for this path.
- Function-local `static` variables are scoped separately from main/global
  statics, which lets scripts such as `gspiral.pss` use a scalar `px` in the
  main function and an unrelated `px[]` array inside helper functions.
- Host assignments can shadow enum constants with local variables after
  evaluating the right-hand side, matching scripts such as `gspiral.pss` where
  `enum {N=2^16}` coexists with a local `n`. `gspiral.pss` now traces a correct
  13-vertex spherical fan and renders non-flat through the modern C offscreen
  path.
- JavaScript WebGL runtime has a real modern batch renderer for the default
  position/color/texcoord path. It uploads typed vertex buffers and draws with a
  WebGL shader instead of relying on immediate mode. It also dispatches simple
  `.pss` vertex/fragment shader sections and uploads scalar/vector/matrix
  uniforms for the covered WebGL 1 path.
- JavaScript browser smoke coverage runs through Playwright + Chrome. It renders
  PolyDraw immediate-mode and shader+uniform fixtures into a WebGL canvas,
  checks center pixels, and reports measured FPS in the test diagnostics.
- C `run` command routed through the JS reference interpreter, so C CLI users
  can execute host scripts while the native C IR is still being built.
- C `window-pss` creates a GLFW OpenGL 3.2 core-profile window, renders parsed
  `.pss` batches through the same modern compatibility renderer used by
  offscreen output, and prints JSON with frame count, elapsed seconds, FPS,
  framebuffer size, batch count, texture count, and render log. The window
  backend queries the real framebuffer size each frame and updates the viewport,
  so HiDPI/Retina windows render full-frame instead of only in the lower-left
  backing-pixel quadrant. The window renderer keeps GL
  programs, VAO/VBO, fallback textures, and uploaded textures alive across
  frames instead of rebuilding all resources every frame. Streamed frame traces
  are parsed from memory, so the window loop no longer writes a temporary trace
  file for every frame.
- `klock()` compatibility is implemented in JS and the native C host subset for
  elapsed time and local/UTC date fields, including the original
  `YYYYMMDDHHMMSS.sss` wall-clock format for `klock(1)` and wall-clock
  millisecond field for `klock(9)`. `glklockstart()`/`glklockelapsed()` have a
  CPU-timer fallback in JS and C; true GPU timer query integration is still
  pending.
- C optional LLVM/JIT path. `jit-smoke` builds and executes a generated function
  through LLVM ORC LLJIT when LLVM headers/libs are installed; `jit-run-subset`
  lowers the current C host subset to LLVM IR and executes it through ORC. The
  covered subset includes arithmetic, arrays, one-line `for` loops, user
  functions, reference parameters, array parameters, labels, `goto`, `while`,
  `break`, `continue`, `do while`, plus block `if/else`.
- C-native host rendering has started replacing the JS trace bridge for the
  covered host subset. `trace-native-batches`, `render-pss-native`, and
  `window-pss-native` execute the native C subset with runtime callbacks that
  collect modern compatibility batches for immediate-mode color/vertex drawing,
  `glquad`, shader selection, and scalar uniforms. The native window path keeps
  the compiled host program and environment alive across frames so `static`
  state can evolve without reparsing.
- `window-pss ken/balls.pss` now avoids the Node trace bridge. It uses a native
  fast path for the original 16K-particle `balls.pss` logic and emits one
  merged modern triangle batch per frame instead of 16,384 legacy polygon draw
  calls.

Not complete yet:

- Full C host parser/interpreter.
- Full OpenGL command execution from host scripts. `render-pss` handles the
  current immediate-mode color/vertex/texcoord batch path with shader dispatch,
  uniforms, texture bindings, CPU texture uploads, JS-level `glgettex`,
  MVP/modelview/normal-matrix uniforms, `setfov`/`glulookat` camera setup,
  PPM/JPEG/PNG file textures, and C-side FBO
  capture-to-texture, cubemap atlas upload for `samplerCube`, 3D texture upload
  for `sampler3D`, and geometry shader dispatch for
  `geo_test.pss`/`geo_duptris.pss`, but CPU texture download/readback and fuller
  depth/state semantics still need to be connected.
- ARB assembly shader support is partial. The current ARBvp/ARBfp-to-GLSL
  translator handles the current checked ARB corpus: `ken/interference_asm.pss`,
  `ken/multiarb_asm.pss`, `ken/creepers_asm.pss`, `ken/drawsph_asm.pss`, and a
  bounded `ken/drawcone2_asm.pss`. It is still a compatibility subset rather
  than a complete ARB assembly implementation.
- Native C texture download/readback into host arrays.
- Native GPU timer query backing for `glklockstart()`/`glklockelapsed()`. The
  current CPU fallback gives useful elapsed timing but does not measure GPU
  execution like the original when timer queries are available.
- Full LLVM ORC lowering of the PolyDraw host IR. The current JIT path covers
  a narrow arithmetic/static-array/array-parameter/loop/function/reference-parameter/control-flow
  host subset; GL runtime calls, broader nested block forms, and full
  interpreter semantics still need to be lowered from the shared C IR.
- Full performance parity with original `kasm87`.

## JavaScript Commands

```sh
cd modern/js
npm test
npm run conformance
npm run test:webgl
node src/cli/polydraw-js.js sections ../../ken/balls.pss
node src/cli/polydraw-js.js parse ../../ken/balls.pss
node src/cli/polydraw-js.js run ../../ken/balls.pss
node src/cli/polydraw-js.js run ../../ken/texture.pss --frames=1
```

Use `--step-limit=N` for heavy scripts. Use `POLYDRAW_BATCH_LIMIT=N` with
`render-pss` to debug heavy scripts through a partial batch trace while keeping
the full interpreter path unchanged. Use `POLYDRAW_FRAMES=N` when a script needs
static host state to warm up before the rendered frame is captured.

## C Commands

```sh
cmake -S modern/c -B modern/c/build
cmake --build modern/c/build
ctest --test-dir modern/c/build --output-on-failure
modern/c/build/polydrawc sections ken/balls.pss
modern/c/build/polydrawc run ken/texture.pss
modern/c/build/polydrawc validate-shaders ken/balls.pss
modern/c/build/polydrawc compile-shaders ken/balls.pss
modern/c/build/polydrawc render-quad modern/shared/fixtures/render_gradient.pss /tmp/render_gradient.ppm
modern/c/build/polydrawc compat-smoke /tmp/compat_smoke.ppm
modern/c/build/polydrawc render-pss ken/texture.pss /tmp/texture_host.ppm
modern/c/build/polydrawc render-pss modern/shared/fixtures/uniform_color.pss /tmp/uniform_color.ppm
modern/c/build/polydrawc render-pss modern/shared/fixtures/texture_upload_float.pss /tmp/texture_upload_float.ppm
modern/c/build/polydrawc render-pss modern/shared/fixtures/texture_file_ppm.pss /tmp/texture_file_ppm.ppm
modern/c/build/polydrawc render-pss modern/shared/fixtures/texture_file_jpeg.pss /tmp/texture_file_jpeg.ppm
modern/c/build/polydrawc render-pss modern/shared/fixtures/texture_file_png.pss /tmp/texture_file_png.ppm
modern/c/build/polydrawc render-pss ken/cubetex.pss /tmp/cubetex.ppm
modern/c/build/polydrawc render-pss ken/texture3d.pss /tmp/texture3d.ppm
POLYDRAW_BATCH_LIMIT=32 modern/c/build/polydrawc render-pss "tigrou/disco ball.pss" /tmp/disco_ball.ppm
POLYDRAW_FRAMES=60 POLYDRAW_BATCH_LIMIT=32 modern/c/build/polydrawc render-pss "tigrou/ribbons invasion.pss" /tmp/ribbons.ppm
POLYDRAW_BATCH_LIMIT=32 modern/c/build/polydrawc render-pss "tigrou/dominos.pss" /tmp/dominos.ppm
modern/c/build/polydrawc render-pss "tigrou/tree.pss" /tmp/tree.ppm
modern/c/build/polydrawc render-pss ken/interference_asm.pss /tmp/interference_asm.ppm
modern/c/build/polydrawc render-pss ken/multiarb_asm.pss /tmp/multiarb_asm.ppm
modern/c/build/polydrawc render-pss ken/creepers_asm.pss /tmp/creepers_asm.ppm
modern/c/build/polydrawc render-pss ken/drawsph_asm.pss /tmp/drawsph_asm.ppm
POLYDRAW_BATCH_LIMIT=192 modern/c/build/polydrawc render-pss ken/drawcone2_asm.pss /tmp/drawcone2_asm.ppm
modern/c/build/polydrawc jit-status
modern/c/build/polydrawc jit-smoke
modern/c/build/polydrawc jit-run-subset modern/shared/fixtures/host_basic.pss
```

The C test suite also validates generated PPM files are not flat images.
It includes a bounded `drawcone2.pss` render via `POLYDRAW_BATCH_LIMIT=64` so
the shader-heavy cone path stays covered without making the default suite slow.
It also renders `ken/geo_test.pss`, `ken/geo_duptris.pss`, `ken/cubetex.pss`,
`ken/texture3d.pss`, a bounded `tigrou/disco ball.pss`, and a warmed-up
`tigrou/ribbons invasion.pss`, bounded `tigrou/dominos.pss`, and full
`tigrou/tree.pss`, covering geometry
shader dispatch, default projection setup, JPEG file texture resolution,
uniform-array location offsets, cubemap upload, 3D texture upload, and
normal-matrix shader compatibility. The warmed-up render covers multi-frame
host static state while resetting transient GL matrix state between frames.
The tree render covers multi-pass capture textures sampled by later passes, the
dominos render covers `setfov`/`glulookat` camera compatibility, and the ARB
renders cover the current ARBvp/ARBfp-to-GLSL compatibility subset, including
program env/local parameters and ARB matrix row semantics.

## Legacy GL Conversion

Legacy host drawing calls such as `glBegin`, `glVertex`, `glColor`,
`glTexCoord`, and `glEnd` are treated as front-end syntax for a modern batch
renderer. The intended flow is:

1. Host interpreter records draw batches with position/color/texcoord streams.
2. C/WebGL upload those streams to VBOs.
3. A compatibility shader draws them in a core profile or WebGL context.

This is required for WebGL and modern OpenGL; direct immediate mode is not a
supported implementation strategy.

Legacy GLSL snippets without `#version` are also modernized for core contexts:
`attribute`/`varying` are rewritten, `gl_FragColor` is mapped to an explicit
fragment output, and common fixed-function values such as `ftransform()` and
`gl_Vertex` are mapped to compatibility attributes.

LLVM/JIT detection can be controlled with:

```sh
cmake -S modern/c -B modern/c/build -DPOLYDRAW_ENABLE_LLVM=ON
```

If LLVM is found, `jit-status` reports the detected version, `jit-smoke`
executes a generated function through LLVM ORC LLJIT, and `jit-run-subset`
compiles the current narrow native host subset to LLVM IR. Current JIT
regressions cover `host_basic.pss`, `host_math_logic.pss`, and
`functions_goto.pss`, plus `reference_params.pss`, `control_flow.pss`, and
`if_else_blocks.pss`, `do_while.pss`, and `array_params.pss`. The JS
interpreter remains the semantic reference until the full C IR is connected.

`window-smoke` creates a GLFW/OpenGL window for one frame when a display is
available. `window-pss` renders a script in a visible GLFW window through the JS
reference trace stream; without a frame argument it starts the worker in
open-ended stream mode and runs until the window is closed. `window-pss-native`
uses the native C host subset and runtime batch collector for supported scripts.
With a frame argument either command acts as a bounded benchmark and reports FPS:

```sh
modern/c/build/polydrawc window-smoke
modern/c/build/polydrawc window-pss modern/shared/fixtures/webgl_immediate.pss 120
modern/c/build/polydrawc window-pss-native modern/shared/fixtures/webgl_immediate.pss 120
modern/c/build/polydrawc window-pss ken/balls.pss 120
```

## Direction

The JS interpreter is currently the semantic reference. The next major work is
to replace the temporary C host subset runner with the same AST/IR semantics,
then connect both runtimes to real rendering backends and finally lower the C IR
to LLVM ORC JIT.
