// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"

/**
 * The electrical supply a power plan is made for (Docs/ADR/0012-production-reports-and-fixture-benchmark.md §2.3). Configured in
 * Project Settings › StageCraft › Power Distribution; StageCableSchedule::Sanitize clamps every value into its documented range.
 */
struct FStagePowerDistribution
{
	/** Phase-to-neutral voltage, 100..400 V. */
	double PhaseVoltage = 230.0;
	/** 1 (single phase) or 3; anything else reads 3. */
	int32 PhaseCount = 3;
	/** Breaker rating of one circuit, 1..125 A. */
	double CircuitRatingAmps = 16.0;
	/** Share of the rating a circuit may carry continuously, 0.1..1. */
	double UsableShare = 0.8;
	/** Real power / apparent power, 0.5..1. Specs give watts, so 1 treats W as VA. */
	double PowerFactor = 1.0;
};

/** One placed item that draws power, as plain values read from the stage by the caller. */
struct FStagePowerLoad
{
	/** Instance label shown on stage; may be empty. */
	FString Label;
	/** Catalog display name. */
	FString ItemType;
	/** Lighting fixture ID; 0 for other equipment. */
	int32 FixtureId = 0;
	/** Maximum draw of the whole placement (unit power x quantity). A fixture with 0 W is reported as missing power data. */
	double Watts = 0.0;
};

/** One lighting fixture's DMX patch and position, for the DMX cable chains. */
struct FStageDmxNode
{
	int32 FixtureId = 0;
	FString Label;
	/** 0 = unpatched. */
	int32 Universe = 0;
	/** 1..512 when patched. */
	int32 Address = 0;
	int32 Footprint = 0;
	FVector LocationMeters = FVector::ZeroVector;
};

/** A load with its place in the plan. */
struct FStagePlannedLoad
{
	FStagePowerLoad Load;
	/** Index into FStageCableSchedule::Circuits. */
	int32 CircuitIndex = INDEX_NONE;
	double Amps = 0.0;
};

/** One distro circuit (breaker) and what it feeds. */
struct FStagePowerCircuit
{
	/** 1-based, in the order circuits were opened. */
	int32 Number = 0;
	/** 1-based phase (L1..L3). */
	int32 Phase = 1;
	/** Indices into FStageCableSchedule::Loads. */
	TArray<int32> Loads;
	double Watts = 0.0;
	double Amps = 0.0;
	/** Amps / rating, 0..1+ */
	double RatingShare = 0.0;
	/** Holds one load larger than a circuit's usable share: needs a larger feed. */
	bool bOverloaded = false;
};

struct FStagePowerPhase
{
	int32 Phase = 1;
	int32 CircuitCount = 0;
	double Watts = 0.0;
	double Amps = 0.0;
};

enum class EStageCableKind : uint8
{
	Power,
	Dmx
};

/** One line of the wiring list. */
struct FStageCableRun
{
	/** "P-001" for power home-runs, "D1-01" for DMX links (universe 1, link 1). */
	FString CableId;
	EStageCableKind Kind = EStageCableKind::Power;
	FString From;
	FString To;
	/** Power: index into Loads. DMX: INDEX_NONE. */
	int32 LoadIndex = INDEX_NONE;
	/** DMX universe; 0 for power. */
	int32 Universe = 0;
	/** DMX: the start address of the fixture the link goes to. */
	int32 ToAddress = 0;
	/** Straight-line distance between the two ends; negative when an end is not on the stage (distro, console). */
	double DistanceMeters = -1.0;
	/** The stock cable to take; 0 when it is decided on site, negative when the run is too long for one cable. */
	double StockLengthMeters = 0.0;
	/** The last link of a DMX universe: the fixture's DMX out needs a terminator. */
	bool bTerminate = false;
};

/** A power distribution plan and wiring list for a stage, generated deterministically from its equipment. */
struct FStageCableSchedule
{
	FString SceneName;
	FDateTime GeneratedAt;
	/** Sanitized. */
	FStagePowerDistribution Supply;

	/** In circuit order, largest load first within a circuit. */
	TArray<FStagePlannedLoad> Loads;
	TArray<FStagePowerCircuit> Circuits;
	/** One per phase of the supply, L1 first, including phases with no load. */
	TArray<FStagePowerPhase> Phases;
	TArray<FStageCableRun> PowerCables;
	TArray<FStageCableRun> DmxCables;

