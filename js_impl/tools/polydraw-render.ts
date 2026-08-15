// polydraw-render.ts — JS equivalent of c_impl/src/render_main.c ("polydraw-render").
//
// Renders a .pss script offscreen with the software rasterizer and writes a
// PNG, so every script can be compared pixel-wise against the C baseline:
//
//   node --experimental-strip-types tools/polydraw-render.ts file.pss [opts]
//
// Options (defaults match the C reference):
//   --frame N     render up to frame N (default 30)
//   --w W --h H   output size (default 640x480)
//   --fovy DEG    default projection fovy (default 73.74 == setfov(90))
//   -o out.png    output path (default: "<script>_f<frame>.png")
//   --batch DIR   render every *.pss under DIR (recursively)
//   --ref DIR     with --batch: compare against C baseline PNGs in DIR
//   --outdir DIR  with --batch: write JS PNGs here instead of next to scripts
//                 (name: "<script-basename>_f<frame>.png")
//
// Like C, the loop runs frames 0..N and replays the recorded GLCmd stream
// through the renderer every frame (GL textures persist across frames in the
// C renderer via tex_obj[]; SoftRenderer's tex map is a per-instance field,
// so uploading on early frames stays live for later ones).
import { readFileSync, writeFileSync, existsSync, readdirSync, unlinkSync } from 'node:fs';
import { execFileSync, spawnSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { join, dirname, basename } from 'node:path';
import { makeEngine } from '../src/gpu/mainloop.ts';
import { SoftRenderer, glslStats } from '../src/gpu/softrender.ts';
import { writePNG, decodePNG } from '../src/gpu/png.ts';

function usage(): void {
  console.error(`polydraw-render — offscreen .pss renderer (JS, software rasterizer)
Usage:
  polydraw-render file.pss [--frame N] [--w W] [--h H]
                   [--fovy DEG] [-o out.png]
  polydraw-render --batch DIR [--frame N] [--w W] [--h H] [--fovy DEG]
                   [--ref DIR]
Defaults: frame 30, 640x480, fovy 73.74 (setfov(90) effective).`);
}

interface Args {
  script: string | null;
  single: string | null;
  frame: number;
  w: number;
  h: number;
  fovy: number;
  out: string | null;
  batch: string | null;
  ref: string | null;
  outdir: string | null;
}

function parseArgs(argv: string[]): Args {
  const a: Args = { script: null, single: null, frame: 30, w: 640, h: 480, fovy: 73.74, out: null, batch: null, ref: null, outdir: null };
  for (let i = 0; i < argv.length; i++) {
    const s = argv[i];
    if (s === '--frame' && i + 1 < argv.length) a.frame = parseInt(argv[++i], 10);
    else if (s === '--w' && i + 1 < argv.length) a.w = parseInt(argv[++i], 10);
    else if (s === '--h' && i + 1 < argv.length) a.h = parseInt(argv[++i], 10);
    else if (s === '--fovy' && i + 1 < argv.length) a.fovy = parseFloat(argv[++i]);
    else if (s === '-o' && i + 1 < argv.length) a.out = argv[++i];
    else if (s === '--single' && i + 1 < argv.length) a.single = argv[++i];
    else if (s === '--batch' && i + 1 < argv.length) a.batch = argv[++i];
    else if (s === '--ref' && i + 1 < argv.length) a.ref = argv[++i];
    else if (s === '--outdir' && i + 1 < argv.length) a.outdir = argv[++i];
    else if (s[0] !== '-') a.script = s;
  }
  return a;
}

// Decode an image file into the C reference's packed format: one 0xAABBGGRR
// double per texel (exactly pd_polyhost_tex.c decode_file()). Supports PNG via
// the built-in decoder; JPEG via `sips` (macOS; equivalent to stb_image).
function decodeImage(file: string): { w: number; h: number; rgb: number[] } | null {
  try {
    const lower = file.toLowerCase();
    if (!(lower.endsWith('.png') || lower.endsWith('.jpg') || lower.endsWith('.jpeg'))) return null;
    let buf: Buffer;
    if (lower.endsWith('.png')) {
      buf = readFileSync(file);
    } else {
      // JPEG -> PNG via sips into a temp file, then decode.
      const tmp = join(tmpdir(), `pd_${Date.now()}_${Math.random().toString(36).slice(2)}.png`);
      execFileSync('sips', ['-s', 'format', 'png', file, '--out', tmp], { stdio: 'ignore' });
      buf = readFileSync(tmp);
      try { unlinkSync(tmp); } catch { /* best effort */ }
    }
    const d = decodePNG(buf);
    const rgb: number[] = new Array(d.width * d.height);
    for (let i = 0; i < d.width * d.height; i++) {
      const r = d.rgb[i * 3], g = d.rgb[i * 3 + 1], b = d.rgb[i * 3 + 2];
      rgb[i] = (0xff << 24) | (b << 16) | (g << 8) | r; // 0xAABBGGRR
    }
    return { w: d.width, h: d.height, rgb };
  } catch {
    return null;
  }
}

// Texture file lookup mirroring C decode_file(): first relative to the script
// dir, then walking UP parent directories (kensky.jpg lives at project root
// while scripts live in ken/ or tigrou/).
function makeImageLoader(scriptPath: string): (file: string) => { w: number; h: number; rgb: number[] } | null {
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

// Render one pss to a PNG path. Returns a status string for the batch log.
function renderOne(script: string, a: Args, out: string): string {
  const src = readFileSync(script, 'utf8');
  const engine = makeEngine(src, a.w, a.h, {
    defaultFovy: a.fovy,
    imageLoader: makeImageLoader(script),
  });
  if (!engine) return 'COMPILE-ERR';
  engine.ctx.setClockScale(1.0 / 60.0); // deterministic klock() (C: pdrl_set_clock_scale 1/60)
  const sr = new SoftRenderer({ width: a.w, height: a.h, fragment: (v) => [v.r, v.g, v.b] });
  engine.attach(sr as never);
  // Advance the interpreter through frames 0..N, then rasterize ONLY the
  // final frame's recorded command stream. SoftRenderer's framebuffer does not
  // accumulate across frames (each render() redraws from its batches), so
  // rendering frames 0..N-1 would be pure waste — and for per-pixel shaders
  // that multiplies the cost by N for nothing. This is what made heavy scripts
  // appear to "time out": 31 fullscreen shader rasterizes.
  // Texture uploads persist across frames in the C renderer (tex_obj[]), but
  // each frame's GLCmd buffer is cleared, so glsettex() recorded on frame 0
  // (ken/texture.pss: `if (numframes == 0) glsettex(...)`) must be carried
  // forward manually: replay every frame (cheap — no rasterization) and merge
  // texData by texture id.
  const texAll = new Map<number, { id: number }>();
  let batches: ReturnType<typeof engine.ff.replay> = [];
  for (let f = 0; f <= a.frame; f++) {
    engine.ctx.runFrame(f);
    batches = engine.ff.replay(engine.ctx.glbuf);
    for (const t of engine.ff.texData) texAll.set(t.id, t as never);
  }
  sr.render({ batches, captures: engine.ff.captures, texData: [...texAll.values()] as never });
  if (process.env.PD_DEBUG_GLSL) {
    const st = glslStats();
    console.error(`[glsl] shader calls=${st.calls} discards=${st.discards}`);
  }
  const rgb = sr.toRGB8();
  writeFileSync(out, writePNG(rgb, a.w, a.h));
  return 'ok';
}

// Collect every .pss under dir (recursive, deterministic order).
function collectPss(dir: string): string[] {
  const out: string[] = [];
  const walk = (d: string): void => {
    let entries;
    try { entries = readdirSync(d, { withFileTypes: true }); } catch { return; }
    entries.sort((x, y) => (x.name < y.name ? -1 : 1));
    for (const e of entries) {
      const p = join(d, e.name);
      if (e.isDirectory()) walk(p);
      else if (e.name.endsWith('.pss')) out.push(p);
    }
  };
  walk(dir);
  return out;
}

// Compare two PNGs pixel-wise. Returns { diff, total, pct } where pct is the
// share of differing pixels (0 = identical, 100 = totally different). A pixel
// counts as different if any of its RGB channels differ.
function comparePNG(aPath: string, bPath: string): { diff: number; total: number; pct: number } | null {
  try {
    const da = decodePNG(readFileSync(aPath));
    const db = decodePNG(readFileSync(bPath));
    if (da.width !== db.width || da.height !== db.height) {
      return { diff: -1, total: -1, pct: 100 };
    }
    let diff = 0;
    const total = da.width * da.height;
    for (let i = 0; i < total; i++) {
      if (da.rgb[i * 3] !== db.rgb[i * 3] || da.rgb[i * 3 + 1] !== db.rgb[i * 3 + 1] || da.rgb[i * 3 + 2] !== db.rgb[i * 3 + 2]) diff++;
    }
    return { diff, total, pct: (diff / total) * 100 };
  } catch {
    return null;
  }
}

function main(): void {
  const a = parseArgs(process.argv.slice(2));
  if (!a.script && !a.batch && !a.single) { usage(); process.exit(1); }
  if (a.single) {
    // Subprocess mode used by --batch (per-script 5s timeout): render one file.
    const out = a.out ?? `${a.single}_f${a.frame}.png`;
    const status = renderOne(a.single, a, out);
    if (status !== 'ok') { console.error(`render failed: ${status}`); process.exit(1); }
    process.exit(0);
  }

  if (a.batch) {
    const files = collectPss(a.batch);
    if (files.length === 0) { console.error(`no .pss under ${a.batch}`); process.exit(1); }
    // C baseline naming: "<script>_f<frame>.png"
    let ok = 0, err = 0;
    const rows: { name: string; pct: number }[] = [];
    for (const f of files) {
      const out = join(a.outdir ?? dirname(f), `${basename(f)}_f${a.frame}.png`);
      // Render each script in a subprocess with a hard 5s timeout, so a single
      // pathological script (huge per-pixel shader, infinite loop) can't hang
      // the whole batch. The subprocess invokes this same tool in --single mode.
      const args = [
        '--experimental-strip-types', process.argv[1], '--single', f,
        '--frame', String(a.frame), '--w', String(a.w), '--h', String(a.h),
        '--fovy', String(a.fovy), '-o', out,
      ];
      const res = spawnSync(process.execPath, args, {
        timeout: 5000,
        encoding: 'utf8',
        stdio: ['ignore', 'pipe', 'pipe'],
      });
      let status: string;
      if (res.error) status = 'TIMEOUT';
      else if (res.status === 0) status = 'ok';
      else status = 'ERR:' + (res.stderr || res.stdout || '').trim().slice(0, 80);
      if (status !== 'ok') { console.error(`[${status}] ${f}`); err++; continue; }
      ok++;
      if (a.ref) {
        // Baseline lives in --ref DIR, keyed by the script's base name.
        const base = basename(f);
        const refPath = join(a.ref, base + '_f' + a.frame + '.png');
        if (existsSync(refPath)) {
          const c = comparePNG(out, refPath);
          if (c) rows.push({ name: base, pct: c.pct });
        } else {
          rows.push({ name: base, pct: -1 }); // no baseline
        }
      }
    }
    console.log(`rendered ${ok} scripts (${err} errors) under ${a.batch} -> *_f${a.frame}.png`);
    if (a.ref && rows.length) {
      console.log('diff vs C baseline:');
      let total = 0, have = 0;
      for (const r of rows) {
        if (r.pct < 0) { console.log(`  ${r.name}: NO-BASELINE`); continue; }
        total += r.pct; have++;
        const flag = r.pct === 0 ? '== ' : r.pct < 5 ? '~  ' : '!= ';
        console.log(`  ${flag} ${r.name}: ${r.pct.toFixed(2)}% diff`);
      }
      if (have) console.log(`avg diff: ${(total / have).toFixed(2)}%`);
    }
    return;
  }

  const script = a.script!;
  const out = a.out ?? `${script}_f${a.frame}.png`;
  const status = renderOne(script, a, out);
  if (status !== 'ok') { console.error(`render failed: ${status}`); process.exit(1); }
  console.log(`wrote ${out} (${a.w}x${a.h}, frame ${a.frame}, fovy ${a.fovy.toFixed(2)})`);
}

main();
