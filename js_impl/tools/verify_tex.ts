// verify_tex.ts — render ken/texture.pss through the TS SoftRenderer and check
// that the multitexture fragment shader (frag#1) samples tex0/tex1/tex2 via the
// per-unit texFn, producing an earth/wood/red mix. Mirrors C verification.
import { readFileSync, writeFileSync, existsSync, readdirSync, unlinkSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { makeEngine } from '../src/gpu/mainloop.ts';
import { SoftRenderer } from '../src/gpu/softrender.ts';
import { writePNG, decodePNG } from '../src/gpu/png.ts';

function decodeImage(file: string): { w: number; h: number; rgb: number[] } | null {
  try {
    const lower = file.toLowerCase();
    if (!(lower.endsWith('.png') || lower.endsWith('.jpg') || lower.endsWith('.jpeg'))) return null;
    let buf: Buffer;
    if (lower.endsWith('.png')) buf = readFileSync(file);
    else {
      const p = join(tmpdir(), `pd_${Date.now()}_${Math.random().toString(36).slice(2)}.png`);
      execFileSync('sips', ['-s', 'format', 'png', file, '--out', p], { stdio: 'ignore' });
      buf = readFileSync(p);
      try { unlinkSync(p); } catch {}
    }
    // minimal PNG decode reimplemented inline to avoid import churn
    const d = decodePNG(buf);
    const rgb: number[] = new Array(d.width * d.height);
    for (let i = 0; i < d.width * d.height; i++) {
      const r = d.rgb[i * 3], g = d.rgb[i * 3 + 1], b = d.rgb[i * 3 + 2];
      rgb[i] = (0xff << 24) | (b << 16) | (g << 8) | r;
    }
    return { w: d.width, h: d.height, rgb };
  } catch { return null; }
}

function makeImageLoader(scriptPath: string) {
  const dirs: string[] = [];
  let dir = dirname(scriptPath);
  for (;;) { dirs.push(dir); const up = dirname(dir); if (up === dir) break; dir = up; }
  return (file: string) => {
    if (file.startsWith('/')) return decodeImage(file);
    for (const d of dirs) { const p = join(d, file); if (existsSync(p)) return decodeImage(p); }
    return null;
  };
}

function classify(img: Float32Array, total: number) {
  let blue = 0, green = 0, brown = 0, red = 0, other = 0;
  for (let i = 0; i < total; i++) {
    const r = img[i * 3], g = img[i * 3 + 1], b = img[i * 3 + 2];
    // skip near-black background
    if (r + g + b < 0.18) { other++; continue; }
    if (b > r && b > g * 1.1) blue++;
    else if (g >= r && g >= b) green++;
    else if (r > g * 1.3 && r > b * 1.3) red++;
    else if (r > b && g > b * 1.1) brown++;
    else other++;
  }
  return { blue, green, brown, red, other };
}

const script = process.argv[2] || '/Users/wurui/Documents/polydraw/ken/texture.pss';
const frame = process.argv[3] ? parseInt(process.argv[3], 10) : 1;

const src = readFileSync(script, 'utf8');
const eng = makeEngine(src, 640, 480, {
  defaultFovy: 73.74,
  imageLoader: makeImageLoader(script),
});
if (!eng) { console.error('ENGINE COMPILE FAIL'); process.exit(1); }

const sr = new SoftRenderer({ width: 640, height: 480, fragment: (v) => [v.r, v.g, v.b] });
eng.attach(sr as any);

// Render 0..frame sequentially: textures uploaded under `numframes==0` (frame 0)
// persist in the SoftRenderer's tex map (C's tex_obj[] persists across frames).
let sf: any;
for (let f = 0; f <= frame; f++) sf = eng.renderFrame(f)!;
// dump batches: cube should have texUnits [0,1,2,-1] and shaderF frag#1
let cube: any = null, capquad: any = null;
for (const b of sf.batches) {
  if (b.shaderF && b.texUnits && (b.texUnits[0] === 0) && b.texUnits[1] === 1 && b.texUnits[2] === 2) cube = b;
  if (b.captureTarget) capquad = b;
}
console.log('batches:', sf.batches.length, 'captures:', JSON.stringify(sf.captures), 'texData:', sf.texData.length);
if (cube) console.log('CUBE texUnits=', cube.texUnits, 'shaderF.len=', cube.shaderF.length, 'useTex=', cube.useTex);
else console.log('CUBE NOT FOUND');
if (capquad) console.log('CAPQUAD shaderF.len=', capquad.shaderF?.length, 'captureTarget=', capquad.captureTarget);

const img = sr.getImage();
// DEBUG: inspect texture map + cube vertex NDC to find why black
const texMap: any = (sr as any).tex;
console.log('DEBUG tex map size =', texMap ? texMap.size : 'n/a',
  'keys=', texMap ? Array.from(texMap.keys()) : []);
if (cube) {
  const mvp = new Float64Array(16);
  for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++) {
    let s = 0; for (let k = 0; k < 4; k++) s += (cube.projection[k*4+r]) * (cube.modelview[c*4+k]); mvp[c*4+r] = s;
  }
  console.log('DEBUG cube verts (ndc x,y,z):');
  for (const v of cube.verts.slice(0, 8)) {
    const x = v.x, y = v.y, z = v.z, w = 1;
    const cx = mvp[0]*x+mvp[4]*y+mvp[8]*z+mvp[12]*w;
    const cy = mvp[1]*x+mvp[5]*y+mvp[9]*z+mvp[13]*w;
    const cw = mvp[3]*x+mvp[7]*y+mvp[11]*z+mvp[15]*w;
    console.log('  v', x.toFixed(2), y.toFixed(2), z.toFixed(2), '-> clipw', cw.toFixed(4), 'ndc', (cx/cw).toFixed(3), (cy/cw).toFixed(3));
  }
}
const total = 640 * 480;
const cls = classify(img, total);
const pct = (n: number) => ((100 * n) / total).toFixed(2) + '%';
console.log('frame', frame, 'classification:');
console.log('  blue(earth)~', pct(cls.blue), ' green~', pct(cls.green), ' brown(wood)~', pct(cls.brown), ' red~', pct(cls.red), ' other(black/bg)~', pct(cls.other));

// sanity: a multitexture-correct render must contain BOTH earth-blue/green AND
// wood-brown AND a non-trivial amount of red from the captured tex2.
const ok = cls.blue + cls.green > 0 && cls.brown > 0 && cls.red > 0;
console.log(ok ? 'TEXTURE-MIX OK' : 'TEXTURE-MIX FAIL');

const out = process.argv[4] || '/tmp/tex_js_f' + frame + '.png';
const u8 = new Uint8Array(total * 3);
for (let i = 0; i < total * 3; i++) u8[i] = Math.max(0, Math.min(255, Math.round(img[i] * 255)));
const buf = writePNG(u8, 640, 480);
writeFileSync(out, buf);
console.log('wrote', out);
