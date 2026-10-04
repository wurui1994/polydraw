// trace/trace_compare.ts — C/JS render-graph differential comparator.
//
// Usage:
//   npx tsx tools/trace/trace_compare.ts /tmp/c_trace.json /tmp/js_trace.json \
//          --cdir /tmp/c_snaps --jsdir /tmp/js_snaps
//
// Aligns C and JS traces layer by layer, from the contract down to pixels:
//   0. Meta: size / frame / fovy
//   1. GLCmd stream (the contract layer): op-by-op. A divergence here means
//      the HOST/EVAL side records different commands — nothing downstream
//      can match until this is fixed.
//   2. Batch alignment: count, mode, nverts, MVP (by source cmd range)
//   3. Per-vertex: clip-space (cx,cy,cz,cw) and NDC (nx,ny,nz) — finds the
//      EXACT vertex where the two renderers diverge
//   4. Per-drawcall framebuffer: hash + pixel-level diff (with --cdir/--jsdir
//      snapshot dirs) — finds the EXACT pixel, its coords and per-channel
//      delta, where the rendered output first diverges

import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { glcmdName, primName } from './types.ts';

interface TraceCmd {
  op: number;
  mode?: number;
  a?: number;
  b?: number;
  c?: number;
  d?: number;
  s?: string | null;
  pxHash?: string;
  pxCount?: number;
  fv?: number[];
  m16?: number[];
  vsLen?: number;
  fsLen?: number;
  vsHash?: string;
  fsHash?: string;
}

interface TraceVertex {
  ox: number; oy: number; oz: number; ow: number;
  cx: number; cy: number; cz: number; cw: number;
  nx: number; ny: number; nz: number;
  r: number; g: number; b: number; a: number;
  s: number; t: number;
  nrmx: number; nrmy: number; nrmz: number;
}

interface TraceBatch {
  index: number;
  mode: string | number;
  nverts: number;
  cmdStart?: number;
  cmdEnd?: number;
  mvp: number[];
  verts: TraceVertex[];
  fbHash: string | null;
  fbMean: number;
  fbNonBlankPct: number;
}

interface Trace {
  source: string;
  script: string;
  frame: number;
  width: number;
  height: number;
  cmds?: TraceCmd[];
  batches: (TraceBatch | null)[];
  finalHash: string;
}

const EPS = 1e-3;

function numEq(a: number, b: number, eps = EPS): boolean {
  return Math.abs(a - b) <= eps || (Math.abs(a) < eps && Math.abs(b) < eps);
}

/** Normalize a vertex list into a flat TRIANGLES vertex sequence, using the
 *  same fan/strip/quad expansion order as tessellate() in gl_renderer.c:
 *    TRIANGLES      → as-is
 *    TRIANGLE_FAN / POLYGON → (0,k,k+1)
 *    TRIANGLE_STRIP → (k,k+1,k+2)
 *    QUADS          → (k,k+1,k+2) (k,k+2,k+3)
 *  C batches are already expanded (batch_prim==GL_TRIANGLES); JS batches may
 *  still be in the source primitive mode. */
function toTriangles(mode: string | number, verts: TraceVertex[]): TraceVertex[] {
  const m = typeof mode === 'number' ? mode : -1;
  const out: TraceVertex[] = [];
  const nv = verts.length;
  switch (m) {
    case 0: case 1: case 2: case 3:  // POINTS / LINES variants: pass through
    case 4:                          // TRIANGLES
      return verts;
    case 5:                          // TRIANGLE_STRIP
      for (let k = 0; k + 2 < nv; k++) out.push(verts[k], verts[k + 1], verts[k + 2]);
      return out;
    case 6: case 9:                  // TRIANGLE_FAN / POLYGON
      for (let k = 1; k + 1 < nv; k++) out.push(verts[0], verts[k], verts[k + 1]);
      return out;
    case 7:                          // QUADS
      for (let k = 0; k + 3 < nv; k += 4)
        out.push(verts[k], verts[k + 1], verts[k + 2], verts[k], verts[k + 2], verts[k + 3]);
      return out;
    case 8: {                        // QUAD_STRIP
      for (let k = 0; k + 3 < nv; k += 2)
        out.push(verts[k], verts[k + 1], verts[k + 2], verts[k + 1], verts[k + 3], verts[k + 2]);
      return out;
    }
    default:
      return verts;
  }
}

