// glcmd.ts — record immediate-mode GL calls into a flat command buffer.
// Faithful TypeScript port of c_impl/src/render/glcmd.{h,c}.
// This is the cross-backend contract: the C offscreen renderer, the JS WebGL2
// replay, and any reference renderer all consume the same GLCmd stream.

// GL primitive-type constants (canonical OpenGL values).
export const PDGL = {
  POINTS: 0x0000,
  LINES: 0x0001,
  LINE_LOOP: 0x0002,
  LINE_STRIP: 0x0003,
  TRIANGLES: 0x0004,
  TRIANGLE_STRIP: 0x0005,
  TRIANGLE_FAN: 0x0006,
  QUADS: 0x0007,
  QUAD_STRIP: 0x0008,
  POLYGON: 0x0009,
};

export const GLCMD = {
  CLEAR: 0,
  BEGIN: 1,
  END: 2,
  VERTEX: 3,
  COLOR: 4,
  TEXCOORD: 5,
  NORMAL: 6,
  PUSHMATRIX: 7,
  POPMATRIX: 8,
  TRANSLATE: 9,
  ROTATE: 10,
  SCALE: 11,
  MATRIXMODE: 12,
  LOADIDENTITY: 13,
  PERSPECTIVE: 14,
  ORTHO: 15,
  VIEWPORT: 16,
  QUAD: 17,
  ENABLE: 18,
  DISABLE: 19,
  BLENDFUNC: 20,
  CULLFACE: 21,
  LINEWIDTH: 22,
  SETTEXDATA: 23,
  BINDTEX: 24,
  ACTIVETEX: 25,
  CAPTURE: 26,
  CAPTUREEND: 27,
  SETFOV: 28,
  SETSHADER: 29,
  UNIFORMLOC: 30,
  UNIFORM: 31,
  MULTMATRIX: 32,
} as const;
export type GLCMD = (typeof GLCMD)[keyof typeof GLCMD];

export const PD_UNI_F = 0;
export const PD_UNI_I = 1;

export interface GLCmd {
  op: GLCMD;
  mode: number;
  a: number;
  b: number;
  c: number;
  d: number;
  s: number[] | string | null; // texture pixels / uniform floats / matrix
}

export class GLCmdBuf {
  cmds: GLCmd[] = [];

  reset(): void {
    // free owned UNIFORM float arrays
    for (const c of this.cmds) {
      if (c.op === GLCMD.UNIFORM && Array.isArray(c.s)) c.s = null;
    }
    this.cmds.length = 0;
  }

  push(): GLCmd {
    const c: GLCmd = { op: 0, mode: 0, a: 0, b: 0, c: 0, d: 0, s: null };
    this.cmds.push(c);
    return c;
  }

  begin(mode: number): void { const c = this.push(); c.op = GLCMD.BEGIN; c.mode = mode; }
  end(): void { /* no-op marker */ const c = this.push(); c.op = GLCMD.END; }
  clear(): void { const c = this.push(); c.op = GLCMD.CLEAR; }
  vertex(x: number, y: number, z: number, w: number): void {
    const c = this.push(); c.op = GLCMD.VERTEX; c.a = x; c.b = y; c.c = z; c.d = w;
  }
  color(r: number, g: number, b: number, a: number): void {
    const c = this.push(); c.op = GLCMD.COLOR; c.a = r; c.b = g; c.c = b; c.d = a;
  }
}

export function glcmdEqual(a: GLCmdBuf, b: GLCmdBuf): boolean {
  if (a.cmds.length !== b.cmds.length) return false;
  for (let i = 0; i < a.cmds.length; i++) {
    const x = a.cmds[i], y = b.cmds[i];
    if (x.op !== y.op || x.mode !== y.mode) return false;
    if (x.a !== y.a || x.b !== y.b || x.c !== y.c || x.d !== y.d) return false;
    if (Array.isArray(x.s) && Array.isArray(y.s)) {
      if (x.s.length !== y.s.length) return false;
      for (let k = 0; k < x.s.length; k++) if (x.s[k] !== y.s[k]) return false;
    } else if (x.s !== y.s) {
      return false;
    }
  }
  return true;
}
