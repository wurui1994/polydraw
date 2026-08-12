// softgolden.test.ts — M7 software-raster golden test.
//
// The JS EVAL → host → GLCmd → FixedFunc → SoftRenderer pipeline must
// reproduce the reference render bit-for-bit (no GL context required).
// The golden is pyref/golden/balls_f5.png, regenerable via pyref/verify.py
// and cross-checked against the C GL offscreen renderer (99%+ exact, rest
// within 2/255 due to GL vs software edge rules).
import { test } from 'node:test';
import assert from 'node:assert';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { sectionParse, sectionHost } from '../src/host/sections.ts';
import { PolyHostImpl } from '../src/host/polyhost.ts';
import { compile } from '../src/eval/parser.ts';
import { run } from '../src/backend/interp.ts';
import { FixedFunc } from '../src/gpu/fixedfunc.ts';
import { SoftRenderer, ballsFragment } from '../src/gpu/softrender.ts';
import { decodePNG } from '../src/gpu/png.ts';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..', '..');
const GOLDEN = join(HERE, 'golden', 'balls_f5.png');

// Note: the balls' @f discard (length(t.xy)>1) is not handled by ballsFragment
// in isolation from the vertex shader's gl_MultiTexCoord0 — it is: s/t are the
// interpolated glTexCoord from each glVertex's preceding glTexCoord.
function renderBalls(frame: number, w: number, h: number): Uint8Array {
  const full = readFileSync(join(ROOT, 'ken', 'balls.pss'), 'utf8');
  const sl = sectionParse(full);
  const hs = sectionHost(sl);
  assert.ok(hs, 'no host block');
  const ph = new PolyHostImpl();
  const host = ph.install();
  const r = compile(full.slice(hs.start, hs.end), host);
  assert.ok(r.ok, `compile: ${r.err}`);
  ph.state.xres = w; ph.state.yres = h;
  ph.state.clockScale = 1 / 60;
  for (let f = 0; f <= frame; f++) {
    ph.state.numframes = f;
    ph.glbuf.reset();
    run(r.program, null, r.program.globals, null);
  }
  const ff = new FixedFunc(w, h, 73.74);
  const batches = ff.replay(ph.glbuf);
  assert.strictEqual(batches.length, 16384, 'one batch per ball polygon');
  const sr = new SoftRenderer({ width: w, height: h, fragment: ballsFragment });
  sr.render(batches);
  return sr.toRGB8();
}

test('soft golden: balls frame 5 @320x240 matches reference bit-for-bit', () => {
  const got = renderBalls(5, 320, 240);
  const golden = decodePNG(readFileSync(GOLDEN));
  assert.strictEqual(golden.width, 320);
  assert.strictEqual(golden.height, 240);
  assert.strictEqual(got.length, golden.rgb.length);
  let diff = 0;
  for (let i = 0; i < got.length; i++) {
    if (got[i] !== golden.rgb[i]) diff++;
  }
  assert.strictEqual(diff, 0, `${diff} channels differ from golden`);
});

test('soft render: geometry sanity (balls have content, canvas not blank)', () => {
  const got = renderBalls(30, 640, 480);
  assert.strictEqual(got.length, 640 * 480 * 3, 'full-res canvas rendered');
  let nz = 0;
  for (let i = 0; i < got.length; i++) if (got[i] > 0) nz++;
  assert.ok(nz / got.length > 0.5, 'less than half of channels are touching something');
});