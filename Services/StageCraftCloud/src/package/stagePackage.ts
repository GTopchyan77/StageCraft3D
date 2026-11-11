// Server-side reader for `.stage` packages: a port of StagePackage::Read / ParseManifest / ValidateManifest
// (Source/ModularSceneBuilder/Private/Marketplace/StagePackage.cpp, ADR 0021 §2, ADR 0032 §4 "server re-validation").
// A package is untrusted input; the whole package is refused on the first failure. Where the C++ reader is lenient about JSON
// types (UE's TryGet* functions convert numbers to strings and round fractional integers), this reader is stricter: it never
// accepts a package the client would refuse, and the client's own writer always produces the strict form.
import { createHash } from 'node:crypto';
import { inflateSync } from 'node:zlib';
import { checkScene, type SceneSummary } from './sceneCheck.ts';

export const ContainerVersion = 1;
export const ManifestVersion = 3;
export const FirstManifestVersionWithExportFlag = 2;
export const FirstManifestVersionWithFixtureTypes = 3;
export const MaxSections = 8;
export const MaxManifestBytes = 256 * 1024;
export const MaxSceneBytes = 64 * 1024 * 1024;
export const MaxThumbnailBytes = 2 * 1024 * 1024;
/** ADR 0033 §9: the .gdtf files a package carries. */
export const MaxFixtureFiles = 64;
export const MaxFixtureFileBytes = 16 * 1024 * 1024;
export const MaxFixtureTotalBytes = 32 * 1024 * 1024;
/** Count, then per file a 16-byte id and a u32 size, around at most MaxFixtureTotalBytes of files. */
export const MaxFixtureSectionBytes = MaxFixtureTotalBytes + 4 + MaxFixtureFiles * 20;
/** The smallest ZIP archive: an empty one is a 22-byte end-of-central-directory record. */
export const MinFixtureFileBytes = 22;
export const MaxPackageBytes = 96 * 1024 * 1024;
export const MaxTitleLength = 80;
export const MaxDescriptionLength = 2000;
export const MaxAuthorLength = 64;
export const MaxKeywords = 16;
export const MaxKeywordLength = 32;
export const MaxCreditsPrice = 1_000_000;
export const MaxRealMoneyMinorUnits = 100_000_000;
/** FStageSceneStore::MaxItems / MaxCues. */
export const MaxSceneItems = 10_000;
export const MaxSceneCues = 2_000;

export const MAGIC = Buffer.from('STAGEPKG', 'latin1');
export const FIXED_HEADER_BYTES = 8 + 4 + 4;
export const SECTION_HEADER_BYTES = 4 + 4 + 8 + 8 + 20;

export function makeTag(text: string): number {
  return (text.charCodeAt(0) | (text.charCodeAt(1) << 8) | (text.charCodeAt(2) << 16) | (text.charCodeAt(3) << 24)) >>> 0;
}
export const ManifestTag = makeTag('MNFT');
export const SceneTag = makeTag('SCEN');
export const ThumbnailTag = makeTag('THMB');
export const FixtureTag = makeTag('FIXT');

export const CODEC_STORED = 0;
export const CODEC_ZLIB = 1;

export const FORMAT_TAG = 'StageCraftPackage';
export const ANONYMOUS_AUTHOR = 'Anonymous';

export const PACKAGE_KINDS = ['StageDesign', 'LightingRig', 'CueList'] as const;
export type PackageKind = (typeof PACKAGE_KINDS)[number];

export const CATEGORY_ROOT = 'StageCraft.Marketplace.Category';
export const CATEGORIES = ['Concert', 'Festival', 'Theatre', 'Corporate', 'Club', 'Broadcast', 'Worship', 'Other'].map(
  (name) => `${CATEGORY_ROOT}.${name}`,
);
export const CATEGORY_OTHER = `${CATEGORY_ROOT}.Other`;

const PNG_SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
const ZIP_LOCAL_HEADER = Buffer.from([0x50, 0x4b, 0x03, 0x04]);

