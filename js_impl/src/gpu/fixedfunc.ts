// fixedfunc.ts — replay a GLCmd buffer into draw batches (the fixed-function
// immediate-mode layer). This is the dependency-free, testable core of the JS
// GPU pipeline (M7): given a GLCmdBuf (already verified identical to the C
// reference in tests/xbackend.test.ts), it assembles geometry between
// GLBEGIN/GLEND pairs, applies the active matrix stack, and emits a list of
// DrawBatch objects. A WebGL2 renderer (renderer.ts) later draws these.
import { GLCmdBuf, GLCMD, PDGL } from '../host/glcmd.ts';
import type { GLCmd } from '../host/glcmd.ts';
import { MatrixStack } from './matrix.ts';
import type { Mat4 } from './matrix.ts';

export interface Vertex {
  x: number; y: number; z: number; w: number;
  r: number; g: number; b: number; a: number;
  nx: number; ny: number; nz: number;
  s: number; t: number; p: number; q: number;
}

export interface Uniform {
  loc: string;   // uniform name resolved from glUniformLoc
  kind: number;  // PD_UNI_F | PD_UNI_I
  v: number[];   // float payload (or int as number) — already flattened
}

export interface DrawBatch {
  mode: number; // GL primitive (GLCMD.BEGIN mode)
  verts: Vertex[];
  modelview: Mat4;
  projection: Mat4;
  // material/shader state at draw time
  shaderV: string | null;
  shaderF: string | null;
  texUnit: number;
  tex: number;
  // per-unit texture bindings at draw time (glactivetexture+glbindtexture):
  // index = unit, value = tex id (-1 = unbound). Multitexture fragment shaders
  // (ken/texture.pss tex0/tex1/tex2) sample one texture per unit.
  texUnits: number[];
  useTex: boolean;       // sample texture `tex` at (s,t) instead of vertex color
  pointSize: number; // gl_PointSize for GL_POINTS
  lineWidth: number; // glLineWidth for GL_LINES / GL_LINE_STRIP / GL_LINE_LOOP
  // rendering state at draw time
  depthTest: boolean;
  blend: boolean;
  blendSrc: number;
  blendDst: number;
  cullFace: number;      // 0 = none, else GL_FRONT/GL_BACK
  clearColor: [number, number, number, number];
  uniforms: Uniform[];   // resolved uniforms applying to this batch
  // Set on a synthetic batch emitted for GLCMD_CLEAR. The renderer clears the
  // framebuffer (and depth, if enabled) at this point in the stream.
  clear?: [number, number, number, number];
  // When >= 0: this geometry is part of a glcapture()..glcaptureend() range
  // and must be drawn to the OFFSCREEN capture buffer (never the main
  // framebuffer), mirroring C's FBO render-target switch.
  captureTarget?: number;
  // Source GLCmd range [cmdStart, cmdEnd] this batch originates from — the
  // anchor used by the C/JS trace comparator to align batches across the two
  // renderers (C merges BEGIN/END sections, JS keeps them separate).
  cmdStart?: number;
  cmdEnd?: number;
}

export class FixedFunc {
  stack = new MatrixStack();
  private verts: Vertex[] = [];
  private inBegin = false;
  private cur = blankVert();
  batches: DrawBatch[] = [];
  // shader/capture state (lightweight; expanded by renderer as needed)
  shaderV: string | null = null;
  shaderF: string | null = null;
  // default shader from the script's first @v/@f block (C render_main.c)
  defaultShaderV: string | null = null;
  defaultShaderF: string | null = null;
  activeTex = 0;
  boundTex = 0;
  private texUnits: number[] = [-1, -1, -1, -1];
  texBound = false;   // set by glbindtexture; drives texture sampling
  pointSize = 1.0;
  lineWidth = 1.0;
  // rendering state (mirrors gl_renderer.c). Depth test is OFF by default here:
  // the software golden reference (pyref / balls_f5.png) is painter-order, while
  // the WebGL2 backend applies its own default (ON, matching C GL). A batch sets
  // it true only when the script explicitly calls glEnable(DEPTH_TEST).
  depthTest = false;
  blend = false;
  blendSrc = 0x0302; // GL_SRC_ALPHA
  blendDst = 0x0303; // GL_ONE_MINUS_SRC_ALPHA
  cullFace = 0;      // 0 = off, else GL_FRONT(0x0404)/GL_BACK(0x0405)
  clearColor: [number, number, number, number] = [0, 0, 0, 1];
  width = 640;
  height = 480;
  // uniform plumbing
  private uniLoc: Map<string, string> = new Map(); // name -> uniform name
  // Uniforms are PERSISTENT GL program state (mirrors gl_renderer.c
  // GLCMD_UNIFORM -> glUniform*): a value set once (e.g. drawcone2.pss sets
  // "mode" a single time before its drawcone loop) applies to every later
  // batch until overwritten. Each emitted batch snapshots the current state.
  private uniState: Map<string, Uniform> = new Map();
  // texture uploads (glsettex array form) and offscreen capture ops
  texData: { id: number; w: number; h: number; z: number; colmode: number; pixels: number[] | null }[] = [];
  captures: { afterIndex: number; tex: number }[] = [];
  private capTex = -1;
  // inside glcapture()..glcaptureend() range: geometry must go offscreen
  private capturing = false;
  // fullscreen quad mode (glquad)
  quadMode = 0;
  // current GLCmd index being replayed (trace anchor: batches record the
  // command range they originate from)
  private cmdIdx = 0;
  // GLCmd index where the current BEGIN..END section started
  private batchCmdStart = 0;

