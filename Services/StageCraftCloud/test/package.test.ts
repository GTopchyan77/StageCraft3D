import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { deflateSync } from 'node:zlib';
import {
  cleanText,
  isValidThumbnail,
  MaxPackageBytes,
  normalizeCategory,
  PackageError,
  parseGuid,
  parseManifest,
  readDirectory,
  readPackage,
  readSection,
  validateManifest,
  CODEC_ZLIB,
  CODEC_STORED,
  type PackageResult,
} from '../src/package/stagePackage.ts';
import { checkScene, cueThousandths, itemPackageName, isValidItem } from '../src/package/sceneCheck.ts';
import { manifestJson, PNG_1x1, sceneItem, sceneJson, writeContainer, writePackage } from './helpers.ts';

function refused(fn: () => unknown, result: PackageResult): void {
  assert.throws(fn, (error: unknown) => {
    assert.ok(error instanceof PackageError, `expected PackageError, got ${String(error)}`);
    assert.equal(error.result, result, error.message);
    return true;
  });
}

const fixture = readFileSync(new URL('./fixtures/real-writer-505EF34C.stage', import.meta.url));

test('reads a package written by the real C++ writer (StagePackage::Write)', () => {
  const read = readPackage(fixture);
  assert.equal(read.manifest.packageId, '505EF34C-47A1-9708-76AD-7BA25769CB0E');
  assert.equal(read.manifest.title, 'Test');
  assert.equal(read.manifest.manifestVersion, 2);
  assert.equal(read.manifest.kind, 'StageDesign');
  assert.equal(read.manifest.category, 'StageCraft.Marketplace.Category.Concert');
  assert.equal(read.manifest.itemCount, 6);
  assert.equal(read.manifest.fixtureCount, 2);
  assert.equal(read.scene.itemCount, 6);
  assert.equal(read.scene.version, 4);
  assert.equal(read.scene.portable, true);
  assert.deepEqual(read.manifest.requiredItems, [
    '/Game/StageCraft/Data/DA_LineArray_Test.DA_LineArray_Test',
    '/Game/StageCraft/Data/DA_MovingHead_Test.DA_MovingHead_Test',
    '/Game/StageCraft/Data/DA_Truss_Test.DA_Truss_Test',
    '/Game/StageCraft/Items/DA_Span_TrussF34.DA_Span_TrussF34',
  ]);
  assert.equal(read.thumbnail.length, 0);
  // Both sections of the real file are zlib (RFC 1950, 0x78 0x9C), the codec the reader must accept.
  const sections = readDirectory(fixture);
  assert.deepEqual(sections.map((section) => section.codec), [CODEC_ZLIB, CODEC_ZLIB]);
  assert.equal(fixture[sections[0]!.offset], 0x78);
});

test('round trip of the TS writer that mirrors the C++ writer, with a thumbnail', () => {
  const { bytes, id } = writePackage({ manifest: manifestJson({ keywords: ['Arena', ' arena ', '', 'rock\u0001'], title: '  Rig\u0007 One ' }) });
  const read = readPackage(bytes);
  assert.equal(read.manifest.packageId, id);
  assert.equal(read.manifest.title, 'Rig One');
  assert.deepEqual(read.manifest.keywords, ['Arena', 'rock']);
  assert.ok(read.thumbnail.equals(PNG_1x1));
  assert.equal(read.manifest.allowExport, false);
});

