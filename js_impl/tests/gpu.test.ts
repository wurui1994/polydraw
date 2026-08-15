// gpu.test.ts — M7 GPU layer: matrix stack + fixed-function GLCmd replay.
import { test } from 'node:test';
import assert from 'node:assert';
import { MatrixStack, mat4Identity, mat4Translate, mat4Rotate, mat4Perspective } from '../src/gpu/matrix.ts';
import { FixedFunc, transform } from '../src/gpu/fixedfunc.ts';
import { GLCmdBuf, GLCMD } from '../src/host/glcmd.ts';
import { WebGL2Renderer, adaptVertex, adaptFragment } from '../src/gpu/renderer.ts';
import { SoftRenderer } from '../src/gpu/softrender.ts';
import type { GLLike } from '../src/gpu/renderer.ts';
import { readFileSync } from 'node:fs';
import { sectionParse, sectionHost } from '../src/host/sections.ts';
import { PolyHostImpl } from '../src/host/polyhost.ts';
import { compile } from '../src/eval/parser.ts';
import { run } from '../src/backend/interp.ts';

function buildBuffer(): GLCmdBuf {
  const g = new GLCmdBuf();
  // identity modelview assumed; no CLEAR (op 0) so exactly one batch forms
  g.cmds.push({ op: GLCMD.PUSHMATRIX, mode: 0, a: 0, b: 0, c: 0, d: 0, s: null });
  g.cmds.push({ op: GLCMD.TRANSLATE, mode: 0, a: 1, b: 2, c: 3, d: 0, s: null });
  g.cmds.push({ op: GLCMD.BEGIN, mode: 0x0006, a: 0, b: 0, c: 0, d: 0, s: null }); // TRIANGLE_FAN
  g.cmds.push({ op: GLCMD.COLOR, mode: 0, a: 1, b: 0, c: 0, d: 1, s: null });
  g.cmds.push({ op: GLCMD.VERTEX, mode: 0, a: 0, b: 0, c: 0, d: 1, s: null });
  g.cmds.push({ op: GLCMD.VERTEX, mode: 0, a: 1, b: 0, c: 0, d: 1, s: null });
  g.cmds.push({ op: GLCMD.VERTEX, mode: 0, a: 0, b: 1, c: 0, d: 1, s: null });
  g.cmds.push({ op: GLCMD.END, mode: 0, a: 0, b: 0, c: 0, d: 0, s: null });
  g.cmds.push({ op: GLCMD.POPMATRIX, mode: 0, a: 0, b: 0, c: 0, d: 0, s: null });
  return g;
}

test('matrix: translate post-multiplies correctly', () => {
  const m = mat4Identity();
  mat4Translate(m, 1, 2, 3);
  // column-major: translation in elements 12,13,14
  assert.ok(Math.abs(m[12] - 1) < 1e-12);
  assert.ok(Math.abs(m[13] - 2) < 1e-12);
  assert.ok(Math.abs(m[14] - 3) < 1e-12);
  assert.ok(Math.abs(m[15] - 1) < 1e-12);
});

test('matrix: rotate 90deg about Z maps (1,0,0)->(0,-1,0) (C mat4_rotate quirk)', () => {
  // C's mat4_rotate writes its literal row-major but multiplies column-major,
  // which transposes the rotation — so a positive angle rotates the opposite
  // way vs standard GL. The JS port replicates that to stay pixel-identical
  // to the C baseline (gl_renderer.c). Rx of (1,0,0) by 90° about Z therefore
  // maps to (0,-1,0), not (0,1,0).
  const m = mat4Identity();
  mat4Rotate(m, 90, 0, 0, 1);
  const x = m[0], y = m[1];
  assert.ok(Math.abs(x) < 1e-9, `x=${x}`);
  assert.ok(Math.abs(y + 1) < 1e-9, `y=${y}`);
});

test('matrix: stack push/pop restores state', () => {
  const s = new MatrixStack();
  s.loadIdentity();
  s.push();
  s.translate(5, 5, 5);
  assert.ok(Math.abs(s.modelView[12] - 5) < 1e-12);
  s.pop();
  assert.ok(Math.abs(s.modelView[12]) < 1e-12);
});

test('matrix: perspective is invertible-ish (nonzero det elements)', () => {
  const m = mat4Identity();
  mat4Perspective(m, 60, 1.33, 0.1, 100);
  assert.ok(Math.abs(m[0]) > 0);
  assert.ok(Math.abs(m[11] + 1) < 1e-9, `m11=${m[11]}`);
});