  constructor(width = 640, height = 480, defaultFovy = 0) {
    this.width = width; this.height = height; this.defaultFovy = defaultFovy;
    this.applyDefaultProjection();
  }

  private defaultFovy = 0;

  private applyDefaultProjection(): void {
    this.stack.matrixMode(1);
    this.stack.loadIdentity();
    const fovy = this.defaultFovy > 0 ? this.defaultFovy : 45;
    const aspect = this.width / this.height;
    this.stack.perspective(fovy, aspect, 0.1, 1000);
    this.stack.matrixMode(0);
  }

  // Per-frame reset. Mirrors C gl_renderer.c pd_gl_renderer_render: only the
  // matrix stack + pending geometry are frame-local and re-initialized here.
  // The rest of the GL state (cull, texture bindings, shaders, blend, depth,
  // uniforms, clear color, uploaded textures, capture target) is CONTEXT state
  // that persists across frames — commands issued on an early frame (e.g.
  // under `numframes==0`, like curvybuild.pss's glcullface(GL_FRONT) or
  // glsettex) stay live for every later frame. A fresh FixedFunc (constructor
  // field defaults) is still clean, so single-shot callers (tests) are
  // unaffected; only multi-frame replays now carry state forward.
  reset(): void {
    this.stack = new MatrixStack();
    this.applyDefaultProjection();
    this.verts = [];
    this.inBegin = false;
    this.cur = blankVert();
    this.batches = [];
    // capture *events* are frame-local: the snapshot point indexes THIS
    // frame's batch stream. The capture texture target (capTex) persists.
    this.captures = [];
    this.capturing = false;
    this.cmdIdx = 0;
    this.batchCmdStart = 0;
  }

  private pushVert(): void {
    this.verts.push({ ...this.cur });
  }

  replay(g: GLCmdBuf): DrawBatch[] {
    this.reset();
    for (let i = 0; i < g.cmds.length; i++) {
      this.cmdIdx = i;
      this.exec(g.cmds[i] as GLCmd);
    }
    return this.batches;
  }

  // Emit the current vertex list as a batch (used by BEGIN/END and QUAD).
  private emitBatch(mode: number, verts: Vertex[]): void {
    const b: DrawBatch = {
      mode,
      verts: verts.slice(),
      modelview: this.stack.modelView.slice() as Mat4,
      projection: this.stack.projection.slice() as Mat4,
      shaderV: this.shaderV, shaderF: this.shaderF,
      texUnit: this.activeTex, tex: this.boundTex,
      texUnits: this.texUnits.slice(),
      useTex: this.texBound,
      pointSize: this.pointSize,
      lineWidth: this.lineWidth,
      depthTest: this.depthTest,
      blend: this.blend,
      blendSrc: this.blendSrc, blendDst: this.blendDst,
      cullFace: this.cullFace,
      clearColor: this.clearColor.slice() as [number, number, number, number],
      uniforms: [...this.uniState.values()],
      captureTarget: this.capturing ? 1 : undefined,
      cmdStart: this.batchCmdStart,
      cmdEnd: this.cmdIdx,
    };
    this.batches.push(b);
  }

