// runlib.ts — high-level JS API: compile a .pss host block and run frames,
// recording the GL command stream. Mirrors c_impl/src/render/pd_runlib.c.
import { sectionParse, sectionHost, sectionBlocks, SEC_VERTEX, SEC_FRAGMENT } from './sections.ts';
import { PolyHostImpl } from './polyhost.ts';
import type { Block } from './polyhost.ts';
import { compile } from '../eval/parser.ts';
import { run } from '../backend/interp.ts';
import { GLCmdBuf } from './glcmd.ts';
import type { Program } from '../eval/ir.ts';

export class PdrlCtx {
  prog: Program | null = null;
  hostImpl: PolyHostImpl;
  glbuf: GLCmdBuf;
  private xres = 640;
  private yres = 480;

  constructor(hostSrc: string, xres = 640, yres = 480, blocks: Block[] = []) {
    this.xres = xres; this.yres = yres;
    this.hostImpl = new PolyHostImpl();
    this.hostImpl.state.xres = xres;
    this.hostImpl.state.yres = yres;
    this.hostImpl.blocks = blocks;
    this.glbuf = this.hostImpl.glbuf;
    const host = this.hostImpl.install();
    const res = compile(hostSrc, host);
    if (!res.ok) { this.prog = null; this.err = res.err; return; }
    this.prog = res.program;
    this.err = '';
  }
  err = '';

  setClockScale(scale: number) { this.hostImpl.state.clockScale = scale; }
  setResolution(x: number, y: number) { this.xres = x; this.yres = y; this.hostImpl.state.xres = x; this.hostImpl.state.yres = y; }

  runFrame(numframes: number): number {
    if (!this.prog) return 0;
    this.hostImpl.state.numframes = numframes;
    this.glbuf.reset();
    this.hostImpl.srand(1); // deterministic across runs (matches C test harness)
    return run(this.prog, null, this.prog.globals, null);
  }
}

export function pdrlCompileFile(src: string, xres = 640, yres = 480): PdrlCtx | null {
  const sl = sectionParse(src);
  const h = sectionHost(sl);
  if (!h) return null;
  const hostSrc = src.slice(h.start, h.end);
  const blocks: Block[] = sectionBlocks(src, sl).map((b) => ({
    src: b.src, name: b.name, type: b.type,
  }));
  return new PdrlCtx(hostSrc, xres, yres, blocks);
}
