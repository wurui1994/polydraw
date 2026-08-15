// dbg_blur.ts — temp: render clock blur pass and dump a few samples.
import { readFileSync } from 'node:fs';
import { makeEngine } from '../src/gpu/mainloop.ts';
import { SoftRenderer } from '../src/gpu/softrender.ts';

const src = readFileSync('/Users/wurui/Documents/polydraw/tigrou/clock.pss', 'utf8');
const eng = makeEngine(src, 64, 48, { defaultFovy: 73.74 });
if (!eng) process.exit(1);
eng.ctx.setClockScale(1 / 60);
const sr = new SoftRenderer({ width: 64, height: 48, fragment: (v) => [v.r, v.g, v.b] });
eng.attach(sr as never);
for (let f = 0; f <= 30; f++) eng.ctx.runFrame(f);
const batches = eng.ff.replay(eng.ctx.glbuf) as any[];
const blur = batches[batches.length - 1];
console.log('blur batch: tex', blur.tex, 'shaderF', !!blur.shaderF, 'verts', blur.verts.length);
console.log('v0:', JSON.stringify(blur.verts[0]), 'v1:', JSON.stringify(blur.verts[1]), 'v2:', JSON.stringify(blur.verts[2]));
// tex0 content stats
sr.render({ batches, captures: eng.ff.captures, texData: eng.ff.texData });
const t0 = sr['tex'].get(0);
if (t0) {
  let sum = 0, nz = 0;
  for (let i = 0; i < t0.data.length; i += 3) {
    sum += t0.data[i] + t0.data[i + 1] + t0.data[i + 2];
    if (t0.data[i] > 0.1) nz++;
  }
  console.log('tex0 avg rgb:', (sum / (t0.data.length)).toFixed(4), 'nz:', nz, 'w:', t0.w, 'h:', t0.h);
}
// sample some pixels of final image
const img = sr.getImage();
for (const [x, y] of [[10, 10], [32, 24], [50, 30], [20, 40]]) {
  const i = (y * 64 + x) * 3;
  console.log(`px(${x},${y}) =`, img[i].toFixed(3), img[i + 1].toFixed(3), img[i + 2].toFixed(3));
}