function cmdDesc(c: TraceCmd): string {
  const n = glcmdName(c.op);
  switch (c.op) {
    case 24: return `${n} tex=${c.a} ${c.b}x${c.c} pxHash=${c.pxHash}`;       // SETTEXDATA
    case 30: return `${n} vs=${c.vsLen}/${c.vsHash} fs=${c.fsLen}/${c.fsHash}`; // SETSHADER
    case 31: return `${n} id=${c.a} name=${JSON.stringify(c.s)}`;             // UNIFORMLOC
    case 32: return `${n} id=${c.a} vals=[${c.a},${c.b},${c.c},${c.d}]${c.fv ? ' fv=[' + c.fv.slice(0, 8).join(',') + (c.fv.length > 8 ? ',…' : '') + ']' : ''}`; // UNIFORM
    case 33: return `${n} m16=[${(c.m16 ?? []).slice(0, 4).join(',')}…]`;     // MULTMATRIX
    case 1:  return `${n} ${c.mode}`;                                          // BEGIN
    case 3:  return `${n} (${fmt(c.a)},${fmt(c.b)},${fmt(c.c)})`;              // VERTEX
    default: return `${n} mode=${c.mode} a=${fmt(c.a)} b=${fmt(c.b)} c=${fmt(c.c)} d=${fmt(c.d)}`;
  }
}

function fmt(v: number | undefined): string {
  return v === undefined ? 'undef' : Math.abs(v) < 1e-9 ? '0' : v.toPrecision(6);
}

/** Compare one command. Returns list of field diffs. */
function cmdDiff(ca: TraceCmd, cb: TraceCmd): string[] {
  const diffs: string[] = [];
  if (ca.op !== cb.op) diffs.push(`op: C=${glcmdName(ca.op)} JS=${glcmdName(cb.op)}`);
  if ((ca.mode ?? 0) !== (cb.mode ?? 0)) diffs.push(`mode: C=${ca.mode} JS=${cb.mode}`);
  if (ca.op !== 30) {  // SETSHADER: a/b are bit-cast POINTERS in C but source
    for (const f of ['a', 'b', 'c', 'd'] as const) {   // lengths in JS — not
      if (!numEq(ca[f] ?? 0, cb[f] ?? 0)) diffs.push(`${f}: C=${fmt(ca[f])} JS=${fmt(cb[f])}`);  // comparable; vs/fsHash covers content
    }
  }
  if ((ca.s ?? null) !== (cb.s ?? null)) diffs.push(`s: C=${JSON.stringify(ca.s)} JS=${JSON.stringify(cb.s)}`);
  if ((ca.pxHash ?? null) !== (cb.pxHash ?? null)) diffs.push(`pxHash: C=${ca.pxHash} JS=${cb.pxHash}  ← TEXTURE CONTENT DIFFERS`);
  if ((ca.pxCount ?? -999) !== (cb.pxCount ?? -999)) diffs.push(`pxCount: C=${ca.pxCount} JS=${cb.pxCount}`);
  if ((ca.vsHash ?? null) !== (cb.vsHash ?? null)) diffs.push(`vsHash: C=${ca.vsHash} JS=${cb.vsHash}`);
  if ((ca.fsHash ?? null) !== (cb.fsHash ?? null)) diffs.push(`fsHash: C=${ca.fsHash} JS=${cb.fsHash}`);
  const fa = ca.fv ?? [], fb = cb.fv ?? [];
  if (fa.length !== fb.length) diffs.push(`fv.length: C=${fa.length} JS=${fb.length}`);
  else for (let k = 0; k < fa.length; k++)
    if (!numEq(fa[k], fb[k])) { diffs.push(`fv[${k}]: C=${fmt(fa[k])} JS=${fmt(fb[k])}`); break; }
  const ma = ca.m16 ?? [], mb = cb.m16 ?? [];
  if (ma.length !== mb.length) diffs.push(`m16.length: C=${ma.length} JS=${mb.length}`);
  else for (let k = 0; k < ma.length; k++)
    if (!numEq(ma[k], mb[k], 1e-4)) { diffs.push(`m16[${k}]: C=${fmt(ma[k])} JS=${fmt(mb[k])}`); break; }
  return diffs;
}

