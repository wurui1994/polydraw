# Testing Plan

## Test Categories

Parser tests:

- Token snapshots.
- AST snapshots for small host snippets.
- Section parser golden outputs for `.pss` files.
- Diagnostic span tests.

Interpreter tests:

- Expression precedence.
- Built-in math functions.
- Variables and assignment operators.
- `static` persistence.
- Arrays and bounds behavior.
- Control flow.
- User functions.
- External functions and overloads.
- Runtime errors.

Runtime trace tests:

- `glbegin` / `glvertex` / `glend` command order.
- Shader selection calls.
- Texture API argument handling.
- Console output.
- Time/input globals.

Rendering tests:

- Minimal full-screen quad.
- Shader compile and line diagnostics.
- Texture upload from array.
- Texture upload from image.
- FBO capture.
- Offscreen screenshot.

JIT tests:

- Interpreter vs JIT return values.
- Interpreter vs JIT static storage.
- Interpreter vs JIT trace logs.

## Sample Corpus

Use representative scripts:

- `ken/balls.pss`: large statics, loops, immediate mode, GLSL.
- `ken/texture.pss`: textures, capture, multiple fragment sections, bare `@`.
- `ken/gpgpu.pss`: `glgettex`, `glsettex` arrays, FBO capture.
- `ken/texture3d.pss`: 3D arrays and texture upload.
- `ken/geo_test.pss`: geometry shader section metadata.
- `ken/drawcone2.pss`: functions, goto, labels, heavy host logic.
- `tigrou/fractal.pss`: explicit `(){...}` host function and legacy section naming style.
- `tigrou/tree.pss`: multiple fragment shader names such as `@f1` and `@f2` compatibility cases.

## Golden Data

Store generated expected output under:

```text
modern/shared/fixtures/
modern/shared/conformance/
```

Golden outputs should be plain text or JSON:

- Section spans and names.
- AST summary.
- Runtime trace.
- Numeric result.
- Diagnostics.

## Continuous Checks

JS:

```text
npm test
npm run lint
npm run conformance
```

C:

```text
cmake --build build
ctest --test-dir build
```

Cross-runtime:

```text
modern/tools/run-differential
```

## Acceptance Gates

M1 gate:

- All host-only conformance tests pass in JS and C.
- JS/C traces match for shared snippets.

M3 gate:

- Offscreen render smoke test produces non-empty image.
- Windowed app runs a minimal script.

M6 gate:

- JIT matches interpreter for all M1 tests.
- JIT can run at least one rendering host section through the same runtime API.