/** The EStageMarketplaceResult values a read can produce. */
export type PackageResult = 'Corrupt' | 'VersionMismatch' | 'TooLarge' | 'HashMismatch' | 'NotPortable';

export class PackageError extends Error {
  readonly result: PackageResult;
  constructor(result: PackageResult, message: string) {
    super(message);
    this.name = 'PackageError';
    this.result = result;
  }
}

function refuse(result: PackageResult, message: string): never {
  throw new PackageError(result, message);
}

export interface StageManifest {
  packageId: string;
  title: string;
  description: string;
  authorName: string;
  kind: PackageKind;
  category: string;
  keywords: string[];
  credits: number;
  realMoneyMinorUnits: number;
  realMoneyCurrency: string;
  createdAtUtc: string | null;
  sceneVersion: number | null;
  itemCount: number;
  fixtureCount: number;
  cueCount: number;
  requiredItems: string[];
  allowExport: boolean;
  /** Fixture types whose .gdtf files the FIXT section carries, canonical GUID text, strictly ascending. Empty before version 3. */
  fixtureTypes: string[];
  manifestVersion: number;
}

export interface SectionView {
  tag: number;
  codec: number;
  storedSize: bigint;
  rawSize: bigint;
  hash: Buffer;
  offset: number;
}

function maxRawSize(tag: number): number {
  switch (tag) {
    case ManifestTag:
      return MaxManifestBytes;
    case SceneTag:
      return MaxSceneBytes;
    case ThumbnailTag:
      return MaxThumbnailBytes;
    case FixtureTag:
      return MaxFixtureSectionBytes;
    default:
      return 0;
  }
}

/** Container header and section table: counts, duplicates, bounds and trailing bytes. Inflates nothing. */
export function readDirectory(bytes: Buffer): SectionView[] {
  if (bytes.length > MaxPackageBytes) {
    refuse('TooLarge', 'package is over 96 MiB');
  }
  if (bytes.length < FIXED_HEADER_BYTES) {
    refuse('Corrupt', 'package is shorter than its header');
  }
  if (!bytes.subarray(0, 8).equals(MAGIC)) {
    refuse('Corrupt', 'bad magic');
  }
  const version = bytes.readUInt32LE(8);
  const count = bytes.readUInt32LE(12);
  if (version !== ContainerVersion) {
    refuse('VersionMismatch', `container version ${version}`);
  }
  if (count === 0 || count > MaxSections) {
    refuse('Corrupt', `section count ${count}`);
  }
  const total = BigInt(bytes.length);
  let offset = BigInt(FIXED_HEADER_BYTES + count * SECTION_HEADER_BYTES);
  if (offset > total) {
    refuse('Corrupt', 'section table runs past the end');
  }
  const sections: SectionView[] = [];
  for (let index = 0; index < count; index++) {
    const at = FIXED_HEADER_BYTES + index * SECTION_HEADER_BYTES;
    const section: SectionView = {
      tag: bytes.readUInt32LE(at),
      codec: bytes.readUInt32LE(at + 4),
      storedSize: bytes.readBigUInt64LE(at + 8),
      rawSize: bytes.readBigUInt64LE(at + 16),
      hash: Buffer.from(bytes.subarray(at + 24, at + 44)),
      offset: Number(offset),
    };
    // Unsigned comparison before any arithmetic, so a huge size cannot wrap the bounds check.
    if (section.storedSize > total - offset) {
      refuse('Corrupt', 'section runs past the end');
    }
    if (sections.some((other) => other.tag === section.tag)) {
      refuse('Corrupt', 'duplicate section tag');
    }
    offset += section.storedSize;
    sections.push(section);
  }
  if (offset !== total) {
    refuse('Corrupt', 'bytes after the last section');
  }
  return sections;
}

