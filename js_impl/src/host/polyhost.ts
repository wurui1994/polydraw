// polyhost.ts — JS port of the PolyDraw recording host.
// Mirrors c_impl/src/pd_polyhost.c + pd_polyhost_render.c + pd_polyhost_tex.c.
// The gl* externs RECORD into a GLCmdBuf instead of calling a real GL. The
// same GLCmd stream is later replayed by gpu/replay.ts (WebGL2) — and must
// match the C offscreen renderer's stream bit-for-bit (cross-backend test).

import { srand as intSrand } from '../backend/interp.ts';

import { GLCmdBuf, GLCMD, PDGL, PD_UNI_F, PD_UNI_I } from './glcmd.ts';
import type { GLCmd } from './glcmd.ts';
import type { Host, HostFn, HostVar } from '../eval/ir.ts';

const DEG2RAD = Math.PI / 180.0;

// ---- 4x4 column-major matrix helpers (double; matches C gl_renderer.c) ----
type M4 = Float64Array; // length 16, column-major (m[col*4+row])
function mat4Identity(): M4 {
  const m = new Float64Array(16);
  m[0] = m[5] = m[10] = m[15] = 1.0;
  return m;
}
function mat4Mul(a: M4, b: M4): M4 {
  const r = new Float64Array(16);
  for (let c = 0; c < 4; c++)
    for (let rw = 0; rw < 4; rw++) {
      let s = 0;
      for (let k = 0; k < 4; k++) s += a[k * 4 + rw] * b[c * 4 + k];
      r[c * 4 + rw] = s;
    }
  return r;
}
function mat4Translate(m: M4, x: number, y: number, z: number): M4 {
  const t = mat4Identity(); t[12] = x; t[13] = y; t[14] = z;
  return mat4Mul(m, t);
}
function mat4Rotate(m: M4, angDeg: number, x: number, y: number, z: number): M4 {
  const a = angDeg * DEG2RAD, c = Math.cos(a), s = Math.sin(a);
  let len = Math.sqrt(x * x + y * y + z * z);
  if (len === 0) return m.slice();
  x /= len; y /= len; z /= len;
  const k = 1 - c;
  const t = new Float64Array([
    c + x * x * k, x * y * k - z * s, x * z * k + y * s, 0,
    y * x * k + z * s, c + y * y * k, y * z * k - x * s, 0,
    z * x * k - y * s, z * y * k + x * s, c + z * z * k, 0,
    0, 0, 0, 1,
  ]);
  return mat4Mul(m, t);
}
function mat4Scale(m: M4, x: number, y: number, z: number): M4 {
  const t = mat4Identity(); t[0] = x; t[5] = y; t[10] = z;
  return mat4Mul(m, t);
}
function mat4Perspective(fovyDeg: number, aspect: number, near: number, far: number): M4 {
  const f = 1.0 / Math.tan(fovyDeg * Math.PI / 360.0);
  const m = new Float64Array(16);
  m[0] = f / aspect; m[5] = f;
  m[10] = (far + near) / (near - far);
  m[11] = -1.0;
  m[14] = (2 * far * near) / (near - far);
  return m;
}
function mat4LookAt(
  ex: number, ey: number, ez: number,
  cx: number, cy: number, cz: number,
  ux: number, uy: number, uz: number
): M4 {
  let fx = cx - ex, fy = cy - ey, fz = cz - ez;
  let fl = Math.sqrt(fx * fx + fy * fy + fz * fz);
  if (fl !== 0) { fx /= fl; fy /= fl; fz /= fl; }
  let sx = fy * uz - fz * uy, sy = fz * ux - fx * uz, sz = fx * uy - fy * ux;
  let sl = Math.sqrt(sx * sx + sy * sy + sz * sz);
  if (sl !== 0) { sx /= sl; sy /= sl; sz /= sl; }
  const uxx = sy * fz - sz * fy, uyy = sz * fx - sx * fz, uzz = sx * fy - sy * fx;
  let m = new Float64Array([
    sx, uxx, -fx, 0,
    sy, uyy, -fy, 0,
    sz, uzz, -fz, 0,
    0, 0, 0, 1,
  ]);
  const t = new Float64Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -ex, -ey, -ez, 1]);
  m = mat4Mul(m, t);
  return m;
}

