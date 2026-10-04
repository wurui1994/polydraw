// whyfail.ts — print the JS compile error for a .pss host block.
import { readFileSync } from 'node:fs';
import { sectionParse, sectionHost } from '../src/host/sections.ts';
import { PolyHostImpl } from '../src/host/polyhost.ts';
import { compile } from '../src/eval/parser.ts';

const f = process.argv[2];
const full = readFileSync(f, 'utf8');
const sl = sectionParse(full);
const hs = sectionHost(sl);
if (!hs) { console.log('no host block'); process.exit(1); }
const ph = new PolyHostImpl();
const host = ph.install();
const src = full.slice(hs.start, hs.end);
const r = compile(src, host);
if (r.ok) console.log('OK');
else {
  const m = /line (\d+)/.exec(r.err);
  if (m) {
    const ln = parseInt(m[1], 10);
    console.log(`ERR: ${r.err}`);
    console.log('--- context ---');
    for (let k = Math.max(1, ln - 3); k <= Math.min(src.split('\n').length, ln + 1); k++) {
      console.log(`${String(k).padStart(4)}| ${src.split('\n')[k - 1]}`);
    }
  } else console.log(`ERR: ${r.err}`);
}
process.exit(r.ok ? 0 : 1);
