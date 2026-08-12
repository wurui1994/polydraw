// EVAL interpreter — TypeScript port of c_impl/src/eval/pd_interp.c.
// Walks instr[] and evaluates. Mirrors original kasm87c_run (eval.c:5579).

import type { Program, Instr, Reg } from '../eval/ir.ts';
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

// Resolve an array base address (for PEEK/POKE).
function arrayBase(c: Ctx, r: Reg): Float64Array | null {
  switch (r.fam) {
    case Fam.GLOBAL: return c.globals.subarray(r.off / 8);
    case Fam.PARAM: {
      // array passed as pointer: the param slot holds... for now same as global slice
      return null;
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

    switch (op) {
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
        if (outRef && base) outRef.set(base[j]);
        break;
      }
      case Op.POKE: {
        const base = arrayBase(c, in_.out);
        const j = bounds(b | 0, in_.aux);
        if (base) base[j] = a;
        break;
      }
      case Op.POKETIMES: { const base = arrayBase(c, in_.out); const j = bounds(b | 0, in_.aux); if (base) base[j] *= a; break; }
      case Op.POKESLASH: { const base = arrayBase(c, in_.out); const j = bounds(b | 0, in_.aux); if (base) base[j] /= a; break; }
      case Op.POKEPERC: { const base = arrayBase(c, in_.out); const j = bounds(b | 0, in_.aux); if (base) base[j] -= Math.floor(base[j] / Math.abs(a)) * Math.abs(a); break; }
      case Op.POKEPLUS: { const base = arrayBase(c, in_.out); const j = bounds(b | 0, in_.aux); if (base) base[j] += a; break; }
      case Op.POKEMINUS: { const base = arrayBase(c, in_.out); const j = bounds(b | 0, in_.aux); if (base) base[j] -= a; break; }
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
