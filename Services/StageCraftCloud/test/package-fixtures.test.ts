// Scene format v12 (device-library fixture items) and manifest v3 with the FIXT section (ADR 0033 §7.1, §9).
import test from 'node:test';
import assert from 'node:assert/strict';
import {
  FixtureTag,
  MaxFixtureFileBytes,
  MaxFixtureSectionBytes,
  PackageError,
  parseFixtureSection,
  parseManifest,
  readDirectory,
  readPackage,
  readSection,
  type PackageResult,
} from '../src/package/stagePackage.ts';
import { checkScene, fixtureTypeOfPackage, isValidItem } from '../src/package/sceneCheck.ts';
import { fixtureItem, fixturePath, fixtureSection, gdtfBytes, manifestJson, sceneItem, sceneJson, writeContainer, writePackage } from './helpers.ts';

function refused(fn: () => unknown, result: PackageResult): void {
  assert.throws(fn, (error: unknown) => {
    assert.ok(error instanceof PackageError, `expected PackageError, got ${String(error)}`);
    assert.equal(error.result, result, error.message);
    return true;
  });
}

// T1 < T2 only as unsigned numbers (0x7FFFFFFF < 0x80000000); a signed comparison of A would order them the other way.
const T1 = '7FFFFFFF-0000-0000-0000-000000000001';
const T2 = '80000000-0000-0000-0000-000000000001';
const T3 = '80000000-0000-0001-0000-000000000000';
const ZERO = '00000000-0000-0000-0000-000000000000';

/** A v3 package whose scene uses `used` and whose manifest and FIXT section include `included` (both default to the same). */
function v3Package(
  used: string[],
  options: { included?: string[]; fixtures?: Buffer | null; manifest?: Record<string, unknown>; scene?: Record<string, unknown> } = {},
): Buffer {
  const included = options.included ?? used;
  const scene = options.scene ?? sceneJson([sceneItem(), ...used.map((id) => fixtureItem(id))]);
  const manifest = manifestJson({ version: 3, fixtureTypes: included, ...options.manifest }, scene);
  const fixtures = options.fixtures === undefined ? (included.length > 0 ? fixtureSection(included.map((id) => ({ id }))) : undefined) : options.fixtures;
  return writePackage({ scene, manifest, fixtures: fixtures ?? undefined }).bytes;
}

test('scene v12: library fixture items with their fixtureType reference are accepted', () => {
  const scene = sceneJson([sceneItem(), fixtureItem(T2, 'Mode_16ch'), fixtureItem(T1), fixtureItem(T1, 'Mode_2')]);
  const summary = checkScene(JSON.stringify(scene));
  assert.equal(summary.version, 12);
  assert.equal(summary.repairs, 0);
  assert.equal(summary.itemCount, 4);
  assert.equal(summary.portable, true);
  assert.deepEqual(summary.fixtureTypes, [T1, T2]);
  assert.ok(summary.requiredItems.includes(fixturePath(T1, 'Mode_2')));
  // The C++ keeps a library item without a reference (and on any version); its type still counts.
  const bare = { ...fixtureItem(T1) };
  delete bare.fixtureType;
  assert.deepEqual(checkScene(JSON.stringify(sceneJson([bare], { version: 4 }))).fixtureTypes, [T1]);
  // Hex digits of the path are case-insensitive; the root is not.
  assert.equal(fixtureTypeOfPackage(`/Temp/StageCraftFixtures/${'7fffffff000000000000000000000001'}/Mode`), T1);
  assert.equal(fixtureTypeOfPackage(`/temp/StageCraftFixtures/${'7fffffff000000000000000000000001'}/Mode`), null);
  assert.equal(isValidItem({ ...fixtureItem(T1), fixtureType: { id: T1, manufacturer: 'm'.repeat(256), name: '', mode: 'x' } }), true);
});

