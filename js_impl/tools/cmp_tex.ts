import { readFileSync } from 'node:fs';
import { decodePNG } from '../src/gpu/png.ts';

function stats(path: string) {
  const d = decodePNG(readFileSync(path));
  const N = d.width * d.height;
  let blue = 0, green = 0, brown = 0, red = 0, black = 0, dim = 0, nonbg = 0;
  let sr = 0, sg = 0, sb = 0;
  for (let i = 0; i < N; i++) {
    const r = d.rgb[i * 3] / 255, g = d.rgb[i * 3 + 1] / 255, b = d.rgb[i * 3 + 2] / 255;
    sr += r; sg += g; sb += b;
    const sum = r + g + b;
    if (sum < 0.05) { black++; continue; }
    if (sum < 0.18) { dim++; continue; }
    nonbg++;
    if (b > r && b > g * 1.1) blue++;
    else if (g >= r && g >= b) green++;
    else if (r > g * 1.3 && r > b * 1.3) red++;
    else if (r > b && g > b * 1.1) brown++;
  }
  const p = (n: number) => ((100 * n) / N).toFixed(2) + '%';
  return {
    file: path,
    size: `${d.width}x${d.height}`,
    blackBg: p(black),
    dim: p(dim),
    coverage: p(nonbg),
    blue: p(blue), green: p(green), brown: p(brown), red: p(red),
    avgRGB: [ (sr/N).toFixed(3), (sg/N).toFixed(3), (sb/N).toFixed(3) ],
  };
}

console.log('C :', JSON.stringify(stats(process.argv[2]), null, 0));
console.log('TS:', JSON.stringify(stats(process.argv[3]), null, 0));
