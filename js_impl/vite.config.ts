import { defineConfig } from 'vite';

// The Playground UI lives in src/ui and is a standalone browser bundle. The
// rest of the engine (src/gpu, src/host, src/eval, src/backend) is plain TS,
// type-checked with `tsc` and run under Node's strip-types test runner.
export default defineConfig({
  root: 'src/ui',
  base: './',
  build: {
    outDir: '../dist-ui',
    emptyOutDir: true,
    target: 'es2022',
  },
  server: {
    open: true,
    port: 5173,
  },
});