test('scene v12: what the C++ drops, clips or ignores is a repair and the package is Corrupt', () => {
  const hex = T1.replace(/-/g, '');
  const bad: Record<string, unknown>[] = [
    // Paths IsFixtureItemPath refuses.
    { ...sceneItem(`/temp/StageCraftFixtures/${hex}/Mode.Mode`) },
    { ...sceneItem(`/Temp/StageCraftFixtures/${hex.slice(1)}/Mode.Mode`) },
    { ...sceneItem(`/Temp/StageCraftFixtures/${hex.slice(0, 31)}G/Mode.Mode`) },
    { ...sceneItem(`/Temp/StageCraftFixtures/${hex}/Mo-de.Mo-de`) },
    { ...sceneItem(`/Temp/StageCraftFixtures/${hex}/${'S'.repeat(65)}.${'S'.repeat(65)}`) },
    { ...sceneItem(`/Temp/StageCraftFixtures/${hex}/Sub/Mode.Mode`) },
    { ...sceneItem(`/Temp/StageCraftFixtures/${hex}.Mode`) },
    // References the C++ drops (unreadable or zero id, not an object) or ignores (on a non-library item).
    { ...fixtureItem(T1), fixtureType: { id: 'nope' } },
    { ...fixtureItem(T1), fixtureType: { manufacturer: 'x' } },
    { ...fixtureItem(T1), fixtureType: 'x' },
    { ...sceneItem(fixturePath(ZERO)), fixtureType: { id: ZERO } },
    { ...sceneItem(), fixtureType: { id: T1 } },
    // A reference naming another type than its path.
    { ...fixtureItem(T1), fixtureType: { id: T2, manufacturer: 'm', name: 'n', mode: 'Mode_1' } },
    // Text that is not a string, or longer than the C++ keeps.
    { ...fixtureItem(T1), fixtureType: { id: T1, manufacturer: 5 } },
    { ...fixtureItem(T1), fixtureType: { id: T1, name: 'n'.repeat(257) } },
    { ...fixtureItem(T1), fixtureType: { id: T1, mode: null } },
  ];
  for (const item of bad) {
    assert.equal(isValidItem(item), false, JSON.stringify(item).slice(0, 160));
    const scene = sceneJson([sceneItem(), item]);
    refused(() => readPackage(writePackage({ scene, manifest: manifestJson({ stats: { items: 1, fixtures: 0, cues: 0 }, requiredItems: [] }, scene) }).bytes), 'Corrupt');
  }
});

test('manifest v3 with two fixture files round-trips; v1 and v2 still read as before', () => {
  const read = readPackage(v3Package([T2, T1].sort()));
  assert.equal(read.manifest.manifestVersion, 3);
  assert.deepEqual(read.manifest.fixtureTypes, [T1, T2]);
  assert.deepEqual(read.scene.fixtureTypes, [T1, T2]);
  assert.deepEqual(read.fixtures.map((file) => [file.id, file.size]), [[T1, 64], [T2, 64]]);
  assert.ok(read.fixtures[0]!.data.equals(gdtfBytes()));
  assert.ok(read.manifest.requiredItems.includes(fixturePath(T1)));
  // Lower-case hex in the manifest reads as the same ids (FGuid::ParseExact accepts either case).
  const lower = v3Package([T1, T2], { manifest: { fixtureTypes: [T1.toLowerCase(), T2.toLowerCase()] } });
  assert.deepEqual(readPackage(lower).manifest.fixtureTypes, [T1, T2]);
  // A v3 package without library fixtures has an empty list and no FIXT section.
  const plain = readPackage(v3Package([]));
  assert.deepEqual(plain.manifest.fixtureTypes, []);
  assert.deepEqual(plain.fixtures, []);
  for (const version of [1, 2]) {
    const read2 = readPackage(writePackage({ manifest: manifestJson({ version }) }).bytes);
    assert.equal(read2.manifest.manifestVersion, version);
    assert.deepEqual(read2.manifest.fixtureTypes, []);
    assert.deepEqual(read2.fixtures, []);
    // Before v3 a fixtureTypes field means nothing, like allowExport in v1.
    assert.deepEqual(parseManifest(JSON.stringify(manifestJson({ version, fixtureTypes: 'ignored' }))).fixtureTypes, []);
  }
  // Before v3 nothing can be included, so a scene with library fixtures is not portable, as in today's client.
  const scene = sceneJson([fixtureItem(T1)]);
  refused(() => readPackage(writePackage({ scene, manifest: manifestJson({}, scene) }).bytes), 'NotPortable');
});