/** Inflates and verifies one section: size cap before allocating, codec, exact size, hash. */
export function readSection(bytes: Buffer, section: SectionView): Buffer {
  if (section.rawSize > BigInt(maxRawSize(section.tag))) {
    refuse('TooLarge', 'section is over its cap');
  }
  const rawSize = Number(section.rawSize);
  const stored = bytes.subarray(section.offset, section.offset + Number(section.storedSize));
  let raw: Buffer;
  switch (section.codec) {
    case CODEC_STORED:
      if (stored.length !== rawSize) {
        refuse('Corrupt', 'stored section size mismatch');
      }
      raw = Buffer.from(stored);
      break;
    case CODEC_ZLIB: {
      if (rawSize === 0) {
        refuse('Corrupt', 'empty zlib section');
      }
      let inflated: { buffer: Buffer; engine: { bytesWritten: number } };
      try {
        // maxOutputLength stops a zip bomb at RawSize; the C++ reader likewise fails unless exactly RawSize bytes come out.
        inflated = inflateSync(stored, { maxOutputLength: rawSize, info: true }) as unknown as typeof inflated;
      } catch {
        refuse('Corrupt', 'zlib stream is damaged or larger than its declared size');
      }
      if (inflated.buffer.length !== rawSize || inflated.engine.bytesWritten !== stored.length) {
        refuse('Corrupt', 'zlib section size mismatch');
      }
      raw = inflated.buffer;
      break;
    }
    default:
      refuse('Corrupt', `unknown codec ${section.codec}`);
  }
  const hash = createHash('sha1').update(raw).digest();
  if (!hash.equals(section.hash)) {
    refuse('HashMismatch', 'section hash mismatch');
  }
  return raw;
}

function decodeUtf8(raw: Buffer): string {
  try {
    return new TextDecoder('utf-8', { fatal: true }).decode(raw);
  } catch {
    refuse('Corrupt', 'section is not UTF-8');
  }
}

const GUID_FORMS = [
  /^([0-9A-Fa-f]{32})$/,
  /^([0-9A-Fa-f]{8})-([0-9A-Fa-f]{4})-([0-9A-Fa-f]{4})-([0-9A-Fa-f]{4})-([0-9A-Fa-f]{12})$/,
  /^\{([0-9A-Fa-f]{8})-([0-9A-Fa-f]{4})-([0-9A-Fa-f]{4})-([0-9A-Fa-f]{4})-([0-9A-Fa-f]{12})\}$/,
  /^\(([0-9A-Fa-f]{8})-([0-9A-Fa-f]{4})-([0-9A-Fa-f]{4})-([0-9A-Fa-f]{4})-([0-9A-Fa-f]{12})\)$/,
  /^([0-9A-Fa-f]{8})-([0-9A-Fa-f]{8})-([0-9A-Fa-f]{8})-([0-9A-Fa-f]{8})$/,
];

const DIGITS_WITH_HYPHENS = /^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}$/;

/** FGuid::Parse for the formats UE writes; returns the canonical upper-case 8-4-4-4-12 form, or null. A zero GUID parses. */
export function parseGuid(text: string): string | null {
  for (const form of GUID_FORMS) {
    const match = form.exec(text);
    if (match) {
      const hex = match.slice(1).join('').toUpperCase();
      return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
    }
  }
  return null;
}

export function isValidGuid(canonical: string | null): canonical is string {
  return canonical !== null && canonical !== '00000000-0000-0000-0000-000000000000';
}

/** Removes control characters (keeps \n and \t when multiLine) and trims, like StagePackageFormat::CleanText. */
export function cleanText(text: string, multiLine: boolean): string {
  let clean = '';
  for (const char of text) {
    const code = char.charCodeAt(0);
    const control = code < 32 || code === 127;
    if (!control || (multiLine && (char === '\n' || char === '\t'))) {
      clean += char;
    }
  }
  return clean.trim();
}

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function isInt(value: unknown, min: number, max: number): value is number {
  return typeof value === 'number' && Number.isInteger(value) && value >= min && value <= max;
}

const INT32_MIN = -2147483648;
const INT32_MAX = 2147483647;

