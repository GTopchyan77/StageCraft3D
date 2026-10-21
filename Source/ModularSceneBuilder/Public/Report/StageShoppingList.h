// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Where the distros or the DMX control node stand (Docs/ADR/0014-shopping-and-assembly-list.md §2.2). */
enum class EStageCableAnchor : uint8
{
	/** Under the equipment it serves: the centre of that equipment's floor positions, on the floor (Z = 0). */
	Automatic,
	/** A fixed position in the level, in metres. */
	Fixed
};

/**
 * How cable runs are measured and stocked, and how hardware scales with the rig (Docs/ADR/0014-shopping-and-assembly-list.md).
 * Configured in Project Settings › StageCraft Cabling & Rigging; StageShoppingList::Sanitize clamps every value into its
 * documented range, so planning never sees a bad value.
 */
struct FStageCablingRules
{
	/** Added to every measured route before it is rounded up to stock, 0..1 (0.1 = +10 %). */
	double SafetyMargin = 0.1;
	/** Stock cable lengths in metres, ascending and unique, 0.5..500 each; never empty. */
	TArray<double> StockLengthsMeters = { 2.0, 5.0, 10.0, 15.0, 20.0 };
	/** Printed names of the cable types, e.g. "PowerCON TRUE1 15 m". Never empty. */
	FString PowerCableName = TEXT("PowerCON TRUE1");
	FString DmxCableName = TEXT("XLR 5-pin DMX");

	EStageCableAnchor DistroAnchor = EStageCableAnchor::Automatic;
	/** Used when DistroAnchor is Fixed; every component within ±10 km. */
	FVector DistroLocationMeters = FVector::ZeroVector;
	EStageCableAnchor ControlAnchor = EStageCableAnchor::Automatic;
	/** Used when ControlAnchor is Fixed; every component within ±10 km. */
	FVector ControlLocationMeters = FVector::ZeroVector;

	/** Circuit outputs on one power distro, 1..96. */
	int32 DistroOutputs = 12;
	/** Current one distro may take on each phase of its feed, 1..1000 A. */
	double DistroFeedAmps = 63.0;

	/** A fixture whose position is at least this high (world Z, metres) is rigged, 0..100. */
	double RiggedAboveMeters = 2.0;
	/** Fixture weight one clamp may carry; heavier fixtures get more clamps, 1..1000 kg. */
	double WeightPerClampKg = 20.0;
	/** Working load limits of the stocked safety bonds, ascending and unique, 1..10000 kg each; never empty. */
	TArray<double> SafetyBondRatingsKg = { 10.0, 25.0, 50.0, 100.0 };
};

/** One lighting fixture as rigging sees it, as plain values read from the stage by the caller. */
struct FStageRiggedFixture
{
	int32 FixtureId = 0;
	FString Label;
	/** Catalog weight of the whole placement; 0 or less when the catalog gives none. */
	double WeightKg = 0.0;
	/** World position of the fixture's origin, metres. */
	FVector LocationMeters = FVector::ZeroVector;
	/** Z of the fixture's up vector: 1 standing upright, -1 hanging upside down, 0 on its side. */
	double UpZ = 1.0;
};

/** The stock cables one run is made of: Count x StockMeters, plus one TailMeters cable when the run needs more. */
struct FStageCableAssembly
{
	int32 Count = 0;
	double StockMeters = 0.0;
	/** A shorter stock cable joined to the end; 0 when there is none. */
	double TailMeters = 0.0;

	int32 GetCableCount() const { return Count + (TailMeters > 0.0 ? 1 : 0); }
	double GetTotalMeters() const { return Count * StockMeters + TailMeters; }
	/** More than one cable joined in line. */
	bool IsJoined() const { return GetCableCount() > 1; }
};

enum class EStageShoppingSection : uint8
{
	PowerCables,
	DmxCables,
	Power,
	Dmx,
	Rigging
};

/** One line to pick or buy: "PowerCON TRUE1 15 m - 8x". */
struct FStageShoppingLine
{
	EStageShoppingSection Section = EStageShoppingSection::PowerCables;
	FString Item;
	/** Length, rating or configuration; may be empty. */
	FString Size;
	int32 Quantity = 0;
	FString Note;
	/** Printed bold: something a person must decide before the list is complete. */
	bool bNeedsAttention = false;
};

/** Everything to bring for the cable schedule's runs and the rig's hardware, grouped and counted. */
struct FStageShoppingList
{
	/** In section order; within a section by size (cables shortest first, bonds lightest first). */
	TArray<FStageShoppingLine> Lines;

	int32 PowerCableCount = 0;
	int32 DmxCableCount = 0;
	double PowerCableMeters = 0.0;
	double DmxCableMeters = 0.0;
	/** Runs longer than the longest stock cable, made of joined cables. */
	int32 JoinedRuns = 0;

	int32 RiggedFixtures = 0;
	double RiggedWeightKg = 0.0;
	/** Rigged fixtures heavier than the strongest stocked safety bond: a rigger must specify their secondary suspension. */
	int32 OverweightFixtures = 0;
	/** Rigged fixtures whose catalog entry gives no weight: one clamp and the lightest bond are listed for each. */
	int32 FixturesWithoutWeight = 0;

	bool IsEmpty() const { return Lines.IsEmpty(); }
};

/**
 * Pure cable measuring and shopping-list counting (Docs/ADR/0014-shopping-and-assembly-list.md). Deterministic; covered by
 * the StageCraft.Report.ShoppingList tests. Any thread.
 */
namespace StageShoppingList
{
	/** Every value clamped into its range; non-finite values and empty lists or names take the default. */
	MODULARSCENEBUILDER_API FStageCablingRules Sanitize(const FStageCablingRules& Rules);

	/**
	 * The length a cable follows between two points: the horizontal distance plus the height difference, because cables run
	 * along the floor or the truss and then up or down, not through the air. Non-finite components count as 0.
	 */
	MODULARSCENEBUILDER_API double GetRouteMeters(const FVector& FromMeters, const FVector& ToMeters);

	/**
	 * The stock cables for a route of RouteMeters (Rules sanitized first): the route plus the safety margin, then the shortest stock
	 * length that covers it; a longer run takes as many of the longest stock as fit, plus the shortest stock that covers the rest.
	 * A route of 0 or less, or a non-finite one, takes the shortest stock cable.
	 */
	MODULARSCENEBUILDER_API FStageCableAssembly AssembleRun(double RouteMeters, const FStageCablingRules& Rules);

	/** "15 m", "2 x 20 m + 5 m". */
	MODULARSCENEBUILDER_API FString FormatAssembly(const FStageCableAssembly& Assembly);

	/** Whether a fixture is rigged (needs clamps and a safety bond): mounted high, or not standing upright (tilted over 30°). */
	MODULARSCENEBUILDER_API bool IsRigged(const FStageRiggedFixture& Fixture, const FStageCablingRules& Rules);

	/** Clamps one rigged fixture needs: its weight / WeightPerClampKg rounded up, at least 1 (also without weight data). */
	MODULARSCENEBUILDER_API int32 GetClampCount(double WeightKg, const FStageCablingRules& Rules);

	/** The lightest stocked safety bond rated for WeightKg (the lightest for no weight data); -1 when none is strong enough. */
	MODULARSCENEBUILDER_API double PickSafetyBond(double WeightKg, const FStageCablingRules& Rules);

	/** "Power cables", "Rigging", ... */
	MODULARSCENEBUILDER_API FString GetSectionName(EStageShoppingSection Section);
}
