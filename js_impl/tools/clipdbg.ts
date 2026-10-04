import { readFileSync } from 'node:fs';
import { makeEngine } from '../src/gpu/mainloop.ts';
import { SoftRenderer } from '../src/gpu/softrender.ts';
const src = readFileSync(process.argv[2] ?? '../ken/texture.pss', 'utf8');
const frame = parseInt(process.argv[3] ?? '30', 10);
const engine = makeEngine(src, 640, 480, { defaultFovy: 73.74 });
engine.ctx.setClockScale(1.0 / 60.0);
const sr = new SoftRenderer({ width: 640, height: 480, fragment: (v) => [v.r, v.g, v.b] });
(engine as never as { attach: (s: unknown) => void }).attach(sr as never);
const texAll = new Map<number, { id: number }>();
let batches: ReturnType<typeof engine.ff.replay> = [];
for (let f = 0; f <= frame; f++) { engine.ctx.runFrame(f); batches = engine.ff.replay(engine.ctx.glbuf); for (const t of engine.ff.texData) texAll.set(t.id, t as never); }
sr.render({ batches, captures: engine.ff.captures, texData: [...texAll.values()] as never });
let nz = 0; const rgb = sr.toRGB8();
for (let i = 0; i < rgb.length; i += 3) if (rgb[i] + rgb[i + 1] + rgb[i + 2] > 24) nz++;
console.log('nonblank:', nz, '/', rgb.length / 3, ((nz / (rgb.length / 3)) * 100).toFixed(1) + '%');
import { writeFileSync } from 'node:fs';
import { writePNG } from '../src/gpu/png.ts';
writeFileSync('/tmp/texture_js.png', writePNG(rgb, 640, 480));
console.log('wrote /tmp/texture_js.png');

