// softrender.ts — dependency-free software rasterizer for the JS GPU pipeline
// (M7). Consumes DrawBatch[] from fixedfunc.ts and produces an RGB image by
// executing a per-pixel fragment function in JS. Enables "Node 出图" without a
// GL context, and lets simple scripts (e.g. balls.pss, whose fragment shader is
// 3 lines) be pixel-checked against the C offscreen reference.
import type { DrawBatch, Vertex } from './fixedfunc.ts';
import { PDGL } from '../host/glcmd.ts';
import { compileGLSL, compileVertexGLSL, parseVaryingMap, resetFragDepth, lastFragDepth } from './glsl.ts';
import type { GLSLProgram, GLSLVaryings, GLSLVaryRecord, GLSLVertexRunner, GLSLTexFn } from './glsl.ts';

// Fragment function contract: given interpolated varyings at a pixel, return
// the output RGB in [0,1] or null to discard.
export interface Varyings {
  r: number; g: number; b: number; a: number;
  s: number; t: number; p: number; // p = 3rd texcoord (3D texture R coord)
  nx: number; ny: number; nz: number;
  // object-space position (gl_Vertex): interpolated for shaders that sample p
  ox: number; oy: number; oz: number; ow: number;
  // NDC position (gl_Position): interpolated for `p = gl_Position` shaders
  ndx: number; ndy: number; ndz: number; ndw: number;
}
export type FragmentFn = (v: Varyings) => [number, number, number] | null;

// Compiled GLSL fragment program + its varying map, resolved once per batch.
interface GLSLBatchEntry { prog: GLSLProgram; vmap: Map<string, string> }

// Balls.pss fragment shader, transcribed faithfully:
//   d = length(t.xy); if (d>1.0) discard; gl_FragColor = (1.0-d*.25)*c;
export function ballsFragment(v: Varyings): [number, number, number] | null {
  const d = Math.hypot(v.s, v.t);
  if (d > 1.0) return null;
  const f = 1.0 - d * 0.25;
  return [v.r * f, v.g * f, v.b * f];
}

export interface RasterizeOptions {
  width: number;
  height: number;
  fragment: FragmentFn;
}

export interface BatchTraceInfo {
  index: number;
  mode: number;
  nverts: number;
  mvp: number[];
  verts: { ox: number; oy: number; oz: number; ow: number; cx: number; cy: number; cz: number; cw: number; nx: number; ny: number; nz: number; r: number; g: number; b: number; a: number; s: number; t: number; nrmx: number; nrmy: number; nrmz: number }[];
  shaderF: string | null;
  shaderV: string | null;
  fbHash: string;
  fbMean: number;
  fbNonBlankPct: number;
  // source GLCmd range anchor (from DrawBatch) for cross-renderer alignment
  cmdStart: number;
  cmdEnd: number;
  // framebuffer snapshot AFTER this drawcall: w*h*3 floats in [0,1] (copied).
  // Element-level pixel-diff material for the C/JS differential.
  fbData?: Float32Array;
  fbW?: number;
  fbH?: number;
}

// GLSL debug stats (only counted when PD_DEBUG_GLSL is set)
let g_shaderCalls = 0, g_shaderDiscards = 0;
export function glslStats(): { calls: number; discards: number } {
  return { calls: g_shaderCalls, discards: g_shaderDiscards };
}

let g_smpN = 0;

export class SoftRenderer {
  private img: Float32Array; // w*h*3 in [0,1] — main framebuffer
  private depth: Float32Array; // w*h, NDC z in [-1,1]; -Inf = empty
  private opt: RasterizeOptions;
  // Software backend default: depth test OFF, matching the pyref software golden
  // (painter-order). Set true (or rely on a batch that enables it) for 3D scenes
  // that need Z occlusion — the WebGL2 backend keeps depth ON to mirror C GL.
  depthTest = false;
  // captured/offscreen textures: texId -> RGBA float data (w*h*3) for sampling.
  // `filter`: 0=nearest,1=linear,2+=linear-mipmap-linear. `wrap`: 0=repeat,
  // 1=mirrored,2/3=clamp-to-edge (mirrors C glTexParameteri). `mips` holds
  // pre-filtered half-resolution levels (box filter, matching glGenerateMipmap)
  // for LINEAR_MIPMAP_LINEAR sampling on minified surfaces.
  private tex: Map<number, { w: number; h: number; d: number; data: Float32Array; filter: number; wrap: number; cube?: boolean; faces?: Float32Array[]; mips?: { w: number; h: number; data: Float32Array }[] }> = new Map();
  // Active render target. When capturing (glcapture..glcaptureend), geometry is
  // drawn to capBuf instead of img, mirroring C's FBO render-target switch.
  private target: Float32Array = new Float32Array(0);
  private capBuf: Float32Array | null = null;
  // Per-triangle texture LOD (mip level) for LINEAR_MIPMAP_LINEAR sampling.
  // Set by rasterTri from the screen-space gradient of the `t` (texcoord)
  // varying; read by the tex closure. -1 = no mip (use base level).
  private curTexLod = -1;
  // Per-pixel finite-difference texture-LOD capture state (see rasterTri).
  // When set, every texFn call records (texId, u, v) into texCap.coords in call
  // order; in 'final' mode the implicit LOD is replaced with the per-call value
  // derived from the finite differences (texCap.lods, indexed by call order).
  private texCap: { coords: number[][]; lods: number[]; call: number } | null = null;
  private texMode: 'capture' | 'final' | null = null;
  // GLSL fragment-shader cache: (shaderF, shaderV) -> program + varying map
  private glslCache = new Map<string, GLSLBatchEntry | null>();
  // GLSL vertex-shader cache: shaderV -> runner (null = no/failed VS)
  private vshCache = new Map<string, GLSLVertexRunner | null>();
  // scratch objects reused across pixels (filled then consumed synchronously)
  private gvScratch: GLSLVaryings = { r: 0, g: 0, b: 0, a: 0, s: 0, t: 0, p: 0, q: 1, nx: 0, ny: 0, nz: 0, px: 0, py: 0, pz: 0, pw: 0, ndx: 0, ndy: 0, ndz: 0, ndw: 0, fcx: 0, fcy: 0, fcz: 0, fcw: 1 };

  constructor(opt: RasterizeOptions) {
    this.opt = opt;
    this.img = new Float32Array(opt.width * opt.height * 3);
    this.depth = new Float32Array(opt.width * opt.height).fill(Infinity);
    this.target = this.img;
  }

  getImage(): Float32Array { return this.img; }

  // Returns the active render target (main FB during normal draw, capBuf
  // during capture). Used by the trace hook to snapshot the correct buffer.
  currentTarget(): Float32Array { return this.target; }

  // Write a fragment to pixel (xx,yy) with depth `z` in NDC [-1,1] into the
  // active render target. Honors the batch's depth test and blend state.
  private writePixel(xx: number, yy: number, z: number, rgba: number[], b: DrawBatch): void {
    const { width, height } = this.opt;
    if (xx < 0 || yy < 0 || xx >= width || yy >= height) return;
    const idx = yy * width + xx;
    const dt = b.depthTest || this.depthTest;
    if (dt) {
      if (z > this.depth[idx]) return; // GL default LESS vs depth buffer
      this.depth[idx] = z;
    }
    const o = idx * 3;
    const alpha = rgba[3];
    if (b.blend && alpha < 1) {
      // src*srcA + dst*(1-srcA)
      const ia = 1 - alpha;
      this.target[o] = rgba[0] * alpha + this.target[o] * ia;
      this.target[o + 1] = rgba[1] * alpha + this.target[o + 1] * ia;
      this.target[o + 2] = rgba[2] * alpha + this.target[o + 2] * ia;
    } else {
      this.target[o] = rgba[0]; this.target[o + 1] = rgba[1]; this.target[o + 2] = rgba[2];
    }
  }

  private project(p: Float64Array, v: Vertex): [number, number, number] {
    // MVP = projection * modelview (column-major)
    const x = v.x, y = v.y, z = v.z, w = v.w;
    const ex = p[0] * x + p[4] * y + p[8] * z + p[12] * w;
    const ey = p[1] * x + p[5] * y + p[9] * z + p[13] * w;
    const ez = p[2] * x + p[6] * y + p[10] * z + p[14] * w;
    const ew = p[3] * x + p[7] * y + p[11] * z + p[15] * w;
    if (Math.abs(ew) < 1e-12) return [0, 0, -1];
    return [ex / ew, ey / ew, ez / ew];
  }

  private screenX(nx: number): number { return (nx * 0.5 + 0.5) * this.opt.width; }
  // Top-down image row 0 = NDC y=+1 (matches the C offscreen PNG, whose rows
  // are flipped from GL bottom-left to top-left origin).
  private screenY(ny: number): number { return (0.5 - ny * 0.5) * this.opt.height; }

  // Sample texture `texId` at (s,t). When lod >= 0 and the texture has mipmaps,
  // performs trilinear filtering (bilinear within two adjacent mip levels +
  // linear blend across). `lod` is the base-2 log of the minification factor;
  // 0 = base level, 1 = half-res, etc. lod < 0 = use filter mode directly (no mip).
  private sampleTex(texId: number, s: number, t: number, lod = -1, wCoord?: number): [number, number, number, number] | null {
    const T = this.tex.get(texId);
    if (!T) return null;
    if (process.env.PD_DEBUG_SAMPLE && ((s * 640 + t * 480) % 7919 < 2)) console.error(`[smp] id=${texId} s=${s.toFixed(4)} t=${t.toFixed(4)} lod=${lod.toFixed(3)} filter=${T.filter} mips=${T.mips?.length ?? 0}`);
    if (process.env.PD_DEBUG_SAMPLE && (g_smpN++ % 500) === 0) console.error(`[smp] id=${texId} s=${s.toFixed(4)} t=${t.toFixed(4)} lod=${lod.toFixed(3)} filter=${T.filter} mips=${T.mips?.length ?? 0}`);
    // 3D (volumetric) texture: trilinear over slice[z][y][x]
    if (T.d > 1 && wCoord !== undefined) return this.sampleTex3D(T, s, t, wCoord);
    // Cube map: the coords are a 3D direction (s,t,r) selecting a face
    if (T.cube && wCoord !== undefined) return this.sampleCube(T, s, t, wCoord);
    if (lod < 0 || !T.mips || T.mips.length === 0 || lod < 1e-6) {
      return this.sampleTexLevel(T, s, t);
    }
    // Trilinear: blend between floor(lod) and ceil(lod) mip levels.
    const l0 = Math.min(T.mips.length, Math.floor(lod));
    const l1 = Math.min(T.mips.length, Math.ceil(lod));
    const f = lod - Math.floor(lod);
    if (l0 === l1) {
      // base level (l0=0 -> original; l0>0 -> mip[l0-1])
      const lv = l0 === 0 ? T : T.mips[l0 - 1];
      return this.sampleTexLevel({ ...T, w: lv.w, h: lv.h, data: lv.data, filter: 1 }, s, t);
    }
    const lv0 = l0 === 0 ? T : T.mips[l0 - 1];
    const lv1 = l1 === 0 ? T : T.mips[l1 - 1];
    const c0 = this.sampleTexLevel({ ...T, w: lv0.w, h: lv0.h, data: lv0.data, filter: 1 }, s, t);
    const c1 = this.sampleTexLevel({ ...T, w: lv1.w, h: lv1.h, data: lv1.data, filter: 1 }, s, t);
    return [c0[0] + (c1[0] - c0[0]) * f, c0[1] + (c1[1] - c0[1]) * f, c0[2] + (c1[2] - c0[2]) * f, c0[3] + (c1[3] - c0[3]) * f];
  }