function readStringArray(value: unknown): string[] | null {
  if (!Array.isArray(value) || !value.every((entry) => typeof entry === 'string')) {
    return null;
  }
  return value as string[];
}

function optionalString(root: Record<string, unknown>, field: string): string {
  const value = root[field];
  if (value === undefined) {
    return '';
  }
  if (typeof value !== 'string') {
    refuse('Corrupt', `${field} is not a string`);
  }
  return value;
}

/** UEnum::GetValueByNameString: case-insensitive, with or without the "EStagePackageKind::" prefix. */
function parseKind(text: string): PackageKind | null {
  const bare = text.toLowerCase().startsWith('estagepackagekind::') ? text.slice('EStagePackageKind::'.length) : text;
  return PACKAGE_KINDS.find((kind) => kind.toLowerCase() === bare.toLowerCase()) ?? null;
}

/** Gameplay tag names compare case-insensitively; an unknown or root category becomes Other (NormalizeManifest). */
export function normalizeCategory(text: string): string {
  return CATEGORIES.find((category) => category.toLowerCase() === text.toLowerCase()) ?? CATEGORY_OTHER;
}

export function normalizeManifest(manifest: StageManifest): StageManifest {
  const keywords: string[] = [];
  for (const keyword of manifest.keywords) {
    const clean = cleanText(keyword, false);
    if (clean.length > 0 && !keywords.some((kept) => kept.toLowerCase() === clean.toLowerCase())) {
      keywords.push(clean);
    }
  }
  const authorName = cleanText(manifest.authorName, false);
  return {
    ...manifest,
    title: cleanText(manifest.title, false),
    description: cleanText(manifest.description, true),
    authorName: authorName.length > 0 ? authorName : ANONYMOUS_AUTHOR,
    keywords,
    category: normalizeCategory(manifest.category),
    realMoneyCurrency: manifest.realMoneyMinorUnits === 0 ? '' : manifest.realMoneyCurrency.trim().toUpperCase(),
  };
}

/** StagePackage::ValidateManifest; call after normalizeManifest. Returns null when valid, else the broken rule. */
export function validateManifest(manifest: StageManifest): string | null {
  if (!isValidGuid(manifest.packageId)) return 'id';
  if (manifest.title.length < 1 || manifest.title.length > MaxTitleLength) return 'title';
  if (manifest.description.length > MaxDescriptionLength) return 'description';
  if (manifest.authorName.length < 1 || manifest.authorName.length > MaxAuthorLength) return 'author';
  if (manifest.keywords.length > MaxKeywords) return 'keywords';
  if (manifest.keywords.some((keyword) => keyword.length === 0 || keyword.length > MaxKeywordLength)) return 'keywords';
  if (!CATEGORIES.includes(manifest.category)) return 'category';
  if (!isInt(manifest.credits, 0, MaxCreditsPrice)) return 'credits';
  if (!isInt(manifest.realMoneyMinorUnits, 0, MaxRealMoneyMinorUnits)) return 'realMoney';
  if (manifest.realMoneyMinorUnits === 0 ? manifest.realMoneyCurrency !== '' : !/^[A-Z]{3}$/.test(manifest.realMoneyCurrency)) return 'currency';
  if (!isInt(manifest.itemCount, 0, MaxSceneItems)) return 'items';
  if (!isInt(manifest.cueCount, 0, MaxSceneCues)) return 'cues';
  if (!isInt(manifest.fixtureCount, 0, manifest.itemCount)) return 'fixtures';
  if (manifest.requiredItems.length > manifest.itemCount) return 'requiredItems';
  if (!isValidFixtureTypeList(manifest.fixtureTypes)) return 'fixtureTypes';
  return null;
}

/**
 * At most MaxFixtureFiles valid ids, strictly ascending as (A, B, C, D) unsigned, which also rules out duplicates. Canonical GUID
 * text is fixed-width upper-case hex in A, B, C, D order, so comparing the strings compares the numbers.
 */
