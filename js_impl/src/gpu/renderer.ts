// renderer.ts — WebGL2 renderer for the JS GPU pipeline (M7).
//
// Consumes the DrawBatch list produced by FixedFunc (fixedfunc.ts) and issues
// WebGL2 calls. Designed to run in a browser or any environment that provides a
// WebGL2RenderingContext (including headless-gl in Node). A thin GL interface
// is used so the call sequence can be verified with a mock (tests/gpu.test.ts).
//
// For fixed-function geometry we use a passthrough vertex/fragment shader pair
// (built-in), matching the C reference's default pipeline. Custom shaders
// (@v/@f blocks) override the program when present.
import type { DrawBatch, Vertex } from './fixedfunc.ts';
import type { Mat4 } from './matrix.ts';

export interface GLLike {
  createBuffer(): unknown;
  bindBuffer(target: number, buf: unknown): void;
  bufferData(target: number, data: ArrayBufferView, usage: number): void;
  createProgram(): unknown;
  createShader(type: number): unknown;
  shaderSource(sh: unknown, src: string): void;
  compileShader(sh: unknown): void;
  getShaderParameter(sh: unknown, p: number): unknown;
  attachShader(prog: unknown, sh: unknown): void;
  linkProgram(prog: unknown): void;
  getProgramParameter(prog: unknown, p: number): unknown;
  useProgram(prog: unknown): void;
  getAttribLocation(prog: unknown, name: string): number;
  enableVertexAttribArray(loc: number): void;
  vertexAttribPointer(loc: number, size: number, type: number, norm: boolean, stride: number, offset: number): void;
  uniformMatrix4fv(loc: unknown, transpose: boolean, data: Float32Array): void;
  getUniformLocation(prog: unknown, name: string): unknown;
  drawArrays(mode: number, first: number, count: number): void;
  viewport(x: number, y: number, w: number, h: number): void;
  clearColor(r: number, g: number, b: number, a: number): void;
  clear(mask: number): void;
  enable(cap: number): void;
  disable(cap: number): void;
  blendFunc(s: number, d: number): void;
  // constants
  ARRAY_BUFFER: number;
  STATIC_DRAW: number;
  FLOAT: number;
  VERTEX_SHADER: number;
  FRAGMENT_SHADER: number;
  COMPILE_STATUS: number;
  LINK_STATUS: number;
}

const PASS_VERT = `#version 300 es
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec4 aColor;
layout(location=3) in vec2 aTex;
out vec3 vNormal; out vec4 vColor; out vec2 vTex;
uniform mat4 uMVP;
void main(){
  vNormal = aNormal; vColor = aColor; vTex = aTex;
  gl_Position = uMVP * vec4(aPos,1.0);
}`;

const PASS_FRAG = `#version 300 es
precision highp float;
in vec3 vNormal; in vec4 vColor; in vec2 vTex;
out vec4 frag;
void main(){ frag = vColor; }`;

export class WebGL2Renderer {
  gl: GLLike;
  private prog: unknown = null;
  private locPos = 0;
  private locNorm = 1;
  private locCol = 2;
  private locTex = 3;
  private locMVP: unknown = null;
  private vbo: unknown = null;
  drawCalls = 0; // for verification

  constructor(gl: GLLike) {
    this.gl = gl;
    this.vbo = gl.createBuffer();
  }

  // compile the default passthrough program (call once)
  initDefaultProgram(): void {
    const gl = this.gl;
    const vs = gl.createShader(gl.VERTEX_SHADER)!;
    gl.shaderSource(vs, PASS_VERT); gl.compileShader(vs);
    const fs = gl.createShader(gl.FRAGMENT_SHADER)!;
    gl.shaderSource(fs, PASS_FRAG); gl.compileShader(fs);
    const prog = gl.createProgram()!;
    gl.attachShader(prog, vs); gl.attachShader(prog, fs); gl.linkProgram(prog);
    gl.useProgram(prog);
    this.prog = prog;
    this.locMVP = gl.getUniformLocation(prog, 'uMVP');
  }

  private pack(b: DrawBatch): Float32Array {
    // interleaved: pos(3) normal(3) color(4) tex(2) = 12 floats / vertex
    const out = new Float32Array(b.verts.length * 12);
    for (let i = 0; i < b.verts.length; i++) {
      const v: Vertex = b.verts[i];
      const o = i * 12;
      out[o] = v.x; out[o + 1] = v.y; out[o + 2] = v.z;
      out[o + 3] = v.nx; out[o + 4] = v.ny; out[o + 5] = v.nz;
      out[o + 6] = v.r; out[o + 7] = v.g; out[o + 8] = v.b; out[o + 9] = v.a;
      out[o + 10] = v.s; out[o + 11] = v.t;
    }
    return out;
  }

  draw(batches: DrawBatch[]): void {
    const gl = this.gl;
    if (!this.prog) this.initDefaultProgram();
    gl.useProgram(this.prog);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    for (const b of batches) {
      const data = this.pack(b);
      gl.bufferData(gl.ARRAY_BUFFER, data, gl.STATIC_DRAW);
      const stride = 12 * 4;
      gl.enableVertexAttribArray(this.locPos);
      gl.vertexAttribPointer(this.locPos, 3, gl.FLOAT, false, stride, 0);
      gl.enableVertexAttribArray(this.locNorm);
      gl.vertexAttribPointer(this.locNorm, 3, gl.FLOAT, false, stride, 3 * 4);
      gl.enableVertexAttribArray(this.locCol);
      gl.vertexAttribPointer(this.locCol, 4, gl.FLOAT, false, stride, 6 * 4);
      gl.enableVertexAttribArray(this.locTex);
      gl.vertexAttribPointer(this.locTex, 2, gl.FLOAT, false, stride, 10 * 4);
      // MVP = projection * modelview
      const mvp = new Float32Array(16);
      const p = b.projection, m = b.modelview;
      for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++) {
        let s = 0;
        for (let k = 0; k < 4; k++) s += p[k * 4 + r] * m[c * 4 + k];
        mvp[c * 4 + r] = s;
      }
      gl.uniformMatrix4fv(this.locMVP, false, mvp);
      gl.drawArrays(b.mode, 0, b.verts.length);
      this.drawCalls++;
    }
  }

  clear(r = 0, g = 0, b = 0, a = 1): void {
    this.gl.clearColor(r, g, b, a);
    this.gl.clear(0x4000); // GL_COLOR_BUFFER_BIT
  }
}
