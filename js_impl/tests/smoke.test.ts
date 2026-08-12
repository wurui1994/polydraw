// smoke.test.ts — verify the JS recording host runs a sample .pss frame and
// produces a non-empty GLCmdBuf (M7 core: EVAL -> GLCmd stream).
import { test } from 'node:test';
import assert from 'node:assert';
import { readFileSync } from 'node:fs';
import { pdrlCompileFile } from '../src/host/runlib.ts';
import { GLCMD } from '../src/host/glcmd.ts';

const ROOT = '/Users/wurui/Documents/polydraw';

test('balls.pss records GL commands', () => {
  const src = readFileSync(`${ROOT}/ken/balls.pss`, 'utf8');
  const ctx = pdrlCompileFile(src, 640, 480);
  assert.ok(ctx, 'ctx created');
  assert.equal(ctx!.err, '', `compile err: ${ctx!.err}`);
  ctx!.setClockScale(1 / 60);
  ctx!.runFrame(30);
  const n = ctx!.glbuf.cmds.length;
  assert.ok(n > 100, `expected many GL commands, got ${n}`);
  // must contain a BEGIN/END pair and a SETSHADER
  const hasBegin = ctx!.glbuf.cmds.some((c) => c.op === GLCMD.BEGIN);
  const hasEnd = ctx!.glbuf.cmds.some((c) => c.op === GLCMD.END);
  assert.ok(hasBegin && hasEnd, 'missing BEGIN/END');
});

test('disco ball.pss records SETTEXDATA + matrices', () => {
  const src = readFileSync(`${ROOT}/tigrou/disco ball.pss`, 'utf8');
  const ctx = pdrlCompileFile(src, 640, 480);
  assert.equal(ctx!.err, '', `compile err: ${ctx!.err}`);
  ctx!.setClockScale(1 / 60);
  ctx!.runFrame(30);
  const cmds = ctx!.glbuf.cmds;
  assert.ok(cmds.length > 100, `expected many commands, got ${cmds.length}`);
  assert.ok(cmds.some((c) => c.op === GLCMD.SETTEXDATA), 'missing SETTEXDATA');
  assert.ok(cmds.some((c) => c.op === GLCMD.ROTATE), 'missing ROTATE');
});
