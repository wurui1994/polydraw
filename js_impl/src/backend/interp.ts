// EVAL interpreter — TypeScript port of c_impl/src/eval/pd_interp.c.
// Walks instr[] and evaluates. Mirrors original kasm87c_run (eval.c:5579).

import type { Program, Instr, Reg, Host, HostFn } from '../eval/ir.ts';
import { Op, Fam, NEGMOV } from '../eval/ir.ts';

// RNG — exact port of original krand/nrnd (eval.c:497, 503) so srand()/RND/NRND
// produce sequences identical to the original. Uses explicit 32-bit wraparound
// (the original relied on 32-bit long overflow).
let g_holdrand = 1 >>> 0;
let g_normstat = false;
let g_srand2 = 0;

export function srand(s: number): void {
  g_holdrand = (s >>> 0);
  g_normstat = false;
}

function krand(): number {
  // 32-bit wraparound: kholdrand*214013*2 + 2531011*2, then >>1.
  let v = g_holdrand | 0;
  v = (Math.imul(v, 214013 * 2) + 2531011 * 2) >>> 0;
  v = v >>> 1;
  g_holdrand = v | 0;
  return v >>> 0;
}

function nrnd(): number {
  if (g_normstat) { g_normstat = false; return g_srand2; }
  const oneover2_31 = 1.0 / 2147483648.0;
  let x: number, y: number, r: number;
  do {
    x = ((krand() - 1073741824) >>> 0) * (oneover2_31 * 2.0);
    y = ((krand() - 1073741824) >>> 0) * (oneover2_31 * 2.0);
    r = x * x + y * y;
  } while (r >= 1);
  // Box-Muller (Good & fast) — matches the original: f=sqrt(-2*log(r)/r),
  // the CURRENT pair returns y*f and caches x*f for the next call.
  const f = Math.sqrt(-2.0 * Math.log(r) / r);
  g_srand2 = x * f;
  g_normstat = true;
  return y * f;
}

// factorial via log-gamma (Lanczos), mirrors the original pd_fact.
function fact(num: number): number {
  if (num < 0) num = 0;
  num = Math.floor(num + 0.5);
  if (num <= 1) return 1;
  // small-value direct product is exact; fall back to gamma for larger
  let r = 1;
  if (num <= 20) { for (let i = 2; i <= num; i++) r *= i; return r; }
  // log-gamma Lanczos approximation (matches C pd_fact path).
  const t = num + 5.5;
  return Math.pow(t, num + 0.5) * Math.exp(-t) *
    ((((((num * 2.506628275107298 + 83.8676043423952) * num + 1168.926494792211) * num +
      8687.245297053594) * num + 36308.29514770109) * num +
      80916.62789524846) * num + 75122.63315304522) /
    (((((((num + 21) * num + 175) * num + 735) * num + 1624) * num + 1764) * num + 720) * num);
}

// bounds check helper (eval.txt rules): power-of-2 size → mask; else OOB→0.
function bounds(j: number, size: number): number {
  if (size === 0) return j;
  if (((size - 1) & size) === 0) return j & (size - 1); // power of 2 -> mask
  if (j < 0 || j >= size) return 0;
  return j;
}

export interface Ctx {
  prog: Program;
  frame: Float64Array;
  params: Float64Array | number[];
  globals: Float64Array;
  shouldQuit: { value: boolean } | null;
  parent: Ctx | null;
  root: Program;
}

// Resolve a register to a number value (reads its slot). For arrays/pointers
// use arrayBase() instead.
function slotValue(c: Ctx, r: Reg): number {
  switch (r.fam) {
    case Fam.LOCAL: return c.frame[r.off / 8];
    case Fam.CONST: return c.prog.consts[r.off / 8];
    case Fam.PARAM: return c.params[r.off / 8];
    case Fam.GLOBAL: return c.globals[r.off / 8];
    case Fam.EXT: {
      // host variable: consts slot holds an index into host.vars
      const vi = c.prog.consts[r.off / 8] | 0;
      const v = c.root.host?.vars.get('_' + vi);
      return v ? v.get() : 0;
    }
    default: return 0;
  }
}