test('container: magic, version, counts, bounds, duplicates, trailing bytes', () => {
  const good = writePackage().bytes;
  refused(() => readPackage(Buffer.alloc(10)), 'Corrupt');
  refused(() => readPackage(Buffer.concat([Buffer.from('STAGEPKX'), good.subarray(8)])), 'Corrupt');
  const v2 = Buffer.from(good);
  v2.writeUInt32LE(2, 8);
  refused(() => readPackage(v2), 'VersionMismatch');
  for (const count of [0, 9]) {
    const bad = Buffer.from(good);
    bad.writeUInt32LE(count, 12);
    refused(() => readPackage(bad), 'Corrupt');
  }
  // Count says 3 sections but the file ends inside the table.
  refused(() => readPackage(good.subarray(0, 16 + 44 + 10)), 'Corrupt');
  // Trailing bytes after the last section.
  refused(() => readPackage(Buffer.concat([good, Buffer.from([0])])), 'Corrupt');
  // A section whose stored size runs past the end (including a huge u64 that must not wrap).
  const scene = Buffer.from(JSON.stringify(sceneJson()));
  const manifest = Buffer.from(JSON.stringify(manifestJson()));
  refused(() => readPackage(writeContainer([{ tag: 'MNFT', raw: manifest }, { tag: 'SCEN', raw: scene, storedSize: 0xffffffffffffffffn }])), 'Corrupt');
  // Duplicate tags.
  refused(() => readPackage(writeContainer([{ tag: 'MNFT', raw: manifest }, { tag: 'MNFT', raw: manifest }, { tag: 'SCEN', raw: scene }])), 'Corrupt');
  // Missing scene or manifest.
  refused(() => readPackage(writeContainer([{ tag: 'MNFT', raw: manifest }])), 'Corrupt');
  refused(() => readPackage(writeContainer([{ tag: 'SCEN', raw: scene }])), 'Corrupt');
  // Unknown extra sections are allowed (and never inflated).
  assert.equal(readPackage(writeContainer([{ tag: 'MNFT', raw: manifest }, { tag: 'SCEN', raw: scene }, { tag: 'XTRA', raw: Buffer.from('anything') }])).manifest.title, 'Arena Rig');
});

test('oversize package and oversize sections are TooLarge', () => {
  refused(() => readDirectory(Buffer.alloc(MaxPackageBytes + 1)), 'TooLarge');
  const scene = Buffer.from(JSON.stringify(sceneJson()));
  const manifest = Buffer.from(JSON.stringify(manifestJson()));
  // A manifest that declares more than 256 KiB raw.
  refused(() => readPackage(writeContainer([{ tag: 'MNFT', raw: manifest, rawSize: 256n * 1024n + 1n }, { tag: 'SCEN', raw: scene }])), 'TooLarge');
  const bigThumb = Buffer.alloc(2 * 1024 * 1024 + 1);
  refused(() => readPackage(writeContainer([{ tag: 'MNFT', raw: manifest }, { tag: 'SCEN', raw: scene }, { tag: 'THMB', raw: bigThumb, codec: CODEC_STORED, stored: bigThumb }])), 'TooLarge');
});

