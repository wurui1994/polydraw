// gpu.test.ts — M7 GPU layer: matrix stack + fixed-function GLCmd replay.
import { test } from 'node:test';
import assert from 'node:assert';
import { MatrixStack, mat4Identity, mat4Translate, mat4Rotate, mat4Perspective } from '../src/gpu/matrix.ts';
import { FixedFunc, transform } from '../src/gpu/fixedfunc.ts';
import { GLCmdBuf, GLCMD } from '../src/host/glcmd.ts';
import { WebGL2Renderer } from '../src/gpu/renderer.ts';
import type { GLLike } from '../src/gpu/renderer.ts';
import { readFileSync } from 'node:fs';
import { sectionParse, sectionHost } from '../src/host/sections.ts';
import { PolyHostImpl } from '../src/host/polyhost.ts';
import { compile } from '../src/eval/parser.ts';
import { run } from '../src/backend/interp.ts';

function buildBuffer(): GLCmdBuf {
  const g = new GLCmdBuf();
  g.push(); // identity modelview assumed
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

test('matrix: rotate 90deg about Z maps (1,0,0)->(0,1,0)', () => {
  const m = mat4Identity();
  mat4Rotate(m, 90, 0, 0, 1);
  const x = m[0], y = m[1];
  assert.ok(Math.abs(x) < 1e-9, `x=${x}`);
  assert.ok(Math.abs(y - 1) < 1e-9, `y=${y}`);
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
    getShaderParameter: () => true, attachShader: () => {},
    linkProgram: () => {}, getProgramParameter: () => true,
    useProgram: () => {}, getAttribLocation: (p: unknown, n: string) =>
      n === 'aPos' ? 0 : n === 'aNormal' ? 1 : n === 'aColor' ? 2 : 3,
    enableVertexAttribArray: () => {}, vertexAttribPointer: () => {},
    uniformMatrix4fv: () => {}, getUniformLocation: () => ({}),
    drawArrays: (mode: number, first: number, count: number) => draws.push({ mode, count }),
    viewport: () => {}, clearColor: () => {}, clear: () => {},
    enable: () => {}, disable: () => {}, blendFunc: () => {},
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
  ren.draw(batches);
  assert.equal(ren.drawCalls, 19970);
  assert.equal(draws.length, 19970);
  let total = 0; for (const d of draws) total += d.count;
  assert.equal(total, 79880);
});