test('fixedfunc: replay assembles one batch with 3 verts', () => {
  const ff = new FixedFunc();
  const batches = ff.replay(buildBuffer());
  assert.equal(batches.length, 1);
  assert.equal(batches[0].mode, 0x0006);
  assert.equal(batches[0].verts.length, 3);
  // first vert transformed by translate(1,2,3): (0,0,0,1) -> (1,2,3,1)
  const [x, y, z, w] = transform(ff.batches[0].modelview, batches[0].verts[0]);
  assert.ok(Math.abs(x - 1) < 1e-9 && Math.abs(y - 2) < 1e-9 && Math.abs(z - 3) < 1e-9);
});

test('fixedfunc: real disco GLCmd -> vertex count matches C reference', () => {
  const full = readFileSync('/Users/wurui/Documents/polydraw/tigrou/disco ball.pss', 'utf8');
  const sl = sectionParse(full);
  const h = sectionHost(sl)!;
  const ph = new PolyHostImpl();
  const host = ph.install();
  const r = compile(full.slice(h.start, h.end), host);
  assert.ok(r.ok, r.err);
  ph.glbuf.reset(); ph.srand(1); ph.state.numframes = 30; ph.state.clockScale = 1 / 60;
  run(r.program, null, r.program.globals, null);
  const ff = new FixedFunc();
  const batches = ff.replay(ph.glbuf);
  let totalVerts = 0, nBegin = 0;
  for (const b of batches) { totalVerts += b.verts.length; nBegin++; }
  // disco ball: C reference reports 19970 GLBEGIN and 79880 GLVERTEX.
  assert.equal(nBegin, 19970);
  assert.equal(totalVerts, 79880);
});

// mock GL that records drawArrays calls; verifies the WebGL2 renderer maps
// each batch to exactly one drawArrays with the correct vertex count.
function mockGL(): { gl: GLLike; draws: { mode: number; count: number }[] } {
  const draws: { mode: number; count: number }[] = [];
  const gl = {
    ARRAY_BUFFER: 0x8892, STATIC_DRAW: 0x88e4, FLOAT: 0x1406,
    VERTEX_SHADER: 0x8b31, FRAGMENT_SHADER: 0x8b30,
    COMPILE_STATUS: 0x8b81, LINK_STATUS: 0x8b82,
    createBuffer: () => ({}), bindBuffer: () => {}, bufferData: () => {},
    createProgram: () => ({}), createShader: () => ({}),
    shaderSource: () => {}, compileShader: () => {},
    getShaderParameter: () => true, getShaderInfoLog: () => '', attachShader: () => {},
    linkProgram: () => {}, getProgramParameter: () => true, getProgramInfoLog: () => '',
    deleteShader: () => {}, deleteProgram: () => {}, uniform1i: () => {},
    useProgram: () => {}, getAttribLocation: (p: unknown, n: string) =>
      n === 'aPos' ? 0 : n === 'aNormal' ? 1 : n === 'aColor' ? 2 : 3,
    enableVertexAttribArray: () => {}, vertexAttribPointer: () => {},
    uniformMatrix4fv: () => {}, uniform1f: () => {}, getUniformLocation: () => ({}),
    drawArrays: (mode: number, first: number, count: number) => draws.push({ mode, count }),
    viewport: () => {}, clearColor: () => {}, clear: () => {},
    enable: () => {}, disable: () => {}, blendFunc: () => {},
    createTexture: () => ({}), bindTexture: () => {}, activeTexture: () => {},
    texImage2D: () => {}, texSubImage2D: () => {},
    generateMipmap: () => {}, texParameteri: () => {},
    createFramebuffer: () => ({}), bindFramebuffer: () => {},
    framebufferTexture2D: () => {}, checkFramebufferStatus: () => 0x8cd5,
    readPixels: () => {}, deleteFramebuffer: () => {}, deleteTexture: () => {},
    cullFace: () => {}, lineWidth: () => {},
    TEXTURE_2D: 0xde1, RGBA: 0x1908, UNSIGNED_BYTE: 0x1401, TEXTURE0: 0x84c0,
    RGBA8: 0x8058, RGBA32F: 0x8814, FLOAT_COMPONENT: 0x1907,
    TEXTURE_CUBE_MAP: 0x8513, TEXTURE_CUBE_MAP_POSITIVE_X: 0x8515,
    TEXTURE_CUBE_MAP_NEGATIVE_X: 0x8516, TEXTURE_CUBE_MAP_POSITIVE_Y: 0x8517,
    TEXTURE_CUBE_MAP_NEGATIVE_Y: 0x8518, TEXTURE_CUBE_MAP_POSITIVE_Z: 0x8519,
    TEXTURE_CUBE_MAP_NEGATIVE_Z: 0x851a,
    TEXTURE_MIN_FILTER: 0x2801, TEXTURE_MAG_FILTER: 0x2800,
    TEXTURE_WRAP_S: 0x2802, TEXTURE_WRAP_T: 0x2803,
    LINEAR: 0x2601, NEAREST: 0x2600, LINEAR_MIPMAP_LINEAR: 0x2703,
    MIRRORED_REPEAT: 0x8370, REPEAT: 0x2901, CLAMP_TO_EDGE: 0x812f,
    FRAMEBUFFER: 0x8d40, COLOR_ATTACHMENT0: 0x8ce0, FRAMEBUFFER_COMPLETE: 0x8cd5,
    DEPTH_TEST: 0x0b71, BLEND: 0x0be2, CULL_FACE: 0x0b44, FRONT: 0x0404, BACK: 0x0405,
    POINTS: 0x0000, LINES: 0x0001, LINE_STRIP: 0x0003, TRIANGLES: 0x0004,
  } as unknown as GLLike;
  return { gl, draws };
}

