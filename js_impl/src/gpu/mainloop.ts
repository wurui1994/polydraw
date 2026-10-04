// mainloop.ts — the animation / offline-render loop (M7).
//
// This is the layer that actually turns a compiled pss program into images,
// in both environments:
//   * browser:  a live requestAnimationFrame loop drawing into a real WebGL2
//               canvas (the UI playground uses this).
//   * node:     an offline loop that runs N frames and writes PNGs, used for
//               golden generation and cross-backend verification.
//
// Each frame advances the program's frame counter, replays the recorded GLCmd
// stream through FixedFunc into DrawBatches, and hands the scene to whichever
// renderer is attached (WebGL2Renderer for the browser, SoftRenderer for node).
import { PdrlCtx } from '../host/runlib.ts';
import { sectionParse, sectionHost, sectionBlocks, SEC_VERTEX, SEC_FRAGMENT } from '../host/sections.ts';
import { FixedFunc } from './fixedfunc.ts';
import type { DrawBatch } from './fixedfunc.ts';
import type { RenderScene } from './renderer.ts';

export interface SceneFrame {
  frame: number;
  batches: DrawBatch[];
  captures: { afterIndex: number; tex: number }[];
  texData: { id: number; w: number; h: number; z: number; colmode: number; pixels: number[] | null }[];
}

// A renderer backend consumes a SceneFrame and draws it. Both WebGL2Renderer
// and SoftRenderer satisfy this; the mainloop is backend-agnostic.
export interface RendererBackend {
  setSize(w: number, h: number): void;
  render(scene: RenderScene): void;
  // optional readback for offline PNG export (browser canvas returns null; the
  // caller reads pixels from the DOM canvas instead).
  readRGBA8?(): Uint8Array;
}

export interface EngineOpts {
  defaultFovy?: number;
  imageLoader?: (file: string) => { w: number; h: number; rgb: number[] } | null;
  texSearchDir?: string;
}

export class PdEngine {
  ctx: PdrlCtx;
  ff: FixedFunc;
  width: number;
  height: number;
  lastRendered = -1;
  private renderer: RendererBackend | null = null;

  constructor(ctx: PdrlCtx, width = 640, height = 480, opts: EngineOpts = {}) {
    this.ctx = ctx;
    this.width = width;
    this.height = height;
    this.ff = new FixedFunc(width, height, opts.defaultFovy ?? 0);
    if (opts.imageLoader) this.ctx.hostImpl.imageLoader = opts.imageLoader;
    if (opts.texSearchDir) this.ctx.hostImpl.texSearchDir = opts.texSearchDir;
    // Default shader = the script's first @v / @f block (mirrors C
    // render_main.c: pd_gl_renderer_set_shaders(rd, first_vertex, first_frag)).
    // glsetshader() commands later override these per-batch.
    const blocks = this.ctx.hostImpl.blocks;
    const v0 = blocks.find((b) => b.type === SEC_VERTEX);
    const f0 = blocks.find((b) => b.type === SEC_FRAGMENT);
    if (f0) this.ff.defaultShaderF = f0.src;
    if (v0) this.ff.defaultShaderV = v0.src;
    // Seed the CURRENT program from the default @v/@f so early batches render
    // with the script's shader even before any glsetshader. Shader state is
    // persistent across frames (reset() preserves it), so this stays live.
    if (f0) this.ff.shaderF = f0.src;
    if (v0) this.ff.shaderV = v0.src;
  }

  attach(renderer: RendererBackend): void {
    this.renderer = renderer;
    // WebGL2Renderer exposes setSize; SoftRenderer is sized at construction, so
    // tolerate its absence.
    if (typeof (renderer as { setSize?: (w: number, h: number) => void }).setSize === 'function') {
      renderer.setSize(this.width, this.height);
    }
  }

  // Mimic the C viewer's incremental-playback mainloop: globals/arrays persist
  // across run_frame calls (pd_run reuses prog.globals; only the draw buffer is
  // reset). To render frame N we replay 0..N the first time (so textures
  // generated under `numframes==0` exist), then only the new frames thereafter.
  private ensurePlayedTo(frame: number): void {
    if (frame <= this.lastRendered) {
      for (let f = 0; f <= frame; f++) this.ctx.runFrame(f);
    } else {
      for (let f = this.lastRendered + 1; f <= frame; f++) this.ctx.runFrame(f);
    }
    this.lastRendered = frame;
  }

  // Advance one frame and produce the scene to draw. Returns null on error.
  step(frame: number): SceneFrame | null {
    this.ensurePlayedTo(frame);
    const batches = this.ff.replay(this.ctx.glbuf);
    return {
      frame,
      batches,
      captures: this.ff.captures,
      texData: this.ff.texData,
    };
  }

  // Run one frame end-to-end (record + render). Returns the scene drawn.
  renderFrame(frame: number): SceneFrame | null {
    const sf = this.step(frame);
    if (!sf || !this.renderer) return sf;
    this.renderer.render({
      batches: sf.batches,
      captures: sf.captures,
      texData: sf.texData,
    });
    return sf;
  }

  // Offline render N frames. For each frame, calls onFrame with the frame
  // number and the renderer (so the caller can read back pixels / write PNG).
  runHeadless(frames: number, onFrame: (frame: number, engine: PdEngine) => void): void {
    for (let f = 0; f < frames; f++) {
      this.renderFrame(f);
      onFrame(f, this);
    }
  }

  // Live browser animation loop. Uses requestAnimationFrame when available;
  // stops when `shouldStop` returns true or `frames` is reached (0 = forever).
  start(onFrame?: (frame: number, engine: PdEngine) => void, frames = 0, shouldStop?: () => boolean): () => void {
    let f = 0;
    let raf: number | null = null;
    let timer: unknown = null;
    let stopped = false;
    const stop = () => {
      stopped = true;
      if (raf !== null && typeof cancelAnimationFrame === 'function') cancelAnimationFrame(raf);
      if (timer !== null) clearTimeout(timer as number);
    };
    const tick = () => {
      if (stopped) return;
      if (shouldStop && shouldStop()) { stop(); return; }
      if (frames > 0 && f >= frames) { stop(); return; }
      this.renderFrame(f);
      if (onFrame) onFrame(f, this);
      f++;
      if (typeof requestAnimationFrame === 'function') {
        raf = requestAnimationFrame(tick);
      } else {
        // node fallback: small delay so we don't busy-spin
        timer = setTimeout(tick, 16);
      }
    };
    tick();
    return stop;
  }
}

// Convenience: build an engine from a full pss source string (host + blocks).
export function makeEngine(pssSource: string, width = 640, height = 480, opts: EngineOpts = {}): PdEngine | null {
  const ctx = pdrlCompileSource(pssSource, width, height);
  if (!ctx) return null;
  return new PdEngine(ctx, width, height, opts);
}

// Compile a full pss file (with optional @v/@f blocks) into a PdrlCtx.
export function pdrlCompileSource(src: string, width = 640, height = 480): PdrlCtx | null {
  // section parsing helpers live in host/sections.ts (mirrors c_impl).
  const sl = sectionParse(src);
  const h = sectionHost(sl);
  if (!h) return null;
  const hostSrc = src.slice(h.start, h.end);
  const blocks: { src: string; name: string; type: number }[] = sectionBlocks(src, sl).map((b: any) => ({
    src: b.src, name: b.name, type: b.type,
  }));
  const ctx = new PdrlCtx(hostSrc, width, height, blocks);
  return ctx.prog ? ctx : null;
}
