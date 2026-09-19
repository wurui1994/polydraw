#!/usr/bin/env node
import fs from "node:fs";
import path from "node:path";
import { splitSections, getHostSection } from "../parser/sections.js";
import { parseProgram } from "../parser/parser.js";
import { compileHost, runHost } from "../runtime/interpreter.js";
import { TraceRuntime } from "../runtime/runtime.js";
import { TraceLimitReached } from "../runtime/runtime.js";
import { makeTextureImageLoader } from "./image-loader.js";

const [, , command, file, ...rest] = process.argv;

if (!command || !file) {
  console.error("usage: polydraw-js <parse|run|sections> <file.pss>");
  process.exit(2);
}

const source = fs.readFileSync(path.resolve(file), "utf8");
const sourcePath = path.resolve(file);
const sourceDir = path.dirname(sourcePath);
const sections = splitSections(source);
const loadTextureImage = makeTextureImageLoader(resolveTexturePath);

function runtimeOptions() {
  const xresArg = rest.find((arg) => arg.startsWith("--xres="));
  const yresArg = rest.find((arg) => arg.startsWith("--yres="));
  return {
    loadTextureImage,
    xres: xresArg ? Number(xresArg.slice("--xres=".length)) : 640,
    yres: yresArg ? Number(yresArg.slice("--yres=".length)) : 480,
  };
}

if (command === "sections") {
  console.log(JSON.stringify(sections.map(({ type, name, line, start, end, geometry }) => ({ type, name, line, start, end, geometry })), null, 2));
} else if (command === "parse") {
  const host = getHostSection(sections);
  if (!host) throw new Error("no host section");
  console.log(JSON.stringify(parseProgram(host.text), null, 2));
} else if (command === "run") {
  const host = getHostSection(sections);
  if (!host) throw new Error("no host section");
  const runtime = new TraceRuntime(runtimeOptions());
  const framesArg = rest.find((arg) => arg.startsWith("--frames="));
  const stepArg = rest.find((arg) => arg.startsWith("--step-limit="));
  const frames = framesArg ? Number(framesArg.slice("--frames=".length)) : 1;
  const stepLimit = stepArg ? Number(stepArg.slice("--step-limit=".length)) : undefined;
  const program = compileHost(host.text);
  let result = null;
  for (let i = 0; i < frames; i++) result = program.run(runtime, { stepLimit });
  process.stdout.write(runtime.output);
  console.error(JSON.stringify({ return: result.value, calls: runtime.calls.length, frames }));
} else if (command === "trace") {
  const host = getHostSection(sections);
  if (!host) throw new Error("no host section");
  const runtime = new TraceRuntime(runtimeOptions());
  runtime.recordCalls = false;
  const stepArg = rest.find((arg) => arg.startsWith("--step-limit="));
  const stepLimit = stepArg ? Number(stepArg.slice("--step-limit=".length)) : undefined;
  const program = compileHost(host.text);
  const result = program.run(runtime, { stepLimit });
  console.log(JSON.stringify({
    return: result.value,
    output: runtime.output,
    calls: runtime.calls,
    batches: runtime.batches,
  }));
} else if (command === "trace-batches") {
  const host = getHostSection(sections);
  if (!host) throw new Error("no host section");
  const runtime = new TraceRuntime(runtimeOptions());
  const stepArg = rest.find((arg) => arg.startsWith("--step-limit="));
  const batchLimitArg = rest.find((arg) => arg.startsWith("--batch-limit="));
  const textureDirArg = rest.find((arg) => arg.startsWith("--texture-dir="));
  const framesArg = rest.find((arg) => arg.startsWith("--frames="));
  const textureDir = textureDirArg ? path.resolve(textureDirArg.slice("--texture-dir=".length)) : null;
  if (textureDir) fs.mkdirSync(textureDir, { recursive: true });
  const stepLimit = stepArg ? Number(stepArg.slice("--step-limit=".length)) : 50_000_000;
  const frames = Math.max(1, Math.trunc(framesArg ? Number(framesArg.slice("--frames=".length)) : 1));
  const batchLimit = batchLimitArg ? Number(batchLimitArg.slice("--batch-limit=".length)) : Infinity;
  const program = compileHost(host.text);
  try {
    for (let frame = 0; frame < frames; frame++) {
      runtime.beginFrame();
      runtime.maxBatches = frame + 1 === frames ? batchLimit : Infinity;
      program.run(runtime, { stepLimit });
    }
  } catch (err) {
    if (!(err instanceof TraceLimitReached)) throw err;
  }
  writeBatchTrace(runtime, textureDir);
} else if (command === "trace-batches-stream") {
  const host = getHostSection(sections);
  if (!host) throw new Error("no host section");
  const runtime = new TraceRuntime(runtimeOptions());
  runtime.recordCalls = false;
  const stepArg = rest.find((arg) => arg.startsWith("--step-limit="));
  const batchLimitArg = rest.find((arg) => arg.startsWith("--batch-limit="));
  const textureDirArg = rest.find((arg) => arg.startsWith("--texture-dir="));
  const framesArg = rest.find((arg) => arg.startsWith("--frames="));
  const textureDir = textureDirArg ? path.resolve(textureDirArg.slice("--texture-dir=".length)) : null;
  if (textureDir) fs.mkdirSync(textureDir, { recursive: true });
  const stepLimit = stepArg ? Number(stepArg.slice("--step-limit=".length)) : 50_000_000;
  const frames = framesArg ? Math.max(1, Math.trunc(Number(framesArg.slice("--frames=".length)))) : Infinity;
  const batchLimit = batchLimitArg ? Number(batchLimitArg.slice("--batch-limit=".length)) : Infinity;
  const program = compileHost(host.text);
  for (let frame = 0; frame < frames; frame++) {
    runtime.beginFrame();
    runtime.maxBatches = batchLimit;
    try {
      program.run(runtime, { stepLimit });
    } catch (err) {
      if (!(err instanceof TraceLimitReached)) throw err;
    }
    console.log(`frame ${frame}`);
    writeBatchTrace(runtime, textureDir);
    console.log("endframe");
  }
} else {
  console.error(`unknown command: ${command}`);
  process.exit(2);
}

