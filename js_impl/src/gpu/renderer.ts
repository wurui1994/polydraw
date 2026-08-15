// renderer.ts — real WebGL2 renderer (GLSL ES 3.00).
//
// This is a faithful port of c_impl/src/render/gl_renderer.c: it mirrors the
// identical pipeline (matrix stack, batching, MVP, shader/texture state,
// offscreen capture-to-texture, blending/depth/cull) on top of a WebGL2 context.
//
// Two vertex paths exist, exactly like the C reference:
//   * default material  — a built-in GLSL ES 3.00 program (colour + optional
//     texture modulation), with normal-based lighting omitted to stay in sync
//     with the reference's default "basic colour" shader;
//   * custom shaders    — the PSS `shader` block is a *legacy* GLSL 1.20 source
//     (gl_Position / ftransform() / gl_Vertex / gl_Color / gl_TexCoord /
//     gl_FragColor / varying / texture2D). adapt_vertex/adapt_fragment rewrite
//     it to GLSL ES 3.00 on the fly (attribute->in, varying->out/in,
//     texture2D->texture, ftDepth/ftransform->built-ins preserved).

export interface GLLike {
  // constants the renderer references
  ARRAY_BUFFER: number; STATIC_DRAW: number; DYNAMIC_DRAW: number; FLOAT: number;
  VERTEX_SHADER: number; FRAGMENT_SHADER: number;
  COMPILE_STATUS: number; LINK_STATUS: number;
  TEXTURE_2D: number; RGBA: number; RGBA8: number; RGBA32F: number;
  UNSIGNED_BYTE: number; UNSIGNED_INT: number; FLOAT_COMPONENT: number;
  TEXTURE0: number; TEXTURE_CUBE_MAP: number;
  TEXTURE_CUBE_MAP_POSITIVE_X: number; TEXTURE_CUBE_MAP_NEGATIVE_X: number;
  TEXTURE_CUBE_MAP_POSITIVE_Y: number; TEXTURE_CUBE_MAP_NEGATIVE_Y: number;
  TEXTURE_CUBE_MAP_POSITIVE_Z: number; TEXTURE_CUBE_MAP_NEGATIVE_Z: number;
  TEXTURE_MIN_FILTER: number; TEXTURE_MAG_FILTER: number; TEXTURE_WRAP_S: number;
  TEXTURE_WRAP_T: number; LINEAR: number; NEAREST: number;
  LINEAR_MIPMAP_LINEAR: number; MIRRORED_REPEAT: number; REPEAT: number;
  CLAMP_TO_EDGE: number; FRAMEBUFFER: number; COLOR_ATTACHMENT0: number;
  FRAMEBUFFER_COMPLETE: number; DEPTH_TEST: number; BLEND: number;
  CULL_FACE: number; FRONT: number; BACK: number;
  POINTS: number; LINES: number; LINE_STRIP: number; TRIANGLES: number;
  // buffer / program
  createBuffer(): any; bindBuffer(target: number, buf: any): void;
  bufferData(target: number, data: ArrayBufferView | number[], usage: number): void;
  createProgram(): any; createShader(type: number): any;
  shaderSource(sh: any, src: string): void; compileShader(sh: any): void;
  getShaderParameter(sh: any, pname: number): any; getShaderInfoLog(sh: any): string;
  attachShader(prog: any, sh: any): void; linkProgram(prog: any): void;
  getProgramParameter(prog: any, pname: number): any; getProgramInfoLog(prog: any): string;
  deleteShader(sh: any): void; deleteProgram(p: any): void;
  uniform1i(loc: any, v: number): void; uniform1f(loc: any, v: number): void;
  useProgram(prog: any): void;
  getAttribLocation(prog: any, name: string): number;
  enableVertexAttribArray(loc: number): void; vertexAttribPointer(loc: number, size: number, type: number, norm: boolean, stride: number, off: number): void;
  uniformMatrix4fv(loc: any, transpose: boolean, m: number[] | Float32Array): void;
  getUniformLocation(prog: any, name: string): any;
  drawArrays(mode: number, first: number, count: number): void;
  viewport(x: number, y: number, w: number, h: number): void;
  clearColor(r: number, g: number, b: number, a: number): void;
  clear(mask: number): void;
  enable(cap: number): void; disable(cap: number): void;
  blendFunc(s: number, d: number): void; cullFace(mode: number): void;
  lineWidth(w: number): void;
  createTexture(): any; bindTexture(target: number, tex: any): void;
  activeTexture(unit: number): void;
  texImage2D(target: number, level: number, internal: number, w: number, h: number, border: number, fmt: number, type: number, pixels: any): void;
  texImage2DWithData(target: number, level: number, internal: number, w: number, h: number, border: number, fmt: number, type: number, data: any): void;
  texSubImage2D(target: number, level: number, x: number, y: number, w: number, h: number, fmt: number, type: number, data: any): void;
  generateMipmap(target: number): void;
  texParameteri(target: number, pname: number, param: number): void;
  deleteTexture(tex: any): void;
  createFramebuffer(): any; bindFramebuffer(target: number, fb: any): void;
  framebufferTexture2D(target: number, attachment: number, texTarget: number, tex: any, level: number): void;
  checkFramebufferStatus(target: number): number; deleteFramebuffer(fb: any): void;
  readPixels(x: number, y: number, w: number, h: number, fmt: number, type: number, buf: any): void;
}

