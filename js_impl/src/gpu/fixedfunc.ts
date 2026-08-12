// fixedfunc.ts — replay a GLCmd buffer into draw batches (the fixed-function
// immediate-mode layer). This is the dependency-free, testable core of the JS
// GPU pipeline (M7): given a GLCmdBuf (already verified identical to the C
// reference in tests/xbackend.test.ts), it assembles geometry between
// GLBEGIN/GLEND pairs, applies the active matrix stack, and emits a list of
// DrawBatch objects. A WebGL2 renderer (renderer.ts) later draws these.
import { GLCmdBuf, GLCMD } from '../host/glcmd.ts';
import type { GLCmd } from '../host/glcmd.ts';
import { MatrixStack, mat4Identity, mat4Mul } from './matrix.ts';
import type { Mat4 } from './matrix.ts';

export interface Vertex {
  x: number; y: number; z: number; w: number;
  r: number; g: number; b: number; a: number;
  nx: number; ny: number; nz: number;
  s: number; t: number; p: number; q: number;
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
  activeTex = 0;
  boundTex = 0;

  constructor(width = 640, height = 480, defaultFovy = 0) {
    this.width = width; this.height = height; this.defaultFovy = defaultFovy;
    // Seed the projection matrix with the renderer default (mirrors
    // pd_gl_renderer_create: perspective fovy, aspect w/h, near 0.1, far 1000).
    // Scripts that call gluPerspective overwrite this via PERSPECTIVE commands.
    this.applyDefaultProjection();
  }

  private width = 640;
  private height = 480;
  private defaultFovy = 0;

  private applyDefaultProjection(): void {
    if (this.defaultFovy > 0) {
      this.stack.matrixMode(1);
      this.stack.loadIdentity();
      const fovy = this.defaultFovy, aspect = this.width / this.height;
      this.stack.perspective(fovy, aspect, 0.1, 1000);
      this.stack.matrixMode(0);
    }
  }

  reset(): void {
    this.stack = new MatrixStack();
    this.applyDefaultProjection();
    this.verts = [];
    this.inBegin = false;
    this.cur = blankVert();
    this.batches = [];
    this.shaderV = this.shaderF = null;
    this.activeTex = 0; this.boundTex = 0;
  }

  private pushVert(): void {
    this.verts.push({ ...this.cur });
  }

  replay(g: GLCmdBuf): DrawBatch[] {
    this.reset();
    for (const c of g.cmds as GLCmd[]) this.exec(c);
    return this.batches;
  }

  exec(c: GLCmd): void {
    switch (c.op) {
      case GLCMD.BEGIN:
        this.inBegin = true;
        this.verts = [];
        this.beginMode = c.mode;
        break;
      case GLCMD.END:
        if (this.inBegin) {
          this.batches.push({
            mode: this.beginMode,
            verts: this.verts.slice(),
            modelview: this.stack.modelView.slice() as Mat4,
            projection: this.stack.projection.slice() as Mat4,
            shaderV: this.shaderV, shaderF: this.shaderF,
            texUnit: this.activeTex, tex: this.boundTex,
          });
        }
        this.inBegin = false;
        break;
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
      case GLCMD.ORTHO: this.stack.ortho(c.a, c.b, c.c, c.d); break;
      case GLCMD.MULTMATRIX: if (c.s) this.stack.mult(c.s as Mat4); break;
      case GLCMD.SETSHADER:
        this.shaderV = (c.a as unknown as string) ?? null;
        this.shaderF = (c.b as unknown as string) ?? null;
        break;
      case GLCMD.BINDTEX: this.boundTex = c.a; break;
      case GLCMD.ACTIVETEX: this.activeTex = (c.a & 3); break;
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
