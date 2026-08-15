// peaks_headless.ts — render examples/opengl/28_peaks.pss with the JS SoftRenderer
// (node, no GL context) and write a PNG, for visual equivalence vs the C
// reference `polydraw-render`. Minimal PNG writer included (zlib only).
import { readFileSync, writeFileSync } from 'node:fs';
import { deflateSync } from 'node:zlib';
import { makeEngine } from '../src/gpu/mainloop.ts';
import { SoftRenderer } from '../src/gpu/softrender.ts';

function writePNG(path: string, w: number, h: number, rgb: Uint8Array): void {
  // rgb: w*h*3, top-down rows. PNG wants RGBA per pixel with filter byte per row.
  const stride = w * 4;
  const raw = Buffer.alloc((stride + 1) * h);
  for (let y = 0; y < h; y++) {
    raw[y * (stride + 1)] = 0; // filter: none
    for (let x = 0; x < w; x++) {
      const si = (y * w + x) * 3;
      const di = y * (stride + 1) + 1 + x * 4;
      raw[di] = rgb[si]; raw[di + 1] = rgb[si + 1]; raw[di + 2] = rgb[si + 2]; raw[di + 3] = 255;
    }
  }
  const idat = deflateSync(raw);
  const crcTable = (() => {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      t[n] = c >>> 0;
    }
    return t;
  })();
  const crc32 = (buf: Buffer) => {
    let c = 0xffffffff;
    for (let i = 0; i < buf.length; i++) c = crcTable[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
  };
  const chunk = (type: string, data: Buffer) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length, 0);
    const t = Buffer.from(type, 'ascii');
    const body = Buffer.concat([t, data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(body), 0);
    return Buffer.concat([len, body, crc]);
  };
  const sig = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8; ihdr[9] = 2; // 8-bit, color type 2 (RGB)
  const png = Buffer.concat([sig, chunk('IHDR', ihdr), chunk('IDAT', idat), chunk('IEND', Buffer.alloc(0))]);
  writeFileSync(path, png);
}

const W = 640, H = 480;
const src = readFileSync(process.argv[2], 'utf8');
const eng = makeEngine(src, W, H);
if (!eng) { console.error('compile failed'); process.exit(1); }
const sr = new SoftRenderer({ width: W, height: H, fragment: (v) => [v.r, v.g, v.b] });
eng.attach(sr as any);
eng.renderFrame(30);
const rgb = sr.toRGB8();
const out = process.argv[3] || '/tmp/peaks_js.png';
writePNG(out, W, H, rgb);
console.log('wrote', out);