// Resolve a writable slot (returns getter/setter closures for LOCAL/etc.).
function slotRef(c: Ctx, r: Reg): { get: () => number; set: (v: number) => void } {
  switch (r.fam) {
    case Fam.LOCAL: {
      const i = r.off / 8;
      const get = () => c.frame[i];
      const set = (v: number) => { c.frame[i] = v; };
      return { get, set };
    }
    case Fam.PARAM: {
      const i = r.off / 8;
      const get = () => (c.params as number[] | Float64Array)[i] as number;
      const set = (v: number) => { (c.params as number[])[i] = v; };
      return { get, set };
    }
    case Fam.GLOBAL: {
      const i = r.off / 8;
      const get = () => c.globals[i];
      const set = (v: number) => { c.globals[i] = v; };
      return { get, set };
    }
    case Fam.CONST: {
      const i = r.off / 8;
      const get = () => c.prog.consts[i];
      const set = (v: number) => { c.prog.consts[i] = v; };
      return { get, set };
    }
    default: return { get: () => 0, set: () => {} };
  }
}

// Resolve an array base address (for PEEK/POKE). GLOBAL storage IS the array;
// PARAM/LOCAL slots hold a pointer id from the registry (bit-cast in C).
export type PtrView = Float64Array | { get(): number; set(v: number): void };
function arrayBase(c: Ctx, r: Reg): PtrView | null {
  switch (r.fam) {
    case Fam.GLOBAL: return c.globals.subarray(r.off / 8);
    case Fam.PARAM: {
      const v = (c.params as number[] | Float64Array)[r.off / 8] as number;
      return typeof v === 'number' ? (g_ptrs.get(v) ?? null) : null;
    }
    case Fam.LOCAL: {
      const v = c.frame[r.off / 8];
      return typeof v === 'number' ? (g_ptrs.get(v) ?? null) : null;
    }
    default: return null;
  }
}

// Pointer registry: JS has no bit-cast pointers, so ADDR/ADDRSLOT mint a
// monotonically-increasing id bound to a view. Host functions that receive an
// address (glsettex &arr) copy out immediately, so entries are safe to drop.
const g_ptrs = new Map<number, PtrView>();
let g_ptrId = 1;
export function ptrMint(view: PtrView): number {
  if (g_ptrs.size > 0x10000) g_ptrs.clear(); // bounded: ref-ptrs are call-scoped
  const id = g_ptrId++;
  if (g_ptrId > 0x3fffffff) { g_ptrs.clear(); g_ptrId = 1; }
  g_ptrs.set(id, view);
  return id;
}
export function ptrDeref(id: number): PtrView | null { return g_ptrs.get(id) ?? null; }
function ptrRead(p: PtrView, j: number): number {
  if (p instanceof Float64Array) return p[j];
  return j === 0 ? p.get() : 0;
}
function ptrWrite(p: PtrView, j: number, v: number): void {
  if (p instanceof Float64Array) { p[j] = v; return; }
  if (j === 0) p.set(v);
}
// 1-element view over a writable slot (for ADDRSLOT on frame/globals/params).
function slotView(c: Ctx, r: Reg): PtrView | null {
  switch (r.fam) {
    case Fam.LOCAL: return c.frame.subarray(r.off / 8, r.off / 8 + 1);
    case Fam.PARAM: {
      const arr = c.params as number[];
      if (arr instanceof Float64Array) return arr.subarray(r.off / 8, r.off / 8 + 1);
      // plain array params (host call): wrap index access
      const i = r.off / 8;
      return { get: () => arr[i], set: (v: number) => { arr[i] = v; } };
    }
    case Fam.GLOBAL: return c.globals.subarray(r.off / 8, r.off / 8 + 1);
    case Fam.EXT: {
      const vi = c.prog.consts[r.off / 8] | 0;
      const v = c.root.host?.vars.get('_' + vi);
      return v ?? null;
    }
    default: return null;
  }
}

