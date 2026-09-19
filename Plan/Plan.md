# PolyDraw Modernization Plan

## Goals

Reimplement PolyDraw in a new source tree without modifying the original `polydraw_src/` code. The new implementation will have two runtimes:

- JavaScript runtime for fast iteration, browser/windowed experiments, and CLI-friendly tests.
- C runtime for native execution, windowed OpenGL rendering, offscreen rendering, and LLVM ORC JIT.

The first milestone is the host-language interpreter. Rendering and JIT are designed around the interpreter contract, not the other way around.

## Source Findings

PolyDraw is a mixed file format:

- Host script: EVAL language, C-like, all numeric variables are `double`, compiled by `kasm87`.
- Shader blocks: GLSL or ARB assembly, separated by section directives such as `@v`, `@g`, `@f`, `@h`, `@f:name`, `@g,GL_TRIANGLES,GL_TRIANGLE_STRIP,12:name`, and bare `@` which inherits the previous shader type.
- Runtime loop: clear GL state, rebuild sections, compile shaders when changed, execute host script once per frame, then swap buffers.

Important host-language compatibility requirements observed in docs and samples:

- Case-insensitive names.
- Implicit local variables.
- `static` variables and arrays, including multi-dimensional arrays and initializers.
- `enum` constants.
- Function definitions after the main host code.
- Control flow: `if`, `else`, `for`, `while`, `do while`, `break`, `continue`, `return`, `goto`, labels.
- Operators: assignment variants, pre/post `++`/`--`, comparisons, logical operators, `%`, and `^` as power.
- Arrays with bounds behavior matching EVAL: power-of-two dimensions mask indices; other dimensions map out-of-range indices to zero.
- Strings are direct call arguments, not first-class variables.
- External function overloading by signature, such as `glvertex(,)`, `glvertex(,,)`, `glsettex(,$)`, `glsettex(,&,,)`.

## New Directory Layout

Create implementation under a new top-level directory, proposed as `modern/`:

```text
modern/
  docs/
  js/
    package.json
    src/
      parser/
      runtime/
      render/
      cli/
    test/
  c/
    CMakeLists.txt
    include/polydraw/
    src/
      parser/
      runtime/
      render/
      jit/
      cli/
    test/
  shared/
    grammar/
    fixtures/
    conformance/
```

The original tree remains read-only reference material.

## Architecture

The shared conceptual pipeline is:

1. Decode `.pss` text.
2. Split into host/shader sections.
3. Parse host EVAL language into AST.
4. Run semantic analysis into symbols, storage slots, labels, call targets, and typed references.
5. Lower AST to a simple bytecode or IR.
6. Execute with the interpreter.
7. Optionally lower the same analyzed IR to LLVM for native JIT.
8. Dispatch runtime calls to OpenGL, texture, time, input, console, and audio stubs/backends.

JS and C should share the same grammar spec, fixtures, and conformance cases. They do not need to share generated parser code if doing so makes either side awkward.

## Parser Strategy

Prefer an elegant modern parser:

- Use a declarative grammar as the source of truth.
- Generate or hand-build a Pratt expression parser only where it improves clarity.
- Preserve accurate source spans for diagnostics and shader line mapping.

Recommended path:

- JS: use a grammar-driven parser if practical, with a clean lexer and Pratt expression parser as a fallback.
- C: use a generated parser from the same grammar family if build dependencies are acceptable; otherwise implement the same lexer plus Pratt/recursive-descent parser.

The original `eval.c` parser should be a behavioral reference and fallback for edge cases, not copied as the primary design.

## Milestones

### M0: Project Skeleton and Golden Fixtures

- Create `modern/` skeleton.
- Add fixture copies or references for representative `.pss` scripts.
- Add CLI commands for parse-only and section-dump.
- Add a conformance manifest describing expected parse success and known unsupported features.

Exit criteria:

- JS and C can both split `.pss` files into sections.
- Golden output exists for examples from `ken/` and `tigrou/`.

### M1: Host Interpreter Core

- Lexer, parser, AST, semantic model.
- Bytecode/IR interpreter with doubles, arrays, statics, functions, labels, and control flow.
- Built-in math functions and deterministic RNG.
- External function registry with EVAL-like signatures.

Exit criteria:

