// Drives the real Web Remote page in headless Microsoft Edge (or Chrome) against a running StageCraft, as a phone would (WR4,
// Docs/ADR/0036 section 12). Started by RunWebRemotePageCheck.ps1 while StageCraft runs with the Web Remote on.
//
// Flow: wait for the PIN in the game log; open the QR link (/#pin=...) so the page pairs by itself; check the PIN left the address bar;
// change the layout through the Layout sheet's own buttons (Masters off, Large, playbacks 1-4); reload and check it was kept; press GO;
// lock, check Layout and Unpair are gone, unlock with a real 2-second press; reset; unpair (StageCraft forgets the device).
// Screenshots go next to the game log. Prints one JSON line of results; exit code 0 = every check passed.
//
// Usage: node WebRemotePageBrowserCheck.mjs <game.log> <port> <outDir>   (Node 22: global fetch and WebSocket, no packages)

import { spawn } from 'node:child_process';
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const [logPath, portText, outDir] = process.argv.slice(2);
const port = Number(portText);
const debugPort = 9339;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const checks = [];
const check = (name, ok, detail) => { checks.push({ name, ok: ok === true, detail }); };

function findBrowser() {
  const candidates = [
    'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
    'C:/Program Files/Microsoft/Edge/Application/msedge.exe',
    'C:/Program Files/Google/Chrome/Application/chrome.exe'
  ];
  return candidates.find((p) => existsSync(p));
}

async function waitFor(what, fn, timeoutMs) {
  const end = Date.now() + timeoutMs;
  for (;;) {
    const v = await fn().catch(() => undefined);
    if (v) return v;
    if (Date.now() > end) throw new Error(`timed out waiting for ${what}`);
    await sleep(250);
  }
}

class Cdp {
  constructor(url) {
    this.next = 1;
    this.pending = new Map();
    this.handlers = [];
    this.ws = new WebSocket(url);
    this.ws.addEventListener('message', (e) => {
      const m = JSON.parse(e.data);
      if (m.id && this.pending.has(m.id)) {
        const { resolve, reject } = this.pending.get(m.id);
        this.pending.delete(m.id);
        if (m.error) reject(new Error(m.error.message)); else resolve(m.result);
      } else if (m.method) {
        this.handlers.forEach((h) => h(m));
      }
    });
  }
  open() { return new Promise((resolve, reject) => { this.ws.addEventListener('open', resolve); this.ws.addEventListener('error', reject); }); }
  send(method, params = {}) {
    const id = this.next++;
    this.ws.send(JSON.stringify({ id, method, params }));
    return new Promise((resolve, reject) => this.pending.set(id, { resolve, reject }));
  }
  async eval(expression) {
    const r = await this.send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.text + ' ' + (r.exceptionDetails.exception?.description || ''));
    return r.result.value;
  }
  async shot(name) {
    const r = await this.send('Page.captureScreenshot', { format: 'png' });
    const file = join(outDir, name);
    writeFileSync(file, Buffer.from(r.data, 'base64'));
    return file;
  }
}

const browserPath = findBrowser();
if (!browserPath) { console.log(JSON.stringify({ ok: false, error: 'no Edge or Chrome' })); process.exit(2); }
const profile = mkdtempSync(join(tmpdir(), 'stagecraft-webremote-'));
const browser = spawn(browserPath, ['--headless=new', `--remote-debugging-port=${debugPort}`, `--user-data-dir=${profile}`, '--no-first-run',
  '--no-default-browser-check', '--disable-extensions', 'about:blank'], { stdio: 'ignore' });

