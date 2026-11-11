// The parts of FStageSceneStore::Parse (Source/ModularSceneBuilder/Private/Scene/StageSceneStore.cpp) that decide whether a
// package is accepted: format and version, the item cap, every item rule of ParseItem (including the v12 device-library fixture
// paths and their "fixtureType" reference), duplicate item ids, the origin listing id, and the cue list's numbers, cap and
// duplicates. A package needing any repair is refused (StagePackage::Read), so this check
// reports repairs instead of making them.
//
// Not ported (validated by the client's StagePackage::Read when it acquires the package): cue fixture states, cue timing and
// effects, the programmer, groups, palettes, playbacks and the environment, i.e. the FStageSceneParseReport counters
// InvalidShowEntries and the per-fixture part of InvalidCueEntries.
import { parseGuid, isValidGuid, PackageError } from './stagePackage.ts';
import { isStrictBase64 } from '../base64.ts';

export const SceneFormatTag = 'StageCraftScene';
export const SceneFormatVersion = 12;
export const MinReadableSceneVersion = 1;
export const MaxSceneItemsInFile = 10_000;
export const MaxSceneCuesInFile = 2_000;
export const MaxStateBytes = 64 * 1024;
export const MaxCoordinate = 1.0e6;
const KINDA_SMALL_NUMBER = 1.0e-4;
const MIN_CUE_THOUSANDTHS = 1;
const MAX_CUE_THOUSANDTHS = 9_999_999;
export const ImportedItemRoot = '/Temp/StageCraft/Imported/';
/** StageFixtureLibrary::PackageRoot: device-library fixture modes live at <root><32 hex digits>/<slug> (ADR 0033 §4.2). */
export const FixtureItemRoot = '/Temp/StageCraftFixtures/';
/** StageFixtureLibrary::MaxSlugLength / MaxRefTextLength. */
export const MaxFixtureSlugLength = 64;
export const MaxFixtureRefTextLength = 256;

export interface SceneSummary {
  version: number;
  itemCount: number;
  cueCount: number;
  /** Items or cues the C++ parser would have dropped, re-identified or cut short. */
  repairs: number;
  /** False when an item comes from an imported model. Library fixtures are portable only with their files (fixtureTypes). */
  portable: boolean;
  /** The device-library fixture types the items use, canonical GUID text, ascending as (A, B, C, D) unsigned. */
  fixtureTypes: string[];
  /** Distinct item paths, case-insensitively, sorted like TArray<FString>::Sort. */
  requiredItems: string[];
  originListingId: string | null;
}

function corrupt(message: string): never {
  throw new PackageError('Corrupt', message);
}

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

/** Exactly count finite numbers (ReadNumbers). */
function readNumbers(value: unknown, count: number): number[] | null {
  if (!Array.isArray(value) || value.length !== count || !value.every((entry) => typeof entry === 'number' && Number.isFinite(entry))) {
    return null;
  }
  return value as number[];
}

// UE package and asset names never contain these (INVALID_LONGPACKAGE_CHARACTERS / INVALID_OBJECTNAME_CHARACTERS).
const NAME = `[^\\s"',.:&!~@#/\\\\]+`;
const ITEM_PATH = new RegExp(`^/(?:${NAME}/)*${NAME}(?:\\.${NAME}(?::[^\\s"',&!~@#\\\\]+)?)?$`);

/** The long package name of a soft object path ("/Game/A/B.B" -> "/Game/A/B"), or null when it is not a valid path. */
export function itemPackageName(path: string): string | null {
  if (!ITEM_PATH.test(path)) {
    return null;
  }
  const dot = path.indexOf('.');
  return dot < 0 ? path : path.slice(0, dot);
}

const FIXTURE_ITEM_PACKAGE = new RegExp(`^${FixtureItemRoot}([0-9A-Fa-f]{32})/[A-Za-z0-9_]{1,${MaxFixtureSlugLength}}$`);

/**
 * StageFixtureLibrary::IsFixtureItemPath + ParseFixtureItemPath on a long package name: the type id (canonical GUID text, which
 * for a 32-digit FGuid::Digits form keeps A, B, C, D in order) of a library fixture mode, or null for any other package. The
 * root is case-sensitive like the C++; the hex digits are not. A zero id is returned as is: no package can include that type.
 */
export function fixtureTypeOfPackage(packageName: string): string | null {
  const match = FIXTURE_ITEM_PACKAGE.exec(packageName);
  return match ? parseGuid(match[1]!) : null;
}

/**
 * The v12 "fixtureType" reference (ADR 0033 §7.1). The C++ drops an unreadable reference, clips long text and ignores one on a
 * non-library item, all silently; the writer produces none of these (MakeTypeRef clips before writing and only library modes get
 * one), so here each counts as a repair. The id must also name the type the path belongs to: the C++ does not compare them, but a
 * reference to another type would name the wrong GDTF file on a PC that misses it.
 */
function isValidFixtureTypeRef(ref: unknown, pathTypeId: string | null): boolean {
  if (ref === undefined) {
    return true;
  }
  if (pathTypeId === null || !isObject(ref) || typeof ref.id !== 'string' || parseGuid(ref.id) !== pathTypeId || !isValidGuid(pathTypeId)) {
    return false;
  }
  return [ref.manufacturer, ref.name, ref.mode].every(
    (text) => text === undefined || (typeof text === 'string' && text.length <= MaxFixtureRefTextLength),
  );
}