test('codecs: zip bomb, size mismatches, damaged streams, unknown codec, empty zlib', () => {
  const manifest = Buffer.from(JSON.stringify(manifestJson()));
  const scene = Buffer.from(JSON.stringify(sceneJson()));
  // Zip bomb: 32 MiB of zeros deflates to ~32 KiB but declares a raw size of 1 KiB. maxOutputLength stops it at RawSize.
  const bomb = deflateSync(Buffer.alloc(32 * 1024 * 1024));
  refused(() => readPackage(writeContainer([{ tag: 'MNFT', raw: manifest }, { tag: 'SCEN', raw: Buffer.alloc(1024), stored: bomb, codec: CODEC_ZLIB }])), 'Corrupt');
  // Declared raw size larger than what inflates.
  refused(() => readPackage(writeContainer([{ tag: 'MNFT', raw: manifest }, { tag: 'SCEN', raw: scene, rawSize: BigInt(scene.length + 10), codec: CODEC_ZLIB, stored: deflateSync(scene) }])), 'Corrupt');
  // Truncated stream.
  const truncated = deflateSync(scene).subarray(0, 20);
  refused(() => readPackage(writeContainer([{ tag: 'MNFT', raw: manifest }, { tag: 'SCEN', raw: scene, codec: CODEC_ZLIB, stored: truncated }])), 'Corrupt');
  // Garbage after a complete zlib stream inside the section.
  const padded = Buffer.concat([deflateSync(scene), Buffer.from([1, 2, 3])]);
  refused(() => readPackage(writeContainer([{ tag: 'MNFT', raw: manifest }, { tag: 'SCEN', raw: scene, codec: CODEC_ZLIB, stored: padded }])), 'Corrupt');
  // Stored codec whose sizes differ.
  refused(() => readPackage(writeContainer([{ tag: 'MNFT', raw: manifest }, { tag: 'SCEN', raw: scene, codec: CODEC_STORED, stored: scene, rawSize: BigInt(scene.length - 1) }])), 'Corrupt');
  // Unknown codec.
  refused(() => readPackage(writeContainer([{ tag: 'MNFT', raw: manifest }, { tag: 'SCEN', raw: scene, codec: 7, stored: scene }])), 'Corrupt');
  // Zlib with RawSize 0.
  refused(() => readPackage(writeContainer([{ tag: 'MNFT', raw: manifest }, { tag: 'SCEN', raw: Buffer.alloc(0), codec: CODEC_ZLIB, stored: deflateSync(Buffer.alloc(0)) }])), 'Corrupt');
});

test('hash mismatch, and sections that are not UTF-8', () => {
  const manifest = Buffer.from(JSON.stringify(manifestJson()));
  const scene = Buffer.from(JSON.stringify(sceneJson()));
  refused(() => readPackage(writeContainer([{ tag: 'MNFT', raw: manifest, hash: Buffer.alloc(20) }, { tag: 'SCEN', raw: scene }])), 'HashMismatch');
  refused(() => readPackage(writeContainer([{ tag: 'MNFT', raw: manifest }, { tag: 'SCEN', raw: scene, hash: createHash('sha1').update('x').digest() }])), 'HashMismatch');
  refused(() => readPackage(writeContainer([{ tag: 'MNFT', raw: Buffer.from([0xff, 0xfe, 0x00]) }, { tag: 'SCEN', raw: scene }])), 'Corrupt');
});