export function runCtx(c: Ctx): number {
  const p = c.prog;
  const instr = p.instr;
  let i = 0;
  let quitCounter = 0;

  while (i < instr.length) {
    // freeze protection: check every 4096 dynamic instructions
    if (++quitCounter >= 4096) {
      quitCounter = 0;
      if (c.shouldQuit && c.shouldQuit.value) return 0;
    }
    const in_ = instr[i];
    const op = in_.op;

    // destination slot (null for GOTO/IF0/IF1)
    const hasOut = op !== Op.GOTO && op !== Op.IF0 && op !== Op.IF1;
    const outRef = hasOut ? slotRef(c, in_.out) : null;
    const a = in_.nIn >= 1 ? slotValue(c, in_.in0) : 0;
    const b = in_.nIn >= 2 ? slotValue(c, in_.in1) : 0;

    switch (op as number) {
      case Op.NOP: break;
      case Op.GOTO: i = in_.out.off; continue;
      case Op.RETURN: return a;
      case Op.RND: if (outRef) outRef.set(krand() * (1.0 / 2147483648.0)); break;
      case Op.NRND: if (outRef) outRef.set(nrnd()); break;
      case Op.MOV: if (outRef) outRef.set(a); break;
      case NEGMOV: if (outRef) outRef.set(-a); break;
      case Op.NEQU0: if (outRef) outRef.set(a !== 0.0 ? 1 : 0); break;
      case Op.IF0: if (a === 0.0) { i = in_.out.off; continue; } break;
      case Op.IF1: if (a !== 0.0) { i = in_.out.off; continue; } break;
      // 1-input math
      case Op.FABS: if (outRef) outRef.set(Math.abs(a)); break;
      case Op.SGN: if (outRef) outRef.set((a > 0 ? 1 : 0) - (a < 0 ? 1 : 0)); break;
      case Op.UNIT: if (outRef) outRef.set((a === 0.0 ? 0.5 : 0) + (a > 0 ? 1 : 0)); break;
      case Op.FLOOR: if (outRef) outRef.set(Math.floor(a)); break;
      case Op.CEIL: if (outRef) outRef.set(Math.ceil(a)); break;
      case Op.ROUND0: if (outRef) outRef.set(a >= 0 ? Math.floor(a) : -Math.floor(-a)); break;
      case Op.SIN: if (outRef) outRef.set(Math.sin(a)); break;
      case Op.COS: if (outRef) outRef.set(Math.cos(a)); break;
      case Op.TAN: if (outRef) outRef.set(Math.tan(a)); break;
      case Op.ASIN: if (outRef) outRef.set(Math.asin(a)); break;
      case Op.ACOS: if (outRef) outRef.set(Math.acos(a)); break;
      case Op.ATAN: if (outRef) outRef.set(Math.atan(a)); break;
      case Op.SQRT: if (outRef) outRef.set(Math.sqrt(a)); break;
      case Op.EXP: if (outRef) outRef.set(Math.exp(a)); break;
      case Op.FACT: if (outRef) outRef.set(fact(a)); break;
      case Op.LOG: if (outRef) outRef.set(Math.log(a)); break;
      // 2-input
      case Op.TIMES: if (outRef) outRef.set(a * b); break;
      case Op.SLASH: if (outRef) outRef.set(a / b); break;
      case Op.PERC: if (outRef) outRef.set(a - Math.floor(a / Math.abs(b)) * Math.abs(b)); break;
      case Op.PLUS:
      case Op.FADD: if (outRef) outRef.set(a + b); break;
      case Op.MINUS: if (outRef) outRef.set(a - b); break;
      case Op.POW: if (outRef) outRef.set(Math.pow(a, b)); break;
      case Op.MIN: if (outRef) outRef.set(b < a ? b : a); break;
      case Op.MAX: if (outRef) outRef.set(b > a ? b : a); break;
      case Op.FMOD: if (outRef) outRef.set(a % b); break;
      case Op.ATAN2: if (outRef) outRef.set(Math.atan2(a, b)); break;
      case Op.LOGB: if (outRef) outRef.set(Math.log(a) / Math.log(b)); break;
      case Op.LES: if (outRef) outRef.set(a < b ? 1 : 0); break;
      case Op.LESEQ: if (outRef) outRef.set(a <= b ? 1 : 0); break;
      case Op.MOR: if (outRef) outRef.set(a > b ? 1 : 0); break;
      case Op.MOREQ: if (outRef) outRef.set(a >= b ? 1 : 0); break;
      case Op.EQU: if (outRef) outRef.set(a === b ? 1 : 0); break;
      case Op.NEQU: if (outRef) outRef.set(a !== b ? 1 : 0); break;
      case Op.LAND: if (outRef) outRef.set((a !== 0.0 && b !== 0.0) ? 1 : 0); break;
      case Op.LOR: if (outRef) outRef.set((a !== 0.0 || b !== 0.0) ? 1 : 0); break;
      // arrays
      case Op.PEEK: {
        const base = arrayBase(c, in_.in0);
        const j = bounds(b | 0, in_.aux);
        if (outRef && base) outRef.set(ptrRead(base, j));
        break;
      }
      case Op.ADDR: {
        const base = arrayBase(c, in_.in0);
        if (outRef) outRef.set(base ? ptrMint(base) : 0);
        break;
      }
      case Op.ADDRSLOT: {
        const sv = slotView(c, in_.in0);
        if (outRef) outRef.set(sv ? ptrMint(sv) : 0);
        break;
      }
      case Op.POKE: {
        const base = arrayBase(c, in_.out);
        const j = bounds(b | 0, in_.aux);
        if (base) ptrWrite(base, j, a);
        break;
      }
      case Op.POKETIMES: { const base = arrayBase(c, in_.out); const j = bounds(b | 0, in_.aux); if (base) ptrWrite(base, j, ptrRead(base, j) * a); break; }
      case Op.POKESLASH: { const base = arrayBase(c, in_.out); const j = bounds(b | 0, in_.aux); if (base) ptrWrite(base, j, ptrRead(base, j) / a); break; }
      case Op.POKEPERC: { const base = arrayBase(c, in_.out); const j = bounds(b | 0, in_.aux); if (base) { const v = ptrRead(base, j); ptrWrite(base, j, v - Math.floor(v / Math.abs(a)) * Math.abs(a)); } break; }
      case Op.POKEPLUS: { const base = arrayBase(c, in_.out); const j = bounds(b | 0, in_.aux); if (base) ptrWrite(base, j, ptrRead(base, j) + a); break; }
      case Op.POKEMINUS: { const base = arrayBase(c, in_.out); const j = bounds(b | 0, in_.aux); if (base) ptrWrite(base, j, ptrRead(base, j) - a); break; }
      case Op.CALL: {
        const na = in_.nIn;
        const argbuf: number[] = [];
        if (na > 0) argbuf.push(slotValue(c, in_.in0));
        if (na > 1) argbuf.push(slotValue(c, in_.in1));
        // extra args live in the CURRENT program's extra[] table
        for (let k = 2; k < na; k++) argbuf.push(slotValue(c, p.extra[in_.extraIdx + k - 2]));
        const root = c.root;
        if (in_.aux <= -1000) {
          // external host function (host idx = -1000 - aux)
          const hidx = -1000 - in_.aux;
          const hfn = root.host?.fns[hidx];
          if (outRef) outRef.set(hfn ? hfn.fn(na, argbuf) : 0);
          break;
        }
        if (in_.aux >= 0 && in_.aux < root.funcs.length) {
          const fn = root.funcs[in_.aux];
          const child: Ctx = {
            prog: fn,
            frame: new Float64Array(fn.nLocals || 1),
            params: Float64Array.from(argbuf),
            globals: c.globals,
            shouldQuit: c.shouldQuit,
            parent: c,
            root,
          };
          const r = runCtx(child);
          if (outRef) outRef.set(r);
        } else {
          if (outRef) outRef.set(0);
        }
        break;
      }
      default: break;
    }
    i++;
  }
  return 0;
}

