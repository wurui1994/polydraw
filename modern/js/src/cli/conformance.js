#!/usr/bin/env node
import fs from "node:fs";
import path from "node:path";
import { splitSections, getHostSection } from "../parser/sections.js";
import { compileHost } from "../runtime/interpreter.js";
import { TraceRuntime } from "../runtime/runtime.js";

const here = path.dirname(new URL(import.meta.url).pathname);
const manifestPath = path.resolve(here, "../../../shared/conformance/manifest.json");
const manifest = JSON.parse(fs.readFileSync(manifestPath, "utf8"));
const manifestDir = path.dirname(manifestPath);

let failures = 0;

for (const item of manifest.host) {
  const file = path.resolve(manifestDir, item.path);
  const source = fs.readFileSync(file, "utf8");
  const host = getHostSection(splitSections(source));
  const runtime = new TraceRuntime();
  const program = compileHost(host.text);
  const result = program.run(runtime);
  const okReturn = Math.abs(result.value - item.expectedReturn) < 1e-9;
  const okPrint = runtime.output === item.expectedPrint;
  const ok = okReturn && okPrint;
  console.log(JSON.stringify({
    path: item.path,
    ok,
    return: result.value,
    expectedReturn: item.expectedReturn,
    print: runtime.output,
    expectedPrint: item.expectedPrint,
  }));
  if (!ok) failures++;
}

process.exit(failures ? 1 : 0);