test('manifest rules mirror ParseManifest / NormalizeManifest / ValidateManifest', () => {
  const parse = (fields: Record<string, unknown>) => parseManifest(JSON.stringify(manifestJson(fields)));
  const corrupt = (fields: Record<string, unknown>) => refused(() => parse(fields), 'Corrupt');
  refused(() => parseManifest('not json'), 'Corrupt');
  refused(() => parseManifest('[1]'), 'Corrupt');
  corrupt({ format: 'Other' });
  corrupt({ version: 0 });
  corrupt({ version: 1.5 });
  corrupt({ version: '2' });
  refused(() => parse({ version: 3 }), 'VersionMismatch');
  corrupt({ id: 'nope' });
  corrupt({ id: '00000000-0000-0000-0000-000000000000' });
  corrupt({ title: 5 });
  corrupt({ title: '   ' });
  corrupt({ title: 'x'.repeat(81) });
  assert.equal(parse({ title: 'x'.repeat(80) }).title.length, 80);
  corrupt({ description: 'd'.repeat(2001) });
  corrupt({ description: 7 });
  corrupt({ kind: 'Unknown' });
  corrupt({ kind: 3 });
  assert.equal(parse({ kind: 'lightingrig' }).kind, 'LightingRig');
  assert.equal(parse({ kind: 'EStagePackageKind::CueList' }).kind, 'CueList');
  corrupt({ price: null });
  corrupt({ stats: 'x' });
  corrupt({ keywords: 'arena' });
  corrupt({ keywords: [1] });
  corrupt({ keywords: Array.from({ length: 17 }, (_, i) => `k${i}`) });
  corrupt({ keywords: ['k'.repeat(33)] });
  corrupt({ requiredItems: undefined });
  corrupt({ requiredItems: [5] });
  corrupt({ category: 5 });
  corrupt({ author: 'a'.repeat(65) });
  corrupt({ author: 9 });
  assert.equal(parse({ author: '' }).authorName, 'Anonymous');
  assert.equal(parse({ author: undefined, description: undefined }).description, '');
  // Prices.
  corrupt({ price: { realMoney: { amount: 0, currency: '' } } });
  corrupt({ price: { credits: 1.5 } });
  corrupt({ price: { credits: -1 } });
  corrupt({ price: { credits: 1_000_001 } });
  corrupt({ price: { credits: 0, realMoney: 'x' } });
  corrupt({ price: { credits: 0, realMoney: { amount: 'x' } } });
  corrupt({ price: { credits: 0, realMoney: { amount: 5, currency: 9 } } });
  corrupt({ price: { credits: 0, realMoney: { amount: 100_000_001, currency: 'USD' } } });
  corrupt({ price: { credits: 0, realMoney: { amount: 500, currency: '' } } });
  corrupt({ price: { credits: 0, realMoney: { amount: 500, currency: 'DOLLARS' } } });
  const priced = parse({ price: { credits: 0, realMoney: { amount: 500, currency: ' usd ' } } });
  assert.equal(priced.realMoneyCurrency, 'USD');
  assert.equal(parse({ price: { credits: 0, realMoney: { amount: 0, currency: 'USD' } } }).realMoneyCurrency, '');
  assert.equal(parse({ price: { credits: 3 } }).credits, 3);
  // Stats.
  corrupt({ stats: { items: 1, fixtures: 0 } });
  corrupt({ stats: { items: 10001, fixtures: 0, cues: 0 } });
  corrupt({ stats: { items: 1, fixtures: 2, cues: 0 } });
  corrupt({ stats: { items: 1, fixtures: 0, cues: 2001 } });
  corrupt({ stats: { items: -1, fixtures: 0, cues: 0 } });
  corrupt({ stats: { items: 0, fixtures: 0, cues: 0 }, requiredItems: ['/Game/A.A'] });
  // Categories: unknown, root and missing become Other; case-insensitive known names are canonicalised.
  assert.equal(parse({ category: 'StageCraft.Marketplace.Category' }).category, 'StageCraft.Marketplace.Category.Other');
  assert.equal(parse({ category: 'Something.Else' }).category, 'StageCraft.Marketplace.Category.Other');
  assert.equal(parse({ category: undefined }).category, 'StageCraft.Marketplace.Category.Other');
  assert.equal(parse({ category: 'stagecraft.marketplace.category.theatre' }).category, 'StageCraft.Marketplace.Category.Theatre');
  // allowExport: required boolean from version 2; version 1 reads as false.
  corrupt({ allowExport: 'yes' });
  corrupt({ allowExport: undefined });
  assert.equal(parse({ allowExport: true }).allowExport, true);
  assert.equal(parse({ version: 1, allowExport: undefined }).allowExport, false);
  assert.equal(parse({ version: 1, allowExport: true }).allowExport, false);
  // Optional fields.
  assert.equal(parse({ createdAt: 'garbage' }).createdAtUtc, null);
  assert.equal(parse({ createdAt: undefined, sceneVersion: 'x' }).sceneVersion, null);
  assert.equal(parse({}).createdAtUtc, '2026-11-02T13:43:11.360Z');
});

test('validateManifest names the broken rule', () => {
  const valid = parseManifest(JSON.stringify(manifestJson()));
  assert.equal(validateManifest(valid), null);
  assert.equal(validateManifest({ ...valid, category: 'StageCraft.Marketplace.Category' }), 'category');
  assert.equal(validateManifest({ ...valid, keywords: [''] }), 'keywords');
});

