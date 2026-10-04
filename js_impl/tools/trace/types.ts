// trace/types.ts — shared trace format for C/JS render-graph differential.
//
// A Trace is a full recording of one frame's render pipeline:
//   1. GLCmd[] — the raw GL command stream (contract between EVAL host and renderer)
//   2. Batch[] — how the renderer groups commands into draw calls
//   3. Per-batch: vertex transform results (clip-space positions, varyings)
//   4. Per-batch: framebuffer snapshot AFTER the batch was drawn
//
// Both C and JS dump to this same JSON format, then trace_compare.ts aligns
// them node-by-node: cmd-stream → batch → vertex → pixel.

export interface TraceCmd {
  op: number;            // GLCmdOp number (see glcmdName for the display name)
  mode?: number;
  a?: number;
  b?: number;
  c?: number;
  d?: number;
  s?: string | null;
  s2?: string | null;
  dataSummary?: string;
  // op-specific payloads (mirror trace_dump_cmds() in gl_renderer.c)
  pxHash?: string;       // SETTEXDATA: djb2 over packed pixel doubles
  pxCount?: number;      // SETTEXDATA: w*h, or -1 when pixels absent
  fv?: number[];         // UNIFORM: float array values (mode&0xffff of them)
  m16?: number[];        // MULTMATRIX: 16 doubles column-major
  vsLen?: number;        // SETSHADER: vertex source length
  fsLen?: number;        // SETSHADER: fragment source length
  vsHash?: string;       // SETSHADER: djb2 of vertex source
  fsHash?: string;       // SETSHADER: djb2 of fragment source
}

export interface TraceVertex {
  ox: number; oy: number; oz: number; ow: number;
  cx: number; cy: number; cz: number; cw: number;
  nx: number; ny: number; nz: number;
  r: number; g: number; b: number; a: number;
  s: number; t: number;
  nrmx: number; nrmy: number; nrmz: number;
}

export interface TraceBatch {
  index: number;
  mode: string | number;
  nverts: number;
  // source GLCmd range [cmdStart, cmdEnd] this batch covers — the semantic
  // anchor for aligning C batches (merged) against JS batches (per BEGIN/END)
  cmdStart?: number;
  cmdEnd?: number;
  mvp: number[];
  shaderHash: string;
  texUnits: number[];
  depthTest: boolean;
  blend: boolean;
  cullFace: number;
  captureTarget: number;
  verts: TraceVertex[];
  fbHash: string;
  fbNonBlankPct: number;
  fbMean: number;
}

export interface TraceTex {
  id: number;
  w: number;
  h: number;
  filter: number;
  wrap: number;
  hash: string;
}

export interface Trace {
  source: "c" | "js";
  script: string;
  frame: number;
  width: number;
  height: number;
  fovy: number;
  cmds: TraceCmd[];
  batches: TraceBatch[];
  textures: TraceTex[];
  finalHash: string;
}

const GLCMD_NAMES = [
  "CLEAR", "BEGIN", "END", "VERTEX", "COLOR", "TEXCOORD", "NORMAL",
  "PUSHMATRIX", "POPMATRIX", "TRANSLATE", "ROTATE", "SCALE", "MATRIXMODE",
  "LOADIDENTITY", "PERSPECTIVE", "ORTHO", "VIEWPORT", "QUAD", "ENABLE",
  "DISABLE", "BLENDFUNC", "CULLFACE", "LINEWIDTH", "POINTSIZE",
  "SETTEXDATA", "BINDTEX", "ACTIVETEX", "CAPTURE", "CAPTUREEND",
  "SETFOV", "SETSHADER", "UNIFORMLOC", "UNIFORM", "MULTMATRIX",
];

export function glcmdName(op: number): string {
  return GLCMD_NAMES[op] ?? `OP_${op}`;
}

export const PRIM_NAMES: Record<number, string> = {
  0x0000: "POINTS", 0x0001: "LINES", 0x0002: "LINE_LOOP", 0x0003: "LINE_STRIP",
  0x0004: "TRIANGLES", 0x0005: "TRIANGLE_STRIP", 0x0006: "TRIANGLE_FAN",
  0x0007: "QUADS", 0x0008: "QUAD_STRIP", 0x0009: "POLYGON",
};

export function primName(mode: number): string {
  return PRIM_NAMES[mode] ?? `PRIM_${mode}`;
}
