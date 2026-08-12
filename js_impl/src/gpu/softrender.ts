// softrender.ts — dependency-free software rasterizer for the JS GPU pipeline
// (M7). Consumes DrawBatch[] from fixedfunc.ts and produces an RGB image by
// executing a per-pixel fragment function in JS. Enables "Node 出图" without a
// GL context, and lets simple scripts (e.g. balls.pss, whose fragment shader is
// 3 lines) be pixel-checked against the C offscreen reference.
import type { DrawBatch, Vertex } from './fixedfunc.ts';

// Fragment function contract: given interpolated varyings at a pixel, return
// the output RGB in [0,1] or null to discard.
export interface Varyings {
  r: number; g: number; b: number; a: number;
  s: number; t: number;
  nx: number; ny: number; nz: number;
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

export class SoftRenderer {
  private img: Float32Array; // w*h*3 in [0,1]
  private opt: RasterizeOptions;

  constructor(opt: RasterizeOptions) {
    this.opt = opt;
    this.img = new Float32Array(opt.width * opt.height * 3);
  }

  getImage(): Float32Array { return this.img; }

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

  render(batches: DrawBatch[]): void {
    const { width, height, fragment } = this.opt;
    const img = this.img;
    for (const b of batches) {
      // MVP once per batch
      const mvp = new Float64Array(16);
      const pr = b.projection as Float64Array, mv = b.modelview as Float64Array;
      for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++) {
        let s = 0; for (let k = 0; k < 4; k++) s += pr[k * 4 + r] * mv[c * 4 + k];
        mvp[c * 4 + r] = s;
      }
      const vs = b.verts;
      // fan/strip/polygon -> triangle list
      const tris: number[][] = [];
      const n = vs.length;
      if (n < 3) continue;
      for (let i = 1; i + 1 < n; i++) tris.push([0, i, i + 1]);
      const proj = vs.map((v) => this.project(mvp, v));
      for (const [a, bb, c] of tris) {
        // skip triangles fully outside clip box
        const pa = proj[a], pb = proj[bb], pc = proj[c];
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
        const s = area2 > 0 ? 1 : -1; // winding-agnostic: accept CW and CCW
        // per-vertex varyings
        const va = { r: vs[a].r, g: vs[a].g, b: vs[a].b, a: vs[a].a, s: vs[a].s, t: vs[a].t, nx: vs[a].nx, ny: vs[a].ny, nz: vs[a].nz };
        const vb = { r: vs[bb].r, g: vs[bb].g, b: vs[bb].b, a: vs[bb].a, s: vs[bb].s, t: vs[bb].t, nx: vs[bb].nx, ny: vs[bb].ny, nz: vs[bb].nz };
        const vc = { r: vs[c].r, g: vs[c].g, b: vs[c].b, a: vs[c].a, s: vs[c].s, t: vs[c].t, nx: vs[c].nx, ny: vs[c].ny, nz: vs[c].nz };
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
            };
            const out = fragment(vv);
            if (!out) continue;
            const o = (yy * width + xx) * 3;
            img[o] = out[0]; img[o + 1] = out[1]; img[o + 2] = out[2];
          }
        }
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