test('manifest v3 fixtureTypes refusals; version 4 is VersionMismatch', () => {
  const parse = (fields: Record<string, unknown>) => parseManifest(JSON.stringify(manifestJson({ version: 3, fixtureTypes: [], ...fields })));
  const corrupt = (fields: Record<string, unknown>) => refused(() => parse(fields), 'Corrupt');
  corrupt({ fixtureTypes: undefined });
  corrupt({ fixtureTypes: null });
  corrupt({ fixtureTypes: T1 });
  corrupt({ fixtureTypes: [5] });
  corrupt({ fixtureTypes: [T1, 5] });
  corrupt({ fixtureTypes: ['nope'] });
  // Only the DigitsWithHyphens layout (FGuid::ParseExact), not the other forms FGuid::Parse reads for "id".
  corrupt({ fixtureTypes: [`{${T1}}`] });
  corrupt({ fixtureTypes: [T1.replace(/-/g, '')] });
  corrupt({ fixtureTypes: ['7FFFFFFF-00000000-00000000-00000001'] });
  corrupt({ fixtureTypes: [ZERO] });
  corrupt({ fixtureTypes: [T2, T1] });
  corrupt({ fixtureTypes: [T1, T1] });
  corrupt({ fixtureTypes: [T1, T1.toLowerCase()] });
  const many = Array.from({ length: 65 }, (_, index) => `00000000-0000-0000-0000-${(index + 1).toString(16).padStart(12, '0')}`);
  corrupt({ fixtureTypes: many });
  assert.equal(parse({ fixtureTypes: many.slice(0, 64) }).fixtureTypes.length, 64);
  assert.deepEqual(parse({ fixtureTypes: [T1, T2, T3] }).fixtureTypes, [T1, T2, T3]);
  refused(() => parse({ version: 4 }), 'VersionMismatch');
  refused(() => readPackage(v3Package([T1], { manifest: { version: 4 } })), 'VersionMismatch');
});

test('FIXT section structure refusals', () => {
  const section = (entries: Parameters<typeof fixtureSection>[0], options: Parameters<typeof fixtureSection>[1] = {}) =>
    v3Package([T1, T2], { fixtures: fixtureSection(entries, options) });
  const both = [{ id: T1 }, { id: T2 }];
  refused(() => readPackage(section([], { count: 0 })), 'Corrupt');
  refused(() => readPackage(section(both, { count: 65 })), 'Corrupt');
  refused(() => readPackage(section([{ id: T1, data: gdtfBytes(21) }, { id: T2 }])), 'Corrupt');
  assert.equal(readPackage(section([{ id: T1, data: gdtfBytes(22) }, { id: T2 }])).fixtures[0]!.size, 22);
  refused(() => readPackage(section([{ id: T1, data: Buffer.alloc(64) }, { id: T2 }])), 'Corrupt');
  refused(() => readPackage(section([{ id: T1 }, { id: T2, data: Buffer.concat([Buffer.from('PK\u0005\u0006'), Buffer.alloc(60)]) }])), 'Corrupt');
  refused(() => readPackage(section([{ id: [0, 0, 0, 0] }, { id: T2 }])), 'Corrupt');
  refused(() => readPackage(section([{ id: T2 }, { id: T1 }])), 'Corrupt');
  refused(() => readPackage(section([{ id: T1 }, { id: T1 }])), 'Corrupt');
  refused(() => readPackage(section(both, { trailing: Buffer.from([0]) })), 'Corrupt');
  // Count says 2 but only one entry is there; an entry whose size runs past the end; a header cut short.
  refused(() => readPackage(section([{ id: T1 }], { count: 2 })), 'Corrupt');
  const full = fixtureSection(both);
  refused(() => readPackage(v3Package([T1, T2], { fixtures: full.subarray(0, full.length - 1) })), 'Corrupt');
  refused(() => readPackage(v3Package([T1, T2], { fixtures: full.subarray(0, 4 + 84 + 10) })), 'Corrupt');
  refused(() => readPackage(v3Package([T1, T2], { fixtures: Buffer.from([2, 0]) })), 'Corrupt');
  // Ordering is (A, B, C, D) as unsigned words: B decides when A is equal.
  assert.deepEqual(parseFixtureSection(fixtureSection([{ id: T2 }, { id: T3 }])).map((file) => file.id), [T2, T3]);
  refused(() => parseFixtureSection(fixtureSection([{ id: T3 }, { id: T2 }])), 'Corrupt');
  // The id is the FGuid words A, B, C, D, each written as 8 hex digits.
  assert.equal(parseFixtureSection(fixtureSection([{ id: [0x01234567, 0x89abcdef, 0xfedcba98, 0x76543210] }]))[0]!.id, '01234567-89AB-CDEF-FEDC-BA9876543210');
});