export function run(prog: Program, params: Float64Array | number[] | null, globals: Float64Array, shouldQuit: { value: boolean } | null): number {
  // JIT fast path: compile the program body once, then execute natively.
  const DBG = typeof process !== 'undefined' && !!process.env.PD_JIT_DEBUG;
  const NO_JIT = typeof process !== 'undefined' && !!process.env.PD_NO_JIT;
  let f = jitRoots.get(prog);
  if (!NO_JIT && f === undefined) {
    try {
      f = makeJitFn(prog);
      if (DBG) console.error(`[jit] root compile ok: ${prog.instr.length} instrs`);
    } catch (e) {
      if (DBG) console.error(`[jit] root compile FAILED: ${e}`);
      f = null;
    }
    jitRoots.set(prog, f);
  }
  if (f) {
    const orig = params;
    const P = params instanceof Float64Array ? params : Float64Array.from(params ?? [0]);
    const H = prog.host ?? null;
    const B = makeJitBridge(prog, globals, shouldQuit, H);
    try {
      const r = f(P, globals, prog.consts, shouldQuit, prog, H, B);
      // mirror the interpreter: PARAM writes go back to a plain-array caller
      if (Array.isArray(orig)) {
        const m = Math.min(orig.length, P.length);
        for (let k = 0; k < m; k++) orig[k] = P[k];
      }
      return r;
    } catch (e) {
      if (DBG) console.error(`[jit] RUNTIME error: ${(e as Error)?.stack ?? e}`);
      if (DBG) return 0;
      throw e;
    }
  }
  const c: Ctx = {
    prog,
    frame: new Float64Array(prog.nLocals || 1),
    params: params ?? new Float64Array(1),
    globals,
    shouldQuit,
    parent: null,
    root: prog,
  };
  return runCtx(c);
}