  // 3D (volumetric) texture sampling: data is laid out slice[z][y][x] with
  // w*h*3 floats per slice — the same order the script filled the buffer
  // (iz outermost). Internal filter convention: 0=NEAREST, 1=LINEAR
  // (trilinear 8-tap over the volume); 3D textures never carry mips.
  private sampleTex3D(T: { w: number; h: number; d: number; data: Float32Array; filter: number; wrap: number }, s: number, t: number, r: number): [number, number, number, number] {
    const { w, h, d, data, filter, wrap } = T;
    let u: number, v: number, q: number;
    if (wrap === 2 || wrap === 3) {
      u = s < 0 ? 0 : s > 1 ? 1 : s;
      v = t < 0 ? 0 : t > 1 ? 1 : t;
      q = r < 0 ? 0 : r > 1 ? 1 : r;
    } else if (wrap === 1) {
      u = s - Math.floor(s); if ((Math.floor(s) & 1) !== 0) u = 1 - u;
      v = t - Math.floor(t); if ((Math.floor(t) & 1) !== 0) v = 1 - v;
      q = r - Math.floor(r); if ((Math.floor(r) & 1) !== 0) q = 1 - q;
    } else {
      u = s - Math.floor(s);
      v = t - Math.floor(t);
      q = r - Math.floor(r);
    }
    if (filter === 0) {
      const px = Math.min(w - 1, Math.max(0, Math.floor(u * w)));
      const py = Math.min(h - 1, Math.max(0, Math.floor(v * h)));
      const pz = Math.min(d - 1, Math.max(0, Math.floor(q * d)));
      const o = ((pz * h + py) * w + px) * 4;
      return [data[o], data[o + 1], data[o + 2], data[o + 3]];
    }
    // GL_LINEAR on TEXTURE_3D = trilinear: bilinear in x/y on the two
    // bracketing z slices, then linear across them. Hot path: no closures,
    // all 8 taps indexed directly. Stride is 4 (RGBA — alpha drives blend).
    const sx = u * w - 0.5, sy = v * h - 0.5, sz = q * d - 0.5;
    let x0 = Math.floor(sx), y0 = Math.floor(sy), z0 = Math.floor(sz);
    const fx = sx - x0, fy = sy - y0, fz = sz - z0;
    let x1 = x0 + 1, y1 = y0 + 1, z1 = z0 + 1;
    if (wrap === 2 || wrap === 3) {
      x0 = x0 < 0 ? 0 : x0 > w - 1 ? w - 1 : x0;
      x1 = x1 < 0 ? 0 : x1 > w - 1 ? w - 1 : x1;
      y0 = y0 < 0 ? 0 : y0 > h - 1 ? h - 1 : y0;
      y1 = y1 < 0 ? 0 : y1 > h - 1 ? h - 1 : y1;
      z0 = z0 < 0 ? 0 : z0 > d - 1 ? d - 1 : z0;
      z1 = z1 < 0 ? 0 : z1 > d - 1 ? d - 1 : z1;
    } else if (wrap === 1) {
      x0 = ((x0 % (2 * w)) + 2 * w) % (2 * w); if (x0 >= w) x0 = 2 * w - 1 - x0;
      x1 = ((x1 % (2 * w)) + 2 * w) % (2 * w); if (x1 >= w) x1 = 2 * w - 1 - x1;
      y0 = ((y0 % (2 * h)) + 2 * h) % (2 * h); if (y0 >= h) y0 = 2 * h - 1 - y0;
      y1 = ((y1 % (2 * h)) + 2 * h) % (2 * h); if (y1 >= h) y1 = 2 * h - 1 - y1;
      z0 = ((z0 % (2 * d)) + 2 * d) % (2 * d); if (z0 >= d) z0 = 2 * d - 1 - z0;
      z1 = ((z1 % (2 * d)) + 2 * d) % (2 * d); if (z1 >= d) z1 = 2 * d - 1 - z1;
    } else {
      x0 = ((x0 % w) + w) % w; x1 = ((x1 % w) + w) % w;
      y0 = ((y0 % h) + h) % h; y1 = ((y1 % h) + h) % h;
      z0 = ((z0 % d) + d) % d; z1 = ((z1 % d) + d) % d;
    }
    const p00 = ((z0 * h + y0) * w + x0) * 4, p10 = ((z0 * h + y0) * w + x1) * 4;
    const p01 = ((z0 * h + y1) * w + x0) * 4, p11 = ((z0 * h + y1) * w + x1) * 4;
    const q00 = ((z1 * h + y0) * w + x0) * 4, q10 = ((z1 * h + y0) * w + x1) * 4;
    const q01 = ((z1 * h + y1) * w + x0) * 4, q11 = ((z1 * h + y1) * w + x1) * 4;
    const c0 = data[p00] + (data[p10] - data[p00]) * fx;
    const c1 = data[p01] + (data[p11] - data[p01]) * fx;
    const d0 = data[q00] + (data[q10] - data[q00]) * fx;
    const d1 = data[q01] + (data[q11] - data[q01]) * fx;
    const e0 = c0 + (c1 - c0) * fy;
    const e1 = d0 + (d1 - d0) * fy;
    const rv = e0 + (e1 - e0) * fz;
    const c0g = data[p00 + 1] + (data[p10 + 1] - data[p00 + 1]) * fx;
    const c1g = data[p01 + 1] + (data[p11 + 1] - data[p01 + 1]) * fx;
    const d0g = data[q00 + 1] + (data[q10 + 1] - data[q00 + 1]) * fx;
    const d1g = data[q01 + 1] + (data[q11 + 1] - data[q01 + 1]) * fx;
    const e0g = c0g + (c1g - c0g) * fy;
    const e1g = d0g + (d1g - d0g) * fy;
    const gv = e0g + (e1g - e0g) * fz;
    const c0b = data[p00 + 2] + (data[p10 + 2] - data[p00 + 2]) * fx;
    const c1b = data[p01 + 2] + (data[p11 + 2] - data[p01 + 2]) * fx;
    const d0b = data[q00 + 2] + (data[q10 + 2] - data[q00 + 2]) * fx;
    const d1b = data[q01 + 2] + (data[q11 + 2] - data[q01 + 2]) * fx;
    const e0b = c0b + (c1b - c0b) * fy;
    const e1b = d0b + (d1b - d0b) * fy;
    const bv = e0b + (e1b - e0b) * fz;
    const c0a = data[p00 + 3] + (data[p10 + 3] - data[p00 + 3]) * fx;
    const c1a = data[p01 + 3] + (data[p11 + 3] - data[p01 + 3]) * fx;
    const d0a = data[q00 + 3] + (data[q10 + 3] - data[q00 + 3]) * fx;
    const d1a = data[q01 + 3] + (data[q11 + 3] - data[q01 + 3]) * fx;
    const e0a = c0a + (c1a - c0a) * fy;
    const e1a = d0a + (d1a - d0a) * fy;
    const av = e0a + (e1a - e0a) * fz;
    return [rv, gv, bv, av];
  }

  // Cube-map sampling: a direction (x,y,z) selects a face by dominant axis,
  // then maps to (s,t) UV per the OpenGL cubemap table (spec 3.9.x):
  //   major axis  face           sc   tc   ma
  //   +x  POSITIVE_X (0)  -rz  -ry  rx
  //   -x  NEGATIVE_X (1)  +rz  -ry  rx
  //   +y  POSITIVE_Y (2)  +rx  +rz  ry
  //   -y  NEGATIVE_Y (3)  +rx  -rz  ry
  //   +z  POSITIVE_Z (4)  +rx  -ry  rz
  //   -z  NEGATIVE_Z (5)  -rx  -ry  rz
  //   s = (sc/|ma| + 1)/2, t = (tc/|ma| + 1)/2.
  // Cube faces always use CLAMP_TO_EDGE (bilinear within the face).
  private sampleCube(T: { w: number; h: number; filter: number; faces?: Float32Array[] }, x: number, y: number, z: number): [number, number, number, number] {
    const ax = Math.abs(x), ay = Math.abs(y), az = Math.abs(z);
    let fi: number, sc: number, tc: number, ma: number;
    if (ax >= ay && ax >= az) { fi = x >= 0 ? 0 : 1; ma = ax; sc = x >= 0 ? -z : z; tc = -y; }
    else if (ay >= az) { fi = y >= 0 ? 2 : 3; ma = ay; sc = x; tc = y >= 0 ? z : -z; }
    else { fi = z >= 0 ? 4 : 5; ma = az; sc = z >= 0 ? x : -x; tc = -y; }
    if (ma < 1e-12) return [0, 0, 0, 1];
    const s = 0.5 * (sc / ma + 1);
    const t = 0.5 * (tc / ma + 1);
    const face = T.faces?.[fi];
    if (!face) return [0, 0, 0, 1];
    return this.sampleTexLevel({ w: T.w, h: T.h, data: face, filter: T.filter, wrap: 2 }, s, t);
  }

  private sampleTexLevel(T: { w: number; h: number; data: Float32Array; filter: number; wrap: number }, s: number, t: number): [number, number, number, number] {
    const { w, h, data, filter, wrap } = T;
    const wrapCoord = (c: number) => {
      if (wrap === 2 || wrap === 3) return Math.min(1, Math.max(0, c));
      if (wrap === 1) { const i = Math.floor(c); const f = c - i; return (i & 1) ? 1 - f : f; }
      return c - Math.floor(c);
    };
    const u = wrapCoord(s);
    const v = wrapCoord(t);
    if (filter === 0) {
      const px = Math.min(w - 1, Math.max(0, Math.floor(u * w)));
      const py = Math.min(h - 1, Math.max(0, Math.floor(v * h)));
      const o = (py * w + px) * 4;
      return [data[o], data[o + 1], data[o + 2], data[o + 3]];
    }
    const sx = u * w - 0.5;
    const sy = v * h - 0.5;
    const x0 = Math.floor(sx), y0 = Math.floor(sy);
    const fx = sx - x0, fy = sy - y0;
    const idx = (xi: number, yi: number) => {
      let x = xi, y = yi;
      if (wrap === 2 || wrap === 3) { x = Math.min(w - 1, Math.max(0, x)); y = Math.min(h - 1, Math.max(0, y)); }
      else if (wrap === 1) { x = ((x % (2 * w)) + 2 * w) % (2 * w); if (x >= w) x = 2 * w - 1 - x; y = ((y % (2 * h)) + 2 * h) % (2 * h); if (y >= h) y = 2 * h - 1 - y; }
      else { x = ((x % w) + w) % w; y = ((y % h) + h) % h; }
      return (y * w + x) * 4;
    };
    const i00 = idx(x0, y0), i10 = idx(x0 + 1, y0), i01 = idx(x0, y0 + 1), i11 = idx(x0 + 1, y0 + 1);
    const lerp = (a: number, b: number, f: number) => a + (b - a) * f;
    const r = lerp(lerp(data[i00], data[i10], fx), lerp(data[i01], data[i11], fx), fy);
    const g = lerp(lerp(data[i00 + 1], data[i10 + 1], fx), lerp(data[i01 + 1], data[i11 + 1], fx), fy);
    const b = lerp(lerp(data[i00 + 2], data[i10 + 2], fx), lerp(data[i01 + 2], data[i11 + 2], fx), fy);
    const a = lerp(lerp(data[i00 + 3], data[i10 + 3], fx), lerp(data[i01 + 3], data[i11 + 3], fx), fy);
    return [r, g, b, a];
  }