test('renderer: disco batches -> 19970 drawArrays, total 79880 verts', () => {
  const full = readFileSync('/Users/wurui/Documents/polydraw/tigrou/disco ball.pss', 'utf8');
  const sl = sectionParse(full);
  const h = sectionHost(sl)!;
  const ph = new PolyHostImpl();
  const host = ph.install();
  const r = compile(full.slice(h.start, h.end), host);
  ph.glbuf.reset(); ph.srand(1); ph.state.numframes = 30; ph.state.clockScale = 1 / 60;
  run(r.program, null, r.program.globals, null);
  const ff = new FixedFunc();
  const batches = ff.replay(ph.glbuf);
  const { gl, draws } = mockGL();
  const ren = new WebGL2Renderer(gl);
  ren.setSize(300, 300);
  ren.draw({ batches });
  assert.equal(ren.drawCalls, 19970);
  assert.equal(draws.length, 19970);
  let total = 0; for (const d of draws) total += d.count;
  assert.equal(total, 79880);
});

// --- glPointSize / GL_POINTS (M-alignment with current C level) ---
// Mirrors the C viewer mainloop: globals/arrays persist across run_frame calls,
// so textures generated under `numframes==0` exist by the time we render frame 30.
function replayExample(rel: string, w = 300, h = 300) {
  const full = readFileSync('/Users/wurui/Documents/polydraw/' + rel, 'utf8');
  const sl = sectionParse(full);
  const h0 = sectionHost(sl)!;
  const ph = new PolyHostImpl();
  const host = ph.install();
  const r = compile(full.slice(h0.start, h0.end), host);
  assert.ok(r.ok, r.err);
  ph.state.xres = w; ph.state.yres = h; ph.state.clockScale = 1 / 60;
  for (let f = 0; f <= 30; f++) {
    ph.glbuf.reset(); ph.srand(1); ph.state.numframes = f;
    ph.attachMemory(r.program.globals);
    run(r.program, null, r.program.globals, null);
  }
  const ff = new FixedFunc(w, h);
  const batches = ff.replay(ph.glbuf);
  return { batches, ff };
}

test('fixedfunc: 03_point emits GL_POINTS with pointSize 9', () => {
  const { batches, ff } = replayExample('examples/opengl/03_point.pss');
  const pts = batches.filter((b) => b.mode === 0x0000); // PDGL.POINTS
  assert.equal(pts.length, 1);
  assert.equal(pts[0].verts.length, 1);
  assert.equal(pts[0].pointSize, 9);
});

test('fixedfunc: 05_rotate_points emits GL_POINTS with pointSize 4', () => {
  const { batches, ff } = replayExample('examples/opengl/05_rotate_points.pss');
  const pts = batches.filter((b) => b.mode === 0x0000);
  assert.equal(pts.length, 1);
  assert.equal(pts[0].verts.length, 3);
  assert.equal(pts[0].pointSize, 4);
});

test('softrender: 03_point rasterizes a 9x9 point', () => {
  const { batches, ff } = replayExample('examples/opengl/03_point.pss');
  const sr = new SoftRenderer({ width: 300, height: 300, fragment: (v) => [v.r, v.g, v.b] });
  sr.render({ batches, captures: ff.captures, texData: ff.texData });
  const img = sr.getImage();
  let lit = 0;
  for (let i = 0; i < img.length; i += 3) if (img[i] > 0.5 && img[i + 1] < 0.1) lit++;
  assert.equal(lit, 9 * 9, `expected 81 red pixels, got ${lit}`);
});

// --- offscreen capture (M7 acceptance) ---
test('fixedfunc: 25_offscreen_capture emits a CAPTUREEND capture op', () => {
  const { batches, ff } = replayExample('examples/opengl/25_offscreen_capture.pss');
  assert.ok(ff.captures.length >= 1, 'expected at least one capture op');
  const cap = ff.captures[0];
  assert.equal(cap.tex, 2);          // glcaptureend(2)
  assert.ok(cap.afterIndex < batches.length, 'capture should precede the textured quad batch');
  // at least one batch samples the captured texture
  assert.ok(batches.some((b) => b.useTex && b.tex === 2));
});