	double TotalWatts = 0.0;
	/** (largest - smallest phase load) / largest, in percent; 0 for a single phase or no load. */
	double ImbalancePercent = 0.0;
	int32 OverloadedCircuits = 0;
	int32 UnpatchedFixtures = 0;
	/** Lighting fixtures whose catalog entry gives no power draw: their load is missing from the plan. */
	int32 FixturesWithoutPowerData = 0;
	/** DMX links longer than the longest stock cable. */
	int32 OverlongDmxLinks = 0;
	/** Patched universes, ascending. */
	TArray<int32> Universes;

	bool IsEmpty() const { return Loads.IsEmpty() && DmxCables.IsEmpty(); }
};

/**
 * Pure planning and export of the cable schedule and power distribution (Docs/ADR/0012-production-reports-and-fixture-benchmark.md
 * §2.3). Deterministic for the same inputs in any order; covered by the StageCraft.Report.CableSchedule tests. Any thread.
 */
namespace StageCableSchedule
{
	/** Phase imbalance above this is reported as a warning. */
	inline constexpr double ImbalanceWarningPercent = 15.0;

	/** Slack added to a DMX link's straight-line distance for routing along truss and drops. */
	inline constexpr double DmxRoutingSlack = 1.2;

	/** Every value clamped into its range; non-finite values take the default. */
	MODULARSCENEBUILDER_API FStagePowerDistribution Sanitize(const FStagePowerDistribution& Supply);

	/** The real power one circuit may carry continuously: voltage x rating x usable share x power factor (Supply sanitized first). */
	MODULARSCENEBUILDER_API double GetUsableWattsPerCircuit(const FStagePowerDistribution& Supply);

	/** The shortest stock DMX cable (1, 2, 3, 5, 10, 15, 20, 25, 30, 50 m) at least Meters long; -1 when none is, 1 for 0 or less. */
	MODULARSCENEBUILDER_API double GetStockLength(double Meters);

	/**
	 * Plans the circuits and the wiring list.
	 * - Loads with no finite positive watts draw nothing and are left out (a lighting fixture among them is counted in
	 *   FixturesWithoutPowerData).
	 * - Loads are taken largest first (ties: label in natural order, fixture ID, type), each onto the least-loaded phase (ties:
	 *   the lower phase), on that phase's first circuit with room within the usable share, or a new circuit there. A load larger
	 *   than a circuit's usable share gets a circuit of its own, marked overloaded; it is never split.
	 * - One power home-run per load. Patched fixtures are chained per universe in address order; link lengths are the
	 *   straight-line distance x DmxRoutingSlack rounded up to stock lengths. Unpatched fixtures are counted, not chained.
	 */
	MODULARSCENEBUILDER_API FStageCableSchedule Build(TArray<FStagePowerLoad> Loads, TArray<FStageDmxNode> Nodes, const FStagePowerDistribution& Supply,
		const FString& SceneName, const FDateTime& GeneratedAt);

	/** "L1".."L3". */
	MODULARSCENEBUILDER_API FString FormatPhase(int32 Phase);

	/** "C03". */
	MODULARSCENEBUILDER_API FString FormatCircuit(int32 Number);

	/** What a crew must look at, in order of severity, as printed on the first page and in the CSV. Empty when nothing is wrong. */
	MODULARSCENEBUILDER_API TArray<FString> GetWarnings(const FStageCableSchedule& Schedule);

	/**
	 * The plan as CSV with the bill of materials' conventions (UTF-8 byte order mark, CRLF, "." decimals, every text field through
	 * StageBillOfMaterials::EscapeCsvField): a title block with the supply and warnings, then the phases, circuits, power cables
	 * and DMX cables, each with its own header row and separated by a blank line.
	 */
	MODULARSCENEBUILDER_API FString ToCsv(const FStageCableSchedule& Schedule);

	/**
	 * The plan as a print-ready A4 landscape PDF in black and grey: warnings and the supply first, then phase loads, circuits,
	 * power cables and DMX cables, with "Page n of m" footers. The footer states that the plan is a generated proposal.
	 */
	MODULARSCENEBUILDER_API TArray<uint8> ToPdf(const FStageCableSchedule& Schedule);

	/** "CableSchedule_<Scene>_<yyyyMMdd-HHmmss>" without extension, sanitized like StageBillOfMaterials::MakeFileName. */
	MODULARSCENEBUILDER_API FString MakeFileStem(const FString& SceneName, const FDateTime& GeneratedAt);
}