/** ParseItem: true when the item would load unchanged. */
export function isValidItem(item: unknown): item is { id: string; item: string } {
  if (!isObject(item) || typeof item.id !== 'string' || !isValidGuid(parseGuid(item.id)) || typeof item.item !== 'string') {
    return false;
  }
  const packageName = itemPackageName(item.item);
  if (packageName === null) {
    return false;
  }
  const fixtureType = fixtureTypeOfPackage(packageName);
  if (!(packageName.startsWith('/Game/') || packageName.startsWith(ImportedItemRoot) || fixtureType !== null)) {
    return false;
  }
  if (!isValidFixtureTypeRef(item.fixtureType, fixtureType)) {
    return false;
  }
  const location = readNumbers(item.location, 3);
  const rotation = readNumbers(item.rotation, 4);
  const scale = readNumbers(item.scale, 3);
  if (!location || !rotation || !scale) {
    return false;
  }
  if (Math.max(...location.map(Math.abs)) > MaxCoordinate) {
    return false;
  }
  const sizeSquared = rotation.reduce((sum, value) => sum + value * value, 0);
  if (sizeSquared < KINDA_SMALL_NUMBER) {
    return false;
  }
  if (item.label !== undefined && typeof item.label !== 'string') {
    return false;
  }
  if (item.state !== undefined) {
    if (typeof item.state !== 'string') {
      return false;
    }
    if (item.state.length > 0 && (item.state.length > (Math.floor(MaxStateBytes / 3) + 1) * 4 || !isStrictBase64(item.state))) {
      return false;
    }
  }
  return true;
}

/** StageCueRules::IsValidCueNumber on the float the C++ reads, as thousandths; null when invalid. */
export function cueThousandths(value: unknown): number | null {
  if (typeof value !== 'number') {
    return null;
  }
  const asFloat = Math.fround(value);
  if (!Number.isFinite(asFloat) || Math.abs(asFloat) > 1.0e7) {
    return null;
  }
  const thousandths = Math.floor(asFloat * 1000 + 0.5);
  return thousandths >= MIN_CUE_THOUSANDTHS && thousandths <= MAX_CUE_THOUSANDTHS ? thousandths : null;
}

function compareIgnoreCase(a: string, b: string): number {
  const left = a.toLowerCase();
  const right = b.toLowerCase();
  return left < right ? -1 : left > right ? 1 : 0;
}

export function checkScene(json: string): SceneSummary {
  let root: unknown;
  try {
    root = JSON.parse(json);
  } catch {
    corrupt('scene is not JSON');
  }
  if (!isObject(root) || root.format !== SceneFormatTag || typeof root.version !== 'number') {
    corrupt('scene format or version');
  }
  const version = root.version;
  if (!Number.isInteger(version) || version < MinReadableSceneVersion || version > SceneFormatVersion) {
    throw new PackageError('VersionMismatch', `scene version ${version}`);
  }
  const items = root.items;
  if (!Array.isArray(items) || items.length > MaxSceneItemsInFile) {
    corrupt('scene items');
  }

  let repairs = 0;
  let cueCount = 0;
  if (version >= 3 && root.cues !== undefined) {
    if (Array.isArray(root.cues)) {
      const seen = new Set<number>();
      for (const cue of root.cues) {
        const number = isObject(cue) ? cueThousandths(cue.number) : null;
        if (number === null || cueCount >= MaxSceneCuesInFile || seen.has(number)) {
          repairs++;
          continue;
        }
        seen.add(number);
        cueCount++;
      }
    }
  }

  let originListingId: string | null = null;
  if (version >= 4 && typeof root.originListingId === 'string') {
    originListingId = parseGuid(root.originListingId);
    if (!isValidGuid(originListingId)) {
      corrupt('originListingId');
    }
  }

  const seenIds = new Set<string>();
  const paths = new Map<string, string>();
  let itemCount = 0;
  let portable = true;
  const fixtureTypes = new Set<string>();
  for (const item of items) {
    if (!isValidItem(item)) {
      repairs++;
      continue;
    }
    const id = parseGuid(item.id)!;
    if (seenIds.has(id)) {
      repairs++;
    }
    seenIds.add(id);
    itemCount++;
    const packageName = itemPackageName(item.item)!;
    if (packageName.startsWith(ImportedItemRoot)) {
      portable = false;
    }
    const fixtureType = fixtureTypeOfPackage(packageName);
    if (fixtureType !== null) {
      fixtureTypes.add(fixtureType);
    }
    const key = item.item.toLowerCase();
    if (!paths.has(key)) {
      paths.set(key, item.item);
    }
  }
  const requiredItems = [...paths.values()].sort(compareIgnoreCase);
  // Canonical GUID text is fixed-width upper-case hex in A, B, C, D order, so code-unit order is the unsigned (A, B, C, D) order.
  const sortedFixtureTypes = [...fixtureTypes].sort();
  return { version, itemCount, cueCount, repairs, portable, fixtureTypes: sortedFixtureTypes, requiredItems, originListingId };
}