test('scene checks: version, items, ids, portability, origin, cues, counts', () => {
  const pkg = (scene: Record<string, unknown>, manifestFields: Record<string, unknown> = {}) => writePackage({ scene, manifest: manifestJson(manifestFields, scene) }).bytes;
  refused(() => readPackage(pkg(sceneJson([], { version: 12 }))), 'VersionMismatch');
  refused(() => readPackage(pkg(sceneJson([], { version: 0 }))), 'VersionMismatch');
  refused(() => readPackage(pkg(sceneJson([], { format: 'Nope' }))), 'Corrupt');
  refused(() => readPackage(writePackage({ sceneRaw: '{bad' }).bytes), 'Corrupt');
  refused(() => readPackage(pkg({ format: 'StageCraftScene', version: 11, items: 'x' })), 'Corrupt');
  // Invalid item -> the C++ parser would drop it -> refused as Corrupt.
  const badItems: Record<string, unknown>[] = [
    { ...sceneItem(), id: 'bad' },
    { ...sceneItem(), item: '/Engine/Foo.Foo' },
    { ...sceneItem(), item: 'Game/Foo.Foo' },
    { ...sceneItem(), item: 5 },
    { ...sceneItem(), location: [0, 0] },
    { ...sceneItem(), location: [2e6, 0, 0] },
    { ...sceneItem(), rotation: [0, 0, 0, 0] },
    { ...sceneItem(), scale: [1, 1, 'x'] },
    { ...sceneItem(), state: 'not base64!' },
    { ...sceneItem(), state: 'A'.repeat(90000) },
    { ...sceneItem(), state: 5 },
    { ...sceneItem(), label: 5 },
    'not an object',
  ];
  for (const item of badItems) {
    const scene = sceneJson([sceneItem(), item]);
    refused(() => readPackage(pkg(scene, { stats: { items: 1, fixtures: 0, cues: 0 }, requiredItems: [] })), 'Corrupt');
  }
  // Duplicate instance ids would be repaired by the C++ parser -> Corrupt.
  const id = sceneItem().id as string;
  refused(() => readPackage(pkg(sceneJson([sceneItem(undefined, id), sceneItem(undefined, id)]))), 'Corrupt');
  // Imported models are not portable.
  refused(() => readPackage(pkg(sceneJson([sceneItem('/Temp/StageCraft/Imported/IMP_1.IMP_1')]))), 'NotPortable');
  // Counts must match the manifest.
  refused(() => readPackage(pkg(sceneJson([sceneItem()]), { stats: { items: 2, fixtures: 0, cues: 0 } })), 'Corrupt');
  // An unreadable origin listing id is Corrupt; a valid one is reported.
  refused(() => readPackage(pkg(sceneJson([sceneItem()], { originListingId: 'xyz' }))), 'Corrupt');
  const origin = readPackage(pkg(sceneJson([sceneItem()], { originListingId: '505ef34c47a1970876ad7ba25769cb0e' })));
  assert.equal(origin.scene.originListingId, '505EF34C-47A1-9708-76AD-7BA25769CB0E');
  // Cues: invalid numbers and duplicates are repairs -> Corrupt; valid cues are counted.
  const cues = [{ number: 1 }, { number: 2.5 }];
  assert.equal(readPackage(pkg(sceneJson([sceneItem()], { cues }))).scene.cueCount, 2);
  refused(() => readPackage(pkg(sceneJson([sceneItem()], { cues: [{ number: 1 }, { number: 1.0001 }] }), { stats: { items: 1, fixtures: 0, cues: 1 } })), 'Corrupt');
  refused(() => readPackage(pkg(sceneJson([sceneItem()], { cues: [{ number: 0 }] }), { stats: { items: 1, fixtures: 0, cues: 0 } })), 'Corrupt');
  refused(() => readPackage(pkg(sceneJson([sceneItem()], { cues: ['x'] }), { stats: { items: 1, fixtures: 0, cues: 0 } })), 'Corrupt');
  // Cues before version 3 are ignored, like the C++ migration; a non-array cues field reads as no cues.
  assert.equal(readPackage(pkg(sceneJson([sceneItem()], { version: 2, cues: [{ number: 1 }] }), { stats: { items: 1, fixtures: 0, cues: 0 } })).scene.cueCount, 0);
  assert.equal(checkScene(JSON.stringify(sceneJson([], { cues: 'x' }))).cueCount, 0);
  // Required items: distinct case-insensitively, sorted case-insensitively.
  const items = [sceneItem('/Game/b/Z.Z'), sceneItem('/Game/a/Y.Y'), sceneItem('/Game/A/y.y')];
  const read = readPackage(pkg(sceneJson(items)));
  assert.deepEqual(read.manifest.requiredItems, ['/Game/a/Y.Y', '/Game/b/Z.Z']);
});

