// Development only: writes a real-money copy of the C++-written test package (test/fixtures), for the end-to-end run of the UE client
// against a local server (Scripts/Tests/RunCloudEndToEnd.ps1). The scene section is kept byte for byte, so the client reads it exactly
// as one of its own; only the manifest changes (new id, title, a USD price).
// Usage: node scripts/make-dev-package.ts <out.stage> <priceMinorUnits>
import { randomUUID } from 'node:crypto';
import { readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { readDirectory, readSection } from '../src/package/stagePackage.ts';
import { writeContainer } from '../test/helpers.ts';

const [out, priceText] = process.argv.slice(2);
const price = Number(priceText ?? '1500');
if (!out || !Number.isSafeInteger(price) || price <= 0) {
  console.error('usage: node scripts/make-dev-package.ts <out.stage> <priceMinorUnits>');
  process.exit(2);
}
const fixtures = join(import.meta.dirname, '..', 'test', 'fixtures');
const source = readdirSync(fixtures).find((name) => name.endsWith('.stage'));
if (!source) {
  console.error('no .stage fixture found');
  process.exit(1);
}
const bytes = readFileSync(join(fixtures, source));
const sections = readDirectory(bytes);
const tag = (text: string) => text.charCodeAt(0) | (text.charCodeAt(1) << 8) | (text.charCodeAt(2) << 16) | (text.charCodeAt(3) << 24);
const raw = (name: string) => {
  const section = sections.find((each) => each.tag === tag(name));
  return section ? readSection(bytes, section) : null;
};
const manifest = JSON.parse(raw('MNFT')!.toString('utf8'));
manifest.id = randomUUID().toUpperCase();
manifest.title = 'Paid Arena Rig (dev)';
manifest.price = { credits: 0, realMoney: { amount: price, currency: 'USD' } };
const parts = [{ tag: 'MNFT', raw: Buffer.from(JSON.stringify(manifest), 'utf8') }, { tag: 'SCEN', raw: raw('SCEN')! }];
const thumbnail = raw('THMB');
if (thumbnail) parts.push({ tag: 'THMB', raw: thumbnail });
writeFileSync(out, writeContainer(parts));
console.log(manifest.id);
