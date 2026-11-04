// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Show/StageEffectTypes.h"
#include "Show/StageShowMix.h"

/**
 * The running effects of one show (Docs/ADR/0027-effects-engine.md §3): each effect's clock, and what it gives each of its fixtures
 * now as an effect layer of the output mix (StageShowMix::FEffectMap). Owned by UShowControlSubsystem, which feeds it the entries
 * (the programmer's), advances it from its tick and writes the mix; the runner never touches a fixture's output. Game thread.
 *
 * The clock is counted in cycles and advanced by each effect's own rate, so changing the rate keeps the pattern where it is; an
 * entry that stays (same Id) keeps its clock through any edit.
 */
class MODULARSCENEBUILDER_API FStageEffectRunner
{
public:
	/** Looks a fixture up by ID on the stage; null when none has it. */
	using FFindFixture = TFunctionRef<class ALightingFixtureActor*(int32 /*FixtureId*/)>;

	/** Replaces the running effects (sanitized entries). Entries already running keep their clock; new ones start at 0. */
	void SetEffects(TConstArrayView<FStageEffectEntry> Entries, FFindFixture FindFixture);

	/** Finds every effect's fixtures again: one arrived, left, or changed its ID. */
	void RefreshFixtures(FFindFixture FindFixture);

	bool IsRunning() const { return !Running.IsEmpty(); }

	/** Moves every clock on by its rate. */
	void Advance(float DeltaSeconds);

	bool DrivesFixture(int32 FixtureId) const { return ByFixture.Contains(FixtureId); }

	/** A fixture an effect drives, and the ID it was found under (a different ID now means it was renumbered). */
	struct FDrivenFixture
	{
		TWeakObjectPtr<class ALightingFixtureActor> Fixture;
		int32 FixtureId = 0;
	};

	/** The fixtures on stage that effects drive, each once. Changes on SetEffects and RefreshFixtures only. */
	const TArray<FDrivenFixture>& GetDrivenFixtures() const { return DrivenFixtures; }

	/**
	 * What the effects give Fixture now (OutValues reset first): Position adds degrees to pan and tilt; Intensity and Beam set each
	 * attribute between Low and High of the fixture's range; Color sets the fixture's own match of the colour. Where two effects
	 * drive one attribute, the later entry wins. False when no effect drives Fixture.
	 */
	bool Evaluate(const class ALightingFixtureActor& Fixture, StageShowMix::FEffectMap& OutValues) const;

	/** The clock of the effect with Id in cycles, or 0 when it is not running (for the panel's preview). */
	double GetCycles(const FGuid& Id) const;

private:
	struct FRunning
	{
		FStageEffectEntry Entry;
		double Cycles = 0.0;
	};

	/** One effect's hold on a fixture: which effect, and the fixture's place in its order. */
	struct FDrive
	{
		int32 Effect = INDEX_NONE;
		int32 Index = INDEX_NONE;
	};

	void EvaluateDrive(const class ALightingFixtureActor& Fixture, const FDrive& Drive, StageShowMix::FEffectMap& OutValues) const;

	TArray<FRunning> Running;
	/** Per fixture ID, the effects driving it in entry order. */
	TMap<int32, TArray<FDrive, TInlineAllocator<2>>> ByFixture;
	TArray<FDrivenFixture> DrivenFixtures;
};