function encodeTextureRgba(values, width, height, depth, format) {
  const pixels = Buffer.alloc(Math.max(1, width) * Math.max(1, height) * Math.max(1, depth) * 4);
  const pixelCount = width * height * Math.max(1, depth);
  const baseFormat = Math.trunc(Number(format) || 0) & 0x0f;
  if (baseFormat === 4) {
    for (let i = 0; i < pixelCount; i++) {
      const v = Math.max(0, Math.min(1, Number(values[i] ?? 0)));
      const b = Math.round(v * 255);
      pixels[i * 4 + 0] = b;
      pixels[i * 4 + 1] = b;
      pixels[i * 4 + 2] = b;
      pixels[i * 4 + 3] = 255;
    }
  } else if (baseFormat === 5) {
    for (let i = 0; i < pixelCount; i++) {
      const base = i * 4;
      pixels[base + 0] = Math.round(Math.max(0, Math.min(1, Number(values[base + 0] ?? 0))) * 255);
      pixels[base + 1] = Math.round(Math.max(0, Math.min(1, Number(values[base + 1] ?? 0))) * 255);
      pixels[base + 2] = Math.round(Math.max(0, Math.min(1, Number(values[base + 2] ?? 0))) * 255);
      pixels[base + 3] = Math.round(Math.max(0, Math.min(1, Number(values[base + 3] ?? 1))) * 255);
    }
  } else {
    for (let i = 0; i < pixelCount; i++) {
      const packed = Number(values[i] ?? 0) >>> 0;
      pixels[i * 4 + 0] = (packed >> 16) & 255;
      pixels[i * 4 + 1] = (packed >> 8) & 255;
      pixels[i * 4 + 2] = packed & 255;
      pixels[i * 4 + 3] = (packed >> 24) & 255;
    }
  }
  return pixels;
}