// ==================== JIT-JS backend ====================
// The tree-walking interpreter above costs ~2 closures + 1 object per
// instruction; texture3d.pss calls voxfunc 262144 times and profiles at
// >90% of CPU in runCtx. Each user function / program body is instead
// compiled to a native JS function (while+switch dispatch over the flat
// bytecode, all slots as direct Float64Array accesses).

interface JitBridge {
  KRAND: typeof krand; NRND: typeof nrnd; FACT: typeof fact; BND: typeof bounds;
  GV: (i: number) => Float64Array; DEREF: typeof ptrDeref;
  PRD: (p: PtrView | null, j: number) => number; PWR: (p: PtrView | null, j: number, v: number) => void;
  MINT: (v: PtrView | null) => number; HV: (vi: number) => PtrView | null;
  HVG: (vi: number) => number; HF: (hidx: number) => HostFn | undefined;
  CAL: (idx: number, args: number[]) => number;
}

type JitFn = (P: Float64Array, G: Float64Array, C: number[], SQ: { value: boolean } | null, RT: Program, H: Host | null, B: JitBridge) => number;

const jitRoots = new WeakMap<Program, JitFn | null>();
const jitFuncs = new WeakMap<Program, (JitFn | null)[]>();

function jitRd(r: Reg): string {
  switch (r.fam) {
    case Fam.LOCAL: return `_L[${r.off / 8}]`;
    case Fam.PARAM: return `_P[${r.off / 8}]`;
    case Fam.GLOBAL: return `_G[${r.off / 8}]`;
    case Fam.CONST: return `_C[${r.off / 8}]`;
    case Fam.EXT: return `B.HVG(_C[${r.off / 8}])`;
    default: return '0';
  }
}

function jitWr(r: Reg): string | null {
  switch (r.fam) {
    case Fam.LOCAL: return `_L[${r.off / 8}]`;
    case Fam.PARAM: return `_P[${r.off / 8}]`;
    case Fam.GLOBAL: return `_G[${r.off / 8}]`;
    case Fam.CONST: return `_C[${r.off / 8}]`;
    default: return null; // EXT/LABEL/PTR/VOID: mirrors slotRef default noop
  }
}