let cdp;
const shots = [];
try {
  const pin = await waitFor('the PIN in the game log', async () => {
    if (!existsSync(logPath)) return null;
    const m = /StageCraft\.WebRemote\.Pin=(\d{6})/.exec(readFileSync(logPath, 'utf8'));
    return m ? m[1] : null;
  }, 120000);

  const target = await waitFor('the browser', async () => {
    const list = await (await fetch(`http://127.0.0.1:${debugPort}/json/list`)).json();
    return list.find((t) => t.type === 'page');
  }, 20000);
  cdp = new Cdp(target.webSocketDebuggerUrl);
  await cdp.open();
  await cdp.send('Page.enable');
  await cdp.send('Runtime.enable');
  // Unpair asks window.confirm: accept it, as the user would.
  cdp.handlers.push((m) => { if (m.method === 'Page.javascriptDialogOpening') cdp.send('Page.handleJavaScriptDialog', { accept: true }); });
  // An iPhone-sized screen, touch-style.
  await cdp.send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });

  const page = `http://127.0.0.1:${port}/`;
  const connected = () => cdp.eval("document.getElementById('conn').dataset.s === 'connected' && !document.getElementById('ctl').hidden");
  const click = (sel) => cdp.eval(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e || e.offsetParent === null) return false; e.click(); return true; })()`);
  const shown = (sel) => cdp.eval(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); return !!e && !e.hidden && e.offsetParent !== null; })()`);

  // 1. The QR link pairs by itself and leaves no PIN behind.
  await cdp.send('Page.navigate', { url: `${page}#pin=${pin}` });
  await waitFor('pairing from the QR link', connected, 20000);
  check('paired from the QR link and connected', true);
  check('the PIN left the address bar', (await cdp.eval('location.href')) === page, await cdp.eval('location.href'));
  check('a token is stored', await cdp.eval("/^[A-Za-z0-9_-]{8,256}$/.test(localStorage.getItem('stagecraft.webremote.token') || '')"));
  check('default: Layout and Unpair shown, Locked hidden', (await shown('#layoutBtn')) && (await shown('#unpair')) && !(await shown('#lockBtn')));
  shots.push(await cdp.shot('wr4-1-default.png'));

  // 2. The Layout sheet, through its own buttons.
  check('Layout opens', await click('#layoutBtn'));
  await sleep(200);
  check('sheet shown', await shown('#layout .card'));
  await click('#optPanes button[data-pane="masters"]');
  await click('#optSize button[data-size="large"]');
  for (let n = 5; n <= 10; n++) await click(`#optPbs button[data-pb="${n}"]`);
  check('sheet marks the choices', await cdp.eval(`document.querySelector('#optPanes [data-pane=masters]').getAttribute('aria-pressed') === 'false'
    && document.querySelector('#optSize [data-size=large]').getAttribute('aria-pressed') === 'true'
    && [...document.querySelectorAll('#optPbs button')].filter(b => b.getAttribute('aria-pressed') === 'true').length === 4`));
  shots.push(await cdp.shot('wr4-2-sheet.png'));
  // The last section and the last playback cannot be switched off.
  await click('#optPanes button[data-pane="cues"]');
  await click('#optPanes button[data-pane="playbacks"]');
  check('the last section stays on', await cdp.eval("document.querySelector('#optPanes [data-pane=playbacks]').getAttribute('aria-pressed') === 'true'"));
  await click('#optPanes button[data-pane="cues"]');
  await click('#layoutDone');

  const layoutState = () => cdp.eval(`({
    large: document.body.classList.contains('large'),
    mastersHidden: document.getElementById('pMasters').hidden,
    mastersTabHidden: document.querySelector('#tabs [data-tab=pMasters]').hidden,
    strips: [...document.querySelectorAll('#strips .strip')].filter(s => !s.hidden).length,
    stored: localStorage.getItem('stagecraft.webremote.layout')
  })`);
  let s = await layoutState();
  check('applied: large, Masters hidden with its tab, 4 strips', s.large && s.mastersHidden && s.mastersTabHidden && s.strips === 4, s);
  await click('#tabs button[data-tab="pPlaybacks"]');
  await sleep(200);
  shots.push(await cdp.shot('wr4-3-playbacks-large.png'));
  await click('#tabs button[data-tab="pCues"]');
  await sleep(200);
  shots.push(await cdp.shot('wr4-4-cues-large.png'));

  // 3. Kept across a reload (the page reconnects with its token, no PIN).
  await cdp.send('Page.reload');
  await waitFor('reconnecting after reload', connected, 20000);
  s = await layoutState();
  check('kept after reload', s.large && s.mastersHidden && s.strips === 4, s);

  // 4. GO from the page reaches StageCraft (counted in the game's status line at the end of the run).
  check('GO pressed', await click('#go'));

  // 5. Lock: Layout and Unpair disappear; a short tap does not unlock; a 2-second press does.
  await click('#layoutBtn');
  await click('#optLock');
  await sleep(200);
  check('locked: Layout and Unpair hidden, Locked shown', !(await shown('#layoutBtn')) && !(await shown('#unpair')) && (await shown('#lockBtn')) && (await shown('#layout .card')) === false);
  shots.push(await cdp.shot('wr4-5-locked.png'));
  const box = await cdp.eval("(() => { const r = document.getElementById('lockBtn').getBoundingClientRect(); return { x: r.x + r.width / 2, y: r.y + r.height / 2 }; })()");
  const press = async (ms) => {
    await cdp.send('Input.dispatchMouseEvent', { type: 'mousePressed', x: box.x, y: box.y, button: 'left', clickCount: 1 });
    await sleep(ms);
    await cdp.send('Input.dispatchMouseEvent', { type: 'mouseReleased', x: box.x, y: box.y, button: 'left', clickCount: 1 });
  };
  await press(300);
  await sleep(200);
  check('a short tap keeps the lock', await shown('#lockBtn'));
  await press(2300);
  await sleep(300);
  check('a 2-second press unlocks', (await shown('#layoutBtn')) && (await shown('#unpair')) && !(await shown('#lockBtn')));

  // 6. Reset brings the default back.
  await click('#layoutBtn');
  await click('#layoutReset');
  await click('#layoutDone');
  s = await layoutState();
  check('reset: default layout', !s.large && !s.mastersHidden && s.strips === 10, s);

  // 7. Unpair: the page forgets its token and StageCraft the device; the layout stays on the device.
  await click('#unpair');
  await waitFor('the pairing screen', () => shown('#pair .card'), 10000);
  await sleep(1000);
  check('unpaired: token gone, layout kept', await cdp.eval("localStorage.getItem('stagecraft.webremote.token') === null && localStorage.getItem('stagecraft.webremote.layout') !== null"));
} catch (e) {
  check('run', false, String(e && e.message || e));
} finally {
  try { cdp?.ws.close(); } catch (e) { /* ignore */ }
  browser.kill();
  await sleep(500);
  try { rmSync(profile, { recursive: true, force: true }); } catch (e) { /* the browser may still hold it */ }
}

const ok = checks.length > 0 && checks.every((c) => c.ok);
console.log(JSON.stringify({ ok, checks, shots }));
process.exit(ok ? 0 : 1);
