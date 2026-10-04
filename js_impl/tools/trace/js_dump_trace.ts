// trace/js_dump_trace.ts — dump the full JS render-graph trace for one frame.
//
// Usage:
//   npx tsx tools/trace/js_dump_trace.ts <script.pss> --frame N --w W --h H --fovy F -o trace.json
//
// The output JSON contains:
//   - batches: per-batch vertex transform results (clip-space, NDC, varyings)
//     + framebuffer snapshot hash AFTER each batch
//   - textures: texture upload info (id, size, filter, wrap, pixel hash)
//
// This is the JS half of the C/JS render-graph differential. The C half
// (c_dump_trace) produces the same JSON format, then trace_compare.ts aligns
// them node-by-node.

import { readFileSync, existsSync, unlinkSync, writeFileSync, mkdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { execFileSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { decodePNG } from '../../src/gpu/png.ts';
import { makeEngine } from '../../src/gpu/mainloop.ts';
import { SoftRenderer } from '../../src/gpu/softrender.ts';
import type { Trace, TraceCmd, TraceBatch, TraceTex } from './types.ts';
import { GLCMD } from '../../src/host/glcmd.ts';

function decodeImage(file: string): { w: number; h: number; rgb: number[] } | null {
  try {
    const lower = file.toLowerCase();
    if (!lower.endsWith('.png') && !lower.endsWith('.jpg') && !lower.endsWith('.jpeg')) return null;
    let buf: Buffer;
    if (lower.endsWith('.png')) buf = readFileSync(file);
    else {
      const tmp = join(tmpdir(), `pd_${Date.now()}.png`);
      execFileSync('sips', ['-s', 'format', 'png', file, '--out', tmp], { stdio: 'ignore' });
      buf = readFileSync(tmp);
      try { unlinkSync(tmp); } catch {}
    }
    const d = decodePNG(buf);
    const rgb: number[] = new Array(d.width * d.height);
    for (let i = 0; i < d.width * d.height; i++) {
      const r = d.rgb[i * 3], g = d.rgb[i * 3 + 1], b = d.rgb[i * 3 + 2];
      rgb[i] = ((0xff << 24) | (r << 16) | (g << 8) | b) >>> 0; // 0xAARRGGBB
    }
    return { w: d.width, h: d.height, rgb };
  } catch { return null; }
}

function makeImageLoader(scriptPath: string) {
  const dirs: string[] = [];
  let dir = dirname(scriptPath);
  for (;;) {
    dirs.push(dir);
    const up = dirname(dir);
    if (up === dir) break;
    dir = up;
  }
  return (file: string) => {
    if (file.startsWith('/')) return decodeImage(file);
    for (const d of dirs) {
      const p = join(d, file);
      if (existsSync(p)) return decodeImage(p);
    }
    return null;
  };
}

function texHash(pixels: number[] | null, w: number, h: number): string {
  if (!pixels) return 'null';
  let hash = 0;
  for (let i = 0; i < pixels.length; i++) hash = (hash * 31 + (pixels[i] | 0)) | 0;
  return 'h' + (hash >>> 0).toString(16);
}

/** djb2 over a string's bytes — matches trace_str_hash() in gl_renderer.c */
function djb2(s: string | null | undefined): string {
  let h = 5381;
  if (s) for (let i = 0; i < s.length; i++) h = ((h << 5) + h + s.charCodeAt(i)) | 0;
  return 'h' + (h >>> 0).toString(16);
}

/** Serialize one GLCmd to the trace format. Mirrors trace_dump_cmds() in
 *  gl_renderer.c exactly: op/mode/a/b/c/d + op-specific payload fields
 *  (pxHash for SETTEXDATA, fv for UNIFORM, m16 for MULTMATRIX, s for
 *  UNIFORMLOC, vs/fs hash+length for SETSHADER). */
function serializeCmd(c: { op: number; mode: number; a: number; b: number; c: number; d: number; s: unknown; s2?: unknown }): TraceCmd {
  const out: TraceCmd = { op: c.op, mode: c.mode, a: c.a, b: c.b, c: c.c, d: c.d };
  switch (c.op) {
    case GLCMD.SETTEXDATA: {
      const px = c.s as number[] | null;
      const cnt = (c.b | 0) * (c.c | 0);
      let h = 5381;
      if (px) for (let k = 0; k < cnt; k++) h = ((h << 5) + h + (px[k] | 0)) | 0;
      out.pxHash = 'h' + (h >>> 0).toString(16);
      out.pxCount = px ? cnt : -1;
      break;
    }
    case GLCMD.UNIFORM: {
      const fv = c.s as number[] | null;
      const cnt = c.mode & 0xffff;
      if (fv && cnt > 0) out.fv = fv.slice(0, cnt);
      break;
    }
    case GLCMD.MULTMATRIX: {
      const m = c.s as number[] | null;
      if (m) out.m16 = m.slice(0, 16);
      break;
    }
    case GLCMD.UNIFORMLOC:
      out.s = (c.s as string | null) ?? null;
      break;
    case GLCMD.SETSHADER: {
      const vs = (c.s as string | null) ?? '';
      const fs = (c.s2 as string | null) ?? '';
      out.vsLen = vs.length; out.fsLen = fs.length;
      out.vsHash = djb2(vs); out.fsHash = djb2(fs);
      break;
    }
  }
  return out;
}

function main() {
  const args = process.argv.slice(2);
  let script = '', frame = 0, w = 640, h = 480, fovy = 73.74, outPath = '', snapDir = '';
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--frame') frame = parseInt(args[++i]);
    else if (args[i] === '--w') w = parseInt(args[++i]);
    else if (args[i] === '--h') h = parseInt(args[++i]);
    else if (args[i] === '--fovy') fovy = parseFloat(args[++i]);
    else if (args[i] === '-o') outPath = args[++i];
    else if (args[i] === '--dir') snapDir = args[++i];
    else if (!args[i].startsWith('-')) script = args[i];
  }
  if (!script || !outPath) {
    console.error('Usage: js_dump_trace.ts <script.pss> --frame N --w W --h H --fovy F --dir snapdir -o trace.json');
    process.exit(1);
  }
  if (snapDir) mkdirSync(snapDir, { recursive: true });

  const src = readFileSync(script, 'utf8');
  const engine = makeEngine(src, w, h, { defaultFovy: fovy, imageLoader: makeImageLoader(script) });
  engine.ctx.setClockScale(1.0 / 60.0);
  const sr = new SoftRenderer({ width: w, height: h, fragment: (v) => [v.r, v.g, v.b] });
  (engine as never as { attach: (s: unknown) => void }).attach(sr as never);

  const texAll = new Map<number, { id: number; w: number; h: number; z: number; colmode: number; pixels: number[] | null }>();
  let batches: ReturnType<typeof engine.ff.replay> = [];
  let cmds: TraceCmd[] = [];
  for (let f = 0; f <= frame; f++) {
    engine.ctx.runFrame(f);
    // snapshot the raw command stream of the LAST frame before replay
    // consumes/resets the buffer — this is the contract layer both C and JS
    // renderers start from, so any divergence here poisons everything below.
    if (f === frame) cmds = engine.ctx.glbuf.cmds.map(serializeCmd);
    batches = engine.ff.replay(engine.ctx.glbuf);
    for (const t of engine.ff.texData) texAll.set(t.id, t);
  }

  const batchInfos: import('../../src/gpu/softrender.ts').BatchTraceInfo[] = [];
  sr.render(
    { batches, captures: engine.ff.captures, texData: [...texAll.values()] },
    (info) => {
      batchInfos.push(info);
      // element-level snapshot: batch_NNN.ppm (P6, top-down) — same format
      // as C's trace_dump_batch, for per-drawcall pixel diffing
      if (snapDir && info.fbData) {
        const fw = info.fbW ?? w, fh = info.fbH ?? h;
        const d = info.fbData;
        const buf = Buffer.alloc(fw * fh * 3);
        for (let i = 0; i < fw * fh * 3; i++)
          buf[i] = Math.max(0, Math.min(255, Math.round(d[i] * 255)));
        writeFileSync(join(snapDir, `batch_${String(info.index).padStart(3, '0')}.ppm`),
          Buffer.concat([Buffer.from(`P6\n${fw} ${fh}\n255\n`), buf]));
      }
    },
  );

  const traceBatches: TraceBatch[] = batchInfos.map((bi) => ({
    index: bi.index,
    mode: bi.mode,            // keep the numeric GL mode for normalization
    nverts: bi.nverts,
    cmdStart: bi.cmdStart,
    cmdEnd: bi.cmdEnd,
    mvp: bi.mvp,
    shaderHash: (bi.shaderF ?? '').slice(0, 64),
    texUnits: [],
    depthTest: false,
    blend: false,
    cullFace: 0,
    captureTarget: -1,
    verts: bi.verts,
    fbHash: bi.fbHash,
    fbNonBlankPct: bi.fbNonBlankPct,
    fbMean: bi.fbMean,
  }));

  const textures: TraceTex[] = [...texAll.values()].map((t) => ({
    id: t.id,
    w: t.w,
    h: t.h,
    filter: 1,
    wrap: 0,
    hash: texHash(t.pixels, t.w, t.h),
  }));

  // Final framebuffer hash: djb2 over top-left row-major clamped RGB bytes,
  // matching the C trace's finalHash readback.
  const img = sr.getImage();
  let finalHash = 5381, finalSum = 0;
  for (let i = 0; i < img.length; i++) {
    const v = Math.max(0, Math.min(255, Math.round(img[i] * 255)));
    finalHash = ((finalHash << 5) + finalHash + v) | 0;
    finalSum += v;
  }

  const trace: Trace = {
    source: 'js',
    script,
    frame,
    width: w,
    height: h,
    fovy,
    cmds,
    batches: traceBatches,
    textures,
    finalHash: 'h' + (finalHash >>> 0).toString(16),
  };

  writeFileSync(outPath, JSON.stringify(trace, null, 2));
  console.error(`JS trace: ${cmds.length} cmds, ${traceBatches.length} batches, ${textures.length} textures → ${outPath}`);
}

main();
