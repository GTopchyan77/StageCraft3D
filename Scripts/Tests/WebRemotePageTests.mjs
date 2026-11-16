// Tests for the Web Remote phone page (Source/ModularSceneBuilder/Resources/WebRemote/index.html, Docs/ADR/0036).
// The page is one HTML file served by StageCraft, so its script cannot be imported: this test reads the file, checks that the whole
// script parses and that every element id it uses exists, and runs the pure layout block (layout:pure:begin ... layout:pure:end, WR4,
// ADR 0036 section 12) in a sandbox with no DOM and no storage.
// Run: node Scripts/Tests/WebRemotePageTests.mjs   (Node 18+, no packages). Exit code 0 = pass.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import vm from 'node:vm';
import assert from 'node:assert/strict';

const here = dirname(fileURLToPath(import.meta.url));
const pagePath = join(here, '..', '..', 'Source', 'ModularSceneBuilder', 'Resources', 'WebRemote', 'index.html');
const html = readFileSync(pagePath, 'utf8');

let passed = 0;
const failures = [];
function test(name, fn) {
  try { fn(); passed++; } catch (e) { failures.push(`${name}: ${e.message}`); }
}

// ---- the page as a whole ---------------------------------------------------------------------------------------------------------
const scripts = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((m) => m[1]);
test('one inline script (the CSP allows inline scripts only)', () => assert.equal(scripts.length, 1));
const script = scripts[0] || '';
test('the script parses', () => { new vm.Script(script, { filename: 'index.html' }); });
test('no external resources (CSP default-src none)', () => {
  assert.ok(!/<script[^>]+src=/i.test(html), 'external script');
  assert.ok(!/<link[^>]+href=/i.test(html), 'external stylesheet');
});
test('every $(id) the script uses exists in the page', () => {
  const used = new Set([...script.matchAll(/\$\('([A-Za-z][\w-]*)'\)/g)].map((m) => m[1]));
  const declared = new Set([...html.matchAll(/\sid="([^"]+)"/g)].map((m) => m[1]));
  const missing = [...used].filter((id) => !declared.has(id));
  assert.deepEqual(missing, []);
});
test('element ids are unique', () => {
  const ids = [...html.matchAll(/\sid="([^"]+)"/g)].map((m) => m[1]);
  const dupes = ids.filter((id, i) => ids.indexOf(id) !== i);
  assert.deepEqual(dupes, []);
});

// ---- the pure layout block -------------------------------------------------------------------------------------------------------
const block = /\/\* layout:pure:begin[\s\S]*?\*\/([\s\S]*?)\/\* layout:pure:end \*\//.exec(script);
test('the pure layout block is marked', () => assert.ok(block));
const forbidden = ['document', 'window', 'localStorage', '$(', 'navigator', 'fetch('];
test('the pure block uses no DOM or storage', () => {
  const body = block ? block[1] : '';
  for (const word of forbidden) assert.ok(!body.includes(word), `uses ${word}`);
});

const sandbox = {};
vm.createContext(sandbox);
vm.runInContext(`${block ? block[1] : ''}
  this.api = { LAYOUT_VERSION, LAYOUT_MAX_CHARS, LAYOUT_PLAYBACKS, allPlaybacks, defaultLayout, sanitizeLayout, parseLayout, serializeLayout,
    withPane, withPlayback, visiblePanes, visibleStrips, pickPane };`, sandbox);
const L = sandbox.api;
// Objects from the sandbox have its prototypes; compare as JSON.
const plain = (v) => JSON.parse(JSON.stringify(v));
const all = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];

test('default layout shows everything at normal size, unlocked', () => {
  assert.deepEqual(plain(L.defaultLayout()), { v: 1, panes: { cues: true, playbacks: true, masters: true }, playbacks: all, hideEmpty: false, size: 'normal', lock: false });
});

test('parse: nothing stored, broken or foreign text gives the default', () => {
  for (const text of [null, undefined, '', 'not json', '[]', '42', 'null', '{"v":2}', '{"v":"1"}', 'x'.repeat(L.LAYOUT_MAX_CHARS + 1)]) {
    assert.deepEqual(plain(L.parseLayout(text)), plain(L.defaultLayout()), String(text).slice(0, 20));
  }
});

test('a saved layout round-trips', () => {
  const saved = { v: 1, panes: { cues: true, playbacks: false, masters: true }, playbacks: [2, 4], hideEmpty: true, size: 'large', lock: true };
  assert.deepEqual(plain(L.parseLayout(L.serializeLayout(saved))), saved);
});

test('each bad field falls back on its own', () => {
  const l = plain(L.sanitizeLayout({ v: 1, panes: { cues: 'yes', playbacks: false, masters: 1 }, playbacks: [3, 3, 0, 11, 2.5, '4', -1, 1], hideEmpty: 'true', size: 'huge', lock: 1 }));
  assert.deepEqual(l, { v: 1, panes: { cues: true, playbacks: false, masters: true }, playbacks: [1, 3], hideEmpty: false, size: 'normal', lock: false });
});