function vertexDiff(va: TraceVertex, vb: TraceVertex): string[] {
  const diffs: string[] = [];
  const fields: (keyof TraceVertex)[] = ['ox', 'oy', 'oz', 'cx', 'cy', 'cz', 'cw', 'nx', 'ny', 'nz', 'r', 'g', 'b', 'a', 's', 't', 'nrmx', 'nrmy', 'nrmz'];
  for (const f of fields) {
    if (!numEq(va[f] as number, vb[f] as number)) {
      diffs.push(`    ${f}: C=${(va[f] as number).toFixed(6)} JS=${(vb[f] as number).toFixed(6)}`);
    }
  }
  return diffs;
}

function mvpDiff(a: number[], b: number[]): string[] {
  const diffs: string[] = [];
  for (let i = 0; i < 16; i++) {
    if (!numEq(a[i] ?? 0, b[i] ?? 0, 1e-4)) {
      diffs.push(`    mvp[${i}]: C=${(a[i] ?? 0).toFixed(6)} JS=${(b[i] ?? 0).toFixed(6)}`);
    }
  }
  return diffs;
}

/** Load a binary P6 PPM (top-down row order, RGB). Returns null if absent. */
function loadPPM(path: string): { w: number; h: number; px: Buffer } | null {
  if (!existsSync(path)) return null;
  const buf = readFileSync(path);
  if (buf[0] !== 0x50 || buf[1] !== 0x36) return null; // "P6"
  let pos = 2, fields: number[] = [];
  while (fields.length < 3 && pos < buf.length) {
    // skip whitespace/comments, then read a number
    while (pos < buf.length && (buf[pos] === 0x20 || buf[pos] === 0x0a || buf[pos] === 0x0d || buf[pos] === 0x09 || buf[pos] === 0x23)) {
      if (buf[pos] === 0x23) { while (pos < buf.length && buf[pos] !== 0x0a) pos++; }
      pos++;
    }
    let v = 0;
    while (pos < buf.length && buf[pos] >= 0x30 && buf[pos] <= 0x39) { v = v * 10 + (buf[pos] - 0x30); pos++; }
    fields.push(v);
  }
  pos++; // single whitespace after maxval
  const [w, h] = fields;
  return { w, h, px: buf.subarray(pos, pos + w * h * 3) };
}

/** Pixel-level diff of two PPM snapshots (element-level comparison).
 *  Returns a human-readable report, or null if both absent. */
