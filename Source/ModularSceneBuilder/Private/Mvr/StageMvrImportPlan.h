// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/BaseItemData.h"
#include "Data/StageSpanData.h"
#include "Mvr/StageMvrImportTypes.h"
#include "Mvr/StageMvrTypes.h"
#include "Show/ShowControlTypes.h"

/**
 * Turns a parsed MVR scene into what to place on the stage (Docs/ADR/0034 §6, MV2) and, for a plot that is on the stage already,
 * what to update and remove (§18, MV5): pure maths and bookkeeping, no world and no spawning, so every rule is unit-tested. The
 * caller resolves catalog items (FStageMvrModeTarget) on the game thread and places the plan through the spawn system
 * (UStageMvrImportSubsystem).
 *
 * Rules:
 *  - A fixture whose GDTFSpec and GDTFMode resolve uses that mode's catalog item; otherwise the fallback fixture, named in the report.
 *  - MVR fixtures hang by default (GDTF: beam along -Z). A StageCraft fixture at rest shines along its local +Z, so every fixture
 *    gets a half turn about X under its MVR transform. Scale is dropped.
 *  - Fixture ID: FixtureIDNumeric, else a whole-number FixtureID, when free (not used on the stage nor earlier in the file); else the
 *    next free ID, reported. Patch: the first address with break 0 (others are reported by the parser); no address = unpatched.
 *  - Focus: pan and tilt that aim the beam at the FocusPoint (SolvePanTilt), clamped to the mode's ranges and reported when clamped.
 *  - Truss: one straight run along the longest horizontal side of its geometry's bounds, at the bounds' bottom, in whole sections.
 *    Without readable geometry it is skipped and reported.
 *  - Scene objects, supports, video screens and projectors are counted, not placed (v1).
 *
 * Re-import (MV5), for items whose MVR UUID is on the stage (FInputs::Existing):
 *  - Each field (placement, name, patch, type, aim) is compared with what the file said at the last import (FStageMvrItemOrigin's
 *    fingerprints): only fields the FILE changed are written, so what the user changed in StageCraft stays. Items imported before
 *    MV5 have no fingerprints; their placement, name and patch are compared with the stage instead, type and aim are left alone,
 *    and they are updated once to record their fingerprints. A UUID that changed kind (truss <-> fixture) is a new item.
 *  - A locked item keeps its placement and its aim (a lock forbids moving and resizing); the file's other changes still apply.
 *  - An updated fixture keeps its fixture ID (cues, groups and palettes find it by that ID), its instance id and everything else it
 *    carries (home look, hidden, locked). It is re-aimed only when its aim changed (its focus point or its own placement moved).
 *  - Items of the same plot (same MVR layer; the file name only for items recorded without one) whose UUID the file no longer has are removed or kept (Missing). A
 *    removed fixture's ID is free for the file's new fixtures.
 *  - Nothing changed and nothing new = an empty plan.
 */
namespace StageMvrImport
{
	/** A catalog fixture an MVR fixture can become: resolved on the game thread, read here as plain values. */
	struct FStageMvrModeTarget
	{
		TWeakObjectPtr<UBaseItemData> Item;
		FString Label;
		bool bHasPanTilt = false;
		FVector2D PanRange = FVector2D(-270.0, 270.0);
		FVector2D TiltRange = FVector2D(-135.0, 135.0);
		/** Pan pivot plus tilt pivot offset in the fixture's local space (cm): where the beam turns. */
		FVector PivotOffset = FVector::ZeroVector;
	};

	struct FInputs
	{
		/** Lower-case GDTFSpec file name -> lower-case GDTFMode name -> target. */
		TMap<FString, TMap<FString, FStageMvrModeTarget>> Modes;
		/** For fixtures without a usable type; Item null = such fixtures are skipped. */
		FStageMvrModeTarget Fallback;

		/** Builds MVR trusses as runs of whole sections (its own rules); null = trusses are skipped. */
		TWeakObjectPtr<UStageSpanData> TrussItem;

		/** Lower-case geometry file name -> bounds in that file's own space (MVR mm, Z up). */
		TMap<FString, FBox> GeometryBounds;

		/** Fixture IDs already on the stage. */
		TSet<int32> UsedFixtureIds;

		/** Items on the stage that came from MVR (re-import). */
		TArray<FStageMvrExistingItem> Existing;
		/** The .mvr file's name (no folder): recorded on placed items, and with the layers what "the same plot" means. */
		FString SourceFile;
		/** Items of the same plot the file no longer has. Keep unless the user chose otherwise: removing is never a default here. */
		EStageMvrMissingItems Missing = EStageMvrMissingItems::Keep;
	};

	/** Which fields of an item on the stage a re-import changes. */
	enum class EChange : uint8
	{
		None = 0,
		Placement = 1 << 0,
		Name = 1 << 1,
		Patch = 1 << 2,
		Type = 1 << 3,
		Aim = 1 << 4,
		/** Only the origin is written: an item imported before MV5 gets its fingerprints (once). */
		Origin = 1 << 5,
	};
	ENUM_CLASS_FLAGS(EChange);

