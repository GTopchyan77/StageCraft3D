// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/BaseItemData.h"
#include "Data/StageSpanData.h"
#include "Mvr/StageMvrTypes.h"
#include "Show/ShowControlTypes.h"

/**
 * Turns a parsed MVR scene into what to place on the stage (Docs/ADR/0034 §6, MV2): pure maths and bookkeeping, no world and no
 * spawning, so every rule is unit-tested. The caller resolves catalog items (FStageMvrModeTarget) on the game thread and places the
 * plan through the spawn system (UStageMvrImportSubsystem).
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
 *  - Items whose MVR UUID is already on the stage are skipped (no duplicates); re-import that updates them is MV5.
 *  - Scene objects, supports, video screens and projectors are counted, not placed (v1).
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

		/** Fixture IDs already on the stage, and MVR UUIDs of items already on the stage. */
		TSet<int32> UsedFixtureIds;
		TSet<FGuid> ExistingMvrIds;
	};

	struct FFixturePlacement
	{
		FGuid MvrId;
		TWeakObjectPtr<UBaseItemData> Item;
		FTransform Transform = FTransform::Identity;
		FString Label;
		int32 FixtureId = 0;
		FStageDMXPatch Patch;
		/** Set when the fixture aims at a focus point. */
		TOptional<FVector2D> PanTilt;
		int32 Layer = INDEX_NONE;
		bool bFallback = false;
	};

	struct FTrussPlacement
	{
		FGuid MvrId;
		FTransform Transform = FTransform::Identity;
		/** Actor-local points (cm), the first at the origin. */
		TArray<FVector> LocalPoints;
		int32 Sections = 1;
		FString Label;
		int32 Layer = INDEX_NONE;
	};

	struct FPlan
	{
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
		int32 SkippedExisting = 0;
		int32 SkippedFixtures = 0;
		int32 SkippedTrusses = 0;
		/** Scene objects, supports, video screens and projectors: not placed in v1. */
		int32 NotPlacedObjects = 0;

		int32 Num() const { return Fixtures.Num() + Trusses.Num(); }
	};

	constexpr int32 MaxNotes = 40;

	/** The plan for Scene. Pure; game thread or worker. */
	FPlan Build(const FStageMvrScene& Scene, const FInputs& Inputs);

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