test('thumbnail must be a PNG within its cap', () => {
  const scene = sceneJson();
  const sections = (thumb: Buffer) =>
    writeContainer([
      { tag: 'MNFT', raw: Buffer.from(JSON.stringify(manifestJson({}, scene))) },
      { tag: 'SCEN', raw: Buffer.from(JSON.stringify(scene)) },
      { tag: 'THMB', raw: thumb },
    ]);
  refused(() => readPackage(sections(Buffer.from('GIF89a-not-a-png'))), 'Corrupt');
  assert.equal(readPackage(sections(Buffer.alloc(0))).thumbnail.length, 0);
  assert.equal(isValidThumbnail(Buffer.alloc(0)), true);
  assert.equal(isValidThumbnail(PNG_1x1.subarray(0, 8)), false);
});

test('helpers: GUID forms, clean text, categories, item paths, cue numbers', () => {
  assert.equal(parseGuid('505EF34C47A1970876AD7BA25769CB0E'), '505EF34C-47A1-9708-76AD-7BA25769CB0E');
  assert.equal(parseGuid('{505ef34c-47a1-9708-76ad-7ba25769cb0e}'), '505EF34C-47A1-9708-76AD-7BA25769CB0E');
  assert.equal(parseGuid('(505ef34c-47a1-9708-76ad-7ba25769cb0e)'), '505EF34C-47A1-9708-76AD-7BA25769CB0E');
  assert.equal(parseGuid('505EF34C-47A19708-76AD7BA2-5769CB0E'), '505EF34C-47A1-9708-76AD-7BA25769CB0E');
  assert.equal(parseGuid('505EF34C'), null);
  assert.equal(cleanText(' a\tb\nc\u007f ', true), 'a\tb\nc');
  assert.equal(cleanText(' a\tb\nc ', false), 'abc');
  assert.equal(normalizeCategory('stagecraft.marketplace.category.club'), 'StageCraft.Marketplace.Category.Club');
  assert.equal(itemPackageName('/Game/A/B.B'), '/Game/A/B');
  assert.equal(itemPackageName('/Game/A/B'), '/Game/A/B');
  assert.equal(itemPackageName('/Game/A/B.B:Sub.Obj'), '/Game/A/B');
  assert.equal(itemPackageName('/Game/A B/C.C'), null);
  assert.equal(itemPackageName('/Game/../C.C'), null);
  assert.equal(isValidItem(null), false);
  assert.equal(cueThousandths(1.5), 1500);
  assert.equal(cueThousandths(0.001), 1);
  assert.equal(cueThousandths(9999.999), 9999999);
  assert.equal(cueThousandths(10000), null);
  assert.equal(cueThousandths(-1), null);
  assert.equal(cueThousandths(2e7), null);
  assert.equal(cueThousandths('1'), null);
  assert.equal(cueThousandths(Number.POSITIVE_INFINITY), null);
});

test('readSection refuses a section over its cap before allocating', () => {
  const bytes = writePackage().bytes;
  const [manifest] = readDirectory(bytes);
  refused(() => readSection(bytes, { ...manifest!, rawSize: 1n << 40n }), 'TooLarge');
});
