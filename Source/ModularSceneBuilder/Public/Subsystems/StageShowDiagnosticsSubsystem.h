// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/WorldSubsystem.h"
#include "Show/StageFixtureExplain.h"
#include "Show/StageShowDiagnostics.h"
#include "StageShowDiagnosticsSubsystem.generated.h"

/** The Problems list changed (some finding came, went or changed its subjects). */
DECLARE_MULTICAST_DELEGATE(FOnStageProblemsChanged);

/**
 * Live diagnostics for one stage (Docs/ADR/0030 §5): gathers the show's state from its subsystems into a snapshot for
 * StageShowDiagnostics::Run (the Problems list) and one fixture's facts for StageFixtureExplain::Explain ("why is it dark?").
 * Read-only: it never changes the show.
 *
 * Push, not poll: it listens to the show, patch, programmer, palettes, DMX in and out and the music source, and while anyone listens
 * to OnProblemsChanged it runs the checks once on the next frame after a change (many changes in a frame cost one run) and
 * broadcasts only when the findings differ. With no listener nothing runs until GetProblems is asked. A world subsystem because
 * everything it reads is per stage (Game and PIE worlds only).
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageShowDiagnosticsSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** The current findings, worst first; recomputed first when something changed since the last run. */
	const TArray<FStageDiagnostic>& GetProblems();

	/** Runs the checks again now, whatever changed (the Problems panel's REFRESH: the music's signal is not pushed). */
	const TArray<FStageDiagnostic>& RefreshProblems();

	/** Everything the explanation reads about FixtureId now; bExists false when no fixture has that ID. */
	FStageFixtureFacts GatherFixtureFacts(int32 FixtureId) const;

	/** "Why is fixture FixtureId dark / not responding?" in Language. */
	FStageFixtureVerdict ExplainFixture(int32 FixtureId, EStageAILanguage Language) const;

	FOnStageProblemsChanged OnProblemsChanged;

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	/** Fills Snapshot from the subsystems; its cue and playback views point into the show and are valid until the show changes. */
	void MakeSnapshot(FStageShowSnapshot& Snapshot);

	/** Something the checks read changed: run again on the next frame if anyone listens, else on the next GetProblems. */
	void MarkDirty();
	bool HandleRefreshTick(float DeltaSeconds);

	UFUNCTION()
	void HandleDynamicChange();
	UFUNCTION()
	void HandleControlSourceChanged(EStageControlSource NewSource);
	UFUNCTION()
	void HandleCueEvent(float CueNumber);
	void HandleProgrammerFixturesChanged(TConstArrayView<int32> FixtureIds);

	UPROPERTY(Transient)
	TObjectPtr<class UShowControlSubsystem> ShowControl = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageDMXPatchSubsystem> Patch = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageProgrammerSubsystem> Programmer = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageShowLibrarySubsystem> Library = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageDMXNetworkSubsystem> Network = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageAudioReactiveSubsystem> Audio = nullptr;

	FDelegateHandle ProgrammerFixturesHandle;
	FDelegateHandle ProgrammerEffectsHandle;
	FDelegateHandle InputHandle;
	FDelegateHandle OutputHandle;
	FDelegateHandle AudioHandle;

	/** The pending next-frame run; reset when it fires and in Deinitialize. */
	FTSTicker::FDelegateHandle RefreshTicker;

	TArray<FStageDiagnostic> Problems;
	bool bDirty = true;
};