  exec(c: GLCmd): void {
    switch (c.op) {
      case GLCMD.BEGIN:
        this.inBegin = true;
        this.verts = [];
        this.beginMode = c.mode;
        this.batchCmdStart = this.cmdIdx;
        break;
      case GLCMD.END:
        if (this.inBegin) this.emitBatch(this.beginMode, this.verts);
        this.inBegin = false;
        break;
      case GLCMD.QUAD: {
        // Fullscreen quad in NDC (mirrors C gl_renderer draw_quad): bypass the
        // modelview/projection entirely — vertices are emitted already in
        // clip space so the rasterizer just maps them to the full frame.
        this.quadMode = c.a;
        const mk = (x: number, y: number): Vertex => {
          const v = { ...this.cur }; v.x = x; v.y = y; v.z = 0; v.w = 1; return v;
        };
        const v00 = mk(-1, -1), v10 = mk(1, -1), v11 = mk(1, 1), v01 = mk(-1, 1);
        v00.s = 0; v00.t = 0; v10.s = 1; v10.t = 0; v11.s = 1; v11.t = 1; v01.s = 0; v01.t = 1;
        // identity MVP so the NDC coords pass through unchanged
        const id = new Float64Array(16);
        for (let k = 0; k < 16; k += 5) id[k] = 1;
        const b: DrawBatch = {
          mode: PDGL.TRIANGLES,
          verts: [v00, v10, v11, v00, v11, v01],
          modelview: id.slice() as Mat4,
          projection: id.slice() as Mat4,
          shaderV: this.shaderV, shaderF: this.shaderF,
          texUnit: this.activeTex, tex: this.boundTex,
          texUnits: this.texUnits.slice(),
          useTex: this.texBound,
          pointSize: this.pointSize,
          lineWidth: this.lineWidth,
          depthTest: this.depthTest,
          blend: this.blend,
          blendSrc: this.blendSrc, blendDst: this.blendDst,
          cullFace: 0,
          clearColor: this.clearColor.slice() as [number, number, number, number],
          uniforms: [...this.uniState.values()],
          captureTarget: this.capturing ? 1 : undefined,
          cmdStart: this.cmdIdx,
          cmdEnd: this.cmdIdx,
        };
        this.batches.push(b);
        break;
      }
      case GLCMD.VERTEX:
        this.cur.x = c.a; this.cur.y = c.b; this.cur.z = c.c; this.cur.w = c.d ?? 1;
        this.pushVert();
        break;
      case GLCMD.COLOR:
        this.cur.r = c.a; this.cur.g = c.b; this.cur.b = c.c; this.cur.a = c.d ?? 1;
        break;
      case GLCMD.NORMAL:
        this.cur.nx = c.a; this.cur.ny = c.b; this.cur.nz = c.c;
        break;
      case GLCMD.TEXCOORD:
        this.cur.s = c.a; this.cur.t = c.b; this.cur.p = c.c; this.cur.q = c.d ?? 1;
        break;
      case GLCMD.PUSHMATRIX: this.stack.push(); break;
      case GLCMD.POPMATRIX: this.stack.pop(); break;
      case GLCMD.TRANSLATE: this.stack.translate(c.a, c.b, c.c); break;
      case GLCMD.ROTATE: this.stack.rotate(c.a, c.b, c.c, c.d); break;
      case GLCMD.SCALE: this.stack.scale(c.a, c.b, c.c); break;
      case GLCMD.MATRIXMODE: this.stack.matrixMode(c.mode); break;
      case GLCMD.LOADIDENTITY: this.stack.loadIdentity(); break;
      case GLCMD.PERSPECTIVE: this.stack.perspective(c.a, c.b, c.c, c.d); break;
      case GLCMD.SETFOV: {
        this.stack.matrixMode(1);
        this.stack.loadIdentity();
        this.stack.perspective(c.a, this.width / this.height, 0.1, 1000);
        this.stack.matrixMode(0);
        break;
      }
      case GLCMD.ORTHO: this.stack.ortho(c.a, c.b, c.c, c.d); break;
      case GLCMD.MULTMATRIX: if (c.s) this.stack.mult(c.s as unknown as Mat4); break;
      case GLCMD.VIEWPORT:
        // renderer size is fixed by caller; record nothing extra
        break;
      case GLCMD.CLEAR: {
        // Mirrors C gl_renderer GLCMD_CLEAR: flush pending geometry, then clear
        // at this point in the stream with the command's color. We emit a
        // synthetic batch the renderer understands as a clear marker.
        const clr: [number, number, number, number] = [c.a, c.b, c.c, c.d];
        this.clearColor = clr;
        const b: DrawBatch = {
          mode: -1,
          verts: [],
          modelview: this.stack.modelView.slice() as Mat4,
          projection: this.stack.projection.slice() as Mat4,
          shaderV: this.shaderV, shaderF: this.shaderF,
          texUnit: this.activeTex, tex: this.boundTex,
          useTex: false,
          pointSize: this.pointSize,
          lineWidth: this.lineWidth,
          depthTest: this.depthTest,
          blend: this.blend,
          blendSrc: this.blendSrc, blendDst: this.blendDst,
          cullFace: this.cullFace,
          clearColor: this.clearColor.slice() as [number, number, number, number],
          uniforms: [],
          clear: clr,
          cmdStart: this.cmdIdx,
          cmdEnd: this.cmdIdx,
        };
        this.batches.push(b);
        break;
      }
      case GLCMD.SETSHADER:
        this.shaderV = (c.s as string) ?? null;
        this.shaderF = (c.s2 as string) ?? null;
        break;
      case GLCMD.SETTEXDATA:
        this.texData.push({ id: c.a, w: c.b, h: c.c, z: c.d, colmode: c.mode, pixels: c.s as number[] | null });
        // glsettex binds the texture to the current active unit immediately
        // (gl_renderer.c SETTEXDATA does glBindTexture(target, tex_obj) after
        // upload — scripts like mipmap.pss never call glbindtexture).
        if (this.activeTex >= 0 && this.activeTex < 4) this.texUnits[this.activeTex] = c.a;
        break;
      case GLCMD.BINDTEX:
        this.boundTex = c.a;
        this.texBound = true;
        // glbindtexture binds to the CURRENT active unit (mirrors
        // gl_renderer.c GLCMD_BINDTEX -> glActiveTexture(GL_TEXTURE0+unit))
        if (this.activeTex >= 0 && this.activeTex < 4) this.texUnits[this.activeTex] = c.a;
        break;
      case GLCMD.ACTIVETEX: this.activeTex = (c.a & 3); break;
      case GLCMD.POINTSIZE: this.pointSize = c.a; break;
      case GLCMD.LINEWIDTH: this.lineWidth = c.a; break;
      case GLCMD.ENABLE:
        if (c.mode === 0x0B71) this.depthTest = true; // GL_DEPTH_TEST
        else if (c.mode === 0x0BE2) this.blend = true; // GL_BLEND
        break;
      case GLCMD.DISABLE:
        if (c.mode === 0x0B71) this.depthTest = false;
        else if (c.mode === 0x0BE2) this.blend = false;
        break;
      case GLCMD.BLENDFUNC:
        this.blend = true;
        this.blendSrc = (c.mode >> 16) & 0xFFFF;
        this.blendDst = c.mode & 0xFFFF;
        break;
      case GLCMD.CULLFACE:
        this.cullFace = c.mode;
        break;
      case GLCMD.UNIFORMLOC: {
        // name string -> uniform name; store for subsequent UNIFORM calls
        const name = c.s as string;
        const slot = String(c.a);
        this.uniLoc.set(slot, name);
        break;
      }
      case GLCMD.UNIFORM: {
        // c.mode = kind<<24 | comps<<16 | nelem, c.s = float array, c.a = loc slot.
        // Mirrors gl_renderer.c GLCMD_UNIFORM: scalar form packs the VALUES in
        // b/c/d (a is the loc id, NOT a value; k>=2 shares d).
        const name = this.uniLoc.get(String(c.a)) ?? ('u' + c.a);
        const nelem = c.mode & 0xffff;
        const arr: number[] = Array.isArray(c.s)
          ? (c.s as number[]).slice()
          : Array.from({ length: Math.max(0, nelem) }, (_, k) => (k === 0 ? c.b : k === 1 ? c.c : c.d));
        this.uniState.set(name, { loc: name, kind: (c.mode >> 24) & 0xff, v: arr });
        break;
      }
      case GLCMD.CAPTURE:
        // glcapture() / glcapture(tex,w,h,col): switch to the offscreen
        // capture target; geometry until glcaptureend() is drawn there.
        this.capturing = true;
        if (c.a >= 0) this.capTex = c.a;
        break;
      case GLCMD.CAPTUREEND:
        this.capturing = false;
        this.capTex = c.a;
        this.captures.push({ afterIndex: this.batches.length, tex: this.capTex });
        break;
      default:
        break;
    }
  }

  private beginMode = 0;
}

function blankVert(): Vertex {
  return { x: 0, y: 0, z: 0, w: 1, r: 1, g: 1, b: 1, a: 1, nx: 0, ny: 0, nz: 1, s: 0, t: 0, p: 0, q: 1 };
}

// transform a vertex by combined matrix (column-major), returns [x,y,z,w].
export function transform(m: Mat4, v: Vertex): [number, number, number, number] {
  const x = v.x, y = v.y, z = v.z, w = v.w;
  return [
    m[0] * x + m[4] * y + m[8] * z + m[12] * w,
    m[1] * x + m[5] * y + m[9] * z + m[13] * w,
    m[2] * x + m[6] * y + m[10] * z + m[14] * w,
    m[3] * x + m[7] * y + m[11] * z + m[15] * w,
  ];
}