function isValidFixtureTypeList(ids: string[]): boolean {
  return ids.length <= MaxFixtureFiles && ids.every((id, index) => isValidGuid(id) && (index === 0 || ids[index - 1]! < id));
}

/** StagePackage::ParseManifest: Corrupt for anything malformed or invalid, VersionMismatch for a newer manifest. */
export function parseManifest(json: string): StageManifest {
  let root: unknown;
  try {
    root = JSON.parse(json);
  } catch {
    refuse('Corrupt', 'manifest is not JSON');
  }
  if (!isObject(root)) {
    refuse('Corrupt', 'manifest is not an object');
  }
  const version = root.version;
  if (root.format !== FORMAT_TAG || !isInt(version, 1, INT32_MAX)) {
    refuse('Corrupt', 'manifest format or version');
  }
  if (version > ManifestVersion) {
    refuse('VersionMismatch', `manifest version ${version}`);
  }

  const packageId = typeof root.id === 'string' ? parseGuid(root.id) : null;
  const kind = typeof root.kind === 'string' ? parseKind(root.kind) : null;
  const price = root.price;
  const stats = root.stats;
  if (packageId === null || typeof root.title !== 'string' || kind === null || !isObject(price) || !isObject(stats)) {
    refuse('Corrupt', 'manifest id, title, kind, price or stats');
  }

  const category = root.category === undefined ? '' : root.category;
  if (typeof category !== 'string') {
    refuse('Corrupt', 'category is not a string');
  }
  const keywords = root.keywords === undefined ? [] : readStringArray(root.keywords);
  const requiredItems = readStringArray(root.requiredItems);
  if (keywords === null || requiredItems === null) {
    refuse('Corrupt', 'keywords or requiredItems');
  }

  if (!isInt(price.credits, Number.MIN_SAFE_INTEGER, Number.MAX_SAFE_INTEGER)) {
    refuse('Corrupt', 'price.credits');
  }
  let realMoneyMinorUnits = 0;
  let realMoneyCurrency = '';
  if (price.realMoney !== undefined) {
    if (!isObject(price.realMoney)) {
      refuse('Corrupt', 'price.realMoney');
    }
    const { amount, currency } = price.realMoney;
    if (amount !== undefined && !isInt(amount, Number.MIN_SAFE_INTEGER, Number.MAX_SAFE_INTEGER)) {
      refuse('Corrupt', 'price.realMoney.amount');
    }
    if (currency !== undefined && typeof currency !== 'string') {
      refuse('Corrupt', 'price.realMoney.currency');
    }
    realMoneyMinorUnits = (amount as number | undefined) ?? 0;
    realMoneyCurrency = (currency as string | undefined) ?? '';
  }

  if (!isInt(stats.items, INT32_MIN, INT32_MAX) || !isInt(stats.fixtures, INT32_MIN, INT32_MAX) || !isInt(stats.cues, INT32_MIN, INT32_MAX)) {
    refuse('Corrupt', 'stats');
  }

  let createdAtUtc: string | null = null;
  if (typeof root.createdAt === 'string') {
    const parsed = Date.parse(root.createdAt);
    createdAtUtc = Number.isNaN(parsed) ? null : new Date(parsed).toISOString();
  }
  const sceneVersion = isInt(root.sceneVersion, INT32_MIN, INT32_MAX) ? root.sceneVersion : null;

  // Deny by default: version 1 predates the flag; version 2 must state it as a JSON boolean.
  let allowExport = false;
  if (version >= FirstManifestVersionWithExportFlag) {
    if (typeof root.allowExport !== 'boolean') {
      refuse('Corrupt', 'allowExport must be a boolean');
    }
    allowExport = root.allowExport;
  }

  // Version 3 must list the included fixture types, even when there are none; older manifests predate them and include none.
  let fixtureTypes: string[] = [];
  if (version >= FirstManifestVersionWithFixtureTypes) {
    const texts = readStringArray(root.fixtureTypes);
    if (texts === null || texts.length > MaxFixtureFiles) {
      refuse('Corrupt', 'fixtureTypes must be an array of at most 64 strings');
    }
    // FGuid::ParseExact(DigitsWithHyphens): only the 8-4-4-4-12 layout the writer uses, unlike the lenient FGuid::Parse of "id".
    if (!texts.every((text) => DIGITS_WITH_HYPHENS.test(text))) {
      refuse('Corrupt', 'fixtureTypes holds an id that is not 8-4-4-4-12 hex');
    }
    fixtureTypes = texts.map((text) => parseGuid(text)!);
  }

  const manifest = normalizeManifest({
    packageId,
    title: root.title,
    description: optionalString(root, 'description'),
    authorName: optionalString(root, 'author'),
    kind,
    category,
    keywords,
    credits: price.credits,
    realMoneyMinorUnits,
    realMoneyCurrency,
    createdAtUtc,
    sceneVersion,
    itemCount: stats.items,
    fixtureCount: stats.fixtures,
    cueCount: stats.cues,
    requiredItems,
    allowExport,
    fixtureTypes,
    manifestVersion: version,
  });
  const broken = validateManifest(manifest);
  if (broken !== null) {
    refuse('Corrupt', `manifest field ${broken} is out of its rules`);
  }
  return manifest;
}

