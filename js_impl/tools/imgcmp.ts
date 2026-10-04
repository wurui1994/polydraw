// imgcmp.ts — compare two render PNGs and report where they differ.
//
//   node --experimental-strip-types tools/imgcmp.ts a.png b.png [--grid]
//
// Prints per-image stats (mean luminance, % non-blank, top colors) plus a
// difference breakdown: overall %, and a 4x4 grid of per-cell diff % so
// "top half wrong" vs "everything shifted" is visible at a glance.
import { readFileSync } from 'node:fs';
import { decodePNG } from '../src/gpu/png.ts';

function stats(path: string): string {
  const d = decodePNG(readFileSync(path));
  let sum = 0, nz = 0;
  const hist = new Map<string, number>();
  const n = d.width * d.height;
  for (let i = 0; i < n; i++) {
    const r = d.rgb[i * 3], g = d.rgb[i * 3 + 1], b = d.rgb[i * 3 + 2];
    sum += (r + g + b) / 3;
    if (r + g + b > 24) nz++;
    const key = `${Math.round(r / 32)},${Math.round(g / 32)},${Math.round(b / 32)}`;
    hist.set(key, (hist.get(key) ?? 0) + 1);
  }
  const top = [...hist.entries()].sort((a, b) => b[1] - a[1]).slice(0, 4)
    .map(([k, v]) => `${k}:${((v / n) * 100).toFixed(0)}%`).join(' ');
  return `mean=${(sum / n).toFixed(1)} nonblank=${((nz / n) * 100).toFixed(1)}% top=[${top}]`;
}

function grid(aPath: string, bPath: string): void {
  const a = decodePNG(readFileSync(aPath));
  const b = decodePNG(readFileSync(bPath));
  if (a.width !== b.width || a.height !== b.height) {
    console.log(`size mismatch: ${a.width}x${a.height} vs ${b.width}x${b.height}`);
    return;
  }
  const G = 4;
  const cells = new Float64Array(G * G);
  const cnt = new Float64Array(G * G);
  let diff = 0;
  const n = a.width * a.height;
  for (let i = 0; i < n; i++) {
    const x = i % a.width, y = (i / a.width) | 0;
    const d = Math.abs(a.rgb[i * 3] - b.rgb[i * 3]) + Math.abs(a.rgb[i * 3 + 1] - b.rgb[i * 3 + 1]) + Math.abs(a.rgb[i * 3 + 2] - b.rgb[i * 3 + 2]);
    if (a.rgb[i * 3] !== b.rgb[i * 3] || a.rgb[i * 3 + 1] !== b.rgb[i * 3 + 1] || a.rgb[i * 3 + 2] !== b.rgb[i * 3 + 2]) diff++;
    const gx = Math.min(G - 1, (x / a.width * G) | 0);
    const gy = Math.min(G - 1, (y / a.height * G) | 0);
    const ci = gy * G + gx;
    if (d > 12) cells[ci]++;
    cnt[ci]++;
  }
  console.log(`overall diff: ${((diff / n) * 100).toFixed(2)}%  (>12/3 chan per cell):`);
  for (let gy = 0; gy < G; gy++) {
    const row: string[] = [];
    for (let gx = 0; gx < G; gx++) {
      const ci = gy * G + gx;
      row.push(`${((cells[ci] / cnt[ci]) * 100).toFixed(0).padStart(4)}%`);
    }
    console.log('  ' + row.join(' '));
  }
}

const args = process.argv.slice(2).filter((s) => s !== '--grid');
const [a, b] = args;
if (!a || !b) { console.error('usage: imgcmp.ts a.png b.png'); process.exit(1); }
console.log(`A ${a}: ${stats(a)}`);
console.log(`B ${b}: ${stats(b)}`);
grid(a, b);