export interface TextureData {
  id: number;
  w: number;
  h: number;
  z: number;
  colmode: number;
  pixels: number[] | null;
}

export interface CaptureOp {
  afterIndex: number;
  tex: number;
}

export interface SceneFrame {
  batches: any[];
  captures: CaptureOp[];
  texData: TextureData[];
}

// Alias used by mainloop.ts / other callers.
export type RenderScene = SceneFrame;

const FLOATS_PER_VERT = 12; // pos3 normal3 color4 tex2

// ---- legacy GLSL (1.20) -> GLSL ES 3.00 adaptation (mirrors C adapt_vertex) ----

export function adaptVertex(src: string): string {
  let s = src;
  // token-safe: only rewrite when used as a variable/field, not inside words
  const id = (name: string) => new RegExp('\\b' + name + '\\b', 'g');
  s = s.replace(id('attribute'), 'in');
  s = s.replace(id('varying'), 'out');
  // gl_Vertex / gl_Normal / gl_Color / gl_TexCoord declared by the host
  s = s.replace(id('ftransform'), '_pd_ftransform');
  // build a position via the supplied MVP (we declare it; see HEADER below)
  s = s.replace(id('gl_Position'), 'gl_Position'); // keep
  return s;
}

export function adaptFragment(src: string): string {
  let s = src;
  const id = (name: string) => new RegExp('\\b' + name + '\\b', 'g');
  s = s.replace(id('varying'), 'in');
  s = s.replace(/\bgl_FragColor\b/g, '_pd_fragColor');
  s = s.replace(/\btexture2D\s*\(/g, 'texture(');
  s = s.replace(/\bgl_TexCoord\b/g, 'gl_TexCoord');
  return s;
}

// Header prepended to adapted custom vertex/fragment shaders.
const VS_HEADER = `#version 300 es
precision highp float;
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec4 aColor;
layout(location=3) in vec2 aTexCoord;
uniform mat4 mvp;          // = projection * modelview
out vec4 vColor;
out vec2 vTexCoord;
out vec3 vNormal;
// legacy declarations injected by the host:
#define gl_Vertex aPos
#define gl_Normal aNormal
#define gl_Color aColor
#define gl_TexCoord vTexCoordIn
in vec2 vTexCoordIn;
vec4 _pd_ftransform() { return mvp * vec4(aPos, 1.0); }
`;

const FS_HEADER = `#version 300 es
precision highp float;
in vec4 vColor;
in vec2 vTexCoord;
in vec3 vNormal;
out vec4 _pd_fragColor;
`;

// Default material program (colour + optional texture).
const VS_DEFAULT = `#version 300 es
precision highp float;
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec4 aColor;
layout(location=3) in vec2 aTexCoord;
uniform mat4 mvp;
uniform float useTex;
out vec4 vColor;
out vec2 vTexCoord;
void main() {
  vColor = aColor;
  vTexCoord = aTexCoord;
  gl_Position = mvp * vec4(aPos, 1.0);
}`;

const FS_DEFAULT = `#version 300 es
precision highp float;
in vec4 vColor;
in vec2 vTexCoord;
uniform sampler2D tex;
uniform float useTex;
out vec4 fragColor;
void main() {
  vec4 c = vColor;
  if (useTex > 0.5) c = texture(tex, vTexCoord);
  fragColor = c;
}`;

// CLEAR flags (unused in WebGL directly; for completeness)
const GL_COLOR_BUFFER_BIT = 0x4000;

export class WebGL2Renderer {
  private gl: GLLike;
  width = 0;
  height = 0;
  drawCalls = 0;

  private defaultProg: any = null;
  private progVS: WebGLShader | null = null;
  private progFS: WebGLShader | null = null;
  private customProg: any = null;
  private customVSsrc = '';
  private customFSsrc = '';

  private vao: any = null;
  private vbo: any = null;
  private vboData: Float32Array = new Float32Array(0);

  // texture registry (mirrors REN_MAX_TEX = 256)
  private texObj: (any | null)[] = new Array(256).fill(null);
  private texTarget: number[] = new Array(256).fill(0);
  private texW: number[] = new Array(256).fill(0);
  private texH: number[] = new Array(256).fill(0);
  private captures: Map<number, any> = new Map();

  // capture FBO
  private capFbo: any = null;

  // current GL state (mirrors renderer state)
  private depthTest = true;
  private blend = false;
  private blendSrc = 0x0302;
  private blendDst = 0x0303;
  private cullFace = 0;
  private activeUnit = 0;
  private boundTex = 0;
  private useTex = false;
  private pointSize = 1.0;
  private lineWidthPx = 1.0;

  // uniform location cache per program
  private uniCache: Map<any, Map<string, any>> = new Map();

  // pending vertex list for batching
  private pending: Float32Array | null = null;
  private pendingCount = 0;
  private pendingMode = 0;
  private pendingMVP: number[] | null = null;

  // MVP bake (disabled by default so custom shaders using gl_Vertex behave)
  mvpBake = false;

  constructor(gl: GLLike) {
    this.gl = gl;
    this.initDefaultProgram();
    this.initBuffers();
    this.initCaptureFbo();
  }

  private initDefaultProgram(): void {
    const gl = this.gl;
    const p = this.makeProgram(VS_DEFAULT, FS_DEFAULT);
    this.defaultProg = p;
  }

  private initBuffers(): void {
    const gl = this.gl;
    this.vao = (gl as any).createVertexArray ? (gl as any).createVertexArray() : {};
    this.vbo = gl.createBuffer();
  }

  private initCaptureFbo(): void {
    const gl = this.gl;
    if ((gl as any).createFramebuffer) this.capFbo = gl.createFramebuffer();
  }

  private makeProgram(vsSrc: string, fsSrc: string): any {
    const gl = this.gl;
    const vs = gl.createShader(gl.VERTEX_SHADER);
    gl.shaderSource(vs, vsSrc);
    gl.compileShader(vs);
    if (!gl.getShaderParameter(vs, gl.COMPILE_STATUS)) {
      throw new Error('vertex shader compile failed: ' + gl.getShaderInfoLog(vs));
    }
    const fs = gl.createShader(gl.FRAGMENT_SHADER);
    gl.shaderSource(fs, fsSrc);
    gl.compileShader(fs);
    if (!gl.getShaderParameter(fs, gl.COMPILE_STATUS)) {
      throw new Error('fragment shader compile failed: ' + gl.getShaderInfoLog(fs));
    }
    const prog = gl.createProgram();
    gl.attachShader(prog, vs);
    gl.attachShader(prog, fs);
    gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
      throw new Error('program link failed: ' + gl.getProgramInfoLog(prog));
    }
    gl.deleteShader(vs);
    gl.deleteShader(fs);
    this.uniCache.set(prog, new Map());
    return prog;
  }

  private getUni(prog: any, name: string): any {
    const cache = this.uniCache.get(prog)!;
    if (cache.has(name)) return cache.get(name);
    const loc = this.gl.getUniformLocation(prog, name);
    cache.set(name, loc);
    return loc;
  }

  setSize(w: number, h: number): void {
    this.width = w;
    this.height = h;
    this.gl.viewport(0, 0, w, h);
  }

  // --- shader management (mirrors set_shader) ---
  private ensureCustomProgram(vs: string, fs: string): any {
    if (this.customProg && this.customVSsrc === vs && this.customFSsrc === fs) {
      return this.customProg;
    }
    const gl = this.gl;
    if (this.customProg) gl.deleteProgram(this.customProg);
    const adaptedVS = VS_HEADER + adaptVertex(vs);
    const adaptedFS = FS_HEADER + adaptFragment(fs);
    const prog = this.makeProgram(adaptedVS, adaptedFS);
    this.customProg = prog;
    this.customVSsrc = vs;
    this.customFSsrc = fs;
    return prog;
  }

  // --- texture upload (mirrors GLCMD_SETTEXDATA) ---
  uploadTex(td: TextureData): void {
    const gl = this.gl;
    const { id, w, h, z, colmode, pixels } = td;
    if (id < 0 || id >= 256 || w < 1 || h < 1 || !pixels) return;
    if (!this.texObj[id]) this.texObj[id] = gl.createTexture();
    const cube = (w * 6 === h);
    const target = cube ? gl.TEXTURE_CUBE_MAP : gl.TEXTURE_2D;
    this.texTarget[id] = target;
    gl.activeTexture(gl.TEXTURE0 + this.activeUnit);
    gl.bindTexture(target, this.texObj[id]);
    // BGRA32 packed 0xAABBGGRR -> GL RGBA8 byte order
    const nf = cube ? 6 : 1;
    const fw = w, fh = cube ? h / 6 : h;
    const bytes = new Uint8Array((cube ? 6 : 1) * fw * fh * 4);
    for (let f = 0; f < nf; f++) {
      if (cube) {
        const row = (5 - f) * fh; // faces stored bottom-to-top
        for (let y = 0; y < fh; y++)
          for (let x = 0; x < fw; x++) {
            const src = (row + y) * w + x;
            const v = Math.round(pixels[src] ?? 0) >>> 0;
            const d = ((f * fh + y) * fw + x) * 4;
            bytes[d] = v & 255; bytes[d + 1] = (v >> 8) & 255; bytes[d + 2] = (v >> 16) & 255; bytes[d + 3] = (v >> 24) & 255;
          }
      } else {
        for (let p = 0; p < fw * fh; p++) {
          const v = Math.round(pixels[p] ?? 0) >>> 0;
          bytes[p * 4] = v & 255; bytes[p * 4 + 1] = (v >> 8) & 255; bytes[p * 4 + 2] = (v >> 16) & 255; bytes[p * 4 + 3] = (v >> 24) & 255;
        }
      }
    }
    if (cube) {
      const faces = [
        gl.TEXTURE_CUBE_MAP_POSITIVE_X, gl.TEXTURE_CUBE_MAP_NEGATIVE_X,
        gl.TEXTURE_CUBE_MAP_POSITIVE_Y, gl.TEXTURE_CUBE_MAP_NEGATIVE_Y,
        gl.TEXTURE_CUBE_MAP_POSITIVE_Z, gl.TEXTURE_CUBE_MAP_NEGATIVE_Z,
      ];
      for (let f = 0; f < 6; f++) {
        const data = bytes.subarray(f * fw * fh * 4, (f + 1) * fw * fh * 4);
        gl.texImage2D(faces[f], 0, gl.RGBA8, fw, fh, 0, gl.RGBA, gl.UNSIGNED_BYTE, data as any);
      }
    } else {
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, fw, fh, 0, gl.RGBA, gl.UNSIGNED_BYTE, bytes as any);
    }
    // filtering / wrap from colmode bits
    const filter = (colmode >> 4) & 0xF;
    const wrap = (colmode >> 8) & 0xF;
    let minf = gl.LINEAR, magf = gl.LINEAR;
    if (filter === 1) { minf = magf = gl.NEAREST; }
    else if (filter >= 2) { minf = gl.LINEAR_MIPMAP_LINEAR; magf = gl.LINEAR; }
    let wt = (wrap === 1) ? gl.MIRRORED_REPEAT : (wrap === 2 || wrap === 3) ? gl.CLAMP_TO_EDGE : gl.REPEAT;
    if (cube) wt = gl.CLAMP_TO_EDGE;
    gl.texParameteri(target, gl.TEXTURE_MIN_FILTER, minf);
    gl.texParameteri(target, gl.TEXTURE_MAG_FILTER, magf);
    gl.texParameteri(target, gl.TEXTURE_WRAP_S, wt);
    gl.texParameteri(target, gl.TEXTURE_WRAP_T, wt);
    if (filter >= 2) gl.generateMipmap(target);
    this.texW[id] = fw; this.texH[id] = fh;
  }

  // --- offscreen capture (mirrors g_capture / g_capture_end) ---
  private beginCapture(tex: number): void {
    const gl = this.gl;
    if (!this.capFbo || tex < 0 || tex >= 256) return;
    if (!this.texObj[tex]) this.texObj[tex] = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.texObj[tex]);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, this.width, this.height, 0, gl.RGBA, gl.UNSIGNED_BYTE, null as any);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.capFbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, this.texObj[tex], 0);
    gl.viewport(0, 0, this.width, this.height);
  }

  private endCapture(tex: number): void {
    const gl = this.gl;
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, this.width, this.height);
    if (tex >= 0 && tex < 256) this.captures.set(tex, this.texObj[tex]);
  }

  getCapture(tex: number): any {
    return this.captures.get(tex);
  }

  // --- batching ---
  private flush(): void {
    if (!this.pending || this.pendingCount === 0) return;
    const gl = this.gl;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    gl.bufferData(gl.ARRAY_BUFFER, this.pending!.subarray(0, this.pendingCount * FLOATS_PER_VERT), gl.DYNAMIC_DRAW);
    const prog = this.currentProg;
    gl.useProgram(prog);
    const stride = FLOATS_PER_VERT * 4;
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 3, gl.FLOAT, false, stride, 0);
    gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 3, gl.FLOAT, false, stride, 3 * 4);
    gl.enableVertexAttribArray(2); gl.vertexAttribPointer(2, 4, gl.FLOAT, false, stride, 6 * 4);
    gl.enableVertexAttribArray(3); gl.vertexAttribPointer(3, 2, gl.FLOAT, false, stride, 10 * 4);
    // MVP uniform
    const mvpLoc = this.getUni(prog, 'mvp');
    if (mvpLoc) gl.uniformMatrix4fv(mvpLoc, false, this.pendingMVP!);
    // draw
    gl.drawArrays(this.pendingMode, 0, this.pendingCount);
    this.drawCalls++;
    this.pendingCount = 0;
  }

  private currentProg: any = null;

  private pushVertex(v: any, mvp: number[], mode: number): void {
    if (this.pendingMode !== mode || !this.pendingMVP ||
        this.pendingMVP[0] !== mvp[0] || this.pendingMVP[12] !== mvp[12]) {
      this.flush();
      this.pendingMode = mode;
      this.pendingMVP = mvp.slice();
    }
    if (!this.pending || this.pendingCount * FLOATS_PER_VERT + FLOATS_PER_VERT > this.pending.length) {
      const bigger = new Float32Array((this.pendingCount + 4096) * FLOATS_PER_VERT + FLOATS_PER_VERT * 4096);
      if (this.pending) bigger.set(this.pending.subarray(0, this.pendingCount * FLOATS_PER_VERT));
      this.pending = bigger;
    }
    const o = this.pendingCount * FLOATS_PER_VERT;
    this.pending[o] = v.x; this.pending[o + 1] = v.y; this.pending[o + 2] = v.z;
    this.pending[o + 3] = v.nx ?? 0; this.pending[o + 4] = v.ny ?? 0; this.pending[o + 5] = v.nz ?? 0;
    this.pending[o + 6] = v.r ?? 1; this.pending[o + 7] = v.g ?? 1; this.pending[o + 8] = v.b ?? 1; this.pending[o + 9] = v.a ?? 1;
    this.pending[o + 10] = v.s ?? 0; this.pending[o + 11] = v.t ?? 0;
    this.pendingCount++;
  }

  // --- apply a batch ---
  private drawBatch(b: any): void {
    const gl = this.gl;
    // program selection
    let prog = this.defaultProg;
    if (b.shaderV && b.shaderF) prog = this.ensureCustomProgram(b.shaderV, b.shaderF);
    this.currentProg = prog;
    // uniforms from batch
    if (b.uniforms) {
      gl.useProgram(prog);
      for (const u of b.uniforms) {
        const loc = this.getUni(prog, u.loc);
        if (!loc) continue;
        const v = u.v;
        if (u.kind === 1) { // int
          if (v.length >= 4) gl.uniform1i(loc, v[0]); // simplified
          else gl.uniform1i(loc, v[0]);
        } else {
          if (v.length === 1) gl.uniform1f(loc, v[0]);
          else if (v.length === 2) (gl as any).uniform2f?.(loc, v[0], v[1]);
          else if (v.length === 3) (gl as any).uniform3f?.(loc, v[0], v[1], v[2]);
          else (gl as any).uniform4f?.(loc, v[0], v[1], v[2], v[3]);
        }
      }
    }
    // texture binding
    this.useTex = !!b.useTex;
    if (this.useTex && b.tex >= 0 && this.texObj[b.tex]) {
      gl.activeTexture(gl.TEXTURE0 + b.texUnit);
      gl.bindTexture(gl.TEXTURE_2D, this.texObj[b.tex]);
      const ut = this.getUni(prog, 'tex');
      if (ut) gl.uniform1i(ut, b.texUnit);
      const utx = this.getUni(prog, 'useTex');
      if (utx) gl.uniform1f(utx, 1.0);
    } else {
      const utx = this.getUni(prog, 'useTex');
      if (utx) gl.uniform1f(utx, 0.0);
    }
    // state
    this.applyState(b);
    // build MVP
    const mvp = mul4(b.projection, b.modelview);
    // push transformed (object-space) vertices; shader does MVP
    const mode = this.glMode(b.mode);
    for (const vv of b.verts) this.pushVertex(vv, mvp, mode);
    this.flush();
  }

  private glMode(pdMode: number): number {
    const gl = this.gl;
    switch (pdMode) {
      case 0x0000: return gl.POINTS;
      case 0x0001: return gl.LINES;
      case 0x0003: return gl.LINE_STRIP;
      case 0x0004: return gl.TRIANGLES;
      case 0x0005: return gl.TRIANGLE_STRIP;
      case 0x0006: return gl.TRIANGLE_FAN;
      default: return gl.TRIANGLES;
    }
  }

  private applyState(b: any): void {
    const gl = this.gl;
    // backend default (this.depthTest=true) unless the batch explicitly disables.
    const dt = b.depthTest || this.depthTest;
    if (dt) gl.enable(gl.DEPTH_TEST); else gl.disable(gl.DEPTH_TEST);
    const bl = b.blend || this.blend;
    if (bl) { gl.enable(gl.BLEND); gl.blendFunc(b.blendSrc, b.blendDst); } else gl.disable(gl.BLEND);
    if (b.cullFace) { gl.enable(gl.CULL_FACE); gl.cullFace(b.cullFace); } else gl.disable(gl.CULL_FACE);
    if ((gl as any).lineWidth) (gl as any).lineWidth(Math.max(1, b.lineWidth));
  }

  // --- main entry: render a frame (mirrors glcmd_dispatch loop) ---
  render(scene: SceneFrame): void {
    const gl = this.gl;
    const captures = scene.captures || [];
    gl.clearColor(0, 0, 0, 1);
    gl.clear(GL_COLOR_BUFFER_BIT);
    gl.enable(gl.DEPTH_TEST);
    let capturing = false;
    let capTex = -1;
    for (let i = 0; i < scene.batches.length; i++) {
      const b = scene.batches[i];
      this.drawBatch(b);
      // capture ops are inserted after the batch that precedes them
      const cap = captures.find((c) => c.afterIndex === i);
      if (cap) {
        if (!capturing) { this.beginCapture(cap.tex); capturing = true; capTex = cap.tex; }
        else { this.endCapture(capTex); this.beginCapture(cap.tex); capTex = cap.tex; }
      }
    }
    if (capturing) this.endCapture(capTex);
    this.drawCalls = this.drawCalls; // keep counter
  }

  // alias for tests
  draw(scene: SceneFrame): void { this.render(scene); }

  // --- read back the default framebuffer as RGBA8 (mirrors readRGBA8) ---
  readRGBA8(): Uint8Array {
    const gl = this.gl;
    const buf = new Uint8Array(this.width * this.height * 4);
    gl.readPixels(0, 0, this.width, this.height, gl.RGBA, gl.UNSIGNED_BYTE, buf as any);
    // flip vertically (GL origin bottom-left)
    const out = new Uint8Array(this.width * this.height * 4);
    const w = this.width, h = this.height;
    for (let y = 0; y < h; y++) {
      out.set(buf.subarray(y * w * 4, (y + 1) * w * 4), (h - 1 - y) * w * 4);
    }
    return out;
  }
}

// column-major 4x4 multiply: out = a * b
function mul4(a: number[] | Float64Array, b: number[] | Float64Array): number[] {
  const o = new Array(16).fill(0);
  for (let c = 0; c < 4; c++) {
    for (let r = 0; r < 4; r++) {
      let s = 0;
      for (let k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
      o[c * 4 + r] = s;
    }
  }
  return o;
}