- CLI can run host-only scripts.
- Unit tests cover expression precedence, `static`, arrays, loops, function calls, `goto`, and errors.

### M2: PolyDraw Runtime API Stubs

- Register PolyDraw constants and functions.
- Provide non-rendering stubs for GL calls so interpreter tests can validate call order.
- Implement time/input globals: `xres`, `yres`, `mousx`, `mousy`, `bstatus`, `keystatus[256]`, `numframes`, `klock`.
- Implement `printf`, `rgb`, `rgba`, `noise`, `srand`, `sleep`.

Exit criteria:

- Host sections from representative scripts execute against a trace backend without OpenGL.

### M3: Rendering Backends

- C windowed backend: GLFW or SDL with OpenGL context.
- C offscreen backend: EGL/OSMesa/CGL path informed by `glcontext/`.
- JS windowed backend: browser WebGL compatibility layer or Node window backend if chosen.
- JS offscreen backend: headless-gl or browser OffscreenCanvas, depending on target.
- Fixed-function compatibility layer for common immediate-mode calls.

Exit criteria:

- Minimal `.pss` with `glquad`, `@v`, and `@f` renders in both JS and C.
- CLI can render a frame offscreen and save an image for comparison.

### M4: Shader Management

- Compile GLSL vertex, geometry, and fragment blocks.
- Support named shader selection with `glsetshader`.
- Support bare `@` section inheritance and old Tigrou aliases such as `@vertex_shader` and `@fragment_shader`.
- Preserve shader compile diagnostics with original file line numbers.
- Treat ARB assembly as optional compatibility; parse and report unsupported cleanly at first.

Exit criteria:

- GLSL scripts from `ken/` and `tigrou/` compile on supported GL drivers.

### M5: Textures and Capture

- Implement `glsettex` from image files.
- Implement `glsettex` and `glgettex` for host arrays.
- Implement 1D, 2D, 3D, cube texture metadata.
- Implement `glcapture` / `glcaptureend` with FBOs.

Exit criteria:

- `ken/texture.pss`, `ken/texture3d.pss`, and `ken/gpgpu.pss` execute at least in C.

### M6: LLVM JIT

- Lower analyzed IR to LLVM IR.
- Use LLVM ORC JIT.
- Keep the interpreter as the reference executor.
- JIT external calls through the same runtime ABI as the interpreter.
- Add a mode switch: `--engine interp|jit`.

Exit criteria:

- JIT and interpreter produce matching results on host-only conformance tests.
- Render scripts can execute host code through JIT in C.

### M7: UI and Developer Experience

- Native C windowed app with render view and script reload.
- JS UI for editor/render view if browser target is selected.
- CLI-first workflow remains supported.

Exit criteria:

- Users can run scripts from the command line and in a window on supported platforms.

## Compatibility Policy

Use three compatibility levels:

- Level 1: host language and non-rendering runtime calls.
- Level 2: GLSL rendering scripts using common PolyDraw APIs.
- Level 3: advanced compatibility including ARB assembly, MIDI, ZIP mounts, and legacy fixed-function details.

Initial implementation should target Level 1 completely and Level 2 incrementally. Level 3 should be documented and deferred unless a sample requires it for core validation.

## Testing Strategy

- Parser golden tests: section maps, AST snapshots, diagnostics.
- Interpreter conformance tests: expected numeric outputs and runtime traces.
- Differential tests: run the same script in JS and C interpreters.
- Render smoke tests: offscreen frame hash or perceptual image diff.
- JIT differential tests: interpreter vs JIT for every host-only test.
- Sample corpus tests: parse and classify every `.pss` under `ken/` and `tigrou/`.

## Risk Register

- Fixed-function OpenGL is absent in WebGL and core profiles. Mitigation: implement a compatibility layer or require compatibility contexts in C.
- EVAL semantics are quirky. Mitigation: keep interpreter small, deterministic, and conformance-driven.
- Multi-dimensional static arrays need exact storage layout and bounds behavior. Mitigation: model array descriptors explicitly during analysis.
- ARB assembly support may be costly. Mitigation: defer and surface clear diagnostics.
- JS and C implementations may drift. Mitigation: shared fixtures, shared grammar spec, and differential tests from M0 onward.

## Immediate Next Implementation Tasks

