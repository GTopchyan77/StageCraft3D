// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Marketplace/StageMarketplaceTypes.h"
#include "Scene/StageSceneStore.h"
#include "Scene/StageSceneTypes.h"

/** What a package says about itself (Docs/ADR/0021-community-marketplace.md §2.2). */
struct FStagePackageManifest
{
	FGuid PackageId;
	FString Title;
	FString Description;
	FString AuthorName;
	EStagePackageKind Kind = EStagePackageKind::StageDesign;
	FGameplayTag Category;
	TArray<FString> Keywords;
	FStageMarketplacePrice Price;
	FDateTime CreatedAtUtc;
	/** The scene format version of the SCEN section, for diagnostics. */
	int32 SceneVersion = FStageSceneStore::FormatVersion;
	int32 ItemCount = 0;
	int32 FixtureCount = 0;
	int32 CueCount = 0;
	/** Distinct catalog paths the scene uses, sorted. Recomputed from the scene on a full read, never trusted. */
	TArray<FString> RequiredItems;
	/**
	 * The publisher's choice: buyers may send this stage to 3D apps and save it as a file (Docs/ADR/0022-dcc-bridge.md §11.1).
	 * Manifest version 2; a version 1 package reads as false. Never permits republishing.
	 */
	bool bAllowExport = false;
};

/** A complete package in memory: manifest, scene, optional thumbnail. Plain value type, safe on any thread. */
struct FStagePackage
{
	FStagePackageManifest Manifest;
	FStageSceneDocument Scene;
	/** PNG bytes, or empty. */
	TArray<uint8> ThumbnailPng;
};

/**
 * Reads, writes and validates `.stage` packages (Docs/ADR/0021-community-marketplace.md §2): a small binary container of named,
 * zlib-compressed, SHA-1-hashed sections (manifest JSON, scene JSON, optional PNG thumbnail).
 *
 * A package is untrusted input. The readers check the magic, versions, section count, sizes against caps before allocating,
 * bounds, codecs, exact decompressed sizes and hashes, and refuse the whole package on any failure: a package is never half-read.
 * The hashes detect damage, not tampering; authenticity belongs to a server backend (Rules.md §66).
 *
 * Pure logic: no world, no UObjects, no file I/O. Safe on any thread.
 */
namespace StagePackage
{
	inline constexpr TCHAR FileExtension[] = TEXT(".stage");

	inline constexpr uint32 ContainerVersion = 1;
	/** 2 added allowExport (ADR 0022 §11.1). Older manifests are still read. */
	inline constexpr int32 ManifestVersion = 2;
	inline constexpr int32 FirstManifestVersionWithExportFlag = 2;
	inline constexpr int32 MaxSections = 8;
	inline constexpr int64 MaxManifestBytes = 256 * 1024;
	inline constexpr int64 MaxSceneBytes = 64ll * 1024 * 1024;
	inline constexpr int64 MaxThumbnailBytes = 2 * 1024 * 1024;
	inline constexpr int64 MaxPackageBytes = 96ll * 1024 * 1024;

	inline constexpr int32 MaxTitleLength = 80;
	inline constexpr int32 MaxDescriptionLength = 2000;
	inline constexpr int32 MaxAuthorLength = 64;
	inline constexpr int32 MaxKeywords = 16;
	inline constexpr int32 MaxKeywordLength = 32;
	inline constexpr int64 MaxCreditsPrice = 1000000;
	/** A real-money ceiling well below int64 overflow in any per-currency sum. */
	inline constexpr int64 MaxRealMoneyMinorUnits = 100000000;

	/**
	 * Trims and cleans the free-text fields (control characters removed, newlines kept in the description), drops empty and
	 * case-insensitively repeated keywords, gives an empty author "Anonymous" and maps an unknown category to Other. Does not
	 * shorten anything: an over-long field is refused by ValidateManifest, never silently cut.
	 */
	void NormalizeManifest(FStagePackageManifest& Manifest);

	/** Success, or InvalidRequest when a field is out of its rules (§2.2). Call after NormalizeManifest. */
	EStageMarketplaceResult ValidateManifest(const FStagePackageManifest& Manifest);

	/** False when the scene contains items made from imported models (they exist only where they were imported). */
	bool IsPortable(const FStageSceneDocument& Scene);

	/** Distinct catalog paths of the scene's items, sorted. */
	TArray<FString> CollectRequiredItems(const FStageSceneDocument& Scene);

	/** True for a PNG of at most MaxThumbnailBytes (empty is valid: no thumbnail). */
	bool IsValidThumbnail(TConstArrayView<uint8> Png);

	/**
	 * Writes Package to OutBytes. The manifest's counts and required items must describe the scene (ItemCount, CueCount,
	 * RequiredItems), the manifest must validate and the thumbnail must be valid; otherwise InvalidRequest. TooLarge when a
	 * section or the whole package is over its cap.
	 */
	EStageMarketplaceResult Write(const FStagePackage& Package, TArray<uint8>& OutBytes);

	/** Reads only the manifest (for browsing): the scene is skipped without being inflated. */
	EStageMarketplaceResult ReadManifest(TConstArrayView<uint8> Bytes, FStagePackageManifest& OutManifest, bool& bOutHasThumbnail);

	/**
	 * Reads and verifies the whole package. The scene must parse with no dropped items and must match the manifest's counts;
	 * RequiredItems is recomputed from the scene.
	 */
	EStageMarketplaceResult Read(TConstArrayView<uint8> Bytes, FStagePackage& OutPackage);

	/** A browser's view of a manifest. */
	FStageMarketplaceListing ToListing(const FStagePackageManifest& Manifest, bool bHasThumbnail, int64 PackageBytes, int64 Downloads);

	/** Exposed for tests and diagnostics. */
	FString SerializeManifest(const FStagePackageManifest& Manifest);
	EStageMarketplaceResult ParseManifest(const FString& Json, FStagePackageManifest& OutManifest);
}
