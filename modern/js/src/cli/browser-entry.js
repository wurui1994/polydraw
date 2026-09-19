import { splitSections, getHostSection } from "../parser/sections.js";
import { compileHost } from "../runtime/interpreter.js";
import { WebGLRuntime } from "../render/webgl-runtime.js";

export function runPolyDrawInCanvas(source, canvas, options = {}) {
  const sections = splitSections(source);
  const host = getHostSection(sections);
  if (!host) throw new Error("no host section");
  const program = compileHost(host.text);
  const runtime = new WebGLRuntime(canvas, { xres: canvas.width, yres: canvas.height, sections });
  let frameCount = 0;
  let running = true;

  function step() {
    runtime.beginFrame();
    program.run(runtime);
    frameCount++;
    return { frameCount, batchCount: runtime.batches.length, drawnBatchCount: runtime.drawnBatchCount };
  }

  function frame() {
    if (!running) return;
    step();
    requestAnimationFrame(frame);
  }

  if (options.autoStart !== false) requestAnimationFrame(frame);
  return {
    sections,
    program,
    runtime,
    step,
    stop() {
      running = false;
    },
    stats() {
      return { frameCount, batchCount: runtime.batches.length, drawnBatchCount: runtime.drawnBatchCount };
    },
  };
}