export function isValidThumbnail(png: Buffer): boolean {
  return png.length === 0 || (png.length <= MaxThumbnailBytes && png.length > PNG_SIGNATURE.length && png.subarray(0, 8).equals(PNG_SIGNATURE));
}

/** One .gdtf file of the FIXT section. */
export interface FixtureFile {
  /** Canonical GUID text of the fixture type. */
  id: string;
  size: number;
  /** The file's bytes, a view into the inflated section. */
  data: Buffer;
}

function guidFromWords(a: number, b: number, c: number, d: number): string {
  const hex = [a, b, c, d].map((word) => word.toString(16).toUpperCase().padStart(8, '0')).join('');
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

/**
 * The raw FIXT payload (ADR 0033 §9): u32 Count (1..64), then Count entries of a 16-byte FGuid (A, B, C, D as u32 LE), a u32 Size
 * and Size bytes of .gdtf, all little-endian, and nothing after the last entry. Sizes are checked as they are declared, before the
 * bytes they claim are looked at, so a declared total over the cap is TooLarge even when the payload is cut short. Ids must be
 * non-zero and strictly ascending as (A, B, C, D) unsigned. Only the ZIP magic is checked here: the client opens each file with
 * its own GDTF validation (ADR 0033 §8) when it imports it.
 */
export function parseFixtureSection(raw: Buffer): FixtureFile[] {
  if (raw.length < 4) {
    refuse('Corrupt', 'fixture section is shorter than its count');
  }
  const count = raw.readUInt32LE(0);
  if (count < 1 || count > MaxFixtureFiles) {
    refuse('Corrupt', `fixture file count ${count}`);
  }
  const files: FixtureFile[] = [];
  let offset = 4;
  let total = 0;
  for (let index = 0; index < count; index++) {
    if (raw.length - offset < 20) {
      refuse('Corrupt', 'fixture entry header runs past the end');
    }
    const id = guidFromWords(raw.readUInt32LE(offset), raw.readUInt32LE(offset + 4), raw.readUInt32LE(offset + 8), raw.readUInt32LE(offset + 12));
    const size = raw.readUInt32LE(offset + 16);
    offset += 20;
    // Same order as StagePackageFormat::DecodeFixtureFiles, so a package with several faults gets the client's answer.
    if (size > MaxFixtureFileBytes) {
      refuse('TooLarge', 'fixture file is over 16 MiB');
    }
    total += size;
    if (total > MaxFixtureTotalBytes) {
      refuse('TooLarge', 'fixture files are over 32 MiB in total');
    }
    if (size < MinFixtureFileBytes) {
      refuse('Corrupt', 'fixture file is too small to be a ZIP archive');
    }
    if (raw.length - offset < size) {
      refuse('Corrupt', 'fixture file runs past the end');
    }
    const data = raw.subarray(offset, offset + size);
    if (!isValidGuid(id)) {
      refuse('Corrupt', 'fixture type id is zero');
    }
    if (!data.subarray(0, 4).equals(ZIP_LOCAL_HEADER)) {
      refuse('Corrupt', 'fixture file is not a ZIP archive');
    }
    if (index > 0 && files[index - 1]!.id >= id) {
      refuse('Corrupt', 'fixture type ids are not strictly ascending');
    }
    files.push({ id, size, data });
    offset += size;
  }
  if (offset !== raw.length) {
    refuse('Corrupt', 'bytes after the last fixture file');
  }
  return files;
}

export interface ReadPackage {
  manifest: StageManifest;
  scene: SceneSummary;
  /** PNG bytes, or empty. */
  thumbnail: Buffer;
  /** The .gdtf files of the FIXT section, in manifest.fixtureTypes order; empty when the package includes none. */
  fixtures: FixtureFile[];
}

function sameList(left: string[], right: string[]): boolean {
  return left.length === right.length && left.every((value, index) => value === right[index]);
}

function findSection(sections: SectionView[], tag: number): SectionView | undefined {
  return sections.find((section) => section.tag === tag);
}

/** StagePackage::Read: the whole package verified; RequiredItems recomputed from the scene. Throws PackageError. */
export function readPackage(bytes: Buffer): ReadPackage {
  const sections = readDirectory(bytes);
  const manifestSection = findSection(sections, ManifestTag);
  const sceneSection = findSection(sections, SceneTag);
  if (!manifestSection || !sceneSection) {
    refuse('Corrupt', 'manifest or scene section missing');
  }
  const manifest = parseManifest(decodeUtf8(readSection(bytes, manifestSection)));
  const scene = checkScene(decodeUtf8(readSection(bytes, sceneSection)));
  // The writer never produces a scene the parser has to repair, and the counts must match what a browser was shown.
  if (scene.repairs > 0 || manifest.itemCount !== scene.itemCount || manifest.cueCount !== scene.cueCount) {
    refuse('Corrupt', 'scene does not match its manifest or needed repairs');
  }
  if (!scene.portable) {
    refuse('NotPortable', 'scene contains imported models');
  }
  // Library fixtures travel with their .gdtf files: a type the scene uses but the package lacks cannot be shown to a buyer, and a
  // type the package carries but no item uses is not what the writer produces. Decided before the files are inflated.
  const included = new Set(manifest.fixtureTypes);
  if (scene.fixtureTypes.some((id) => !included.has(id))) {
    refuse('NotPortable', 'scene uses device-library fixtures whose files the package does not include');
  }
  if (!sameList(scene.fixtureTypes, manifest.fixtureTypes)) {
    refuse('Corrupt', 'package includes fixture files no item uses');
  }
  manifest.requiredItems = scene.requiredItems;

  let fixtures: FixtureFile[] = [];
  const fixtureSection = findSection(sections, FixtureTag);
  if ((fixtureSection !== undefined) !== (manifest.fixtureTypes.length > 0)) {
    refuse('Corrupt', 'fixture section and manifest fixtureTypes disagree');
  }
  if (fixtureSection) {
    fixtures = parseFixtureSection(readSection(bytes, fixtureSection));
    if (!sameList(fixtures.map((file) => file.id), manifest.fixtureTypes)) {
      refuse('Corrupt', 'fixture section ids differ from manifest fixtureTypes');
    }
  }

  let thumbnail = Buffer.alloc(0);
  const thumbnailSection = findSection(sections, ThumbnailTag);
  if (thumbnailSection) {
    thumbnail = readSection(bytes, thumbnailSection);
    if (!isValidThumbnail(thumbnail)) {
      refuse('Corrupt', 'thumbnail is not a PNG');
    }
  }
  return { manifest, scene, thumbnail, fixtures };
}
