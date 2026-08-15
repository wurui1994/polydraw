// UI playground entry — wires the editor, WebGL2 canvas and log panel together.
// This runs only in the browser (Vite/esbuild bundle). It uses the real
// WebGL2Renderer against the canvas's WebGL2 context: no fakes, no mocks.
import { makeEngine, PdEngine } from '../gpu/mainloop.ts';
import { WebGL2Renderer } from '../gpu/renderer.ts';
import type { RendererBackend } from '../gpu/mainloop.ts';
import { EXAMPLES } from './examples.ts';

const srcEl = document.getElementById('src') as HTMLTextAreaElement;
const canvas = document.getElementById('gl') as HTMLCanvasElement;
const logEl = document.getElementById('log') as HTMLPreElement;
const runBtn = document.getElementById('run') as HTMLButtonElement;
const stopBtn = document.getElementById('stop') as HTMLButtonElement;
const loadBtn = document.getElementById('load') as HTMLButtonElement;
const resSel = document.getElementById('res') as HTMLSelectElement;
const exSel = document.getElementById('examples') as HTMLSelectElement;

let engine: PdEngine | null = null;
let renderer: WebGL2Renderer | null = null;
let stopFn: (() => void) | null = null;

// ---- log panel (redirect console) ----
function log(msg: string, cls = '') {
  const line = document.createElement('div');
  if (cls) line.className = cls;
  line.textContent = msg;
  logEl.appendChild(line);
  logEl.scrollTop = logEl.scrollHeight;
}
const origLog = console.log.bind(console);
console.log = (...a: unknown[]) => { origLog(...a); log(a.map(String).join(' ')); };
console.error = (...a: unknown[]) => { origLog(...a); log(a.map(String).join(' '), 'err'); };

function parseRes(): [number, number] {
  const [w, h] = resSel.value.split('x').map(Number);
  return [w || 640, h || 480];
}

function ensureRenderer(): WebGL2Renderer | null {
  if (renderer) return renderer;
  const gl = canvas.getContext('webgl2', { preserveDrawingBuffer: true, antialias: true }) as WebGL2RenderingContext | null;
  if (!gl) { log('WebGL2 not available in this browser', 'err'); return null; }
  renderer = new WebGL2Renderer(gl as any);
  return renderer;
}

function buildEngine(): boolean {
  const [w, h] = parseRes();
  canvas.width = w; canvas.height = h;
  const r = ensureRenderer();
  if (!r) return false;
  r.setSize(w, h);
  const src = srcEl.value;
  log('compiling ' + src.length + ' chars…');
  let eng: PdEngine | null = null;
  try {
    eng = makeEngine(src, w, h);
  } catch (e) {
    log('compile error: ' + (e as Error).message, 'err');
    return false;
  }
  if (!eng) {
    log('compile failed (see above)', 'err');
    return false;
  }
  eng.attach(r as unknown as RendererBackend);
  engine = eng;
  log('compiled ok; starting render loop', 'ok');
  return true;
}

runBtn.addEventListener('click', () => {
  if (!buildEngine() || !engine) return;
  stopFn = engine.start((frame) => {
    if (frame % 30 === 0) log('frame ' + frame);
  });
  runBtn.disabled = true; stopBtn.disabled = false;
});

stopBtn.addEventListener('click', () => {
  if (stopFn) { stopFn(); stopFn = null; }
  runBtn.disabled = false; stopBtn.disabled = true;
});

loadBtn.addEventListener('click', () => {
  const name = exSel.value;
  const ex = EXAMPLES[name];
  if (ex) { srcEl.value = ex; log('loaded ' + name); }
});

// populate examples dropdown
for (const name of Object.keys(EXAMPLES)) {
  const o = document.createElement('option');
  o.value = name; o.textContent = name;
  exSel.appendChild(o);
}
// default source
srcEl.value = EXAMPLES['03_point.pss'] ?? '';
