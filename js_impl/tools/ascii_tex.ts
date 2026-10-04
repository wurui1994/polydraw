import { readFileSync } from 'node:fs';
import { decodePNG } from '../src/gpu/png.ts';

function ascii(path: string, cols = 80, rows = 40) {
  const d = decodePNG(readFileSync(path));
  const { width: w, height: h, rgb } = d;
  // brightness ramp
  const ramp = ' .:-=+*#%@';
  const lines: string[] = [];
  for (let ry = 0; ry < rows; ry++) {
    let line = '';
    for (let rx = 0; rx < cols; rx++) {
      // sample center of cell
      const x = Math.min(w - 1, Math.floor((rx + 0.5) / cols * w));
      const y = Math.min(h - 1, Math.floor((ry + 0.5) / rows * h));
      const i = (y * w + x) * 3;
      const r = rgb[i] / 255, g = rgb[i + 1] / 255, b = rgb[i + 2] / 255;
      const lum = (r * 0.3 + g * 0.59 + b * 0.11);
      const idx = Math.max(0, Math.min(ramp.length - 1, Math.round(lum * (ramp.length - 1))));
      line += ramp[idx];
    }
    lines.push(line);
  }
  return lines.join('\n');
}

console.log('===== C  (/tmp/tex_c_f1.png) =====');
console.log(ascii(process.argv[2]));
console.log('\n===== TS (/tmp/tex_js_f1.png) =====');
console.log(ascii(process.argv[3]));