// GLSL section block (for glsetshader name lookup).
export interface Block { src: string; name: string; type: number; }
export const SEC_VERTEX = 0, SEC_GEOMETRY = 1, SEC_FRAGMENT = 2, SEC_HOST = 3;

function blkClamp(blocks: Block[], type: number, idx: number): Block | null {
  const ofType = blocks.filter((b) => b.type === type);
  if (ofType.length === 0) return null;
  return ofType[Math.min(idx, ofType.length - 1)];
}
function blkFindName(blocks: Block[], type: number, name: string): Block | null {
  return blocks.find((b) => b.type === type && b.name === name) ?? null;
}

// Texture snapshot table.
interface Tex { valid: boolean; w: number; h: number; z: number; colmode: number; pixels: number[] | null; nam: string; }
const PD_MAX_TEX = 32;
function newTex(): Tex { return { valid: false, w: 0, h: 0, z: 1, colmode: 0, pixels: null, nam: '' }; }

export class PolyState {
  xres = 640; yres = 480;
  mousx = 0; mousy = 0; bstatus = 0; numframes = 0;
  clockScale = 0;
  private startTime = Date.now() / 1000;
}

// RNG — must match C pd_interp / interp.ts exactly.
let g_holdrand = 1 >>> 0;
let g_normstat = false;
let g_srand2 = 0;
function krand(): number {
  let v = g_holdrand | 0;
  v = (Math.imul(v, 214013 * 2) + 2531011 * 2) >>> 0;
  v = v >>> 1;
  g_holdrand = v | 0;
  return v >>> 0;
}
function nrnd(): number { /* not needed in host */ g_normstat = false; return 0; }

export class PolyHostImpl {
  state = new PolyState();
  glbuf = new GLCmdBuf();
  cur_color = [1, 1, 1, 1];
  cur_texcoord = [0, 0, 0, 1];
  cur_normal = [0, 0, 1];
  tex: Tex[] = Array.from({ length: PD_MAX_TEX }, newTex);
  locs: string[] = [];
  blocks: Block[] = [];
  logBuf = '';
  // string arg cache (bit-cast slot -> string)
  strSlots = new Map<number, string>();

  srand(s: number) { g_holdrand = s >>> 0; g_normstat = false; intSrand(s); }