test('softrender: 25_offscreen_capture draws a non-empty frame using captured tex', () => {
  const { batches, ff } = replayExample('examples/opengl/25_offscreen_capture.pss', 300, 300);
  const sr = new SoftRenderer({ width: 300, height: 300, fragment: (v) => [v.r, v.g, v.b] });
  sr.render({ batches, captures: ff.captures, texData: ff.texData });
  const img = sr.getImage();
  let lit = 0;
  for (let i = 0; i < img.length; i += 3) if (img[i] + img[i + 1] + img[i + 2] > 0.05) lit++;
  assert.ok(lit > 100, `expected captured scene to cover the frame, got ${lit} lit px`);
});

test('renderer: 25_offscreen_capture renders into an FBO-backed texture (tex 2)', () => {
  const { batches, ff } = replayExample('examples/opengl/25_offscreen_capture.pss', 300, 300);
  const { gl } = mockGL();
  const ren = new WebGL2Renderer(gl);
  ren.setSize(300, 300);
  let bindFramebuffer = 0;
  (gl as { bindFramebuffer: () => void }).bindFramebuffer = () => { bindFramebuffer++; };
  ren.draw({ batches, captures: ff.captures, texData: ff.texData });
  // The capture path uses a framebuffer object (render-to-texture), not a slow
  // readPixels readback — this mirrors the C reference's capture_to_tex().
  assert.ok(bindFramebuffer >= 1, 'expected capture to bind an offscreen framebuffer');
  assert.ok(ren.getCapture(2) !== undefined, 'captured tex 2 should be stored');
});

// --- legacy GLSL (1.20) -> GLSL ES 3.00 adaptation (mirrors C gl_renderer.c) ---
test('glsl adapter: legacy shader rewritten to ES 3.00', () => {
  const vs = `attribute vec3 pos; varying vec4 c; void main(){ gl_Position = ftransform(); vec3 n = gl_Normal; }`;
  const fs = `varying vec4 c; void main(){ gl_FragColor = texture2D(tex, gl_TexCoord.xy) * gl_Color; }`;
  const av = adaptVertex(vs);
  const af = adaptFragment(fs);
  // legacy tokens must be gone
  assert.ok(!/\battribute\b/.test(av), 'attribute should be rewritten');
  assert.ok(!/\bvarying\b/.test(av) && !/\bvarying\b/.test(af), 'varying should be rewritten');
  assert.ok(/\bftransform\b/.test(av) === false || av.includes('_pd_ftransform'), 'ftransform should be renamed');
  assert.ok(af.includes('texture('), 'texture2D should become texture()');
  assert.ok(af.includes('_pd_fragColor'), 'gl_FragColor should be renamed');
  // the host header still declares gl_Vertex/gl_Normal/gl_Color/gl_TexCoord as macros
  assert.ok(av.includes('gl_Vertex') || av.includes('_pd_ftransform'), 'legacy vertex names preserved for macro injection');
});

// --- procedural texture (26) ---
test('fixedfunc: 26_texture_procedural uploads a texture and samples it', () => {
  const { batches, ff } = replayExample('examples/opengl/26_texture_procedural.pss', 300, 300);
  assert.ok(ff.texData.length >= 1, 'expected a glsettex upload');
  assert.ok(batches.some((b) => b.useTex && b.tex >= 0), 'expected a textured quad batch');
});

test('softrender: 26_texture_procedural draws a textured surface', () => {
  const { batches, ff } = replayExample('examples/opengl/26_texture_procedural.pss', 300, 300);
  const sr = new SoftRenderer({ width: 300, height: 300, fragment: (v) => [v.r, v.g, v.b] });
  sr.render({ batches, captures: ff.captures, texData: ff.texData });
  const b0 = batches[0];
  console.log('DBG26 batch mode=', b0.mode, 'useTex=', b0.useTex, 'tex=', b0.tex, 'nvert=', b0.verts.length, 'proj=', Array.from(b0.projection as Float64Array).map(x=>x.toFixed(2)).join(','), 'mv=', Array.from(b0.modelview as Float64Array).map(x=>x.toFixed(2)).join(','));
  const img = sr.getImage();
  let lit = 0;
  for (let i = 0; i < img.length; i += 3) if (img[i] + img[i + 1] + img[i + 2] > 0.05) lit++;
  let mx = 0; for (let i = 0; i < img.length; i += 3) mx = Math.max(mx, img[i] + img[i + 1] + img[i + 2]);
  console.log('DBG26 lit=', lit, 'maxsum=', mx.toFixed(2), 'tex0?', sr['tex'] ? sr['tex'].size : 'n/a');
  assert.ok(lit > 100, `expected textured surface to cover the frame, got ${lit} lit px`);
});