  // Run the batch's vertex shader per vertex (mirrors C adapt_vertex + the
  // GPU vertex stage). Returns clip-space positions and per-vertex varyings.
  // When the batch has no vertex shader (or it fails to compile), positions
  // come from the fixed-function MVP and varyings are synthesized from the
  // fixed-function attributes (c/t/p/n) — matching C's default program.
  private vertexStage(b: DrawBatch, mvp: Float64Array, mv: Float64Array, pr: Float64Array, vs: Vertex[]): { clip: number[][]; vary: GLSLVaryRecord[]; hasVS: boolean } {
    let vsh: GLSLVertexRunner | null = null;
    // C's mvp_bake (gl_renderer.c): when the vertex shader source never
    // references gl_Vertex, the MVP is folded into each vertex position on the
    // CPU (baking it to NDC with w=1) and the shader receives an identity
    // matrix. Every baked vertex then has gl_Position.w=1, so varying
    // interpolation becomes AFFINE in screen space (no perspective correction).
    // Mirror that exactly so UVs/colors match the C reference.
    const mvpBake = !!(b.shaderV && b.shaderV.indexOf('gl_Vertex') < 0);
    // C gl_renderer.c adapt_vertex leaves gl_FrontColor/gl_TexCoord (GLSL 1.x
    // builtins) unreplaced, so a vertex shader using them fails to compile and
    // C renders with the default passthrough program (u_mvp * a_vertex +
    // vertex color). Skip the script's vertex shader here too so geometry and
    // colors match C (geo_test.pss).
    const legacyVS = !!(b.shaderV && (b.shaderV.indexOf('gl_FrontColor') >= 0 || b.shaderV.indexOf('gl_TexCoord') >= 0));
    if (b.shaderV && !legacyVS) {
      if (!this.vshCache.has(b.shaderV)) this.vshCache.set(b.shaderV, compileVertexGLSL(b.shaderV));
      vsh = this.vshCache.get(b.shaderV) ?? null;
    }
    const clip: number[][] = [];
    const vary: GLSLVaryRecord[] = [];
    if (vsh) {
      // normal matrix: upper-left 3x3 of the modelview
      const nrm = [mv[0], mv[1], mv[2], mv[4], mv[5], mv[6], mv[8], mv[9], mv[10]];
      const identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
      for (const v of vs) {
        let vertex = [v.x, v.y, v.z, v.w];
        let mvpAttr = Array.from(mvp);
        if (mvpBake) {
          // C bakes against (x,y,z,1) — GVertex is xyz-only, w implicit 1
          const x = v.x, y = v.y, z = v.z;
          const o0 = mvp[0] * x + mvp[4] * y + mvp[8] * z + mvp[12];
          const o1 = mvp[1] * x + mvp[5] * y + mvp[9] * z + mvp[13];
          const o2 = mvp[2] * x + mvp[6] * y + mvp[10] * z + mvp[14];
          const o3 = mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15];
          vertex = o3 !== 0 && o3 !== 1 ? [o0 / o3, o1 / o3, o2 / o3, 1] : [o0, o1, o2, 1];
          mvpAttr = identity;
        }
        const r = vsh({
          vertex,
          color: [v.r, v.g, v.b, v.a],
          texcoord: [v.s, v.t, v.p, v.q],
          normal: [v.nx, v.ny, v.nz],
          mvp: mvpAttr, modelview: Array.from(mv), projection: Array.from(pr), normalMat: nrm,
        }, b.uniforms as never);
        if (!r) {
          if (mvpBake) {
            const x = v.x, y = v.y, z = v.z;
            const o0 = mvp[0] * x + mvp[4] * y + mvp[8] * z + mvp[12];
            const o1 = mvp[1] * x + mvp[5] * y + mvp[9] * z + mvp[13];
            const o2 = mvp[2] * x + mvp[6] * y + mvp[10] * z + mvp[14];
            const o3 = mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15];
            clip.push(o3 !== 0 && o3 !== 1 ? [o0 / o3, o1 / o3, o2 / o3, 1] : [o0, o1, o2, 1]);
          } else {
            clip.push(this.project(mvp, v).concat(1));
          }
          vary.push({});
          continue;
        }
        clip.push(r.pos.length >= 4 ? r.pos.slice(0, 4) : r.pos.concat([0, 0, 0, 1]).slice(0, 4));
        vary.push(r.vary);
      }
      return { clip, vary, hasVS: true };
    }
    // No vertex shader: fixed-function path with default varyings (C's
    // passthrough program outputs only c = a_color; t/p/n are exposed too so
    // fragment shaders written against the fixed pipeline keep working).
    for (const v of vs) {
      const p = this.project(mvp, v);
      clip.push([p[0], p[1], p[2], 1]);
      vary.push({
        c: [v.r, v.g, v.b, v.a],
        t: [v.s, v.t, v.p, v.q],
        p: [v.x, v.y, v.z, v.w],
        n: [v.nx, v.ny, v.nz],
      });
    }
    return { clip, vary, hasVS: false };
  }

  // Resolve the output color for a fragment. Mirrors the C renderer's default
  // program, which is color-only (fragColor = c) even when a texture is bound:
  // texture sampling only happens when the script installs a fragment shader
  // via glsetshader (b.shaderF non-null). Scripts that bind a texture but
  // never install a shader (e.g. 26_texture_procedural.pss) therefore render
  // as vertex color in the C baseline — the same here.
  // `vr` carries the interpolated vertex-stage varyings (by declared name)
  // when a script vertex shader ran; null uses the fixed semantic bindings.
  private resolveColor(b: DrawBatch, vv: Varyings, vr: GLSLVaryRecord | null, entry: GLSLBatchEntry | null, tex: GLSLTexFn | null, fc?: { x: number; y: number; z: number; w: number }): [number, number, number, number] | null {
    resetFragDepth(); // shader-written gl_FragDepth (if any) applies per pixel
    if (entry) {
      // Execute the script's actual fragment shader through the GLSL subset
      // interpreter; texture2D samples the batch's bound texture.
      const gv = this.gvScratch;
      gv.r = vv.r; gv.g = vv.g; gv.b = vv.b; gv.a = vv.a;
      gv.s = vv.s; gv.t = vv.t; gv.p = vv.p; gv.q = 1;
      gv.nx = vv.nx; gv.ny = vv.ny; gv.nz = vv.nz;
      gv.px = vv.ox; gv.py = vv.oy; gv.pz = vv.oz; gv.pw = vv.ow;
      gv.ndx = vv.ndx; gv.ndy = vv.ndy; gv.ndz = vv.ndz; gv.ndw = vv.ndw;
      if (fc) { gv.fcx = fc.x; gv.fcy = fc.y; gv.fcz = fc.z; gv.fcw = fc.w; }
      try {
        const rc = entry.prog.run(gv, tex, entry.vmap, b.uniforms as never, vr ?? undefined);
        if (process.env.PD_DEBUG_GLSL) {
          g_shaderCalls++;
          if (rc === null) g_shaderDiscards++;
          if (process.env.PD_DEBUG_PIX && b.shaderF!.length > 100 && (g_shaderCalls & 8191) === 0)
            console.error(`[pix] rc=${JSON.stringify(rc)} v=${vr && vr['v'] ? JSON.stringify(vr['v']) : '?'}`);
        }
        return rc;
      } catch (e) {
        if (process.env.PD_DEBUG_GLSL) console.error('[soft] glsl run error:', (e as Error).message);
        return null;
      }
    }
    if (!entry) {
      // unsupported GLSL -> fall through to passthrough color (alpha from vertex)
      const fc = this.opt.fragment(vv) ?? [0, 0, 0];
      return [fc[0], fc[1], fc[2], vv.a];
    }
    return null;
  }

  // Resolve the compiled GLSL program + varying map once per draw batch.
  // The cache key is the (shaderV, shaderF) pair — per-pixel lookup here
  // (string concat + Map.get) was a major hot-path cost.
  private glslEntryFor(b: DrawBatch): GLSLBatchEntry | null {
    if (!b.shaderF) return null;
    const key = b.shaderV + '\u0000' + b.shaderF;
    let entry = this.glslCache.get(key);
    if (entry === undefined) {
      entry = null;
      try {
        // C gl_renderer.c adapt_vertex does NOT rewrite gl_FrontColor /
        // gl_TexCoord (GLSL 1.x builtins), so any vertex shader using them
        // fails to compile under GLSL 330 core and C falls back to its default
        // passthrough program (vertex color only, no texture sampling). Mirror
        // that here: when the vertex shader carries such legacy builtins,
        // skip the fragment program so resolveColor falls through to
        // passthrough color too (geo_test.pss / geo_duptris.pss).
        const hasLegacyBuiltins = (b.shaderV || '').includes('gl_FrontColor') ||
                                  (b.shaderV || '').includes('gl_TexCoord');
        if (!hasLegacyBuiltins) {
          const vmap = parseVaryingMap(b.shaderV ?? '');
          const prog = compileGLSL(b.shaderF, vmap);
          if (prog) entry = { prog, vmap };
        }
      } catch { entry = null; }
      this.glslCache.set(key, entry);
    }
    return entry;
  }

  // Texture sampler closure for a batch — created once per batch instead of
  // once per pixel. It reads this.curTexLod lazily so the same closure stays
  // valid across all triangles/pixels of the batch.
  private texFnFor(b: DrawBatch): GLSLTexFn | null {
    const units = b.texUnits ?? null;
    if (units) {
      return (unit: number, u: number, v: number, w?: number, lod?: number) => {
        // C gl_renderer.c flush_batch rebinds the ACTIVE texture unit to
        // tex_obj[active_unit] on every draw call (a safety net for scripts
        // that sample a texture left bound by glcaptureend without an explicit
        // glbindtexture). tex_obj[] is indexed by texture ID, so the active
        // unit's texture is the texture whose ID equals the active unit index
        // — NOT whatever glbindtexture last bound to it (e.g. curvybuild.pss
        // binds tex 1 for the floor while the active unit is still 0, so C
        // samples tex 0 there too). Non-active units keep their last
        // glbindtexture/SETTEXDATA binding. If the ID==active-unit texture was
        // never created (tex_obj[active_unit]==0 in C) no rebind happens and
        // the explicit binding wins.
        const tid = (unit === b.texUnit && this.tex.has(b.texUnit))
          ? b.texUnit
          : (units[unit] ?? units[0]);
        const t = tid >= 0 ? this.sampleTexCap(tid, u, v, lod, w) : null;
        return t ?? [0, 0, 0, 1];
      };
    }
    if (b.tex >= 0) {
      return (_unit: number, u: number, v: number, w?: number, lod?: number) => {
        const t = this.sampleTexCap(b.tex, u, v, lod, w);
        return t ?? [0, 0, 0, 1];
      };
    }
    return null;
  }

  // Sample texture `tid` at (u,v), wrapping sampleTex with finite-difference
  // LOD capture. When this.texCap is set every call records (texId, u, v) into
  // this.texCap.coords in call order; in 'final' mode the per-call LOD derived
  // from those finite differences (this.texCap.lods, indexed by call order) is
  // substituted for the implicit LOD. The C GPU derives the mip level from the
  // fragment shader's ACTUAL texture-coordinate derivatives, which can be a
  // nonlinear function of the varyings (e.g. orthoglobe's acos() mapping), so
  // the LOD cannot be approximated from the raw s/t varying gradient alone.
  private sampleTexCap(tid: number, u: number, v: number, lod: number | undefined, w?: number): [number, number, number, number] | null {
    const cap = this.texCap;
    if (cap) {
      cap.coords.push([tid, u, v]);
      if (this.texMode === 'final') {
        const li = cap.call++;
        if (lod === undefined) lod = cap.lods[li];
      }
    }
    if (tid < 0) return null;
    return this.sampleTex(tid, u, v, lod ?? this.curTexLod, w);
  }

  // Upload a procedural texture array (glsettex array form). Mirrors the
  // original polydraw: KGL_BGRA32 packs one uint32 (0xAARRGGBB — GL_BGRA
  // little-endian layout, same as the RGBA() builtin) per texel;
  // KGL_FLOAT/KGL_VEC4 store 4 floats per texel directly. The colmode bits
  // encode filter (>>4 & 0xF) and wrap (>>8 & 0xF) exactly like C.
  uploadTex(id: number, w: number, h: number, pixels: number[] | null, colmode = 0, z = 1): void {
    if (!pixels) return;
    const cm = colmode & 15;
    const fnib = (colmode >> 4) & 0xF;
    // KGL filter nibble: 0=KGL_LINEAR (bilinear), 1=KGL_NEAREST, 2..5=MIPMAP
    // (polydraw.c:191). Normalize to this renderer's internal convention
    // (0=NEAREST, 1=LINEAR, 2+=MIPMAP) — the same one captureToTex uses.
    const filter = fnib === 0 ? 1 : fnib === 1 ? 0 : fnib;
    const wrap = (colmode >> 8) & 0xF;
    const dep = z > 1 ? z : 1;
    // A vertical strip whose height is exactly 6× its width is a cubemap
    // (mirrors C: kglsettex detects xs*6==ys and switches to a cubemap).
    const cube = (z <= 1) && (w * 6 === h);
    const fw = w;
    const fh = cube ? h / 6 : h;
    const n = w * h * dep;
    const data = new Float32Array(n * 4);
    if (cm === 0) {
      // ARGB32: one uint32 per texel (0xAARRGGBB), decoded to RGBA8 normalized.
      // Alpha is essential — volume renders (texture3d.pss) store per-voxel
      // coverage in the alpha channel and rely on SRC_ALPHA blending.
      for (let i = 0; i < n; i++) {
        const v = Math.round(pixels[i] ?? 0) >>> 0;
        data[i * 4] = ((v >> 16) & 255) / 255;
        data[i * 4 + 1] = ((v >> 8) & 255) / 255;
        data[i * 4 + 2] = (v & 255) / 255;
        data[i * 4 + 3] = ((v >> 24) & 255) / 255;
      }
    } else {
      // KGL_FLOAT/KGL_VEC4/CHAR/SHORT/INT: the C renderer's GLCMD_SETTEXDATA
      // packs EVERY scalar as a 0xAARRGGBB uint32 (double -> uint32 truncation),
      // regardless of colmode — the host-side elem only affects how many
      // doubles were copied into the buffer, and only the first n are read.
      // Fractional values truncate to 0 (e.g. gpgpu.pss's 0.5 seeds -> black).
      // Mirror that exactly so the software texture bytes match C's GL upload.
      for (let i = 0; i < n; i++) {
        const v = Math.trunc(pixels[i] ?? 0) >>> 0;
        data[i * 4] = ((v >> 16) & 255) / 255;
        data[i * 4 + 1] = ((v >> 8) & 255) / 255;
        data[i * 4 + 2] = (v & 255) / 255;
        data[i * 4 + 3] = ((v >> 24) & 255) / 255;
      }
    }
    // Cubemap strips store faces bottom-to-top (C: row = (5-f)*fh); face index
    // 0..5 = +X,-X,+Y,-Y,+Z,-Z, matching C's GL_TEXTURE_CUBE_MAP_*_FACE array.
    // Reorder each face into its own fw×fh RGBA buffer so sampling is a plain
    // 2D lookup on faces[fi].
    let faces: Float32Array[] | undefined;
    if (cube) {
      faces = new Array(6);
      for (let fi = 0; fi < 6; fi++) {
        const row0 = (5 - fi) * fh;
        const fd = new Float32Array(fw * fh * 4);
        for (let y = 0; y < fh; y++) fd.set(data.subarray((row0 + y) * fw * 4, (row0 + y) * fw * 4 + fw * 4), y * fw * 4);
        faces[fi] = fd;
      }
    }
    // 3D textures / cubemaps: no mip chain (genMips is 2D; C's 3D/cube paths
    // skip mips unless explicitly requested — ken scripts never do).
    this.tex.set(id, { w: fw, h: fh, d: dep, data, cube, faces, filter, wrap, mips: filter >= 2 && dep === 1 && !cube ? this.genMips(w, h, data, wrap) : undefined });
    if (process.env.PD_DEBUG_TEX && id === 0) {
      console.error(`[tex] id=${id} w=${fw} h=${fh} cube=${!!cube} filter=${filter} wrap=${wrap} mips=${(this.tex.get(0)?.mips?.length ?? 0)}`);
      console.error(`[tex] tl(0,0)=(${(data[0] * 255).toFixed(0)},${(data[1] * 255).toFixed(0)},${(data[2] * 255).toFixed(0)}) tl(1,0)=(${(data[4] * 255).toFixed(0)},${(data[5] * 255).toFixed(0)},${(data[6] * 255).toFixed(0)}) tl(0,1)=(${(data[h * 4] * 255).toFixed(0)},${(data[h * 4 + 1] * 255).toFixed(0)},${(data[h * 4 + 2] * 255).toFixed(0)})`);
    }
  }

  // Box-filter mip chain (mirrors glGenerateMipmap). Each level is half the
  // previous in both dimensions; stop at 1×1. Sampling uses the level whose
  // footprint best matches the on-screen derivative (bilinear within + trilinear
  // across, matching GL_LINEAR_MIPMAP_LINEAR).
  private genMips(w: number, h: number, data: Float32Array, wrap: number): { w: number; h: number; data: Float32Array }[] {
    const mips: { w: number; h: number; data: Float32Array }[] = [];
    let cw = w, ch = h, cd = data;
    const wrapIdx = (x: number, mx: number) => {
      if (wrap === 2 || wrap === 3) return Math.min(mx, Math.max(0, x));
      if (wrap === 1) { x = ((x % (2 * mx)) + 2 * mx) % (2 * mx); if (x >= mx) x = 2 * mx - 1 - x; return x; }
      return ((x % mx) + mx) % mx;
    };
    while (cw > 1 || ch > 1) {
      const nw = Math.max(1, cw >> 1), nh = Math.max(1, ch >> 1);
      const nd = new Float32Array(nw * nh * 4);
      for (let y = 0; y < nh; y++) for (let x = 0; x < nw; x++) {
        // 2×2 box (handles non-power-of-2 by clamping/wrapping at the seam)
        const sx0 = wrapIdx(x * 2, cw), sx1 = wrapIdx(x * 2 + 1, cw);
        const sy0 = wrapIdx(y * 2, ch), sy1 = wrapIdx(y * 2 + 1, ch);
        for (let c = 0; c < 4; c++) {
          const s = cd[(sy0 * cw + sx0) * 4 + c] + cd[(sy0 * cw + sx1) * 4 + c]
                  + cd[(sy1 * cw + sx0) * 4 + c] + cd[(sy1 * cw + sx1) * 4 + c];
          nd[(y * nw + x) * 4 + c] = s * 0.25;
        }
      }
      mips.push({ w: nw, h: nh, data: nd });
      cw = nw; ch = nh; cd = nd;
    }
    return mips;
  }

  // Snapshot the active render target into tex `id` (mirrors C glcaptureend:
  // glReadPixels from the FBO into tex_obj[id]). GL textures are bottom-up
  // (row 0 = NDC y=-1) but the framebuffer is top-down (row 0 = NDC y=+1),
  // so flip rows to match GL's glReadPixels orientation. Restore the main
  // target afterward.
  private captureToTex(id: number): void {
    const w = this.opt.width, h = this.opt.height;
    const data = new Float32Array(w * h * 4);
    const src = this.capBuf ?? this.img;
    for (let y = 0; y < h; y++) {
      const srow = (h - 1 - y) * w * 3, drow = y * w * 4;
      for (let x = 0; x < w; x++) {
        data[drow + x * 4] = src[srow + x * 3];
        data[drow + x * 4 + 1] = src[srow + x * 3 + 1];
        data[drow + x * 4 + 2] = src[srow + x * 3 + 2];
        data[drow + x * 4 + 3] = 1;
      }
    }
    this.tex.set(id, { w, h, d: 1, data, filter: 1, wrap: 2 }); // GL_LINEAR + CLAMP_TO_EDGE (C captureToTex)
    this.target = this.img;
    this.capBuf = null;
  }

  // Clear the active render target (and depth, if depth test enabled) to `clr`.
  // Mirrors C gl_renderer GLCMD_CLEAR.
  private clearTo(clr: [number, number, number, number], clearDepth: boolean): void {
    const t = this.target;
    for (let i = 0; i < t.length; i += 3) {
      t[i] = clr[0]; t[i + 1] = clr[1]; t[i + 2] = clr[2];
    }
    if (clearDepth) this.depth.fill(Infinity);
  }

  // Begin an offscreen capture: all following geometry goes to capBuf instead
  // of the main framebuffer (C: GLCMD_CAPTURE binds an FBO + black clear).
  private beginCapture(): void {
    if (this.capBuf) return; // already capturing
    this.capBuf = new Float32Array(this.opt.width * this.opt.height * 3);
    this.target = this.capBuf;
    this.clearTo([0, 0, 0, 1], this.depthTest);
  }

  render(scene: { batches: DrawBatch[]; captures?: { afterIndex: number; tex: number }[]; texData?: { id: number; w: number; h: number; z: number; colmode: number; pixels: number[] | null }[] }, onBatch?: (info: BatchTraceInfo) => void): void {
    const { width, height } = this.opt;
    const batches = scene.batches;
    for (const td of scene.texData ?? []) this.uploadTex(td.id, td.w, td.h, td.pixels, td.colmode, td.z);
    const captures = scene.captures ?? [];
    let capIdx = 0;
    // C clears at the start of each frame unless the stream has an explicit
    // GLCMD_CLEAR (gl_renderer.c: has_explicit_clear). Mirrors that here.
    const hasExplicitClear = batches.some((b) => b.clear !== undefined);
    this.target = this.img;
    this.capBuf = null;
    if (!hasExplicitClear) this.clearTo([0, 0, 0, 1], this.depthTest);
    for (let bi = 0; bi < batches.length; bi++) {
      const b = batches[bi];
      // Enter/leave the capture render target based on the batch's target tag.
      // A capture-range batch draws into the offscreen buffer; the first
      // non-capture batch after glcaptureend() triggers the snapshot.
      const isCapture = b.captureTarget !== undefined && b.captureTarget >= 0;
      if (isCapture && !this.capBuf) this.beginCapture();
      if (!isCapture && this.capBuf) {
        // End of capture range: snapshot the offscreen buffer into the
        // captured texture, then restore the main render target.
        if (capIdx < captures.length && captures[capIdx].afterIndex <= bi) {
          this.captureToTex(captures[capIdx].tex);
          capIdx++;
        }
      }
      // flush any pending capture whose scene point we've just rendered
      while (capIdx < captures.length && captures[capIdx].afterIndex <= bi) {
        this.captureToTex(captures[capIdx].tex);
        capIdx++;
        this.capBuf = null;
      }
      if (b.clear) { this.clearTo(b.clear, b.depthTest); continue; }
      // Compiled fragment program + sampler closure, resolved once per batch
      // (both were per-pixel allocations/lookups before).
      const entry = this.glslEntryFor(b);
      const tex = this.texFnFor(b);
      // MVP once per batch
      const mvp = new Float64Array(16);
      const pr = b.projection as Float64Array, mv = b.modelview as Float64Array;
      for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++) {
        let s = 0; for (let k = 0; k < 4; k++) s += pr[k * 4 + r] * mv[c * 4 + k];
        mvp[c * 4 + r] = s;
      }
      const vs = b.verts;
      const n = vs.length;
      if (n === 0) continue;
      // Vertex stage: run the script's vertex shader (or the fixed-function
      // fallback) -> clip-space positions + per-vertex varyings
      const vstage = this.vertexStage(b, mvp, mv, pr, vs);
      const clip = vstage.clip;
      const vvary = vstage.vary;
      const ndc = clip.map((c4) => {
        const w = c4[3];
        if (Math.abs(w) < 1e-12) return [0, 0, -2];
        return [c4[0] / w, c4[1] / w, c4[2] / w];
      });
      // GL_POINTS: each vertex becomes a filled square of pointSize pixels.
      if (b.mode === PDGL.POINTS) {
        const ps = Math.max(1, Math.round(b.pointSize ?? 1.0));
        const half = (ps - 1) / 2;
        for (let i = 0; i < n; i++) {
          if (ndc[i][2] < -1 || ndc[i][2] > 1) continue;
          const cx = this.screenX(ndc[i][0]), cy = this.screenY(ndc[i][1]);
          const minX = Math.max(0, Math.floor(cx - half));
          const maxX = Math.min(width - 1, Math.ceil(cx + half));
          const minY = Math.max(0, Math.floor(cy - half));
          const maxY = Math.min(height - 1, Math.ceil(cy + half));
          const vv: Varyings = { r: vs[i].r, g: vs[i].g, b: vs[i].b, a: vs[i].a, s: vs[i].s, t: vs[i].t, p: vs[i].p, nx: vs[i].nx, ny: vs[i].ny, nz: vs[i].nz, ox: vs[i].x, oy: vs[i].y, oz: vs[i].z, ow: vs[i].w, ndx: ndc[i][0], ndy: ndc[i][1], ndz: ndc[i][2], ndw: 1 };
          const out = this.resolveColor(b, vv, vvary[i] ?? null, entry, tex, { x: cx + 0.5, y: (height - 1 - cy) + 0.5, z: ndc[i][2], w: 1 });
          if (!out) continue;
          const fd = lastFragDepth();
          for (let yy = minY; yy <= maxY; yy++) for (let xx = minX; xx <= maxX; xx++) {
            this.writePixel(xx, yy, fd ?? ndc[i][2], out, b);
          }
        }
        continue;
      }
      // GL_LINES / GL_LINE_STRIP / GL_LINE_LOOP: rasterize each segment.
      if (b.mode === PDGL.LINES || b.mode === PDGL.LINE_STRIP || b.mode === PDGL.LINE_LOOP) {
        let segs: [number, number][] = [];
        if (b.mode === PDGL.LINES) {
          for (let i = 0; i + 1 < n; i += 2) segs.push([i, i + 1]);
        } else if (b.mode === PDGL.LINE_STRIP) {
          for (let i = 0; i + 1 < n; i++) segs.push([i, i + 1]);
        } else { // LINE_LOOP
          for (let i = 0; i < n; i++) segs.push([i, (i + 1) % n]);
        }
        const lw = Math.max(1, Math.round(b.lineWidth ?? 1.0));
        const half = Math.floor(lw / 2);
        for (const [ia, ib] of segs) {
          const pa = ndc[ia], pb = ndc[ib];
          // Clip the segment to NDC z ∈ [-1,1] like GL (a segment crossing the
          // near/far plane draws only its visible portion — the C baseline
          // relies on this for rotating wireframe meshes). Simple parametric
          // clip against the two z planes.
          let t0 = 0, t1 = 1;
          const z0 = pa[2], dz = pb[2] - pa[2];
          if (dz === 0) {
            if (z0 < -1 || z0 > 1) continue; // fully outside
          } else {
            const tNear = (-1 - z0) / dz; // where z crosses -1
            const tFar = (1 - z0) / dz;   // where z crosses +1
            if (dz > 0) { t0 = Math.max(t0, tNear); t1 = Math.min(t1, tFar); }
            else        { t0 = Math.max(t0, tFar); t1 = Math.min(t1, tNear); }
            if (t0 > t1) continue;
          }
          const px0 = pa[0] + (pb[0] - pa[0]) * t0, py0 = pa[1] + (pb[1] - pa[1]) * t0;
          const px1 = pa[0] + (pb[0] - pa[0]) * t1, py1 = pa[1] + (pb[1] - pa[1]) * t1;
          const pz0 = pa[2] + (pb[2] - pa[2]) * t0, pz1 = pa[2] + (pb[2] - pa[2]) * t1;
          const mk = (idx: number, t: number) => ({
            r: vs[idx].r + (vs[ib].r - vs[idx].r) * t,
            g: vs[idx].g + (vs[ib].g - vs[idx].g) * t,
            b: vs[idx].b + (vs[ib].b - vs[idx].b) * t,
            a: vs[idx].a + (vs[ib].a - vs[idx].a) * t,
            s: vs[idx].s + (vs[ib].s - vs[idx].s) * t,
            t: vs[idx].t + (vs[ib].t - vs[idx].t) * t,
            p: vs[idx].p + (vs[ib].p - vs[idx].p) * t,
            nx: vs[idx].nx, ny: vs[idx].ny, nz: vs[idx].nz,
            ox: vs[idx].x + (vs[ib].x - vs[idx].x) * t,
            oy: vs[idx].y + (vs[ib].y - vs[idx].y) * t,
            oz: vs[idx].z + (vs[ib].z - vs[idx].z) * t,
            ow: vs[idx].w + (vs[ib].w - vs[idx].w) * t,
            ndx: ndc[idx][0] + (ndc[ib][0] - ndc[idx][0]) * t,
            ndy: ndc[idx][1] + (ndc[ib][1] - ndc[idx][1]) * t,
            ndz: ndc[idx][2] + (ndc[ib][2] - ndc[idx][2]) * t,
            ndw: 1,
          });
          const va = mk(ia, t0), vb = mk(ib, t1);
          this.rasterLine(this.screenX(px0), this.screenY(py0), this.screenX(px1), this.screenY(py1), pz0, pz1, va, vb, half, b, entry, tex);
        }
        continue;
      }
      // fan/strip/polygon -> triangle list
      // Mirrors C tessellate() (gl_renderer.c): distinct splits per primitive
      // — a 24-vertex QUADS list is 6 independent quads (2 tris each), NOT a
      // single 22-triangle fan. TRIANGLES are groups of 3, TRIANGLE_STRIP
      // walks a strip, TRIANGLE_FAN/POLYGON pivot on vertex 0.
      const tris: number[][] = [];
      if (n < 3) continue;
      switch (b.mode) {
        case PDGL.TRIANGLES:
          for (let i = 0; i + 2 < n; i += 3) tris.push([i, i + 1, i + 2]);
          break;
        case PDGL.TRIANGLE_STRIP:
          for (let i = 0; i + 2 < n; i++) tris.push([i, i + 1, i + 2]);
          break;
        case PDGL.TRIANGLE_FAN:
        case PDGL.POLYGON:
          for (let i = 1; i + 1 < n; i++) tris.push([0, i, i + 1]);
          break;
        case PDGL.QUADS:
          for (let i = 0; i + 3 < n; i += 4) { tris.push([i, i + 1, i + 2]); tris.push([i, i + 2, i + 3]); }
          break;
        case PDGL.QUAD_STRIP:
          for (let i = 0; i + 3 < n; i += 2) { tris.push([i, i + 1, i + 3]); tris.push([i, i + 3, i + 2]); }
          break;
        default:
          for (let i = 1; i + 1 < n; i++) tris.push([0, i, i + 1]);
          break;
      }
      for (const [a, bb, c] of tris) {
        // Near-plane clip in clip space (z + w >= 0 keeps the camera-visible
        // part). C/OpenGL clips triangles against the near plane before the
        // perspective divide; simply discarding any triangle with a vertex
        // behind the camera would drop the cube's near face (which straddles
        // the near plane and covers the screen). Sutherland-Hodgman against
        // the near plane: subdivide, interpolating clip position + varyings.
        const ca = clip[a], cb = clip[bb], cc = clip[c];
        const da = ca[2] + ca[3], db = cb[2] + cb[3], dc = cc[2] + cc[3];
        const inside = da >= 0, insb = db >= 0, insc = dc >= 0;
        if (inside && insb && insc) {
          this.rasterTri(b, ndc, clip, vs, vvary, a, bb, c, width, height, entry, tex);
          continue;
        }
        if (!inside && !insb && !insc) continue;
        // build the clipped polygon (1 or 2 triangles) against the near plane.
        const verts: { src: number; t: number }[] = [];
        const pushEdge = (i0: number, i1: number, d0: number, d1: number, in0: boolean, in1: boolean) => {
          if (in0) verts.push({ src: i0, t: 0 });
          if (in0 !== in1) {
            const t = d0 / (d0 - d1);
            verts.push({ src: i0, t });
          }
        };
        pushEdge(a, bb, da, db, inside, insb);
        pushEdge(bb, c, db, dc, insb, insc);
        pushEdge(c, a, dc, da, insc, inside);
        if (verts.length < 3) continue;
        // synthesize interpolated clip-space positions + varyings for edge
        // crossings (t in [0,1], 0 = v0, 1 = v1), keeping the original vertex
        // data for pinned vertices. Append to the clip/vs/vvary arrays.
        const idxMap: number[] = [];
        for (const v of verts) {
          if (v.t === 0) { idxMap.push(v.src); continue; }
          const i0 = v.src, i1 = (v.src === a ? bb : v.src === bb ? c : a);
          const t = v.t;
          const c0 = clip[i0], c1 = clip[i1];
          const np = [c0[0] + (c1[0] - c0[0]) * t, c0[1] + (c1[1] - c0[1]) * t, c0[2] + (c1[2] - c0[2]) * t, c0[3] + (c1[3] - c0[3]) * t];
          clip.push(np);
          ndc.push([np[0] / np[3], np[1] / np[3], np[2] / np[3]]);
          const v0 = vs[i0], v1 = vs[i1];
          const lerp = (x: number, y: number) => x + (y - x) * t;
          vs.push({
            r: lerp(v0.r, v1.r), g: lerp(v0.g, v1.g), b: lerp(v0.b, v1.b), a: lerp(v0.a, v1.a),
            s: lerp(v0.s, v1.s), t: lerp(v0.t, v1.t), p: lerp(v0.p, v1.p), q: lerp(v0.q, v1.q),
            nx: lerp(v0.nx, v1.nx), ny: lerp(v0.ny, v1.ny), nz: lerp(v0.nz, v1.nz),
            x: lerp(v0.x, v1.x), y: lerp(v0.y, v1.y), z: lerp(v0.z, v1.z), w: lerp(v0.w, v1.w),
          });
          const vy0 = vvary[i0] ?? {}, vy1 = vvary[i1] ?? {};
          const keys = new Set([...Object.keys(vy0), ...Object.keys(vy1)]);
          const nv: GLSLVaryRecord = {};
          for (const k of keys) {
            const x = vy0[k], y = vy1[k];
            if (Array.isArray(x) || Array.isArray(y)) {
              const xa = Array.isArray(x) ? x : [x as number], ya = Array.isArray(y) ? y : [y as number];
              const n = Math.max(xa.length, ya.length);
              const arr: number[] = [];
              for (let j = 0; j < n; j++) arr.push(lerp(xa[j] ?? 0, ya[j] ?? 0));
              nv[k] = arr;
            } else nv[k] = lerp((x as number) ?? 0, (y as number) ?? 0);
          }
          vvary.push(nv);
          idxMap.push(clip.length - 1);
        }
        // fan-triangulate the clipped polygon (convex after near-plane clip)
        for (let i = 1; i + 1 < idxMap.length; i++)
          this.rasterTri(b, ndc, clip, vs, vvary, idxMap[0], idxMap[i], idxMap[i + 1], width, height, entry, tex);
      }
      // --- trace hook: snapshot vertex transform results + framebuffer ---
      // vs.slice(0, n): only the ORIGINAL batch vertices — the near-plane
      // clipper above appends synthesized verts to `vs`, which are not part
      // of the source drawcall and would break cross-renderer alignment.
      if (onBatch) {
        const tverts = vs.slice(0, n).map((v, i) => {
          const c4 = clip[i] ?? [0, 0, 0, 1];
          const n3 = ndc[i] ?? [0, 0, 0];
          return {
            ox: v.x, oy: v.y, oz: v.z, ow: v.w,
            cx: c4[0] ?? 0, cy: c4[1] ?? 0, cz: c4[2] ?? 0, cw: c4[3] ?? 1,
            nx: n3[0], ny: n3[1], nz: n3[2],
            r: v.r, g: v.g, b: v.b, a: v.a, s: v.s, t: v.t,
            nrmx: v.nx, nrmy: v.ny, nrmz: v.nz,
          };
        });
        const fb = this.currentTarget();
        // djb2 over top-left row-major RGB bytes — MUST match the C trace's
        // flush_batch readback (same order, same formula) so identical
        // framebuffers hash identically across the two renderers.
        let hash = 5381, sum = 0, nonBlank = 0;
        const len = fb.length;
        for (let i = 0; i < len; i++) {
          const v = Math.max(0, Math.min(255, Math.round(fb[i] * 255)));
          hash = ((hash << 5) + hash + v) | 0;
          sum += v;
          if (v > 4) nonBlank++;
        }
        onBatch({
          index: bi, mode: b.mode, nverts: n,
          mvp: Array.from(mvp),
          verts: tverts,
          shaderF: b.shaderF ?? null,
          shaderV: b.shaderV ?? null,
          fbHash: 'h' + (hash >>> 0).toString(16),
          fbMean: sum / len,
          fbNonBlankPct: nonBlank / (len / 3) * 100,
          cmdStart: b.cmdStart ?? -1,
          cmdEnd: b.cmdEnd ?? -1,
          // snapshot of the framebuffer AFTER this drawcall (copied; the
          // live buffer keeps mutating). Element-level pixel diff material.
          fbData: fb.slice(),
          fbW: width, fbH: height,
        });
      }
    }
  }

  // Rasterize one triangle (indices a,bb,c into ndc/clip/vs/vvary). Extracted
  // from the triangle loop so the near-plane clipper can emit sub-triangles.
  private rasterTri(
    b: DrawBatch, ndc: number[][], clip: number[][], vs: Vertex[], vvary: GLSLVaryRecord[],
    a: number, bb: number, c: number,
    width: number, height: number,
    entry: GLSLBatchEntry | null, tex: GLSLTexFn | null,
  ): void {
    const pa = ndc[a], pb = ndc[bb], pc = ndc[c];
    const dt = b.depthTest || this.depthTest;
    const blend = b.blend;
    // skip triangles fully outside the depth range (already near-plane clipped).
    // Use a small epsilon so clip-plane intersections (NDC z == -1) survive
    // floating-point rounding.
    if (pa[2] < -1.0001 || pb[2] < -1.0001 || pc[2] < -1.0001) return;
    if (pa[2] > 1.0001 || pb[2] > 1.0001 || pc[2] > 1.0001) return;
    const ax = this.screenX(pa[0]), ay = this.screenY(pa[1]);
    const bx = this.screenX(pb[0]), by = this.screenY(pb[1]);
    const cx = this.screenX(pc[0]), cy = this.screenY(pc[1]);
    const minX = Math.max(0, Math.floor(Math.min(ax, bx, cx)));
    const maxX = Math.min(width - 1, Math.ceil(Math.max(ax, bx, cx)));
    const minY = Math.max(0, Math.floor(Math.min(ay, by, cy)));
    const maxY = Math.min(height - 1, Math.ceil(Math.max(ay, by, cy)));
    const area2 = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (Math.abs(area2) < 1e-12) return;
    // back-face culling (mirrors gl_cull). C's GLCMD_CULLFACE pins
    // glFrontFace(GL_CW), so CW winding (area2 < 0 in screen space) is the
    // front face. Scripts (ken/texture.pss cube) emit CW-ordered quads.
    if (b.cullFace) {
      const front = area2 < 0; // CW in screen space with GL_CW front-face
      if (b.cullFace === 0x0405 && front) return; // CULL_BACK -> drop front
      if (b.cullFace === 0x0404 && !front) return; // CULL_FRONT -> drop back
    }
    const s = area2 > 0 ? 1 : -1; // winding-agnostic: accept CW and CCW
    // perspective-correct interpolation setup: 1/w per vertex
    const wa = clip[a][3], wb = clip[bb][3], wc = clip[c][3];
    const iwa = 1 / wa, iwb = 1 / wb, iwc = 1 / wc;
    // per-vertex varyings (with NDC for `p = gl_Position` shaders)
    const va = { r: vs[a].r, g: vs[a].g, b: vs[a].b, a: vs[a].a, s: vs[a].s, t: vs[a].t, p: vs[a].p, nx: vs[a].nx, ny: vs[a].ny, nz: vs[a].nz, ox: vs[a].x, oy: vs[a].y, oz: vs[a].z, ow: vs[a].w, ndx: pa[0], ndy: pa[1], ndz: pa[2], ndw: 1 };
    const vb = { r: vs[bb].r, g: vs[bb].g, b: vs[bb].b, a: vs[bb].a, s: vs[bb].s, t: vs[bb].t, p: vs[bb].p, nx: vs[bb].nx, ny: vs[bb].ny, nz: vs[bb].nz, ox: vs[bb].x, oy: vs[bb].y, oz: vs[bb].z, ow: vs[bb].w, ndx: pb[0], ndy: pb[1], ndz: pb[2], ndw: 1 };
    const vc = { r: vs[c].r, g: vs[c].g, b: vs[c].b, a: vs[c].a, s: vs[c].s, t: vs[c].t, p: vs[c].p, nx: vs[c].nx, ny: vs[c].ny, nz: vs[c].nz, ox: vs[c].x, oy: vs[c].y, oz: vs[c].z, ow: vs[c].w, ndx: pc[0], ndy: pc[1], ndz: pc[2], ndw: 1 };
    // flatten the three vertices' varying records for fast interpolation:
    // names[] + per-name component arrays scaled by 1/w (perspective form)
    const vya = vvary[a] ?? {}, vyb = vvary[bb] ?? {}, vyc = vvary[c] ?? {};
    const vnames = Object.keys(vya);
    const vcomps: number[] = [];      // components per varying
    const vdataA: number[][] = [], vdataB: number[][] = [], vdataC: number[][] = [];
    for (const nm of vnames) {
      const x = vya[nm] as number | number[], y = vyb[nm] as number | number[], z = vyc[nm] as number | number[];
      const xa = Array.isArray(x) ? x : [x], ya = Array.isArray(y) ? y : [y], za = Array.isArray(z) ? z : [z];
      const k = Math.max(xa.length, ya.length, za.length);
      vcomps.push(k);
      const A: number[] = [], B: number[] = [], C: number[] = [];
      for (let j = 0; j < k; j++) { A.push((xa[j] ?? 0) * iwa); B.push((ya[j] ?? 0) * iwb); C.push((za[j] ?? 0) * iwc); }
      vdataA.push(A); vdataB.push(B); vdataC.push(C);
    }
    // Texture LOD for LINEAR_MIPMAP_LINEAR: compute the screen-space derivatives
    // of the perspective-correct texcoord (s,t). The barycentric weights wA,wB,wC
    // are linear in screen (x,y); their derivatives are the edge-function slopes
    // divided by area2. u = N/D (perspective-correct), so du/dx = (N'*D - N*D')/D².
    // GL's LOD = log2(max(|du/dx|*tw, |du/dy|*tw, |dv/dx|*th, |dv/dy|*th)).
    let texLod = -1;
    const hasMip = b.texUnits?.some((id) => { const T = this.tex.get(id); return T && T.mips && T.mips.length > 0; })
      ?? (() => { const T = b.tex >= 0 ? this.tex.get(b.tex) : null; return T && T.mips && T.mips.length > 0; })();
    // Per-varying-component screen derivatives (d/dx, d/dy) evaluated at the
    // triangle centroid, for per-pixel texture LOD. The C GPU derives the mip
    // level from the fragment shader's ACTUAL texture coordinate derivatives
    // (which can be a nonlinear function of the varyings — e.g. orthoglobe's
    // acos() mapping), so we re-run the shader at perturbed varyings.
    let vdx: number[][] | null = null, vdy: number[][] | null = null;
    // d(weight)/dx and d(weight)/dy (edge-function slopes / area2); hoisted so
    // the per-pixel finite-difference LOD path can reuse them below.
    let dwAdx = 0, dwBdx = 0, dwCdx = 0, dwAdy = 0, dwBdy = 0, dwCdy = 0;
    if (hasMip) {
       const sa = vs[a].s, ta = vs[a].t;
       const sb = vs[bb].s, tb = vs[bb].t;
       const sc = vs[c].s, tc = vs[c].t;
       // Evaluate barycentric weights at the triangle centroid (screen space)
       const ccx = (ax + bx + cx) / 3, ccy = (ay + by + cy) / 3;
       const eAbc = (bx - ax) * (ccy - ay) - (by - ay) * (ccx - ax);
       const eBcc = (cx - bx) * (ccy - by) - (cy - by) * (ccx - bx);
       const eCac = (ax - cx) * (ccy - cy) - (ay - cy) * (ccx - cx);
       const cwA = eBcc / area2, cwB = eCac / area2, cwC = eAbc / area2;
       // d(weight)/dx and d(weight)/dy (edge-function slopes / area2)
       dwAdx = -(cy - by) / area2; dwAdy = (cx - bx) / area2;
       dwBdx = -(ay - cy) / area2; dwBdy = (ax - cx) / area2;
       dwCdx = -(by - ay) / area2; dwCdy = (bx - ax) / area2;
       // numerator/denominator of perspective-correct s and t at centroid
       const Ns = cwA * sa * iwa + cwB * sb * iwb + cwC * sc * iwc;
       const Nt = cwA * ta * iwa + cwB * tb * iwb + cwC * tc * iwc;
       const D = cwA * iwa + cwB * iwb + cwC * iwc;
      const dNsdx = dwAdx * sa * iwa + dwBdx * sb * iwb + dwCdx * sc * iwc;
      const dNsdy = dwAdy * sa * iwa + dwBdy * sb * iwb + dwCdy * sc * iwc;
      const dNtdx = dwAdx * ta * iwa + dwBdx * tb * iwb + dwCdx * tc * iwc;
      const dNtdy = dwAdy * ta * iwa + dwBdy * tb * iwb + dwCdy * tc * iwc;
      const dDdx = dwAdx * iwa + dwBdx * iwb + dwCdx * iwc;
      const dDdy = dwAdy * iwa + dwBdy * iwb + dwCdy * iwc;
      const invD2 = D !== 0 ? 1 / (D * D) : 0;
      const dudx = (dNsdx * D - Ns * dDdx) * invD2;
      const dudy = (dNsdy * D - Ns * dDdy) * invD2;
      const dvdx = (dNtdx * D - Nt * dDdx) * invD2;
      const dvdy = (dNtdy * D - Nt * dDdy) * invD2;
      // max texel footprint per screen pixel (use texture 0's dimensions)
      const T0 = this.tex.get(b.texUnits?.[0] ?? b.tex);
      const tw = T0?.w ?? 1, th = T0?.h ?? 1;
      const rho = Math.max(Math.hypot(dudx * tw, dvdx * th), Math.hypot(dudy * tw, dvdy * th));
      texLod = rho > 0 ? Math.log2(rho) : 0;
      texLod = Math.max(0, texLod); // no magnification LOD bias (C uses default)
      const _forceLod = process.env.PD_FORCE_LOD;
      if (_forceLod !== undefined) texLod = parseFloat(_forceLod);
      // d(varying)/d(x|y) per component: o = N/D with N,D affine in screen;
      // evaluated at the centroid (exact when w is constant, i.e. affine).
      if (vnames.length > 0) {
        const Dc = cwA * iwa + cwB * iwb + cwC * iwc;
        const dDdx = dwAdx * iwa + dwBdx * iwb + dwCdx * iwc;
        const dDdy = dwAdy * iwa + dwBdy * iwb + dwCdy * iwc;
        const invD2c = Dc !== 0 ? 1 / (Dc * Dc) : 0;
        vdx = []; vdy = [];
        for (let q = 0; q < vnames.length; q++) {
          const A = vdataA[q], B = vdataB[q], C = vdataC[q];
          const dx = new Array<number>(vcomps[q]), dy = new Array<number>(vcomps[q]);
          for (let j = 0; j < vcomps[q]; j++) {
            const N = cwA * A[j] + cwB * B[j] + cwC * C[j];
            const dNdx = dwAdx * A[j] + dwBdx * B[j] + dwCdx * C[j];
            const dNdy = dwAdy * A[j] + dwBdy * B[j] + dwCdy * C[j];
            dx[j] = (dNdx * Dc - N * dDdx) * invD2c;
            dy[j] = (dNdy * Dc - N * dDdy) * invD2c;
          }
          vdx.push(dx); vdy.push(dy);
        }
      }
    }
    this.curTexLod = texLod;
    if (process.env.PD_DEBUG_LOD) console.error(`[lod] tri(${a},${bb},${c}) area=${area2.toFixed(1)} texLod=${texLod.toFixed(3)}`);
    // Reuse a single Varyings object across all pixels of this triangle
    // (filled, then consumed synchronously by resolveColor below).
    const vv: Varyings = { r: 0, g: 0, b: 0, a: 0, s: 0, t: 0, p: 0, nx: 0, ny: 0, nz: 0, ox: 0, oy: 0, oz: 0, ow: 0, ndx: 0, ndy: 0, ndz: 0, ndw: 0 };
    // Preallocate the per-pixel perspective-correct varying record (VR) and its
    // component arrays once per triangle; the fragment shader consumes VR
    // synchronously and never retains it, so the same objects can be refilled
    // every pixel. Eliminates ~1e8 short-lived allocations for volume renders.
    const vrReuse: GLSLVaryRecord | null = vnames.length ? {} : null;
    const vrArr: number[][] = [];
    if (vrReuse) for (let q = 0; q < vnames.length; q++) { vrArr.push(new Array(vcomps[q])); vrReuse[vnames[q]] = vrArr[q]; }
    // Perturbed VR records (varying + d/dx, varying + d/dy) for the per-pixel
    // texture-LOD finite-difference re-run. Filled per pixel from vrArr.
    let vrX: GLSLVaryRecord | null = null, vrY: GLSLVaryRecord | null = null;
    const vrArrX: number[][] = [], vrArrY: number[][] = [];
    if (vdx) {
      vrX = {}; vrY = {};
      for (let q = 0; q < vnames.length; q++) {
        const ax = new Array<number>(vcomps[q]), ay = new Array<number>(vcomps[q]);
        vrArrX.push(ax); vrArrY.push(ay);
        vrX[vnames[q]] = ax; vrY[vnames[q]] = ay;
      }
    }
    // Per-pixel finite-difference texture LOD. The C GPU derives the mip level
    // from the fragment shader's ACTUAL texture-coordinate derivatives, which
    // may be a nonlinear function of the varyings (e.g. orthoglobe's acos()
    // mapping) — so the centroid s/t gradient (texLod above) is only a fallback.
    // When the shader does an implicit-LOD texture2D on a mipmapped texture, we
    // re-evaluate the varyings at the +x/+y neighbors, capture the coordinates
    // each sample requests via sampleTexCap, and derive the per-sample LOD from
    // the finite differences (GL's 2×2-quad dFdx/dFdy).
    const useFdLod = hasMip && tex !== null && /texture2D\(/.test(b.shaderF ?? '') && process.env.PD_FORCE_LOD === undefined && process.env.PD_DISABLE_FDLOD === undefined;
    // Reusable neighbor Varyings objects (filled per pixel in the FD path).
    const vvX: Varyings = { r: 0, g: 0, b: 0, a: 0, s: 0, t: 0, p: 0, nx: 0, ny: 0, nz: 0, ox: 0, oy: 0, oz: 0, ow: 0, ndx: 0, ndy: 0, ndz: 0, ndw: 0 };
    const vvY: Varyings = { r: 0, g: 0, b: 0, a: 0, s: 0, t: 0, p: 0, nx: 0, ny: 0, nz: 0, ox: 0, oy: 0, oz: 0, ow: 0, ndx: 0, ndy: 0, ndz: 0, ndw: 0 };
    // Interpolate the fixed Varyings fields from three barycentric weights.
    const fillVV = (w0: number, w1: number, w2: number, o: Varyings): void => {
      o.r = w0 * va.r + w1 * vb.r + w2 * vc.r;
      o.g = w0 * va.g + w1 * vb.g + w2 * vc.g;
      o.b = w0 * va.b + w1 * vb.b + w2 * vc.b;
      o.a = w0 * va.a + w1 * vb.a + w2 * vc.a;
      o.s = w0 * va.s + w1 * vb.s + w2 * vc.s;
      o.t = w0 * va.t + w1 * vb.t + w2 * vc.t;
      o.p = w0 * va.p + w1 * vb.p + w2 * vc.p;
      o.nx = w0 * va.nx + w1 * vb.nx + w2 * vc.nx;
      o.ny = w0 * va.ny + w1 * vb.ny + w2 * vc.ny;
      o.nz = w0 * va.nz + w1 * vb.nz + w2 * vc.nz;
      o.ox = w0 * va.ox + w1 * vb.ox + w2 * vc.ox;
      o.oy = w0 * va.oy + w1 * vb.oy + w2 * vc.oy;
      o.oz = w0 * va.oz + w1 * vb.oz + w2 * vc.oz;
      o.ow = w0 * va.ow + w1 * vb.ow + w2 * vc.ow;
      o.ndx = w0 * va.ndx + w1 * vb.ndx + w2 * vc.ndx;
      o.ndy = w0 * va.ndy + w1 * vb.ndy + w2 * vc.ndy;
      o.ndz = w0 * va.ndz + w1 * vb.ndz + w2 * vc.ndz;
      o.ndw = w0 * va.ndw + w1 * vb.ndw + w2 * vc.ndw;
    };
    // Perspective-correct interpolation of the custom varyings into `arrs`.
    const fillVR = (w0: number, w1: number, w2: number, arrs: number[][]): void => {
      const den = w0 * iwa + w1 * iwb + w2 * iwc;
      const inv = den !== 0 ? 1 / den : 0;
      for (let q = 0; q < vnames.length; q++) {
        const AA = vdataA[q], BB = vdataB[q], CC = vdataC[q];
        const o = arrs[q];
        if (vcomps[q] === 1) o[0] = (w0 * AA[0] + w1 * BB[0] + w2 * CC[0]) * inv;
        else for (let j = 0; j < vcomps[q]; j++) o[j] = (w0 * AA[j] + w1 * BB[j] + w2 * CC[j]) * inv;
      }
    };
    const _pxOff = process.env.PD_PIXEL_X_OFF, _pyOff = process.env.PD_PIXEL_Y_OFF;
    const xoff = _pxOff !== undefined ? parseFloat(_pxOff) : 0.5;
    const yoff = _pyOff !== undefined ? parseFloat(_pyOff) : 0.5;
    for (let yy = minY; yy <= maxY; yy++) {
      const cyy = yy + yoff;
      for (let xx = minX; xx <= maxX; xx++) {
        const cxx = xx + xoff;
        // canonical oriented edge functions; s flips the sign for CW winding
        const eAb = (bx - ax) * (cyy - ay) - (by - ay) * (cxx - ax);
        const eBc = (cx - bx) * (cyy - by) - (cy - by) * (cxx - bx);
        const eCa = (ax - cx) * (cyy - cy) - (ay - cy) * (cxx - cx);
        if (s * eAb < 0 || s * eBc < 0 || s * eCa < 0) continue;
        const wA = eBc / area2, wB = eCa / area2, wC = eAb / area2;
        vv.r = wA * va.r + wB * vb.r + wC * vc.r;
        vv.g = wA * va.g + wB * vb.g + wC * vc.g;
        vv.b = wA * va.b + wB * vb.b + wC * vc.b;
        vv.a = wA * va.a + wB * vb.a + wC * vc.a;
        vv.s = wA * va.s + wB * vb.s + wC * vc.s;
        vv.t = wA * va.t + wB * vb.t + wC * vc.t;
        vv.p = wA * va.p + wB * vb.p + wC * vc.p;
        vv.nx = wA * va.nx + wB * vb.nx + wC * vc.nx;
        vv.ny = wA * va.ny + wB * vb.ny + wC * vc.ny;
        vv.nz = wA * va.nz + wB * vb.nz + wC * vc.nz;
        vv.ox = wA * va.ox + wB * vb.ox + wC * vc.ox;
        vv.oy = wA * va.oy + wB * vb.oy + wC * vc.oy;
        vv.oz = wA * va.oz + wB * vb.oz + wC * vc.oz;
        vv.ow = wA * va.ow + wB * vb.ow + wC * vc.ow;
        vv.ndx = wA * va.ndx + wB * vb.ndx + wC * vc.ndx;
        vv.ndy = wA * va.ndy + wB * vb.ndy + wC * vc.ndy;
        vv.ndz = wA * va.ndz + wB * vb.ndz + wC * vc.ndz;
        vv.ndw = wA * va.ndw + wB * vb.ndw + wC * vc.ndw;
        // perspective-correct varyings: interp(v/w) / interp(1/w)
        let vr: GLSLVaryRecord | null = vrReuse;
        if (vrReuse) {
          const den = wA * iwa + wB * iwb + wC * iwc;
          const inv = den !== 0 ? 1 / den : 0;
          for (let q = 0; q < vnames.length; q++) {
            const AA = vdataA[q], BB = vdataB[q], CC = vdataC[q];
            const o = vrArr[q];
            if (vcomps[q] === 1) {
              o[0] = (wA * AA[0] + wB * BB[0] + wC * CC[0]) * inv;
            } else {
              for (let j = 0; j < vcomps[q]; j++) o[j] = (wA * AA[j] + wB * BB[j] + wC * CC[j]) * inv;
            }
          }
        }
        const zz = lastFragDepth() ?? (wA * pa[2] + wB * pb[2] + wC * pc[2]);
        let out: [number, number, number, number] | null;
        if (useFdLod) {
          // Barycentric weights are affine in screen, so the +x/+y neighbor
          // weights are the base weights shifted by the (constant) edge-function
          // slopes — exact for the triangle's linear interpolation.
          const wAx = wA + dwAdx, wBx = wB + dwBdx, wCx = wC + dwCdx;
          const wAy = wA + dwAdy, wBy = wB + dwBdy, wCy = wC + dwCdy;
          fillVV(wAx, wBx, wCx, vvX);
          fillVV(wAy, wBy, wCy, vvY);
          fillVR(wAx, wBx, wCx, vrArrX);
          fillVR(wAy, wBy, wCy, vrArrY);
          const fc = { x: xx + 0.5, y: (height - 1 - yy) + 0.5, z: zz, w: 1 };
          // 1) capture runs: record the coordinates each sample requests at the
          //    base, +x, +y pixel centers (the shader's ACTUAL texcoord mapping,
          //    including nonlinear transforms like orthoglobe's acos()).
          this.texCap = { coords: [], lods: [], call: 0 };
          this.texMode = 'capture';
          if (!this.resolveColor(b, vv, vr, entry, tex, fc)) { this.texCap = null; this.texMode = null; continue; }
          const coords0 = this.texCap.coords;
          this.texCap = { coords: [], lods: [], call: 0 };
          this.resolveColor(b, vvX, vrX, entry, tex, { x: xx + 1.5, y: (height - 1 - yy) + 0.5, z: zz, w: 1 });
          const coordsX = this.texCap.coords;
          this.texCap = { coords: [], lods: [], call: 0 };
          this.resolveColor(b, vvY, vrY, entry, tex, { x: xx + 0.5, y: (height - 1 - (yy + 1)) + 0.5, z: zz, w: 1 });
          const coordsY = this.texCap.coords;
          // 2) per-sample LOD from the finite differences (GL's dFdx/dFdy).
          const lods: number[] = [];
          for (let i = 0; i < coords0.length; i++) {
            const c0 = coords0[i];
            const T0 = this.tex.get(c0[0]);
            const tw = T0?.w ?? 1, th = T0?.h ?? 1;
            const cx = coordsX[i] ?? c0, cy = coordsY[i] ?? c0;
            const rho = Math.max(
              Math.hypot((cx[1] - c0[1]) * tw, (cx[2] - c0[2]) * th),
              Math.hypot((cy[1] - c0[1]) * tw, (cy[2] - c0[2]) * th),
            );
            lods.push(rho > 0 ? Math.max(0, Math.log2(rho)) : 0);
          }
          // 3) final run with the derived per-sample LODs.
          this.texCap = { coords: [], lods, call: 0 };
          this.texMode = 'final';
          out = this.resolveColor(b, vv, vr, entry, tex, fc);
          this.texCap = null; this.texMode = null;
        } else {
          out = this.resolveColor(b, vv, vr, entry, tex, { x: xx + 0.5, y: (height - 1 - yy) + 0.5, z: zz, w: 1 });
        }
        if (process.env.PD_DEBUG_AT && b.shaderF && b.shaderF.length > 100) {
          const [dx, dy] = (process.env.PD_DEBUG_AT as string).split(',').map(Number);
          if (xx === dx && yy === dy) console.error(`[at] (${xx},${yy}) vr=${JSON.stringify(vr)} ndw=${vv.ndw} out=${JSON.stringify(out)} uni=${JSON.stringify(b.uniforms)}`);
        }
        if (!out) continue;
        // Inline writePixel (xx/yy already within [minX,minY]⊆viewport, so the
        // per-pixel bounds check is redundant here). Saves a call + branch per
        // covered pixel — material for the ~33M fragment calls of texture3d.
        const _idx = yy * width + xx;
        if (dt) { if (zz > this.depth[_idx]) continue; this.depth[_idx] = zz; }
        const _o = _idx * 3;
        const _alpha = out[3];
        if (blend && _alpha < 1) {
          const _ia = 1 - _alpha;
          this.target[_o] = out[0] * _alpha + this.target[_o] * _ia;
          this.target[_o + 1] = out[1] * _alpha + this.target[_o + 1] * _ia;
          this.target[_o + 2] = out[2] * _alpha + this.target[_o + 2] * _ia;
        } else {
          this.target[_o] = out[0]; this.target[_o + 1] = out[1]; this.target[_o + 2] = out[2];
        }
      }
    }
  }

  // Rasterize one line segment in screen space, GL-style: every pixel whose
  // center lies within half-width of the segment (projected distance to the
  // infinite line AND clamped to the endpoints) is covered. For lineWidth 1
  // the half-width is 0.5, which reproduces GL's 1-pixel diamond-exit line
  // footprint (a line covers ~length pixels, not ~length*sqrt2 as a naive
  // DDA with per-step squares would). Varyings interpolate along the segment.
  private rasterLine(
    ax: number, ay: number, bx: number, by: number,
    za: number, zb: number,
    va: Varyings, vb: Varyings, half: number, b: DrawBatch,
    entry: GLSLBatchEntry | null, tex: GLSLTexFn | null,
  ): void {
    const { width, height } = this.opt;
    const dx = bx - ax, dy = by - ay;
    const len2 = dx * dx + dy * dy;
    if (len2 < 1e-12) {
      // degenerate point line: single pixel
      const xx = Math.round(ax), yy = Math.round(ay);
      if (xx >= 0 && yy >= 0 && xx < width && yy < height) {
        const out = this.resolveColor(b, va, null, entry, tex);
        if (out) this.writePixel(xx, yy, za, out, b);
      }
      return;
    }
    const minX = Math.max(0, Math.floor(Math.min(ax, bx) - half));
    const maxX = Math.min(width - 1, Math.ceil(Math.max(ax, bx) + half));
    const minY = Math.max(0, Math.floor(Math.min(ay, by) - half));
    const maxY = Math.min(height - 1, Math.ceil(Math.max(ay, by) + half));
    const vv: Varyings = { r: 0, g: 0, b: 0, a: 0, s: 0, t: 0, p: 0, nx: va.nx, ny: va.ny, nz: va.nz, ox: 0, oy: 0, oz: 0, ow: 0, ndx: 0, ndy: 0, ndz: 0, ndw: 0 };
    for (let yy = minY; yy <= maxY; yy++) {
      for (let xx = minX; xx <= maxX; xx++) {
        const cx = xx + 0.5, cy = yy + 0.5; // pixel center
        // closest point on the infinite line, clamped to the segment
        let t = ((cx - ax) * dx + (cy - ay) * dy) / len2;
        if (t < 0) t = 0; else if (t > 1) t = 1;
        const px = ax + dx * t, py = ay + dy * t;
        const dist = Math.hypot(cx - px, cy - py);
        if (dist > half + 0.5) continue;
        const zz = za + (zb - za) * t;
        vv.r = va.r + (vb.r - va.r) * t;
        vv.g = va.g + (vb.g - va.g) * t;
        vv.b = va.b + (vb.b - va.b) * t;
        vv.a = va.a + (vb.a - va.a) * t;
        vv.s = va.s + (vb.s - va.s) * t;
        vv.t = va.t + (vb.t - va.t) * t;
        vv.p = va.p + (vb.p - va.p) * t;
        vv.ox = va.ox + (vb.ox - va.ox) * t;
        vv.oy = va.oy + (vb.oy - va.oy) * t;
        vv.oz = va.oz + (vb.oz - va.oz) * t;
        vv.ow = va.ow + (vb.ow - va.ow) * t;
        const out = this.resolveColor(b, vv, null, entry, tex, { x: xx + 0.5, y: (height - 1 - yy) + 0.5, z: zz, w: 1 });
        if (!out) continue;
        this.writePixel(xx, yy, zz, out, b);
      }
    }
  }

  toRGB8(): Uint8Array {
    const out = new Uint8Array(this.img.length);
    for (let i = 0; i < this.img.length; i++) {
      const v = this.img[i];
      out[i] = v < 0 ? 0 : v > 1 ? 255 : Math.round(v * 255);
    }
    return out;
  }
}