  install(): Host {
    const fns: HostFn[] = [];
    const vars = new Map<string, HostVar>();
    const addFn = (name: string, nParams: number, fn: (n: number, a: number[]) => number) =>
      fns.push({ name, nParams, fn });
    const addVar = (name: string, get: () => number, set: (v: number) => void) =>
      vars.set(name, { get, set });

    // ---- state variables ----
    addVar('XRES', () => this.state.xres, (v) => (this.state.xres = v));
    addVar('YRES', () => this.state.yres, (v) => (this.state.yres = v));
    addVar('MOUSX', () => this.state.mousx, (v) => (this.state.mousx = v));
    addVar('MOUSY', () => this.state.mousy, (v) => (this.state.mousy = v));
    addVar('BSTATUS', () => this.state.bstatus, (v) => (this.state.bstatus = v));
    addVar('NUMFRAMES', () => this.state.numframes, (v) => (this.state.numframes = v));

    // ---- utility functions ----
    addFn('PRINTF', 1, (n, a) => { this.hf_printf(n, a); return 0; });
    addFn('PRINTG', 1, () => 0);
    addFn('KLOCK', 1, (n, a) => this.hf_klock(n, a));
    addFn('SRAND', 0, (n, a) => { if (n >= 1) { this.srand(a[0]); intSrand(a[0]); } return 0; });
    addFn('SLEEP', 0, () => 0);
    addFn('RGB', 0, (n, a) => { if (n < 3) return 0; return this.hf_rgb(a[0], a[1], a[2], 1); });
    addFn('RGBA', 0, (n, a) => { if (n < 4) return 0; return this.hf_rgb(a[0], a[1], a[2], a[3]); });
    addFn('NOISE', 1, (n, a) => this.hf_noise(n, a));
    addFn('PLAYNOTE', 0, () => 0);

    // ---- gl* recording externs ----
    addFn('GLBEGIN', 0, (n, a) => { this.glbuf.begin(n >= 1 ? a[0] : 0); return 0; });
    addFn('GLEND', 0, () => { this.glbuf.end(); return 0; });
    addFn('GLVERTEX', 0, (n, a) => {
      const x = n >= 1 ? a[0] : 0, y = n >= 2 ? a[1] : 0, z = n >= 3 ? a[2] : 0, w = n >= 4 ? a[3] : 1;
      this.glbuf.vertex(x, y, z, w); return 0;
    });
    addFn('GLCOLOR', 0, (n, a) => {
      this.cur_color = [n >= 1 ? a[0] : 0, n >= 2 ? a[1] : 0, n >= 3 ? a[2] : 0, n >= 4 ? a[3] : 1];
      const c = this.glbuf.push(); c.op = GLCMD.COLOR; c.a = this.cur_color[0]; c.b = this.cur_color[1]; c.c = this.cur_color[2]; c.d = this.cur_color[3]; return 0;
    });
    addFn('GLTEXCOORD', 0, (n, a) => {
      this.cur_texcoord = [n >= 1 ? a[0] : 0, n >= 2 ? a[1] : 0, n >= 3 ? a[2] : 0, n >= 4 ? a[3] : 1];
      const c = this.glbuf.push(); c.op = GLCMD.TEXCOORD; c.a = this.cur_texcoord[0]; c.b = this.cur_texcoord[1]; c.c = this.cur_texcoord[2]; c.d = this.cur_texcoord[3]; return 0;
    });
    addFn('GLNORMAL', 0, (n, a) => {
      this.cur_normal = [n >= 1 ? a[0] : 0, n >= 2 ? a[1] : 0, n >= 3 ? a[2] : 0];
      const c = this.glbuf.push(); c.op = GLCMD.NORMAL; c.a = this.cur_normal[0]; c.b = this.cur_normal[1]; c.c = this.cur_normal[2]; return 0;
    });
    addFn('GLCLEAR', 0, () => { this.glbuf.clear(); return 0; });
    addFn('GLPUSHMATRIX', 0, () => { const c = this.glbuf.push(); c.op = GLCMD.PUSHMATRIX; return 0; });
    addFn('GLPOPMATRIX', 0, () => { const c = this.glbuf.push(); c.op = GLCMD.POPMATRIX; return 0; });
    addFn('GLTRANSLATE', 0, (n, a) => { const c = this.glbuf.push(); c.op = GLCMD.TRANSLATE; c.a = n >= 1 ? a[0] : 0; c.b = n >= 2 ? a[1] : 0; c.c = n >= 3 ? a[2] : 0; return 0; });
    addFn('GLROTATE', 0, (n, a) => { const c = this.glbuf.push(); c.op = GLCMD.ROTATE; c.a = n >= 1 ? a[0] : 0; c.b = n >= 2 ? a[1] : 0; c.c = n >= 3 ? a[2] : 0; c.d = n >= 4 ? a[3] : 0; return 0; });
    addFn('GLSCALE', 0, (n, a) => { const c = this.glbuf.push(); c.op = GLCMD.SCALE; c.a = n >= 1 ? a[0] : 1; c.b = n >= 2 ? a[1] : 1; c.c = n >= 3 ? a[2] : 1; return 0; });
    addFn('GLENABLE', 0, (n, a) => { const c = this.glbuf.push(); c.op = GLCMD.ENABLE; c.mode = n >= 1 ? a[0] : 0; return 0; });
    addFn('GLDISABLE', 0, (n, a) => { const c = this.glbuf.push(); c.op = GLCMD.DISABLE; c.mode = n >= 1 ? a[0] : 0; return 0; });
    addFn('GLQUAD', 0, (n, a) => { const c = this.glbuf.push(); c.op = GLCMD.QUAD; c.a = n >= 1 ? a[0] : 0; return 0; });
    addFn('GLLINEWIDTH', 0, (n, a) => { const c = this.glbuf.push(); c.op = GLCMD.LINEWIDTH; c.a = n >= 1 ? a[0] : 1; return 0; });
    // C records GLCULLFACE via rh_glCullFace but its parser routes the call to
    // a base no-op extern, so the command never reaches the GLCmd buffer. Match
    // that de-facto behavior for cross-backend parity.
    addFn('GLCULLFACE', 0, () => 0);
    addFn('GLFRONTFACE', 0, () => 0);
    addFn('GLVIEWPORT', 0, (n, a) => { const c = this.glbuf.push(); c.op = GLCMD.VIEWPORT; c.a = n >= 1 ? a[0] : 0; c.b = n >= 2 ? a[1] : 0; return 0; });
    addFn('GLMATRIXMODE', 0, (n, a) => { const c = this.glbuf.push(); c.op = GLCMD.MATRIXMODE; c.mode = n >= 1 ? a[0] : 0; return 0; });
    addFn('GLLOADIDENTITY', 0, () => { const c = this.glbuf.push(); c.op = GLCMD.LOADIDENTITY; return 0; });
    addFn('GLORTHO', 0, (n, a) => { const c = this.glbuf.push(); c.op = GLCMD.ORTHO; c.a = n >= 1 ? a[0] : 0; c.b = n >= 2 ? a[1] : 0; c.c = n >= 3 ? a[2] : 0; c.d = n >= 4 ? a[3] : 0; return 0; });

    // ---- textures & capture ----
    addFn('GLSETTEX', 0, (n, a) => this.hf_glSetTex(n, a));
    addFn('GLGETTEX', 0, (n, a) => 0);
    addFn('GLBINDTEXTURE', 0, (n, a) => { const c = this.glbuf.push(); c.op = GLCMD.BINDTEX; c.a = n >= 1 ? a[0] : 0; return 0; });
    addFn('GLACTIVETEXTURE', 0, (n, a) => { const c = this.glbuf.push(); c.op = GLCMD.ACTIVETEX; c.a = (n >= 1 ? a[0] : 0) & 3; return 0; });
    addFn('GLCAPTURE', 0, (n, a) => this.hf_glCapture(n, a));
    addFn('GLCAPTUREEND', 0, (n, a) => { const c = this.glbuf.push(); c.op = GLCMD.CAPTUREEND; c.a = n >= 1 ? a[0] : 0; return 0; });

    // ---- shaders & uniforms ----
    addFn('GLSETSHADER', 0, (n, a) => this.hf_glSetShader(n, a));
    addFn('GLGETUNIFORMLOC', 0, (n, a) => this.hf_glGetUniformLoc(n, a));
    ['GLUNIFORM1F', 'GLUNIFORM2F', 'GLUNIFORM3F', 'GLUNIFORM4F'].forEach((nm, i) =>
      addFn(nm, 0, (n, a) => this.hf_uniformScalar(PD_UNI_F, i + 1, n, a)));
    ['GLUNIFORM1I', 'GLUNIFORM2I', 'GLUNIFORM3I', 'GLUNIFORM4I'].forEach((nm, i) =>
      addFn(nm, 0, (n, a) => this.hf_uniformScalar(PD_UNI_I, i + 1, n, a)));
    ['GLUNIFORM1FV', 'GLUNIFORM2FV', 'GLUNIFORM3FV', 'GLUNIFORM4FV'].forEach((nm, i) =>
      addFn(nm, 0, (n, a) => this.hf_uniformArray(PD_UNI_F, i + 1, n, a)));
    ['GLUNIFORM1IV', 'GLUNIFORM2IV', 'GLUNIFORM3IV', 'GLUNIFORM4IV'].forEach((nm, i) =>
      addFn(nm, 0, (n, a) => this.hf_uniformArray(PD_UNI_I, i + 1, n, a)));

    // ---- matrix / misc ----
    addFn('GLUPERSPECTIVE', 0, (n, a) => {
      const c0 = this.glbuf.push(); c0.op = GLCMD.MATRIXMODE; c0.mode = 1;
      const c1 = this.glbuf.push(); c1.op = GLCMD.LOADIDENTITY;
      const c2 = this.glbuf.push(); c2.op = GLCMD.PERSPECTIVE;
      c2.a = n >= 1 ? a[0] : 0; c2.b = n >= 2 ? a[1] : 1; c2.c = n >= 3 ? a[2] : 0.1; c2.d = n >= 4 ? a[3] : 1000; return 0;
    });
    addFn('GLULOOKAT', 0, (n, a) => this.hf_glLookAt(n, a));
    addFn('GLMULTMATRIX', 0, (n, a) => { const c = this.glbuf.push(); c.op = GLCMD.MULTMATRIX; c.s = (a[0] as unknown as number[]); return 0; });
    addFn('SETFOV', 0, (n, a) => { const c = this.glbuf.push(); c.op = GLCMD.SETFOV; c.a = n >= 1 ? a[0] : 0; return n >= 1 ? a[0] : 0; });
    addFn('GLKLOCKSTART', 0, () => 0);
    addFn('GLKLOCKELAPSED', 0, () => 0);

    // ---- GL_ constants as vars ----
    const c = (v: number) => () => v;
    addVar('GL_POINTS', c(PDGL.POINTS));
    addVar('GL_LINES', c(PDGL.LINES));
    addVar('GL_LINE_LOOP', c(PDGL.LINE_LOOP));
    addVar('GL_LINE_STRIP', c(PDGL.LINE_STRIP));
    addVar('GL_TRIANGLES', c(PDGL.TRIANGLES));
    addVar('GL_TRIANGLE_STRIP', c(PDGL.TRIANGLE_STRIP));
    addVar('GL_TRIANGLE_FAN', c(PDGL.TRIANGLE_FAN));
    addVar('GL_QUADS', c(PDGL.QUADS));
    addVar('GL_QUAD_STRIP', c(PDGL.QUAD_STRIP));
    addVar('GL_POLYGON', c(PDGL.POLYGON));
    addVar('GL_DEPTH_TEST', c(0x0b71));
    addVar('GL_NONE', c(0));
    addVar('GL_FRONT', c(0x0404));
    addVar('GL_BACK', c(0x0405));
    addVar('GL_FRONT_AND_BACK', c(0x0408));
    addVar('GL_TEXTURE_1D', c(0x0de0));
    addVar('GL_TEXTURE_2D', c(0x0de1));
    addVar('GL_TEXTURE_3D', c(0x806f));
    addVar('GL_TEXTURE_CUBE_MAP', c(0x8513));
    for (let i = 0; i < 8; i++) addVar('GL_TEXTURE' + i, c(0x84c0 + i));
    // KGL_* colmode constants
    const kgl: [string, number][] = [
      ['KGL_BGRA32', 0], ['KGL_CHAR', 1], ['KGL_SHORT', 2], ['KGL_INT', 3], ['KGL_FLOAT', 4], ['KGL_VEC4', 5],
      ['KGL_LINEAR', 0], ['KGL_NEAREST', 1 << 4], ['KGL_MIPMAP', 2 << 4], ['KGL_MIPMAP2', 3 << 4], ['KGL_MIPMAP1', 4 << 4], ['KGL_MIPMAP0', 5 << 4],
      ['KGL_REPEAT', 0], ['KGL_MIRRORED_REPEAT', 1 << 8], ['KGL_CLAMP', 2 << 8], ['KGL_CLAMP_TO_EDGE', 3 << 8],
    ];
    for (const [nm, v] of kgl) addVar(nm, c(v));

    return { fns, vars, strings: this.strSlots };
  }