// PtrView-producing expression for PEEK/POKE/ADDR (mirrors arrayBase).
function jitBase(r: Reg): string {
  switch (r.fam) {
    case Fam.GLOBAL: return `B.GV(${r.off / 8})`;
    case Fam.PARAM: return `B.DEREF(_P[${r.off / 8}])`;
    case Fam.LOCAL: return `B.DEREF(_L[${r.off / 8}])`;
    default: return 'null';
  }
}

function jitBody(prog: Program): string {
  const L: string[] = [];
  L.push(`var _L = new Float64Array(${prog.nLocals || 1});`);
  L.push('var _P = P, _G = G, _C = C;');
  L.push('var pc = 0, _qc = 0;');
  L.push('for (;;) {');
  L.push('  if (++_qc >= 4096) { _qc = 0; if (SQ && SQ.value) return 0; }');
  L.push('  switch (pc) {');
  for (let i = 0; i < prog.instr.length; i++) {
    const in_ = prog.instr[i];
    const hasOut = in_.op !== Op.GOTO && in_.op !== Op.IF0 && in_.op !== Op.IF1;
    const o = hasOut ? jitWr(in_.out) : null;
    const a = in_.nIn >= 1 ? jitRd(in_.in0) : '0';
    const b = in_.nIn >= 2 ? jitRd(in_.in1) : '0';
    const set = (e: string) => (o ? `${o}=${e};` : '');
    let s = '';
    switch (in_.op as number) {
      case Op.NOP: s = ';'; break;
      case Op.GOTO: s = `pc=${in_.out.off};`; break;
      case Op.RETURN: s = `return ${a};`; break;
      case Op.RND: s = set('B.KRAND() * 4.656612873077393e-10'); break;
      case Op.NRND: s = set('B.NRND()'); break;
      case Op.MOV: s = set(a); break;
      case NEGMOV: s = set(`-(${a})`); break;
      case Op.NEQU0: s = set(`(${a} !== 0 ? 1 : 0)`); break;
      case Op.IF0: s = `if (${a} === 0) { pc=${in_.out.off}; } else { pc=${i + 1}; }`; break;
      case Op.IF1: s = `if (${a} !== 0) { pc=${in_.out.off}; } else { pc=${i + 1}; }`; break;
      case Op.FABS: s = set(`Math.abs(${a})`); break;
      case Op.SGN: s = set(`((${a} > 0 ? 1 : 0) - (${a} < 0 ? 1 : 0))`); break;
      case Op.UNIT: s = set(`((${a} === 0 ? 0.5 : 0) + (${a} > 0 ? 1 : 0))`); break;
      case Op.FLOOR: s = set(`Math.floor(${a})`); break;
      case Op.CEIL: s = set(`Math.ceil(${a})`); break;
      case Op.ROUND0: s = set(`(${a} >= 0 ? Math.floor(${a}) : -Math.floor(-${a}))`); break;
      case Op.SIN: s = set(`Math.sin(${a})`); break;
      case Op.COS: s = set(`Math.cos(${a})`); break;
      case Op.TAN: s = set(`Math.tan(${a})`); break;
      case Op.ASIN: s = set(`Math.asin(${a})`); break;
      case Op.ACOS: s = set(`Math.acos(${a})`); break;
      case Op.ATAN: s = set(`Math.atan(${a})`); break;
      case Op.SQRT: s = set(`Math.sqrt(${a})`); break;
      case Op.EXP: s = set(`Math.exp(${a})`); break;
      case Op.FACT: s = set(`B.FACT(${a})`); break;
      case Op.LOG: s = set(`Math.log(${a})`); break;
      case Op.TIMES: s = set(`(${a} * ${b})`); break;
      case Op.SLASH: s = set(`(${a} / ${b})`); break;
      case Op.PERC: s = set(`(${a} - Math.floor(${a} / Math.abs(${b})) * Math.abs(${b}))`); break;
      case Op.PLUS:
      case Op.FADD: s = set(`(${a} + ${b})`); break;
      case Op.MINUS: s = set(`(${a} - ${b})`); break;
      case Op.POW: s = set(`Math.pow(${a}, ${b})`); break;
      case Op.MIN: s = set(`(${b} < ${a} ? ${b} : ${a})`); break;
      case Op.MAX: s = set(`(${b} > ${a} ? ${b} : ${a})`); break;
      case Op.FMOD: s = set(`(${a} % ${b})`); break;
      case Op.ATAN2: s = set(`Math.atan2(${a}, ${b})`); break;
      case Op.LOGB: s = set(`(Math.log(${a}) / Math.log(${b}))`); break;
      case Op.LES: s = set(`(${a} < ${b} ? 1 : 0)`); break;
      case Op.LESEQ: s = set(`(${a} <= ${b} ? 1 : 0)`); break;
      case Op.MOR: s = set(`(${a} > ${b} ? 1 : 0)`); break;
      case Op.MOREQ: s = set(`(${a} >= ${b} ? 1 : 0)`); break;
      case Op.EQU: s = set(`(${a} === ${b} ? 1 : 0)`); break;
      case Op.NEQU: s = set(`(${a} !== ${b} ? 1 : 0)`); break;
      case Op.LAND: s = set(`((${a} !== 0 && ${b} !== 0) ? 1 : 0)`); break;
      case Op.LOR: s = set(`((${a} !== 0 || ${b} !== 0) ? 1 : 0)`); break;
      case Op.PEEK: {
        const base = jitBase(in_.in0);
        s = set(`B.PRD(${base}, B.BND((${b}) | 0, ${in_.aux}))`);
        break;
      }
      case Op.ADDR: {
        const base = jitBase(in_.in0);
        s = set(`B.MINT(${base})`);
        break;
      }
      case Op.ADDRSLOT: {
        let sv: string;
        switch (in_.in0.fam) {
          case Fam.LOCAL: sv = `_L.subarray(${in_.in0.off / 8}, ${in_.in0.off / 8 + 1})`; break;
          case Fam.PARAM: sv = `_P.subarray(${in_.in0.off / 8}, ${in_.in0.off / 8 + 1})`; break;
          case Fam.GLOBAL: sv = `_G.subarray(${in_.in0.off / 8}, ${in_.in0.off / 8 + 1})`; break;
          case Fam.EXT: sv = `B.HV(_C[${in_.in0.off / 8}])`; break;
          default: sv = 'null';
        }
        s = set(`B.MINT(${sv})`);
        break;
      }
      case Op.POKE: {
        const base = jitBase(in_.out);
        s = `B.PWR(${base}, B.BND((${b}) | 0, ${in_.aux}), ${a});`;
        break;
      }
      case Op.POKETIMES: {
        const base = jitBase(in_.out);
        s = `{var _j = B.BND((${b}) | 0, ${in_.aux}); B.PWR(${base}, _j, B.PRD(${base}, _j) * ${a});}`;
        break;
      }
      case Op.POKESLASH: {
        const base = jitBase(in_.out);
        s = `{var _j = B.BND((${b}) | 0, ${in_.aux}); B.PWR(${base}, _j, B.PRD(${base}, _j) / ${a});}`;
        break;
      }
      case Op.POKEPERC: {
        const base = jitBase(in_.out);
        s = `{var _j = B.BND((${b}) | 0, ${in_.aux}); var _v = B.PRD(${base}, _j); B.PWR(${base}, _j, _v - Math.floor(_v / Math.abs(${a})) * Math.abs(${a}));}`;
        break;
      }
      case Op.POKEPLUS: {
        const base = jitBase(in_.out);
        s = `{var _j = B.BND((${b}) | 0, ${in_.aux}); B.PWR(${base}, _j, B.PRD(${base}, _j) + ${a});}`;
        break;
      }
      case Op.POKEMINUS: {
        const base = jitBase(in_.out);
        s = `{var _j = B.BND((${b}) | 0, ${in_.aux}); B.PWR(${base}, _j, B.PRD(${base}, _j) - ${a});}`;
        break;
      }
      case Op.CALL: {
        const na = in_.nIn;
        const args: string[] = [];
        if (na > 0) args.push(jitRd(in_.in0));
        if (na > 1) args.push(jitRd(in_.in1));
        for (let k = 2; k < na; k++) args.push(jitRd(prog.extra[in_.extraIdx + k - 2]));
        const alist = args.join(', ');
        if (in_.aux <= -1000) {
          const hidx = -1000 - in_.aux;
          s = set(`(B.HF(${hidx}) ? B.HF(${hidx}).fn(${na}, [${alist}]) : 0)`);
        } else if (in_.aux >= 0 && in_.aux < prog.funcs.length) {
          s = set(`B.CAL(${in_.aux}, [${alist}])`);
        } else {
          s = set('0');
        }
        break;
      }
      default: s = ';'; break;
    }
    if (in_.op === Op.GOTO || in_.op === Op.RETURN) {
      L.push(`  case ${i}: ${s} break;`);
    } else if (in_.op === Op.IF0 || in_.op === Op.IF1) {
      L.push(`  case ${i}: ${s} break;`);
    } else {
      L.push(`  case ${i}: ${s} pc=${i + 1}; break;`);
    }
  }
  L.push('    default: return 0;');
  L.push('  }');
  L.push('}');
  return L.join('\n');
}

