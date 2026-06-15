# pyref — Python reference renderer (software rasterizer)

Deterministic, GPU-free reference implementation of the PolyDraw render
pipeline. Drives the C EVAL engine via ctypes and replays the recorded
`gl*` command stream through a numpy software rasterizer. Produces PNG
output for visual verification and serves as the golden oracle for the
future C-GL and JS-WebGL2 backends (M4/M7 in the roadmap).

## Why software (not vispy/GL)?

The original plan was to use vispy/PyOpenGL offscreen. In this sandbox no GL
context can be created (no display server, headless GL unavailable). A numpy
software rasterizer is fully deterministic, runs anywhere, and is a *better*
oracle: pixel-identical across runs and machines, no driver variance.

## Architecture (the cross-backend contract)

```
.pss host block ──C EVAL (libpolydraw_eval)──► GLCmd stream ──► rasterizer ──► PNG
                  (ctypes: pdrl_compile/run)                    (software_renderer.py)
```

The `GLCmd` stream (`c_impl/src/render/glcmd.h`) is the contract shared by all
three backends: Python rasterizes it here; the C offscreen renderer and the JS
WebGL2 renderer will both consume the same stream and must produce equivalent
output.

## Usage

Build the C shared library first:
```
cd c_impl && make
```

Render a single frame to PNG:
```
python pyref/render.py ken/balls.pss --frame 30 --w 640 --h 480 --out balls.png
```

Verify against golden references (pixel-identical):
```
python pyref/verify.py            # diff against pyref/golden/
python pyref/verify.py --update   # regenerate goldens
```

## Rendering model

- **Coordinate pipeline** (matches original, see `Plan/05_Graphics.md`):
  `glVertex` eye-space → `gluPerspective(setfov(90)≈73.74°, w/h, 0.1, 1000)`
  → perspective divide → viewport map (GL bottom-left origin, flipped on save).
- **Primitive tessellation**: GL_POLYGON→fan, GL_QUADS→2 tris, GL_TRIANGLES,
  GL_TRIANGLE_FAN/STRIP, GL_QUAD_STRIP. (Lines/points skipped for now.)
- **Clipping**: Sutherland–Hodgman against the viewport so triangles whose
  vertices are off-screen render their visible slice.
- **Interpolation**: barycentric, screen-space linear for color/texcoord/eye-pos.
- **Fragment shaders**: classified by signature from the `@f` block. Currently
  hand-coded for `balls` (length+discard+scale) and `ceilflor2` (the procedural
  rotation/mod pattern). Passthrough for others. A general GLSL-subset
  interpreter is a future extension.
- **Deterministic clock**: `klock()` returns `numframes/60` (set via
  `pdrl_set_clock_scale`) so renders are reproducible for golden diffing.

## Files

- `render.py` — CLI: load `.pss`, compile via ctypes, run frames sequentially
  (state accumulates), rasterize, save PNG.
- `software_renderer.py` — the rasterizer: matrices, tessellation, clipping,
  barycentric fill, fragment-shader dispatch.
- `verify.py` — regenerate/diff against `golden/`.
- `golden/` — checked-in reference PNGs (balls_f5.png, ceilflor2_f2.png).

## Probe scripts (debugging)

- `_probe_vispy.py` — confirmed vispy offscreen is unavailable here.
- `_probe_runlib.py` — dumps the GLCmd stream from a `.pss` (useful for
  inspecting what the EVAL engine emitted).
