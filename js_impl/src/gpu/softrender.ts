// softrender.ts — dependency-free software rasterizer for the JS GPU pipeline
// (M7). Consumes DrawBatch[] from fixedfunc.ts and produces an RGB image by
// executing a per-pixel fragment function in JS. Enables "Node 出图" without a
// GL context, and lets simple scripts (e.g. balls.pss, whose fragment shader is
// 3 lines) be pixel-checked against the C offscreen reference.
import type { DrawBatch, Vertex } from './fixedfunc.ts';
import { PDGL } from '../host/glcmd.ts';
import { compileGLSL, compileVertexGLSL, parseVaryingMap, resetFragDepth, lastFragDepth } from './glsl.ts';
import type { GLSLProgram, GLSLVaryings, GLSLVaryRecord, GLSLVertexRunner } from './glsl.ts';

// Fragment function contract: given interpolated varyings at a pixel, return
// the output RGB in [0,1] or null to discard.
export interface Varyings {
  r: number; g: number; b: number; a: number;
  s: number; t: number;
  nx: number; ny: number; nz: number;
  // object-space position (gl_Vertex): interpolated for shaders that sample p
  ox: number; oy: number; oz: number; ow: number;
  // NDC position (gl_Position): interpolated for `p = gl_Position` shaders
  ndx: number; ndy: number; ndz: number; ndw: number;
}
export type FragmentFn = (v: Varyings) => [number, number, number] | null;

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
  private tex: Map<number, { w: number; h: number; d: number; data: Float32Array; filter: number; wrap: number; mips?: { w: number; h: number; data: Float32Array }[] }> = new Map();
  // Active render target. When capturing (glcapture..glcaptureend), geometry is
  // drawn to capBuf instead of img, mirroring C's FBO render-target switch.
  private target: Float32Array = new Float32Array(0);
  private capBuf: Float32Array | null = null;
  // Per-triangle texture LOD (mip level) for LINEAR_MIPMAP_LINEAR sampling.
  // Set by rasterTri from the screen-space gradient of the `t` (texcoord)
  // varying; read by the tex closure. -1 = no mip (use base level).
  private curTexLod = -1;
  // GLSL fragment-shader cache: (shaderF, shaderV) -> program + varying map
  private glslCache = new Map<string, { prog: GLSLProgram; vmap: Map<string, string> }>();
  // GLSL vertex-shader cache: shaderV -> runner (null = no/failed VS)
  private vshCache = new Map<string, GLSLVertexRunner | null>();

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
  private writePixel(xx: number, yy: number, z: number, rgb: [number, number, number], alpha: number, b: DrawBatch): void {
    const { width, height } = this.opt;
    if (xx < 0 || yy < 0 || xx >= width || yy >= height) return;
    const idx = yy * width + xx;
    const dt = b.depthTest || this.depthTest;
    if (dt) {
      if (z > this.depth[idx]) return; // GL default LESS vs depth buffer
      this.depth[idx] = z;
    }
    const o = idx * 3;
    if (b.blend && alpha < 1) {
      // src*srcA + dst*(1-srcA)
      this.target[o] = rgb[0] * alpha + this.target[o] * (1 - alpha);
      this.target[o + 1] = rgb[1] * alpha + this.target[o + 1] * (1 - alpha);
      this.target[o + 2] = rgb[2] * alpha + this.target[o + 2] * (1 - alpha);
    } else {
      this.target[o] = rgb[0]; this.target[o + 1] = rgb[1]; this.target[o + 2] = rgb[2];
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
  private sampleTex(texId: number, s: number, t: number, lod = -1, wCoord?: number): [number, number, number] | null {
    const T = this.tex.get(texId);
    if (!T) return null;
    // 3D (volumetric) texture: trilinear over slice[z][y][x]
    if (T.d > 1 && wCoord !== undefined) return this.sampleTex3D(T, s, t, wCoord);
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
    return [c0[0] + (c1[0] - c0[0]) * f, c0[1] + (c1[1] - c0[1]) * f, c0[2] + (c1[2] - c0[2]) * f];
  }

  private sampleTexLevel(T: { w: number; h: number; data: Float32Array; filter: number; wrap: number }, s: number, t: number): [number, number, number] {
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
      const o = (py * w + px) * 3;
      return [data[o], data[o + 1], data[o + 2]];
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
      return (y * w + x) * 3;
    };
    const i00 = idx(x0, y0), i10 = idx(x0 + 1, y0), i01 = idx(x0, y0 + 1), i11 = idx(x0 + 1, y0 + 1);
    const lerp = (a: number, b: number, f: number) => a + (b - a) * f;
    const r = lerp(lerp(data[i00], data[i10], fx), lerp(data[i01], data[i11], fx), fy);
    const g = lerp(lerp(data[i00 + 1], data[i10 + 1], fx), lerp(data[i01 + 1], data[i11 + 1], fx), fy);
    const b = lerp(lerp(data[i00 + 2], data[i10 + 2], fx), lerp(data[i01 + 2], data[i11 + 2], fx), fy);
    return [r, g, b];
  }

  // Run the batch's vertex shader per vertex (mirrors C adapt_vertex + the
  // GPU vertex stage). Returns clip-space positions and per-vertex varyings.
  // When the batch has no vertex shader (or it fails to compile), positions
  // come from the fixed-function MVP and varyings are synthesized from the
  // fixed-function attributes (c/t/p/n) — matching C's default program.
  private vertexStage(b: DrawBatch, mvp: Float64Array, mv: Float64Array, pr: Float64Array, vs: Vertex[]): { clip: number[][]; vary: GLSLVaryRecord[]; hasVS: boolean } {
    let vsh: GLSLVertexRunner | null = null;
    if (b.shaderV) {
      if (!this.vshCache.has(b.shaderV)) this.vshCache.set(b.shaderV, compileVertexGLSL(b.shaderV));
      vsh = this.vshCache.get(b.shaderV) ?? null;
    }
    const clip: number[][] = [];
    const vary: GLSLVaryRecord[] = [];
    if (vsh) {
      // normal matrix: upper-left 3x3 of the modelview
      const nrm = [mv[0], mv[1], mv[2], mv[4], mv[5], mv[6], mv[8], mv[9], mv[10]];
      for (const v of vs) {
        const r = vsh({
          vertex: [v.x, v.y, v.z, v.w],
          color: [v.r, v.g, v.b, v.a],
          texcoord: [v.s, v.t, v.p, v.q],
          normal: [v.nx, v.ny, v.nz],
          mvp: Array.from(mvp), modelview: Array.from(mv), projection: Array.from(pr), normalMat: nrm,
        }, b.uniforms as never);
        if (!r) { clip.push(this.project(mvp, v).concat(1)); vary.push({}); continue; }
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
  private resolveColor(b: DrawBatch, vv: Varyings, vr: GLSLVaryRecord | null = null): [number, number, number] | null {
    resetFragDepth(); // shader-written gl_FragDepth (if any) applies per pixel
    if (b.shaderF) {
      // Execute the script's actual fragment shader through the GLSL subset
      // interpreter; texture2D samples the batch's bound texture.
      if (process.env.PD_DEBUG_GLSL) console.error('[soft] shaderF len =', b.shaderF.length);
      const key = b.shaderV + '\u0000' + b.shaderF;
      let entry = this.glslCache.get(key);
      if (!entry) {
        const vmap = parseVaryingMap(b.shaderV ?? '');
        const prog = compileGLSL(b.shaderF, vmap);
        if (prog) {
          entry = { prog, vmap };
          this.glslCache.set(key, entry);
        } else if (process.env.PD_DEBUG_GLSL) console.error('[soft] compileGLSL FAILED');
      }
      if (entry) {
        const gv: GLSLVaryings = {
          r: vv.r, g: vv.g, b: vv.b, a: vv.a,
          s: vv.s, t: vv.t, p: 0, q: 1,
          nx: vv.nx, ny: vv.ny, nz: vv.nz,
          px: vv.ox, py: vv.oy, pz: vv.oz, pw: vv.ow,
          ndx: vv.ndx, ndy: vv.ndy, ndz: vv.ndz, ndw: vv.ndw,
        };
        // A fragment shader that samples a sampler2D (e.g. blur passes after
        // glcapture) must get a texFn whenever the batch has a bound texture —
        // regardless of useTex (which only tracks glBindTexture for the legacy
        // fixed-function color path). Mirrors C: samplers named texN bind unit
        // N (polydraw.c:1064), and each unit samples the texture bound to it
        // by glactivetexture(GL_TEXTURE0+N)+glbindtexture(id).
        const units = b.texUnits ?? null;
        const tex = units
          ? (unit: number, u: number, v: number) => {
            const tid = units[unit] ?? units[0];
            const t = tid >= 0 ? this.sampleTex(tid, u, v, this.curTexLod) : null;
            return t ?? [0, 0, 0];
          }
          : b.tex >= 0 ? (unit: number, u: number, v: number) => {
            const t = this.sampleTex(b.tex, u, v, this.curTexLod);
            return t ?? [0, 0, 0];
          } : null;
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
      // unsupported GLSL -> fall through to passthrough color
    } else if (b.useTex && b.tex >= 0) {
      // No fragment shader: C's default program is color-only (fragColor = c);
      // a bound texture is ignored. Mirrors 26_texture_procedural behavior.
      return this.opt.fragment(vv);
    }
    return this.opt.fragment(vv);
  }

  // Upload a procedural texture array (glsettex array form). Mirrors the
  // original polydraw: KGL_BGRA32 packs one uint32 (0xAARRGGBB — GL_BGRA
  // little-endian layout, same as the RGBA() builtin) per texel;
  // KGL_FLOAT/KGL_VEC4 store 4 floats per texel directly. The colmode bits
  // encode filter (>>4 & 0xF) and wrap (>>8 & 0xF) exactly like C.
  uploadTex(id: number, w: number, h: number, pixels: number[] | null, colmode = 0, z = 1): void {
    if (!pixels) return;
    const cm = colmode & 15;
    const filter = (colmode >> 4) & 0xF;
    const wrap = (colmode >> 8) & 0xF;
    const dep = z > 1 ? z : 1;
    const n = w * h * dep;
    const data = new Float32Array(n * 3);
    if (cm === 0) {
      // ARGB32: one uint32 per texel, decoded to RGB8 (A unused by soft raster)
      for (let i = 0; i < n; i++) {
        const v = Math.round(pixels[i] ?? 0) >>> 0;
        data[i * 3] = ((v >> 16) & 255) / 255;
        data[i * 3 + 1] = ((v >> 8) & 255) / 255;
        data[i * 3 + 2] = (v & 255) / 255;
      }
    } else {
      for (let i = 0; i < n; i++) {
        data[i * 3] = pixels[i * 4] ?? 0;
        data[i * 3 + 1] = pixels[i * 4 + 1] ?? 0;
        data[i * 3 + 2] = pixels[i * 4 + 2] ?? 0;
      }
    }
    this.tex.set(id, { w, h, d: dep, data, filter, wrap, mips: filter >= 2 ? this.genMips(w, h, data, wrap) : undefined });
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
      const nd = new Float32Array(nw * nh * 3);
      for (let y = 0; y < nh; y++) for (let x = 0; x < nw; x++) {
        // 2×2 box (handles non-power-of-2 by clamping/wrapping at the seam)
        const sx0 = wrapIdx(x * 2, cw), sx1 = wrapIdx(x * 2 + 1, cw);
        const sy0 = wrapIdx(y * 2, ch), sy1 = wrapIdx(y * 2 + 1, ch);
        for (let c = 0; c < 3; c++) {
          const s = cd[(sy0 * cw + sx0) * 3 + c] + cd[(sy0 * cw + sx1) * 3 + c]
                  + cd[(sy1 * cw + sx0) * 3 + c] + cd[(sy1 * cw + sx1) * 3 + c];
          nd[(y * nw + x) * 3 + c] = s * 0.25;
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
    const data = new Float32Array(w * h * 3);
    const src = this.capBuf ?? this.img;
    for (let y = 0; y < h; y++) {
      const srow = (h - 1 - y) * w * 3, drow = y * w * 3;
      for (let x = 0; x < w * 3; x++) data[drow + x] = src[srow + x];
    }
    this.tex.set(id, { w, h, data, filter: 1, wrap: 2 }); // GL_LINEAR + CLAMP_TO_EDGE (C captureToTex)
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
    for (const td of scene.texData ?? []) this.uploadTex(td.id, td.w, td.h, td.pixels, td.colmode);
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
          const vv: Varyings = { r: vs[i].r, g: vs[i].g, b: vs[i].b, a: vs[i].a, s: vs[i].s, t: vs[i].t, nx: vs[i].nx, ny: vs[i].ny, nz: vs[i].nz, ox: vs[i].x, oy: vs[i].y, oz: vs[i].z, ow: vs[i].w };
          const out = this.resolveColor(b, vv, vvary[i] ?? null);
          if (!out) continue;
          const fd = lastFragDepth();
          for (let yy = minY; yy <= maxY; yy++) for (let xx = minX; xx <= maxX; xx++) {
            this.writePixel(xx, yy, fd ?? ndc[i][2], out, vs[i].a, b);
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
            nx: vs[idx].nx, ny: vs[idx].ny, nz: vs[idx].nz,
            ox: vs[idx].x + (vs[ib].x - vs[idx].x) * t,
            oy: vs[idx].y + (vs[ib].y - vs[idx].y) * t,
            oz: vs[idx].z + (vs[ib].z - vs[idx].z) * t,
            ow: vs[idx].w + (vs[ib].w - vs[idx].w) * t,
          });
          const va = mk(ia, t0), vb = mk(ib, t1);
          this.rasterLine(this.screenX(px0), this.screenY(py0), this.screenX(px1), this.screenY(py1), pz0, pz1, va, vb, half, b);
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
          this.rasterTri(b, ndc, clip, vs, vvary, a, bb, c, width, height);
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
          this.rasterTri(b, ndc, clip, vs, vvary, idxMap[0], idxMap[i], idxMap[i + 1], width, height);
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
          fbW: this.w, fbH: this.h,
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
  ): void {
    const pa = ndc[a], pb = ndc[bb], pc = ndc[c];
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
    const va = { r: vs[a].r, g: vs[a].g, b: vs[a].b, a: vs[a].a, s: vs[a].s, t: vs[a].t, nx: vs[a].nx, ny: vs[a].ny, nz: vs[a].nz, ox: vs[a].x, oy: vs[a].y, oz: vs[a].z, ow: vs[a].w, ndx: pa[0], ndy: pa[1], ndz: pa[2], ndw: 1 };
    const vb = { r: vs[bb].r, g: vs[bb].g, b: vs[bb].b, a: vs[bb].a, s: vs[bb].s, t: vs[bb].t, nx: vs[bb].nx, ny: vs[bb].ny, nz: vs[bb].nz, ox: vs[bb].x, oy: vs[bb].y, oz: vs[bb].z, ow: vs[bb].w, ndx: pb[0], ndy: pb[1], ndz: pb[2], ndw: 1 };
    const vc = { r: vs[c].r, g: vs[c].g, b: vs[c].b, a: vs[c].a, s: vs[c].s, t: vs[c].t, nx: vs[c].nx, ny: vs[c].ny, nz: vs[c].nz, ox: vs[c].x, oy: vs[c].y, oz: vs[c].z, ow: vs[c].w, ndx: pc[0], ndy: pc[1], ndz: pc[2], ndw: 1 };
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
       const dwAdx = -(cy - by) / area2, dwAdy = (cx - bx) / area2;
       const dwBdx = -(ay - cy) / area2, dwBdy = (ax - cx) / area2;
       const dwCdx = -(by - ay) / area2, dwCdy = (bx - ax) / area2;
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
    }
    this.curTexLod = texLod;
    for (let yy = minY; yy <= maxY; yy++) {
      const cyy = yy + 0.5;
      for (let xx = minX; xx <= maxX; xx++) {
        const cxx = xx + 0.5;
        // canonical oriented edge functions; s flips the sign for CW winding
        const eAb = (bx - ax) * (cyy - ay) - (by - ay) * (cxx - ax);
        const eBc = (cx - bx) * (cyy - by) - (cy - by) * (cxx - bx);
        const eCa = (ax - cx) * (cyy - cy) - (ay - cy) * (cxx - cx);
        if (s * eAb < 0 || s * eBc < 0 || s * eCa < 0) continue;
        const wA = eBc / area2, wB = eCa / area2, wC = eAb / area2;
        const vv: Varyings = {
          r: wA * va.r + wB * vb.r + wC * vc.r,
          g: wA * va.g + wB * vb.g + wC * vc.g,
          b: wA * va.b + wB * vb.b + wC * vc.b,
          a: wA * va.a + wB * vb.a + wC * vc.a,
          s: wA * va.s + wB * vb.s + wC * vc.s,
          t: wA * va.t + wB * vb.t + wC * vc.t,
          nx: wA * va.nx + wB * vb.nx + wC * vc.nx,
          ny: wA * va.ny + wB * vb.ny + wC * vc.ny,
          nz: wA * va.nz + wB * vb.nz + wC * vc.nz,
          ox: wA * va.ox + wB * vb.ox + wC * vc.ox,
          oy: wA * va.oy + wB * vb.oy + wC * vc.oy,
          oz: wA * va.oz + wB * vb.oz + wC * vc.oz,
          ow: wA * va.ow + wB * vb.ow + wC * vc.ow,
          ndx: wA * va.ndx + wB * vb.ndx + wC * vc.ndx,
          ndy: wA * va.ndy + wB * vb.ndy + wC * vc.ndy,
          ndz: wA * va.ndz + wB * vb.ndz + wC * vc.ndz,
          ndw: wA * va.ndw + wB * vb.ndw + wC * vc.ndw,
        };
        // perspective-correct varyings: interp(v/w) / interp(1/w)
        let vr: GLSLVaryRecord | null = null;
        if (vnames.length) {
          const den = wA * iwa + wB * iwb + wC * iwc;
          const inv = den !== 0 ? 1 / den : 0;
          vr = {};
          for (let q = 0; q < vnames.length; q++) {
            const nm = vnames[q], k = vcomps[q];
            const AA = vdataA[q], BB = vdataB[q], CC = vdataC[q];
            if (k === 1) {
              vr[nm] = (wA * AA[0] + wB * BB[0] + wC * CC[0]) * inv;
            } else {
              const o: number[] = new Array(k);
              for (let j = 0; j < k; j++) o[j] = (wA * AA[j] + wB * BB[j] + wC * CC[j]) * inv;
              vr[nm] = o;
            }
          }
        }
        const out = this.resolveColor(b, vv, vr);
        if (process.env.PD_DEBUG_AT && b.shaderF && b.shaderF.length > 100) {
          const [dx, dy] = (process.env.PD_DEBUG_AT as string).split(',').map(Number);
          if (xx === dx && yy === dy) console.error(`[at] (${xx},${yy}) vr=${JSON.stringify(vr)} ndw=${vv.ndw} out=${JSON.stringify(out)} uni=${JSON.stringify(b.uniforms)}`);
        }
        if (!out) continue;
        const zz = lastFragDepth() ?? (wA * pa[2] + wB * pb[2] + wC * pc[2]);
        this.writePixel(xx, yy, zz, out, vv.a, b);
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
  ): void {
    const { width, height } = this.opt;
    const dx = bx - ax, dy = by - ay;
    const len2 = dx * dx + dy * dy;
    if (len2 < 1e-12) {
      // degenerate point line: single pixel
      const xx = Math.round(ax), yy = Math.round(ay);
      if (xx >= 0 && yy >= 0 && xx < width && yy < height) {
        const out = this.resolveColor(b, va);
        if (out) this.writePixel(xx, yy, za, out, va.a, b);
      }
      return;
    }
    const minX = Math.max(0, Math.floor(Math.min(ax, bx) - half));
    const maxX = Math.min(width - 1, Math.ceil(Math.max(ax, bx) + half));
    const minY = Math.max(0, Math.floor(Math.min(ay, by) - half));
    const maxY = Math.min(height - 1, Math.ceil(Math.max(ay, by) + half));
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
        const vv: Varyings = {
          r: va.r + (vb.r - va.r) * t,
          g: va.g + (vb.g - va.g) * t,
          b: va.b + (vb.b - va.b) * t,
          a: va.a + (vb.a - va.a) * t,
          s: va.s + (vb.s - va.s) * t,
          t: va.t + (vb.t - va.t) * t,
          nx: va.nx, ny: va.ny, nz: va.nz,
          ox: va.ox + (vb.ox - va.ox) * t,
          oy: va.oy + (vb.oy - va.oy) * t,
          oz: va.oz + (vb.oz - va.oz) * t,
          ow: va.ow + (vb.ow - va.ow) * t,
        };
        const out = this.resolveColor(b, vv);
        if (!out) continue;
        this.writePixel(xx, yy, zz, out, vv.a, b);
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