function makeJitFn(prog: Program): JitFn {
  const body = jitBody(prog);
  const maker = new Function(
    'P', 'G', 'C', 'SQ', 'RT', 'H', 'B',
    body,
  );
  return maker as unknown as JitFn;
}

function jitCompileFunc(root: Program, idx: number): JitFn | null {
  let arr = jitFuncs.get(root);
  if (!arr) { arr = []; jitFuncs.set(root, arr); }
  if (arr[idx] !== undefined) return arr[idx];
  const fn = root.funcs[idx];
  let f: JitFn | null = null;
  try {
    f = makeJitFn(fn);
    if (typeof process !== 'undefined' && process.env.PD_JIT_DEBUG) {
      console.error(`[jit] func[${idx}] compile ok: ${fn.instr.length} instrs`);
    }
  } catch (e) {
    if (typeof process !== 'undefined' && process.env.PD_JIT_DEBUG) {
      console.error(`[jit] func[${idx}] compile FAILED: ${e}`);
    }
    f = null;
  }
  arr[idx] = f;
  return f;
}

// Bridge object injected into compiled bodies (they are built with
// new Function and cannot capture module closures).
export function makeJitBridge(root: Program, G: Float64Array, SQ: { value: boolean } | null, H: Host | null): JitBridge {
  const B: JitBridge = {
    KRAND: krand,
    NRND: nrnd,
    FACT: fact,
    BND: bounds,
    GV: (i: number) => G.subarray(i),
    DEREF: ptrDeref,
    PRD: (p: PtrView | null, j: number): number =>
      p ? (p instanceof Float64Array ? p[j] : (j === 0 ? p.get() : 0)) : 0,
    PWR: (p: PtrView | null, j: number, v: number): void => {
      if (!p) return;
      if (p instanceof Float64Array) p[j] = v;
      else if (j === 0) p.set(v);
    },
    MINT: (v: PtrView | null) => (v ? ptrMint(v) : 0),
    HV: (vi: number): PtrView | null => H?.vars.get('_' + (vi | 0)) ?? null,
    HVG: (vi: number): number => {
      const v = H?.vars.get('_' + (vi | 0));
      return v ? v.get() : 0;
    },
    HF: (hidx: number) => H?.fns[hidx],
    CAL: (idx: number, args: number[]): number => 0,
  };
  B.CAL = (idx: number, args: number[]): number => {
    const fn = root.funcs[idx];
    if (!fn) return 0;
    let jf = jitFuncs.get(root)?.[idx];
    if (jf === undefined) jf = jitCompileFunc(root, idx);
    if (jf) return jf(Float64Array.from(args), G, fn.consts, SQ, root, H, B);
    // interpreter fallback (compile failure)
    const child: Ctx = {
      prog: fn,
      frame: new Float64Array(fn.nLocals || 1),
      params: Float64Array.from(args),
      globals: G,
      shouldQuit: SQ,
      parent: null,
      root,
    };
    return runCtx(child);
  };
  return B;
}
