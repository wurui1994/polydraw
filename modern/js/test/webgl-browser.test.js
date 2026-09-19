import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { createServer } from "node:http";
import { test } from "node:test";
import path from "node:path";
import { chromium } from "@playwright/test";

const root = path.resolve(import.meta.dirname, "..");
const fixtures = [
  {
    name: "default immediate",
    path: path.resolve(root, "../shared/fixtures/webgl_immediate.pss"),
    expectedPixel: [51, 204, 89, 255],
  },
  {
    name: "shader uniform color",
    path: path.resolve(root, "../shared/fixtures/uniform_color.pss"),
    expectedPixel: [32, 128, 223, 255],
  },
];

test("WebGL runtime renders PolyDraw batches in Chrome and reports FPS", { timeout: 15000 }, async (t) => {
  t.diagnostic("start server");
  const server = await startStaticServer(root);
  let browser = null;
  const launch = await launchChromePreferred();
  t.diagnostic(`browser=${launch.browserName}`);
  browser = launch.browser;
  let result;
  try {
    t.diagnostic("new page");
    const page = await browser.newPage({ viewport: { width: 640, height: 480 } });
    t.diagnostic("goto");
    await page.goto(`${server.url}/test/webgl-smoke.html`, { waitUntil: "domcontentloaded", timeout: 5000 });
    t.diagnostic("wait module");
    await page.waitForFunction(() => typeof window.runPolyDrawInCanvas === "function", null, { timeout: 5000 });
    t.diagnostic("evaluate render");
    result = [];
    for (const fixture of fixtures) {
      const source = await readFile(fixture.path, "utf8");
      result.push(await page.evaluate(async ({ source, name }) => {
        const canvas = document.querySelector("canvas");
        const api = window.runPolyDrawInCanvas(source, canvas, { autoStart: false });
        const frames = 60;
        const start = performance.now();
        for (let i = 0; i < frames; i++) api.step();
        const gl = api.runtime.gl;
        gl.finish();
        const elapsedMs = performance.now() - start;
        const pixels = new Uint8Array(4);
        gl.readPixels(gl.drawingBufferWidth >> 1, gl.drawingBufferHeight >> 1, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
        return {
          name,
          frames,
          fps: frames / (elapsedMs / 1000),
          batchCount: api.runtime.batches.length,
          drawnBatchCount: api.runtime.drawnBatchCount,
          centerPixel: Array.from(pixels),
        };
      }, { source, name: fixture.name }));
    }
  } finally {
    if (browser) await browser.close();
    await server.close();
  }

  for (let i = 0; i < result.length; i++) {
    const item = result[i];
    const fixture = fixtures[i];
    t.diagnostic(`${item.name}: webgl_fps=${item.fps.toFixed(2)} batches=${item.batchCount} pixel=${item.centerPixel.join(",")}`);
    assert.equal(item.frames, 60);
    assert.ok(item.fps > 1, `expected measurable FPS, got ${item.fps}`);
    assert.ok(item.batchCount > 0, "expected at least one batch");
    assert.equal(item.drawnBatchCount, item.batchCount);
    assertPixelClose(item.centerPixel, fixture.expectedPixel, item.name);
  }
});

async function launchChromePreferred() {
  const options = {
    args: [
      "--use-gl=swiftshader",
      "--enable-unsafe-swiftshader",
      "--disable-gpu-watchdog",
    ],
  };
  try {
    return { browserName: "chrome", browser: await chromium.launch({ channel: "chrome", ...options }) };
  } catch (chromeError) {
    try {
      return { browserName: "chromium", browser: await chromium.launch(options) };
    } catch {
      throw chromeError;
    }
  }
}

function assertPixelClose(actual, expected, label) {
  for (let i = 0; i < expected.length; i++) {
    assert.ok(Math.abs(actual[i] - expected[i]) <= 8, `${label} channel ${i}: expected near ${expected[i]}, got ${actual[i]}`);
  }
}

function startStaticServer(rootDir) {
  const server = createServer(async (req, res) => {
    try {
      const url = new URL(req.url ?? "/", "http://127.0.0.1");
      const relative = decodeURIComponent(url.pathname.replace(/^\/+/, "")) || "test/webgl-smoke.html";
      const filePath = path.resolve(rootDir, relative);
      if (!filePath.startsWith(rootDir + path.sep)) {
        res.writeHead(403).end("forbidden");
        return;
      }
      const ext = path.extname(filePath);
      const contentType = ext === ".js" ? "text/javascript" : ext === ".html" ? "text/html" : "application/octet-stream";
      const body = await readFile(filePath);
      res.writeHead(200, { "content-type": contentType });
      res.end(body);
    } catch (err) {
      res.writeHead(404).end(String(err?.message ?? err));
    }
  });
  return new Promise((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", () => {
      const address = server.address();
      resolve({
        url: `http://127.0.0.1:${address.port}`,
        close: () => new Promise((done) => server.close(done)),
      });
    });
  });
}
