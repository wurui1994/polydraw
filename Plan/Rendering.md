# Rendering and Runtime API Plan

## Runtime API Layers

Separate the host interpreter from rendering through a runtime API table:

- Constants and external variables.
- External functions.
- Render command backend.
- Texture backend.
- Shader backend.
- Time/input backend.
- Console/log backend.

This makes CLI trace tests possible before OpenGL is available.

## Required External Variables

- `xres`, `yres`
- `mousx`, `mousy`
- `bstatus`
- `keystatus[256]`
- `numframes`

## Required OpenGL-like Calls

Immediate drawing:

- `glclear`
- `glbegin`, `glend`
- `glvertex` 2D/3D/4D
- `gltexcoord` 2D/3D/4D
- `glcolor` 3D/4D
- `glnormal`
- `gllinewidth`
- `glquad`

Transforms:

- `glpushmatrix`, `glpopmatrix`
- `glmultmatrix`
- `gltranslate`, `glrotate`, `glscale`
- `gluperspective`, `glulookat`, `setfov`

State:

- `glblendfunc`
- `glenable`, `gldisable`
- `glcullface`
- `glalphaenable`, `glalphadisable`
- `glswapinterval`

Shaders:

- `glsetshader`
- `glgetuniformloc`
- `gluniform*`
- `glgetattribloc`
- `glvertexattrib*`
- `glprogramlocalparam`, `glprogramenvparam` for ARB compatibility.

Textures and capture:

- `glsettex`
- `glgettex`
- `glbindtexture`
- `glactivetexture`
- `glcapture`
- `glcaptureend`

General:

- `rgb`, `rgba`
- `noise`
- `printf`, `printg`
- `srand`, `sleep`
- `klock`, `glklockstart`, `glklockelapsed`
- `mountzip`
- `playnote`

## Section and Shader Handling

The section parser must support:

- Host: default first section and explicit `@h`.
- Vertex: `@v`, `@vertex_shader`.
- Geometry: `@g`.
- Fragment: `@f`, `@fragment_shader`.
- Bare `@`: same shader type as previous section.
- Named forms: `@f:name`, `@v:name`.
- Geometry metadata: `@g,input,output,max_vertices:name`.

Shader manager behavior:

- Compile changed shader sections.
- Store sections by type, index, and name.
- Link selected vertex/geometry/fragment programs on demand.
- Bind default `tex0`..`tex3` samplers after link.
- Map diagnostics back to original `.pss` lines.

## C Windowed Rendering

Recommended backend:

- GLFW or SDL for portable windows and input.
- Request compatibility profile where available to preserve immediate mode.
- Load GL functions with a small loader or existing library.

Minimum features:

- Window creation.
- Input state updates.
- Frame loop.
- Shader compilation.
- FBO capture.
- Texture upload/download.
- Screenshot/offscreen save for tests.

## C Offscreen Rendering

Use an explicit offscreen context backend:

- Linux: EGL preferred; OSMesa fallback if needed.
- macOS: CGL or hidden GLFW window if CGL maintenance is too high.
- Windows: pbuffer or hidden window WGL.

The included `glcontext/` project is a useful reference for:

- Runtime loading of GL libraries.
- Platform-specific context creation.
- `load(name)` style function pointer retrieval.
- Standalone/offscreen context modes.

The new C implementation should not depend on Python `glcontext`; it should reuse the design ideas.

## JS Rendering

Two viable targets:

- Browser-first: WebGL canvas, optional OffscreenCanvas for tests.
- Node-first: headless-gl or an equivalent GL context package.

Because PolyDraw uses fixed-function OpenGL, JS rendering needs a compatibility layer:

- Capture immediate-mode vertices between `glbegin` and `glend`.
- Convert matrix state into uniforms.
- Provide default shaders for fixed-function mode.
- Map legacy constants to WebGL-safe equivalents where possible.

Browser-first is better for interactive editing. Node/headless is better for CLI parity.

## Texture Support

Texture metadata:

- Index.
- Target: 1D, 2D, 3D, cube.
- Width, height, depth.
- Color mode.
- Filter and wrap mode.

Color modes:

- `KGL_BGRA32`
- `KGL_CHAR`
- `KGL_SHORT`
- `KGL_INT`
- `KGL_FLOAT`
- `KGL_VEC4`

Implement 2D first, then 3D and cube.

## Deferred Features

- MIDI `playnote` can be a stub initially.
- `mountzip` can initially support directories and normal files.
- ARB assembly can initially parse as shader text and report unsupported unless the C compatibility profile supports it.
- Advanced timer query can fall back to CPU time.