function writeBatchTrace(runtime, textureDir) {
  const referencedTextures = new Set();
  for (const batch of runtime.batches) {
    for (const texture of batch.textures ?? []) referencedTextures.add(Number(texture.textureId ?? 0));
  }
  for (const textureId of Array.from(referencedTextures).sort((a, b) => a - b)) {
    const snapshot = runtime.textureSnapshots.get(textureId);
    const meta = runtime.textureMeta.get(textureId);
    if (!snapshot) {
      if (meta?.source === "file" && meta.path) {
        const filePath = resolveTexturePath(meta.path);
        const width = Math.trunc(meta.width || 0);
        const height = Math.trunc(meta.height || 0);
        const depth = Math.trunc(meta.depth || 1);
        const format = Math.trunc(meta.format || 0);
        console.log(`texfile ${textureId} ${width} ${height} ${depth} ${format} ${filePath}`);
      }
      continue;
    }
    const width = Math.trunc(snapshot.width || 1);
    const height = Math.trunc(snapshot.height || 1);
    const format = Math.trunc(snapshot.format || 0);
    if (snapshot.source === "capture") {
      const start = Math.trunc(snapshot.startBatch ?? 0);
      const end = Math.trunc(snapshot.endBatch ?? runtime.batches.length);
      const depth = Math.trunc(snapshot.depth || 1);
      console.log(`texcapture ${textureId} ${width} ${height} ${depth} ${format} ${start} ${end}`);
      continue;
    }
    const depth = Math.trunc(snapshot.depth || 1);
    if (textureDir && snapshot.array) {
      const filePath = path.join(textureDir, `tex_${textureId}.rgba`);
      fs.writeFileSync(filePath, encodeTextureRgba(snapshot.array.snapshotAll().values, width, height, depth, format));
      console.log(`texfile ${textureId} ${width} ${height} ${depth} ${format} ${filePath}`);
    } else if (!snapshot.truncated) {
      const values = snapshot.values.map((v) => Number(v).toPrecision(9));
      console.log(`texdef ${textureId} ${width} ${height} ${depth} ${format} ${values.length} ${values.join(" ")}`);
    }
  }
  for (const batch of runtime.batches) {
    const shader = batch.shader ?? { vertex: "-", fragment: "-" };
    const uniforms = batch.uniforms ?? [];
    const textures = batch.textures ?? [];
    const state = batch.state ?? {};
    console.log(`batch ${batch.mode} ${batch.vertices.length} ${sanitize(shader.vertex)} ${sanitize(shader.geometry)} ${sanitize(shader.fragment)} ${uniforms.length} ${textures.length} ${state.blend ? 1 : 0} ${state.depthTest === false ? 0 : 1}`);
    for (const uniform of uniforms) {
      const values = (uniform.values ?? []).map((v) => Number(v).toPrecision(9));
      console.log(`uniform ${sanitize(uniform.name)} ${uniformTypeCode(uniform.type)} ${uniform.count ?? 1} ${values.length} ${values.join(" ")}`);
    }
    for (const texture of textures) {
      console.log(`texture ${Number(texture.unit ?? 0)} ${Number(texture.textureId ?? 0)}`);
    }
    for (const vertex of batch.vertices) {
      const values = [
        ...vertex.position,
        ...vertex.color,
        ...vertex.texcoord,
        ...(vertex.normal ?? [0, 0, 1]),
      ].map((v) => Number(v).toPrecision(9));
      console.log(values.join(" "));
    }
  }
}

function resolveTexturePath(texturePath) {
  if (path.isAbsolute(texturePath)) return texturePath;
  const candidates = [
    path.resolve(sourceDir, texturePath),
    path.resolve(process.cwd(), texturePath),
  ];
  let dir = sourceDir;
  for (;;) {
    candidates.push(path.resolve(dir, texturePath));
    const parent = path.dirname(dir);
    if (parent === dir) break;
    dir = parent;
  }
  return candidates.find((candidate) => fs.existsSync(candidate)) ?? candidates[0];
}

function sanitize(value) {
  const text = String(value ?? "-");
  return text.replace(/\s+/g, "_") || "-";
}

function uniformTypeCode(type) {
  switch (String(type)) {
    case "1f": return 1;
    case "2f": return 2;
    case "3f": return 3;
    case "4f": return 4;
    case "1i": return 11;
    case "2i": return 12;
    case "3i": return 13;
    case "4i": return 14;
    case "1fv": return 21;
    case "2fv": return 22;
    case "3fv": return 23;
    case "4fv": return 24;
    case "1iv": return 31;
    case "2iv": return 32;
    case "3iv": return 33;
    case "4iv": return 34;
    case "mat3": return 43;
    case "mat4": return 44;
    default: return 0;
  }
}