test('FIXT size caps: per file, in total via declared sizes, and the raw section cap before inflating', () => {
  refused(() => parseFixtureSection(fixtureSection([{ id: T1, data: Buffer.alloc(0), size: MaxFixtureFileBytes + 1 }])), 'TooLarge');
  // Sizes come first, as in DecodeFixtureFiles: an oversize entry with a zero id is TooLarge, not Corrupt.
  refused(() => parseFixtureSection(fixtureSection([{ id: [0, 0, 0, 0], data: Buffer.alloc(0), size: MaxFixtureFileBytes + 1 }])), 'TooLarge');
  // Two full 16 MiB files are allowed; a third that only declares its size takes the total over 32 MiB.
  const big = gdtfBytes(MaxFixtureFileBytes);
  const two = parseFixtureSection(fixtureSection([{ id: T1, data: big }, { id: T2, data: big }]));
  assert.deepEqual(two.map((file) => file.size), [MaxFixtureFileBytes, MaxFixtureFileBytes]);
  refused(() => parseFixtureSection(fixtureSection([{ id: T1, data: big }, { id: T2, data: big }, { id: T3, data: Buffer.alloc(0), size: 22 }])), 'TooLarge');
  // A FIXT section declaring more than its cap is refused before anything is inflated, through the reader and directly.
  const scene = sceneJson([fixtureItem(T1)]);
  const manifest = Buffer.from(JSON.stringify(manifestJson({ version: 3, fixtureTypes: [T1] }, scene)));
  const raw = fixtureSection([{ id: T1 }]);
  const bytes = writeContainer([
    { tag: 'MNFT', raw: manifest },
    { tag: 'SCEN', raw: Buffer.from(JSON.stringify(scene)) },
    { tag: 'FIXT', raw, rawSize: BigInt(MaxFixtureSectionBytes + 1) },
  ]);
  refused(() => readPackage(bytes), 'TooLarge');
  const fixt = readDirectory(bytes).find((section) => section.tag === FixtureTag)!;
  refused(() => readSection(bytes, { ...fixt, rawSize: 1n << 40n }), 'TooLarge');
  // At the cap the size check passes and the codec check decides.
  refused(() => readSection(bytes, { ...fixt, rawSize: BigInt(MaxFixtureSectionBytes) }), 'Corrupt');
  assert.equal(MaxFixtureSectionBytes, 33_555_716);
});

test('cross-checks between the manifest, the FIXT section and the scene', () => {
  // fixtureTypes listed but no FIXT section.
  refused(() => readPackage(v3Package([T1], { fixtures: null })), 'Corrupt');
  // FIXT section but an empty fixtureTypes, in v3 and in v2.
  refused(() => readPackage(v3Package([], { fixtures: fixtureSection([{ id: T1 }]) })), 'Corrupt');
  const plainScene = sceneJson();
  refused(() => readPackage(writePackage({ scene: plainScene, manifest: manifestJson({}, plainScene), fixtures: fixtureSection([{ id: T1 }]) }).bytes), 'Corrupt');
  // FIXT ids differ from fixtureTypes.
  refused(() => readPackage(v3Package([T1, T2], { fixtures: fixtureSection([{ id: T1 }, { id: T3 }]) })), 'Corrupt');
  refused(() => readPackage(v3Package([T1, T2], { fixtures: fixtureSection([{ id: T1 }]) })), 'Corrupt');
  // The scene uses a type the package does not include.
  refused(() => readPackage(v3Package([T1, T2], { included: [T1] })), 'NotPortable');
  // A library item with a zero type id can never be included.
  const zeroScene = sceneJson([sceneItem(fixturePath(ZERO))]);
  refused(() => readPackage(v3Package([], { scene: zeroScene })), 'NotPortable');
  // The package includes a type no item uses.
  refused(() => readPackage(v3Package([T1], { included: [T1, T2] })), 'Corrupt');
  // Imported models stay not portable, with or without fixture files.
  const imported = sceneJson([sceneItem('/Temp/StageCraft/Imported/IMP_1.IMP_1'), fixtureItem(T1)]);
  refused(() => readPackage(v3Package([T1], { scene: imported })), 'NotPortable');
  // Lower-case hex in an item path names the same type as the canonical manifest entry.
  const lowerPath = `/Temp/StageCraftFixtures/${T1.replace(/-/g, '').toLowerCase()}/Mode.Mode`;
  const lowerScene = sceneJson([{ ...sceneItem(lowerPath), fixtureType: { id: T1 } }]);
  assert.deepEqual(readPackage(v3Package([T1], { scene: lowerScene })).fixtures.map((file) => file.id), [T1]);
});