test('never an empty page: no section or no playback falls back to all', () => {
  assert.deepEqual(plain(L.sanitizeLayout({ v: 1, panes: { cues: false, playbacks: false, masters: false } }).panes), { cues: true, playbacks: true, masters: true });
  assert.deepEqual(plain(L.sanitizeLayout({ v: 1, playbacks: [] }).playbacks), all);
  assert.deepEqual(plain(L.sanitizeLayout({ v: 1, playbacks: 'all' }).playbacks), all);
  assert.deepEqual(plain(L.sanitizeLayout({ v: 1, panes: null }).panes), { cues: true, playbacks: true, masters: true });
});

test('unknown keys are dropped (nothing else is stored)', () => {
  const l = plain(L.sanitizeLayout({ v: 1, token: 'secret', extra: { a: 1 } }));
  assert.deepEqual(Object.keys(l).sort(), ['hideEmpty', 'lock', 'panes', 'playbacks', 'size', 'v']);
});

test('withPane: switch sections, but not the last one off', () => {
  let l = L.withPane(L.defaultLayout(), 'masters', false);
  assert.deepEqual(plain(L.visiblePanes(l)), ['cues', 'playbacks']);
  l = L.withPane(l, 'cues', false);
  assert.deepEqual(plain(L.visiblePanes(l)), ['playbacks']);
  l = L.withPane(l, 'playbacks', false);
  assert.deepEqual(plain(L.visiblePanes(l)), ['playbacks'], 'the last section stays');
  l = L.withPane(l, 'masters', true);
  assert.deepEqual(plain(L.visiblePanes(l)), ['playbacks', 'masters']);
  assert.deepEqual(plain(L.withPane(l, 'settings', false)), plain(l), 'unknown section ignored');
});

test('withPlayback: choose playbacks, kept sorted, the last one stays', () => {
  let l = L.sanitizeLayout({ v: 1, playbacks: [5] });
  l = L.withPlayback(l, 2, true);
  l = L.withPlayback(l, 9, true);
  l = L.withPlayback(l, 2, true);
  assert.deepEqual(plain(l.playbacks), [2, 5, 9]);
  l = L.withPlayback(L.withPlayback(l, 5, false), 9, false);
  assert.deepEqual(plain(l.playbacks), [2]);
  assert.deepEqual(plain(L.withPlayback(l, 2, false).playbacks), [2], 'the last playback stays');
  assert.deepEqual(plain(L.withPlayback(l, 11, true).playbacks), [2], 'out of range ignored');
  assert.deepEqual(plain(L.withPlayback(l, 1.5, true).playbacks), [2], 'not a whole number ignored');
});

test('visibleStrips: chosen playbacks, less the empty ones when asked', () => {
  const empties = [false, true, false, true, false, false, false, false, false, true];
  const l = L.sanitizeLayout({ v: 1, playbacks: [1, 2, 3, 4, 10] });
  assert.deepEqual(plain(L.visibleStrips(l, empties)), [0, 1, 2, 3, 9], 'empty ones shown while hideEmpty is off');
  const hiding = L.sanitizeLayout({ v: 1, playbacks: [1, 2, 3, 4, 10], hideEmpty: true });
  assert.deepEqual(plain(L.visibleStrips(hiding, empties)), [0, 2]);
  assert.deepEqual(plain(L.visibleStrips(hiding, [])), [0, 1, 2, 3, 9], 'unknown means not empty (before the first state)');
  assert.deepEqual(plain(L.visibleStrips(L.sanitizeLayout({ v: 1, playbacks: [2], hideEmpty: true }), empties)), [], 'may be none: the page says so');
});

test('pickPane: the last section if still shown, else the first shown', () => {
  const l = L.sanitizeLayout({ v: 1, panes: { cues: false, playbacks: true, masters: true } });
  assert.equal(L.pickPane(l, 'masters'), 'masters');
  assert.equal(L.pickPane(l, 'cues'), 'playbacks');
  assert.equal(L.pickPane(l, ''), 'playbacks');
  assert.equal(L.pickPane(L.defaultLayout(), 'cues'), 'cues');
});

test('the page keeps the layout per device under its own key', () => {
  assert.ok(script.includes("const LAYOUT_KEY = 'stagecraft.webremote.layout';"));
  assert.ok(/forget\(reason\)[\s\S]*?\{[\s\S]*?\}/.test(script));
  const forget = /function forget\(reason\) \{([\s\S]*?)\n  \}/.exec(script);
  assert.ok(forget && !forget[1].includes('LAYOUT_KEY'), 'unpairing keeps the layout');
});

if (failures.length) {
  console.error(`WebRemotePageTests: ${passed} passed, ${failures.length} failed`);
  for (const f of failures) console.error('  FAIL ' + f);
  process.exit(1);
}
console.log(`WebRemotePageTests: ${passed} passed, 0 failed`);
