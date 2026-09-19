import fs from "node:fs";
import path from "node:path";
import jpeg from "jpeg-js";
import { PNG } from "pngjs";
import { PdArray } from "../runtime/runtime.js";

const KGL_BGRA32 = 0;
const KGL_FLOAT = 4;
const KGL_VEC4 = 5;

export function makeTextureImageLoader(resolveTexturePath) {
  return (texturePath, { format = 0 } = {}) => {
    const resolvedPath = resolveTexturePath(texturePath);
    const rgba = loadRgba(resolvedPath);
    if (!rgba) return null;
    return {
      width: rgba.width,
      height: rgba.height,
      format,
      resolvedPath,
      rgba,
      array: rgbaToPdArray(rgba, format),
    };
  };
}

function loadRgba(filePath) {
  if (!fs.existsSync(filePath)) return null;
  const data = fs.readFileSync(filePath);
  const ext = path.extname(filePath).toLowerCase();
  if (ext === ".ppm") return loadPpm(data);
  if (ext === ".jpg" || ext === ".jpeg") {
    const decoded = jpeg.decode(data, { useTArray: true, colorTransform: true });
    return { width: decoded.width, height: decoded.height, data: decoded.data };
  }
  if (ext === ".png") {
    const decoded = PNG.sync.read(data);
    return { width: decoded.width, height: decoded.height, data: decoded.data };
  }
  if (data.length >= 2 && data[0] === 0x50 && (data[1] === 0x33 || data[1] === 0x36)) return loadPpm(data);
  throw new Error(`unsupported texture image format: ${filePath}`);
}

function loadPpm(data) {
  let offset = 0;
  const token = () => {
    while (offset < data.length) {
      const c = data[offset];
      if (c === 0x23) {
        while (offset < data.length && data[offset] !== 0x0a) offset++;
      } else if (c <= 0x20) {
        offset++;
      } else {
        break;
      }
    }
    const start = offset;
    while (offset < data.length && data[offset] > 0x20) offset++;
    return data.toString("ascii", start, offset);
  };
  const magic = token();
  if (magic !== "P3" && magic !== "P6") throw new Error("unsupported PPM texture");
  const width = Number(token());
  const height = Number(token());
  const max = Number(token());
  if (!Number.isFinite(width) || !Number.isFinite(height) || max <= 0) {
    throw new Error("invalid PPM texture header");
  }
  const rgba = new Uint8Array(width * height * 4);
  if (magic === "P3") {
    for (let i = 0; i < width * height; i++) {
      const dst = i * 4;
      rgba[dst + 0] = scaleByte(Number(token()), max);
      rgba[dst + 1] = scaleByte(Number(token()), max);
      rgba[dst + 2] = scaleByte(Number(token()), max);
      rgba[dst + 3] = 255;
    }
  } else {
    while (offset < data.length && data[offset] <= 0x20) offset++;
    for (let i = 0; i < width * height; i++) {
      const src = offset + i * 3;
      const dst = i * 4;
      rgba[dst + 0] = scaleByte(data[src + 0], max);
      rgba[dst + 1] = scaleByte(data[src + 1], max);
      rgba[dst + 2] = scaleByte(data[src + 2], max);
      rgba[dst + 3] = 255;
    }
  }
  return { width, height, data: rgba };
}

function rgbaToPdArray({ width, height, data }, format) {
  const baseFormat = Number(format) & 0x0f;
  const dimensions = baseFormat === KGL_VEC4 ? [height, width, 4] : [height, width];
  const array = new PdArray(dimensions);
  const pixels = width * height;
  if (baseFormat === KGL_FLOAT) {
    for (let i = 0; i < pixels; i++) {
      const base = i * 4;
      array.setLinear(i, (Number(data[base]) + Number(data[base + 1]) + Number(data[base + 2])) / (255 * 3));
    }
  } else if (baseFormat === KGL_VEC4) {
    for (let i = 0; i < pixels; i++) {
      const base = i * 4;
      array.setLinear(base + 0, Number(data[base + 0]) / 255);
      array.setLinear(base + 1, Number(data[base + 1]) / 255);
      array.setLinear(base + 2, Number(data[base + 2]) / 255);
      array.setLinear(base + 3, Number(data[base + 3]) / 255);
    }
  } else {
    for (let i = 0; i < pixels; i++) {
      const base = i * 4;
      array.setLinear(i, packBgra32(data[base + 0], data[base + 1], data[base + 2], data[base + 3]));
    }
  }
  return array;
}

function packBgra32(r, g, b, a) {
  return (((Number(a) & 255) << 24) >>> 0) + ((Number(r) & 255) << 16) + ((Number(g) & 255) << 8) + (Number(b) & 255);
}

function scaleByte(value, max) {
  if (max === 255) return value;
  return Math.max(0, Math.min(255, Math.round((Number(value) * 255) / max)));
}