function pixelDiffReport(cSnap: { w: number; h: number; px: Buffer } | null,
                         jSnap: { w: number; h: number; px: Buffer } | null): string[] {
  if (!cSnap && !jSnap) return ['  (no PPM snapshots — run both dumpers with --dir/PD_TRACE_DIR)'];
  if (!cSnap) return ['  ✗ C snapshot missing'];
  if (!jSnap) return ['  ✗ JS snapshot missing'];
  if (cSnap.w !== jSnap.w || cSnap.h !== jSnap.h)
    return [`  ✗ size differs: C=${cSnap.w}x${cSnap.h} JS=${jSnap.w}x${jSnap.h}`];
  const { w, h } = cSnap;
  const A = cSnap.px, B = jSnap.px;
  let ndiff = 0, sumDelta = 0, maxDelta = 0;
  let firstX = -1, firstY = -1, firstC: number[] | null = null, firstJ: number[] | null = null;
  const chDelta = [0, 0, 0];
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 3;
      let d = 0;
      const ca = [A[i], A[i + 1], A[i + 2]], ja = [B[i], B[i + 1], B[i + 2]];
      for (let ch = 0; ch < 3; ch++) {
        const dd = Math.abs(A[i + ch] - B[i + ch]);
        chDelta[ch] += dd;
        if (dd > d) d = dd;
        sumDelta += dd;
      }
      if (d > 0) {
        ndiff++;
        if (d > maxDelta) maxDelta = d;
        if (firstX < 0) { firstX = x; firstY = y; firstC = ca; firstJ = ja; }
      }
    }
  }
  const total = w * h;
  if (ndiff === 0) return [`  ✓ PIXELS IDENTICAL (${total} px)`];
  const out = [
    `  ✗ ${ndiff}/${total} pixels differ (${(ndiff / total * 100).toFixed(2)}%), maxΔ=${maxDelta}, meanΔ=${(sumDelta / total / 3).toFixed(3)}`,
    `    first diff at (x=${firstX}, y=${firstY}): C=rgb(${firstC!.join(',')}) JS=rgb(${firstJ!.join(',')})`,
    `    per-channel meanΔ: R=${(chDelta[0] / total).toFixed(3)} G=${(chDelta[1] / total).toFixed(3)} B=${(chDelta[2] / total).toFixed(3)}`,
  ];
  // sample 5 more differing pixels across the image for spatial context
  let shown = 0;
  const step = Math.max(1, Math.floor(total / Math.max(1, ndiff)) * 5);
  for (let p = 0; p < total && shown < 5; p += step) {
    const i = p * 3;
    if (A[i] !== B[i] || A[i + 1] !== B[i + 1] || A[i + 2] !== B[i + 2]) {
      out.push(`    (${p % w},${Math.floor(p / w)}): C=rgb(${A[i]},${A[i + 1]},${A[i + 2]}) JS=rgb(${B[i]},${B[i + 1]},${B[i + 2]})`);
      shown++;
    }
  }
  return out;
}