	/** What the file says about one object, a hash per field (never 0, which means "not known"). See FStageMvrItemOrigin. */
	struct FFingerprint
	{
		uint32 Placement = 0;
		uint32 Name = 0;
		uint32 Patch = 0;
		uint32 Type = 0;
		uint32 Aim = 0;
	};

	struct FFixturePlacement
	{
		FGuid MvrId;
		TWeakObjectPtr<UBaseItemData> Item;
		FTransform Transform = FTransform::Identity;
		FString Label;
		int32 FixtureId = 0;
		FStageDMXPatch Patch;
		/** Set when the fixture aims at a focus point (a new fixture), or its aim changed (an update). Unset keeps an update's look. */
		TOptional<FVector2D> PanTilt;
		int32 Layer = INDEX_NONE;
		bool bFallback = false;
		/** What the item records about where it came from (layer, file, fingerprints, preserved XML). */
		FStageMvrItemOrigin Origin;
		/** An update: the instance id of the item it replaces (and keeps), and what changed. Invalid for a new fixture. */
		FGuid Replaces;
		EChange Changes = EChange::None;
	};

	struct FTrussPlacement
	{
		FGuid MvrId;
		/** The truss type: a new run's is the Sketch window's, an update keeps its own (unless the UUID held something else). */
		TWeakObjectPtr<UStageSpanData> Item;
		FTransform Transform = FTransform::Identity;
		/** Actor-local points (cm), the first at the origin. An update whose placement did not change keeps its path (bSetPath false). */
		TArray<FVector> LocalPoints;
		bool bSetPath = true;
		int32 Sections = 1;
		FString Label;
		int32 Layer = INDEX_NONE;
		FStageMvrItemOrigin Origin;
		FGuid Replaces;
		EChange Changes = EChange::None;
	};

	/** An item of the same plot the file no longer has. */
	struct FRemoval
	{
		FGuid InstanceId;
		FString Label;
	};

	struct FPlan
	{
		/** Applied first, so a removed fixture's ID and address are free for what follows. */
		TArray<FRemoval> Removals;
		TArray<FFixturePlacement> Fixtures;
		TArray<FTrussPlacement> Trusses;
		TWeakObjectPtr<UStageSpanData> TrussItem;
		/** Layer names, by FStageMvrObject::Layer. */
		TArray<FString> LayerNames;

		/** User-facing notes for the report, in file order; capped (MaxNotes, then a count). */
		TArray<FString> Notes;
		int32 FallbackFixtures = 0;
		int32 RenumberedFixtures = 0;
		int32 AimedFixtures = 0;
		int32 ClampedAims = 0;
		int32 SkippedFixtures = 0;
		int32 SkippedTrusses = 0;
		/** Scene objects, supports, video screens and projectors: not placed in v1. */
		int32 NotPlacedObjects = 0;

		/** Re-import: placed new, updated (Fixtures / Trusses with Replaces), matched with nothing to change, and items of the plot the file no longer has that were kept. */
		int32 Added = 0;
		int32 Updated = 0;
		int32 Unchanged = 0;
		int32 KeptMissing = 0;

		int32 Num() const { return Removals.Num() + Fixtures.Num() + Trusses.Num(); }
	};

	/** The re-import comparison alone, for the review (the confirm builds the plan, which applies the same rules). */
	struct FDiff
	{
		int32 New = 0;
		int32 Changed = 0;
		int32 Unchanged = 0;
		int32 Missing = 0;
	};

	constexpr int32 MaxNotes = 40;

	/** The plan for Scene. Pure; game thread or worker. */
	FPlan Build(const FStageMvrScene& Scene, const FInputs& Inputs);

	/**
	 * How Scene compares with Inputs.Existing: fixtures and trusses new to the stage, matched ones changed or not by the file, and
	 * items of the same plot it no longer has. Needs Existing, GeometryBounds, TrussItem and SourceFile; not the modes. Pure.
	 */
	FDiff Diff(const FStageMvrScene& Scene, const FInputs& Inputs);

	/** What the file says about Object (a fixture or truss), per field. GeometryBounds as in FInputs. Pure. */
	FFingerprint Fingerprint(const FStageMvrScene& Scene, const FStageMvrObject& Object, const TMap<FString, FBox>& GeometryBounds);

	/** The fixture's actor transform for an MVR world matrix (mm): converted, scale dropped, turned to hang. */
	FTransform FixtureTransform(const FMatrix& MvrWorld);

	/**
	 * Pan and tilt (degrees) that point a StageCraft fixture's beam along LocalDirection (in the fixture's actor space). The beam
	 * at rest leaves along +Z; tilt turns it about Y towards -X, pan turns that about Z. Of the two solutions and the pan's 360-degree
	 * repeats, the one inside the ranges with the smallest movement wins; when none is inside, the nearest is clamped and bClamped
	 * is set. A zero direction gives (0, 0).
	 */
	FVector2D SolvePanTilt(const FVector& LocalDirection, const FVector2D& PanRange, const FVector2D& TiltRange, bool& bClamped);

	/** The beam direction (actor space) for pan and tilt, as the fixture's pivots turn it. The inverse of SolvePanTilt. */
	FVector BeamDirection(double Pan, double Tilt);
}
