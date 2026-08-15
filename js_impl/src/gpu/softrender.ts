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
  // captured/offscreen textures: texId -> RGBA float data (w*h*3) for sampling
  private tex: Map<number, { w: number; h: number; data: Float32Array }> = new Map();
  // Active render target. When capturing (glcapture..glcaptureend), geometry is
  // drawn to capBuf instead of img, mirroring C's FBO render-target switch.
  private target: Float32Array = new Float32Array(0);
  private capBuf: Float32Array | null = null;
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

  private sampleTex(texId: number, s: number, t: number): [number, number, number] | null {
    const T = this.tex.get(texId);
    if (!T) return null;
    const u = ((s % 1) + 1) % 1, v = ((t % 1) + 1) % 1;
    const px = Math.min(T.w - 1, Math.max(0, Math.floor(u * T.w)));
    const py = Math.min(T.h - 1, Math.max(0, Math.floor(v * T.h)));
    const o = (py * T.w + px) * 3;
    return [T.data[o], T.data[o + 1], T.data[o + 2]];
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
        // fixed-function color path). Mirrors C: the sampler binds tex_obj[tex].
        const tex = b.tex >= 0 ? (u: number, v: number) => {
          const t = this.sampleTex(b.tex, u, v);
          return t ?? [0, 0, 0];
        } : null;
        try {
          const rc = entry.prog.run(gv, tex, entry.vmap, b.uniforms as never, vr ?? undefined);
          if (process.env.PD_DEBUG_GLSL) {
            g_shaderCalls++;
            if (rc === null) g_shaderDiscards++;
            if (process.env.PD_DEBUG_PIX && b.shaderF!.length > 100 && (g_shaderCalls & 8191) === 0)
              console.error(`[pix] rc=${JSON.stringify(rc)} fd=${lastFragDepth()} vrKeys=${vr ? Object.keys(vr).join('/') : '-'} v=${vr && vr['v'] ? JSON.stringify(vr['v']) : '?'}`);
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

  // Upload a procedural texture array (glsettex array form). Mirrors C's
  // gl_renderer decode: KGL_BGRA32 packs one uint32 (0xAABBGGRR) per texel;
  // KGL_FLOAT/KGL_VEC4 store 4 floats per texel directly.
  uploadTex(id: number, w: number, h: number, pixels: number[] | null, colmode = 0): void {
    if (!pixels) return;
    const cm = colmode & 15;
    const data = new Float32Array(w * h * 3);
    if (cm === 0) {
      // BGRA32: one uint32 per texel, decoded to RGB8 (A unused by soft raster)
      for (let i = 0; i < w * h; i++) {
        const v = Math.round(pixels[i] ?? 0) >>> 0;
        data[i * 3] = (v & 255) / 255;
        data[i * 3 + 1] = ((v >> 8) & 255) / 255;
        data[i * 3 + 2] = ((v >> 16) & 255) / 255;
      }
    } else {
      for (let i = 0; i < w * h; i++) {
        data[i * 3] = pixels[i * 4] ?? 0;
        data[i * 3 + 1] = pixels[i * 4 + 1] ?? 0;
        data[i * 3 + 2] = pixels[i * 4 + 2] ?? 0;
      }
    }
    this.tex.set(id, { w, h, data });
  }

  // Capture the current capture buffer into tex `id` (mirrors C glcaptureend:
  // copy the FBO contents into tex_obj[id] and restore the main target).
  private captureToTex(id: number): void {
    const w = this.opt.width, h = this.opt.height;
    const data = new Float32Array(w * h * 3);
    // Capture buffer is stored top-down (row 0 = NDC y=+1); texels use the
    // same orientation, so sampling keeps t aligned with the drawn scene.
    data.set(this.capBuf ?? this.img);
    this.tex.set(id, { w, h, data });
    // Restore the main framebuffer as the render target.
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
  }

  render(scene: { batches: DrawBatch[]; captures?: { afterIndex: number; tex: number }[]; texData?: { id: number; w: number; h: number; z: number; colmode: number; pixels: number[] | null }[] }): void {
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
      const tris: number[][] = [];
      if (n < 3) continue;
      for (let i = 1; i + 1 < n; i++) tris.push([0, i, i + 1]);
      for (const [a, bb, c] of tris) {
        // skip triangles fully outside clip box
        const pa = ndc[a], pb = ndc[bb], pc = ndc[c];
        if (pa[2] < -1 || pb[2] < -1 || pc[2] < -1) continue;
        if (pa[2] > 1 || pb[2] > 1 || pc[2] > 1) continue;
        const ax = this.screenX(pa[0]), ay = this.screenY(pa[1]);
        const bx = this.screenX(pb[0]), by = this.screenY(pb[1]);
        const cx = this.screenX(pc[0]), cy = this.screenY(pc[1]);
        const minX = Math.max(0, Math.floor(Math.min(ax, bx, cx)));
        const maxX = Math.min(width - 1, Math.ceil(Math.max(ax, bx, cx)));
        const minY = Math.max(0, Math.floor(Math.min(ay, by, cy)));
        const maxY = Math.min(height - 1, Math.ceil(Math.max(ay, by, cy)));
        const area2 = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        if (Math.abs(area2) < 1e-12) continue;
        // back-face culling (mirrors gl_cull): positive area2 = CCW front face.
        if (b.cullFace) {
          const front = area2 > 0; // CCW in screen space
          if (b.cullFace === 0x0405 && front) continue; // CULL_BACK -> drop front
          if (b.cullFace === 0x0404 && !front) continue; // CULL_FRONT -> drop back
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
              if (xx === dx && yy === dy) console.error(`[at] (${xx},${yy}) vr=${JSON.stringify(vr)} out=${JSON.stringify(out)} uni=${JSON.stringify(b.uniforms)}`);
            }
            if (!out) continue;
            const zz = lastFragDepth() ?? (wA * pa[2] + wB * pb[2] + wC * pc[2]);
            this.writePixel(xx, yy, zz, out, vv.a, b);
          }
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