function main() {
  // positional trace paths + --cdir/--jsdir snapshot dirs
  const raw = process.argv.slice(2);
  const posArgs: string[] = [];
  let cDir = '', jsDir = '';
  for (let i = 0; i < raw.length; i++) {
    if (raw[i] === '--cdir') cDir = raw[++i];
    else if (raw[i] === '--jsdir') jsDir = raw[++i];
    else posArgs.push(raw[i]);
  }
  const [cPath, jsPath] = posArgs;
  if (!cPath || !jsPath) {
    console.error('Usage: trace_compare.ts <c_trace.json> <js_trace.json> [--cdir DIR] [--jsdir DIR]');
    process.exit(1);
  }

  const cTrace = JSON.parse(readFileSync(cPath, 'utf8')) as Trace;
  const jsTrace = JSON.parse(readFileSync(jsPath, 'utf8')) as Trace;

  const cBatches = (cTrace.batches ?? []).filter(b => b !== null);
  const jsBatches = (jsTrace.batches ?? []).filter(b => b !== null);
  const cCmds = cTrace.cmds ?? [];
  const jsCmds = jsTrace.cmds ?? [];

  console.error('════════════════════════════════════════════════════════════');
  console.error('  C/JS RENDER-GRAPH DIFFERENTIAL');
  console.error('════════════════════════════════════════════════════════════');
  console.error(`  Script: ${cTrace.script || jsTrace.script} (frame ${cTrace.frame}, ${cTrace.width}x${cTrace.height})`);
  console.error(`  C : ${cCmds.length} cmds, ${cBatches.length} batches, finalHash=${cTrace.finalHash}`);
  console.error(`  JS: ${jsCmds.length} cmds, ${jsBatches.length} batches, finalHash=${jsTrace.finalHash}`);
  console.error(`  Final framebuffer: ${cTrace.finalHash === jsTrace.finalHash ? 'MATCH' : 'DIVERGES'}`);
  console.error('------------------------------------------------------------');

  // ---------- Layer 1: GLCmd stream (the contract) ----------
  console.error('\n[LAYER 1] GLCmd stream (host → renderer contract)');
  if (cCmds.length === 0 && jsCmds.length === 0) {
    console.error('  (no cmd dump on either side — C trace needs the new build,');
    console.error('   JS trace needs the cmds field)');
  } else if (cCmds.length !== jsCmds.length) {
    console.error(`  ✗ COUNT DIFFERS: C=${cCmds.length} JS=${jsCmds.length}`);
  }

  let cmdDiffs = 0, firstCmdDiff = -1;
  const nCmd = Math.max(cCmds.length, jsCmds.length);
  for (let i = 0; i < nCmd; i++) {
    const ca = cCmds[i], cb = jsCmds[i];
    if (!ca || !cb) {
      if (cmdDiffs === 0) { firstCmdDiff = i; console.error(`  ✗ cmd[${i}]: missing on ${!ca ? 'C' : 'JS'} side`); }
      cmdDiffs++; continue;
    }
    const d = cmdDiff(ca, cb);
    if (d.length > 0) {
      cmdDiffs++;
      if (firstCmdDiff < 0) {
        firstCmdDiff = i;
        console.error(`  ✗ cmd[${i}] FIRST DIVERGENCE:`);
        console.error(`    C : ${cmdDesc(ca)}`);
        console.error(`    JS: ${cmdDesc(cb)}`);
        d.forEach(x => console.error(`    ${x}`));
        // show a little context of what surrounds the divergence
        const ctx0 = Math.max(0, i - 3);
        console.error(`    context (C side):`);
        for (let k = ctx0; k <= Math.min(i + 3, cCmds.length - 1); k++)
          console.error(`      [${k}] ${cmdDesc(cCmds[k])}`);
        console.error(`    context (JS side):`);
        for (let k = ctx0; k <= Math.min(i + 3, jsCmds.length - 1); k++)
          console.error(`      [${k}] ${cmdDesc(jsCmds[k])}`);
      }
    }
  }
  if (cmdDiffs === 0 && cCmds.length > 0) console.error(`  ✓ all ${nCmd} commands match (op/mode/args/payloads)`);
  else if (cmdDiffs > 0) console.error(`  ✗ ${cmdDiffs}/${nCmd} commands differ (first at index ${firstCmdDiff})`);

  // ---------- Layer 2-4: drawcalls (semantically aligned) / vertices / FB ----------
  console.error('\n[LAYER 2-4] Drawcalls (cmd-range aligned) / vertices / framebuffer');
  // C merges consecutive BEGIN/END sections with the same prim+MVP into one
  // batch; JS keeps one batch per BEGIN/END (plus synthetic CLEAR/QUAD
  // batches). Align by the source GLCmd range: each JS batch maps to the C
  // batch whose [cmdStart,cmdEnd] covers its cmdStart.
  const cGeo = cBatches.filter(b => (b.verts?.length ?? 0) > 0);
  const jsGeo = jsBatches.filter(b => (b.verts?.length ?? 0) > 0);

  function matchC(jb: TraceBatch): TraceBatch | undefined {
    if (jb.cmdStart === undefined) return undefined;
    return cGeo.find(cb =>
      cb.cmdStart !== undefined && cb.cmdEnd !== undefined &&
      jb.cmdStart! >= cb.cmdStart && jb.cmdStart! <= cb.cmdEnd);
  }

  let firstFBDiverge = -1;
  const usedC = new Set<number>();
  for (const jb of jsBatches) {
    const isClear = jb.mode === -1;
    const cb = (jb.verts?.length ?? 0) > 0 ? matchC(jb) : undefined;
    if (cb) usedC.add(cb.index);

    console.error(`\n[JS batch ${jb.index}] ${isClear ? 'CLEAR' : primName(typeof jb.mode === 'number' ? jb.mode : -1)} nverts=${jb.nverts} cmds[${jb.cmdStart}..${jb.cmdEnd}]`);
    if (!cb && !isClear && (jb.verts?.length ?? 0) > 0) {
      console.error(`  ✗ NO MATCHING C BATCH — C side never drew geometry for these cmds`);
      continue;
    }

    if (cb) {
      // MVP compare
      const mvpDiffs = mvpDiff(cb.mvp, jb.mvp);
      if (mvpDiffs.length) {
        console.error(`  vs C batch ${cb.index} (TRIANGLES nverts=${cb.nverts}, cmds[${cb.cmdStart}..${cb.cmdEnd}])`);
        console.error(`  MVP DIFFERS (${mvpDiffs.length} elements):`);
        mvpDiffs.slice(0, 4).forEach(d => console.error(d));
      } else {
        console.error(`  vs C batch ${cb.index} (TRIANGLES nverts=${cb.nverts}, cmds[${cb.cmdStart}..${cb.cmdEnd}]) — MVP matches`);
      }

      // Vertex compare on normalized triangle sequences
      const cv = toTriangles(cb.mode, cb.verts);
      const jv = toTriangles(jb.mode, jb.verts);
      const nv = Math.max(cv.length, jv.length);
      let vDiverge = -1, nMatch = 0;
      for (let v = 0; v < nv; v++) {
        const a = cv[v], b = jv[v];
        if (!a || !b) { if (vDiverge < 0) vDiverge = v; continue; }
        const vd = vertexDiff(a, b);
        if (vd.length > 0) {
          if (vDiverge < 0) {
            vDiverge = v;
            console.error(`  Vertex ${v} (tri-expanded) DIVERGES:`);
            vd.slice(0, 5).forEach(d => console.error(d));
          }
        } else nMatch++;
      }
      if (vDiverge < 0) console.error(`  ✓ all ${nv} triangle-expanded vertices match`);
      else console.error(`  ✗ vertices diverge at ${vDiverge} (${nMatch}/${nv} match)`);
    } else if (isClear) {
      console.error(`  (C clears immediately at GLCMD_CLEAR — no batch to compare)`);
    }

    // FB hash + pixel-level diff: with snapshot dirs, this is the element-
    // level comparison — exact pixels, coordinates and per-channel deltas.
    if (cb) {
      const fbMatch = cb.fbHash === jb.fbHash;
      console.error(`  FB after drawcall: C=${cb.fbHash} JS=${jb.fbHash} ${fbMatch ? 'match' : 'DIFFERS'}`);
      if (!fbMatch && (cDir || jsDir)) {
        const cSnap = loadPPM(join(cDir, `batch_${String(cb.index).padStart(3, '0')}.ppm`));
        const jSnap = loadPPM(join(jsDir, `batch_${String(jb.index).padStart(3, '0')}.ppm`));
        pixelDiffReport(cSnap, jSnap).forEach(l => console.error(l));
      }
    }
  }

  // C batches that no JS batch maps to: geometry C drew but JS did not
  for (const cb of cGeo) {
    if (!usedC.has(cb.index)) {
      console.error(`\n[C batch ${cb.index}] TRIANGLES nverts=${cb.nverts} cmds[${cb.cmdStart}..${cb.cmdEnd}] ← NO JS BATCH in this range`);
      if (firstFBDiverge < 0) firstFBDiverge = cb.index;
    }
  }

  console.error('\n════════════════════════════════════════════════════════════');
  console.error('  SUMMARY');
  console.error('════════════════════════════════════════════════════════════');
  if (cmdDiffs > 0) {
    console.error(`  ✗ cmd stream diverges at index ${firstCmdDiff} (${cmdDiffs} cmds differ)`);
    console.error(`  → Fix the HOST layer first: the two sides record different GL commands.`);
    console.error(`    Nothing downstream (drawcalls/vertices/pixels) can match until this agrees.`);
  } else if (cCmds.length > 0) {
    console.error(`  ✓ cmd stream identical (${cCmds.length} commands)`);
  }
  if (firstFBDiverge >= 0) {
    console.error(`  ✗ geometry asymmetry at C batch ${firstFBDiverge}: one side drew, the other did not`);
  }
  console.error(`  Final framebuffer: ${cTrace.finalHash === jsTrace.finalHash ? 'MATCH' : 'DIVERGES'} (C=${cTrace.finalHash} JS=${jsTrace.finalHash})`);
  console.error(`  → If cmd stream + vertices all match but final differs, the divergence`);
  console.error(`    is in rasterization / fragment shading / texture sampling / blending.`);
}

main();
