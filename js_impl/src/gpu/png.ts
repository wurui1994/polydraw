// png.ts — minimal PNG reader/writer (RGB8) using node:zlib. Zero deps.
import { deflateSync, inflateSync } from 'node:zlib';

export function writePNG(rgb: Uint8Array, w: number, h: number): Buffer {
  const raw = Buffer.alloc(h * (1 + w * 3));
  for (let y = 0; y < h; y++) {
    raw[y * (1 + w * 3)] = 0; // filter: none
    rgb.subarray(y * w * 3, (y + 1) * w * 3).forEach((v, i) => { raw[y * (1 + w * 3) + 1 + i] = v; });
  }
  const idat = deflateSync(raw);
  const chunks: Buffer[] = [];
  chunks.push(Buffer.from('\x89PNG\r\n\x1a\n', 'binary'));
  chunks.push(pngChunk('IHDR', ihdr(w, h)));
  chunks.push(pngChunk('IDAT', idat));
  chunks.push(pngChunk('IEND', Buffer.alloc(0)));
  return Buffer.concat(chunks, chunks.reduce((a, c) => a + c.length, 0));
}

function ihdr(w: number, h: number): Buffer {
  if (w > 0xffffffff || h > 0xffffffff) throw new Error('PNG size overflow');
  const b = Buffer.alloc(13);
  b.writeUInt32BE(w, 0); b.writeUInt32BE(h, 4);
  b[8] = 8; // bit depth
  b[9] = 2; // color type: truecolor RGB
  b[10] = 0; // compression
  b[11] = 0; // filter
  b[12] = 0; // interlace
  return b;
}

function pngChunk(type: string, data: Buffer): Buffer {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length, 0);
  // CRC-32 over type+data
  const crc = crc32(Buffer.concat([Buffer.from(type, 'binary'), data]));
  const c = Buffer.alloc(4);
  c.writeUInt32BE(crc >>> 0, 0);
  return Buffer.concat([len, Buffer.from(type, 'binary'), data, c]);
}

function crc32(buf: Buffer): number {
  let crc = 0xffffffff;
  for (let i = 0; i < buf.length; i++) {
    crc ^= buf[i];
    for (let k = 0; k < 8; k++) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (crc ^ 0xffffffff) >>> 0;
}

// Decode an RGB8 PNG (colour type 2, bit depth 8, no interlace) into
// raw top-left-origin RGB bytes. Used by the soft-render golden test.
export interface DecodedPNG { width: number; height: number; rgb: Uint8Array; }

export function decodePNG(buf: Buffer): DecodedPNG {
  if (!buf || buf.length < 24 || buf.readUInt32BE(0) !== 0x89504e47) {
    throw new Error('not a PNG');
  }
  let w = 0, h = 0;
  let bitDepth = 8, colorType = 2;
  const idat: Buffer[] = [];
  let off = 8;
  while (off + 12 <= buf.length) {
    const len = buf.readUInt32BE(off);
    const type = buf.toString('binary', off + 4, off + 8);
    const data = buf.subarray(off + 8, off + 8 + len);
    if (type === 'IHDR') {
      w = data.readUInt32BE(0); h = data.readUInt32BE(4);
      bitDepth = data[8]; colorType = data[9];
    } else if (type === 'IDAT') {
      idat.push(Buffer.from(data));
    } else if (type === 'IEND') {
      break;
    }
    off += 12 + len;
  }
  if (w === 0 || h === 0) throw new Error('PNG lacks IHDR');
  if (bitDepth !== 8 || colorType !== 2) {
    throw new Error(`unsupported PNG (bitDepth=${bitDepth}, colorType=${colorType})`);
  }
  const raw = inflateSync(Buffer.concat(idat));
  const stride = w * 3;
  const rgb = new Uint8Array(w * h * 3);
  const prev = new Uint8Array(stride);
  for (let y = 0; y < h; y++) {
    const f = raw[y * (stride + 1)];
    const row = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    const cur = new Uint8Array(stride);
    for (let x = 0; x < stride; x++) {
      const a = x >= 3 ? cur[x - 3] : 0;
      const b = prev[x];
      const c = x >= 3 ? prev[x - 3] : 0;
      let v = row[x];
      if (f === 1) v += a;
      else if (f === 2) v += b;
      else if (f === 3) v += (a + b) >> 1;
      else if (f === 4) {
        const p = a + b - c;
        const pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
        v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
      }
      cur[x] = v & 0xff;
    }
    rgb.set(cur, y * stride);
    prev.set(cur);
  }
  return { width: w, height: h, rgb };
}