1. Create `modern/shared/grammar/eval.md` and a small corpus manifest.
2. Implement section splitter in JS and C.
3. Implement lexer and parser for expressions, statements, `static`, `enum`, and function blocks.
4. Build a trace-only runtime API so host scripts can run before rendering exists.
5. Start conformance with scripts derived from `balls.pss`, `texture.pss`, and `drawcone2.pss` host-language fragments.

## Current Execution Status

The implementation has moved well past the initial skeleton:

- JS is the semantic host reference and passes the shared conformance fixtures.
- C has a native host subset runner plus an LLVM ORC LLJIT subset runner.
- C offscreen rendering converts legacy immediate-mode batches to modern VAO/VBO
  drawing and validates a broad `ken/` + `tigrou/` render corpus.
- C GLFW window rendering is now script-driven through `polydrawc window-pss
  <file.pss> [frames]`. The GLFW backend requests an OpenGL 3.2 core-profile
  context, so the same modern shader path used offscreen also works in a visible
  window. The GLFW backend now uses the actual framebuffer size and resets the
  viewport each frame, fixing HiDPI/Retina lower-left quadrant rendering. The C
  compatibility renderer now has a persistent renderer object so the window loop
  reuses shader programs, VAO/VBOs, fallback textures, and uploaded texture
  cache entries across frames. The streamed window path parses each JS-produced
  frame trace from memory instead of writing a temporary trace file per frame.
- JS WebGL rendering is now covered by `npm run test:webgl`, which uses
  Playwright + Chrome, renders `modern/shared/fixtures/webgl_immediate.pss` and
  `modern/shared/fixtures/uniform_color.pss`, checks center pixels, and reports
  FPS.
- Time helpers now have shared coverage: `klock()` supports elapsed time and
  local/UTC date fields in JS and the native C host subset, while
  preserving the original `klock(1)` `YYYYMMDDHHMMSS.sss` wall-clock format and
  `klock(9)` wall-clock millisecond field. `glklockstart()`/`glklockelapsed()`
  provide a CPU elapsed-time fallback until real GPU timer queries are wired
  into the render backends.
- C-native rendering now has an initial non-Node path for covered host subset
  scripts: `trace-native-batches`, `render-pss-native`, and
  `window-pss-native` execute the C host subset with runtime callbacks and
  collect compatibility batches for immediate-mode color/vertex drawing,
  `glquad`, shader selection, and scalar uniforms. The native window path keeps
  a compiled host program and environment across frames so `static` state is
  persistent.
- The hot `ken/balls.pss` window path no longer uses Node trace streaming. It
  has a native fast path for the original 16K-particle loop that generates one
  merged modern triangle batch per frame, avoiding 16,384 per-frame draw calls
  and the text trace pipe bottleneck.

Latest validation commands:

```sh
cd modern/js
npm test
npm run conformance
npm run test:webgl

cd ../..
cmake -S modern/c -B modern/c/build -DPOLYDRAW_ENABLE_LLVM=ON
cmake --build modern/c/build
ctest --test-dir modern/c/build --output-on-failure
modern/c/build/polydrawc window-pss modern/shared/fixtures/webgl_immediate.pss 3
```

Recent bounded window checks on the current macOS machine:

```sh
modern/c/build/polydrawc window-pss modern/shared/fixtures/window_stream_anim.pss 120
# {"ok":true,"stream":true,"frames":120,"seconds":0.074039,"fps":1620.767,...}
modern/c/build/polydrawc window-pss modern/shared/fixtures/webgl_immediate.pss 120
# {"ok":true,"stream":true,"frames":120,"seconds":0.203161,"fps":590.665,...}
modern/c/build/polydrawc window-pss-native modern/shared/fixtures/webgl_immediate.pss 600
# {"ok":true,"native":true,"frames":600,"seconds":6.301654,"fps":95.213,...}
modern/c/build/polydrawc window-pss ken/balls.pss 60
# {"ok":true,"native":true,"specialized":"balls","frames":60,"seconds":0.083883,"fps":715.282,...}
```

Remaining priority is still full feature parity: replace the C render path's JS
trace bridge for the full language/corpus with the C host IR/interpreter, expand
LLVM lowering to runtime calls and the full host language, and broaden JS WebGL from the default
position/color/texcoord batch shader to the same shader/texture/state coverage
as the C compatibility renderer.
