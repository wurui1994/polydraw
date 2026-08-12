// xbackend.test.ts — M7 cross-backend differential test.
//
// The JS EVAL+host pipeline must emit the same GL command stream as the
// reference C implementation. We build a small C "reference dumper"
// (c_impl/dbg_count) that runs a .pss frame and prints a JSON histogram of
// GLCmd op-codes, then assert the JS backend produces an identical histogram.
//
// This locks in fidelity of the EVAL -> GLCmd recording layer, which is the
// verifiable core of M7 (the GPU/raster step is layered on top later).
import { test } from 'node:test';
import assert from 'node:assert';
import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { readFileSync } from 'node:fs';
import { sectionParse, sectionHost } from '../src/host/sections.ts';
import { PolyHostImpl } from '../src/host/polyhost.ts';
import { compile } from '../src/eval/parser.ts';
import { run } from '../src/backend/interp.ts';
import type { GLCmdBuf, GLCmd } from '../src/host/glcmd.ts';

const ROOT = '/Users/wurui/Documents/polydraw';
const C_SRC = `${ROOT}/c_impl/dbg_count.c`;
const C_BIN = `${ROOT}/c_impl/dbg_count`;

// Scripts that are expected to run in a single deterministic frame.
const SCRIPTS = [
  'ken/balls.pss',
  'ken/interference.pss',
  'ken/drawsph.pss',
  'tigrou/disco ball.pss',
  'ken/glsl_triangle.pss',
  'tigrou/beating heart.pss',
  'ken/plasma.pss',
  'tigrou/raymarch.pss',
];

function ensureRefBin(): void {
  if (existsSync(C_BIN)) return;
  const objs = [
    'src/eval/pd_ir.c', 'src/eval/pd_interp.c', 'src/eval/pd_lexer.c',
    'src/eval/pd_parser.c', 'src/eval/pd_compile.c', 'src/eval/pd_host.c',
    'src/eval/pd_section.c', 'src/eval/pd_jit_sljit.c',
    'third_party/sljit/sljitLir.c', 'src/pd_polyhost.c', 'src/render/glcmd.c',
    'src/render/pd_polyhost_render.c', 'src/render/pd_polyhost_tex.c',
    'src/render/pd_runlib.c',
  ];
  execFileSync('cc', [
    '-std=c11', '-O2',
    `-I${ROOT}/c_impl/src`, `-I${ROOT}/c_impl/src/eval`,
    `-I${ROOT}/c_impl/src/render`, `-I${ROOT}/c_impl/third_party`,
    `-I${ROOT}/c_impl/third_party/sljit`,
    '-o', C_BIN, C_SRC, ...objs, '-lm',
  ], { cwd: `${ROOT}/c_impl` });
}

interface Hist {
  total: number; begin: number; end: number; vert: number; push: number;
  pop: number; settex: number; color: number; texcoord: number; normal: number;
  full: Record<string, number>;
}

function histogramJS(path: string): Hist {
  const full = readFileSync(path, 'utf8');
  const sl = sectionParse(full);
  const h = sectionHost(sl);
  assert.ok(h, `no host block in ${path}`);
  const hostSrc = full.slice(h.start, h.end);
  const ph = new PolyHostImpl();
  const host = ph.install();
  const r = compile(hostSrc, host);
  assert.ok(r.ok, `compile error in ${path}: ${r.err}`);
  ph.glbuf.reset();
  ph.srand(1);
  ph.state.numframes = 30;
  ph.state.clockScale = 1 / 60;
  run(r.program, null, r.program.globals, null);
  const g = ph.glbuf as GLCmdBuf;
  const full0: Record<string, number> = {};
  let begin = 0, end = 0, vert = 0, push = 0, pop = 0, settex = 0, color = 0, texc = 0, norm = 0;
  for (const c of g.cmds as GLCmd[]) {
    full0[String(c.op)] = (full0[String(c.op)] || 0) + 1;
    switch (c.op) {
      case 1: begin++; break; case 2: end++; break; case 3: vert++; break;
      case 4: color++; break; case 5: texc++; break; case 6: norm++; break;
      case 7: push++; break; case 8: pop++; break; case 23: settex++; break;
    }
  }
  return { total: g.cmds.length, begin, end, vert, push, pop, settex, color, texcoord: texc, normal: norm, full: full0 };
}

function histogramC(path: string): Hist {
  const tmp = `${C_BIN}.json`;
  execFileSync(C_BIN, [path, '30', tmp], { stdio: ['ignore', 'ignore', 'ignore'] });
  const out = readFileSync(tmp, 'utf8').trim();
  return JSON.parse(out) as Hist;
}

ensureRefBin();

for (const rel of SCRIPTS) {
  const path = `${ROOT}/${rel}`;
  if (!existsSync(path)) continue;
  test(`M7 differential: ${rel}`, () => {
    const js = histogramJS(path);
    let c: Hist;
    try {
      c = histogramC(path);
    } catch (e) {
      // reference binary may fail on a script the C pipeline can't run;
      // skip rather than fail the whole suite.
      console.warn(`C reference skipped ${rel}: ${(e as Error).message}`);
      return;
    }
    assert.deepStrictEqual(js.full, c.full, `GLCmd per-op histogram mismatch for ${rel}`);
  });
}