  // ---- host function impls ----
  private strArg(a: number[], idx: number): string | null {
    const slot = a[idx] | 0;
    return this.strSlots.get(slot) ?? null;
  }
  registerString(slot: number, s: string) { this.strSlots.set(slot, s); }

  private hf_printf(n: number, a: number[]): void {
    if (n < 1) return;
    const fmt = this.strArg(a, 0);
    if (!fmt) return;
    let out = '';
    let ai = 1;
    for (let p = 0; p < fmt.length; p++) {
      if (fmt[p] !== '%') { out += fmt[p]; continue; }
      const spec = fmt[p + 1]; p++;
      if (spec === undefined) break;
      if (ai < n) {
        const v = a[ai++];
        if (spec === 'f') out += v.toFixed(6);
        else if (spec === 'g' || spec === 'G') out += String(v);
        else if (spec === 'd') out += String(Math.trunc(v));
        else if (spec === 'x') out += (Math.trunc(v) >>> 0).toString(16);
        else if (spec === 'c') out += String.fromCharCode(Math.trunc(v) & 0xff);
        else if (spec === '%') { out += '%'; ai--; }
        else { out += '%' + spec; ai--; }
      } else { out += '%' + spec; }
    }
    this.logBuf += out;
  }
  private hf_klock(n: number, a: number[]): number {
    if (this.state.clockScale > 0) return this.state.numframes * this.state.clockScale;
    return Date.now() / 1000 - this.state.startTime;
  }
  private hf_rgb(r: number, g: number, b: number, al: number): number {
    const rc = r < 0 ? 0 : r > 255 ? 255 : Math.trunc(r);
    const gc = g < 0 ? 0 : g > 255 ? 255 : Math.trunc(g);
    const bc = b < 0 ? 0 : b > 255 ? 255 : Math.trunc(b);
    const ac = al < 0 ? 0 : al > 255 ? 255 : Math.trunc(al);
    return (ac << 24) | (rc << 16) | (gc << 8) | bc;
  }
  private hf_noise(n: number, a: number[]): number {
    let s = 0;
    for (let i = 0; i < n; i++) s = s * 12.9898 + a[i] * 78.233;
    s = Math.sin(s) * 43758.5453;
    return s - Math.floor(s);
  }
  private hf_glSetTex(n: number, a: number[]): number {
    const tex = a[0] | 0;
    if (tex < 0 || tex >= PD_MAX_TEX) return -1;
    // file form: glsettex(tex, "file"[, colmode])
    const file = this.strArg(a, 1);
    if (file !== null) {
      const colmode = n >= 3 ? (a[2] | 0) : (8 + 32); // KGL_MIPMAP | KGL_REPEAT
      this.tex[tex] = { valid: true, w: 32, h: 32, z: 1, colmode, pixels: null, nam: file };
      const c = this.glbuf.push();
      c.op = GLCMD.SETTEXDATA; c.mode = colmode; c.a = tex; c.b = 32; c.c = 32; c.d = 1; c.s = null;
      return 0;
    }
    // array form: glsettex(tex, &arr, w[, h[, z]], coltype)
    const arr = a[1] as unknown as number[] | null;
    if (arr) {
      const w = a[2] | 0, h = n >= 4 ? (a[3] | 0) : 1, z = n >= 5 ? (a[4] | 0) : 1;
      const colmode = a[n - 1] | 0;
      if (w < 1 || h < 1 || z < 1) return -1;
      this.tex[tex] = { valid: true, w, h, z, colmode, pixels: arr.slice(), nam: '' };
      const c = this.glbuf.push();
      c.op = GLCMD.SETTEXDATA; c.mode = colmode; c.a = tex; c.b = w; c.c = h; c.d = z; c.s = null;
      return 0;
    }
    return -1;
  }
  private hf_glCapture(n: number, a: number[]): number {
    const c = this.glbuf.push();
    if (n === 1) { c.op = GLCMD.CAPTURE; c.a = -1; c.b = 0; c.c = 0; c.mode = 0; }
    else if (n === 4) { c.op = GLCMD.CAPTURE; c.a = a[0]; c.b = a[1]; c.c = a[2]; c.mode = a[3] | 0; }
    return 0;
  }
  private hf_glSetShader(n: number, a: number[]): number {
    let vi: Block | null = null, fi: Block | null = null;
    if (n === 1) { fi = blkClamp(this.blocks, SEC_FRAGMENT, a[0] | 0); }
    else if (n >= 2) {
      const vn = this.strArg(a, 0), fn = this.strArg(a, 1);
      vi = vn ? blkFindName(this.blocks, SEC_VERTEX, vn) : null;
      fi = fn ? blkFindName(this.blocks, SEC_FRAGMENT, fn) : null;
      if (!vi) vi = blkClamp(this.blocks, SEC_VERTEX, 0);
      if (!fi) fi = blkClamp(this.blocks, SEC_FRAGMENT, 0);
    } else { vi = blkClamp(this.blocks, SEC_VERTEX, 0); fi = blkClamp(this.blocks, SEC_FRAGMENT, 0); }
    const c = this.glbuf.push();
    c.op = GLCMD.SETSHADER;
    c.a = vi ? vi.src.length : 0; // placeholder; replayer keyed by content below
    c.b = fi ? fi.src.length : 0;
    (c as GLCmd & { vsrc?: string; fsrc?: string }).vsrc = vi?.src ?? '';
    (c as GLCmd & { vsrc?: string; fsrc?: string }).fsrc = fi?.src ?? '';
    return 0;
  }
  private hf_glGetUniformLoc(n: number, a: number[]): number {
    const name = this.strArg(a, 0);
    if (!name) return 0;
    let id = this.locs.indexOf(name);
    if (id < 0) { id = this.locs.length; this.locs.push(name); }
    const c = this.glbuf.push(); c.op = GLCMD.UNIFORMLOC; c.a = id; c.s = name;
    return id;
  }
  private hf_uniformScalar(kind: number, count: number, n: number, a: number[]): number {
    const c = this.glbuf.push();
    c.op = GLCMD.UNIFORM;
    c.mode = ((kind & 0xff) << 24) | ((count & 0xff) << 16) | (count & 0xffff);
    c.a = n >= 1 ? a[0] : 0; c.b = n >= 2 ? a[1] : 0; c.c = n >= 3 ? a[2] : 0; c.d = n >= 4 ? a[3] : 0;
    c.s = null; return 0;
  }
  private hf_uniformArray(kind: number, comps: number, n: number, a: number[]): number {
    const id = a[0] | 0;
    const count = n >= 2 ? (a[1] | 0) : 0;
    const arr = (n >= 3 ? a[2] : null) as unknown as number[] | null;
    const total = Math.max(0, count) * comps;
    const buf: number[] = new Array(total).fill(0);
    if (arr) for (let i = 0; i < count && i < arr.length; i++) buf[i] = arr[i];
    const c = this.glbuf.push();
    c.op = GLCMD.UNIFORM;
    c.mode = ((kind & 0xff) << 24) | ((comps & 0xff) << 16) | (count & 0xffff);
    c.a = id; c.s = buf; return 0;
  }
  private hf_glLookAt(n: number, a: number[]): number {
    if (n < 9) return 0;
    const m = mat4LookAt(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
    const c = this.glbuf.push(); c.op = GLCMD.MULTMATRIX; c.s = Array.from(m);
    return 0;
  }
}
