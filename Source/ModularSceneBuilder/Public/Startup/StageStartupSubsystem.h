// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Startup/StageStartupTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "StageStartupSubsystem.generated.h"

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnStageStartupProgress, float /*Progress*/, const FText& /*Status*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnStageStartupStateChanged, EStageStartupState /*NewState*/);

/**
 * Tells the loader how far the app is from usable (Docs/ADR/0024-startup-atmospheres-starter-stages.md §2).
 *
 * It owns nothing that loads. It listens to the systems that do (the item catalog, the panel definitions, the product
 * catalog, the atmosphere presets) and combines their progress into one bar, one status line and one state. Each of them
 * pushes its own progress from its streamable handle, so nothing here ticks or polls.
 *
 * The state only moves forward: Loading, then Ready, Failed or TimedOut, once. A source that fails or a timeout never
 * blocks the app; the loader offers to continue, and each system keeps refusing its own requests until it is ready.
 *
 * Game thread only. One per game instance; in the editor it also exists (tests use it), the loader just isn't shown.
 */
UCLASS(Config = Game)
class MODULARSCENEBUILDER_API UStageStartupSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	EStageStartupState GetState() const { return State; }
	bool IsSettled() const { return State != EStageStartupState::Loading; }

	/** Weighted progress of every source, in [0, 1]. */
	float GetProgress() const { return StageStartup::CombinedProgress(Sources); }

	/** "Loading the library…", or what failed, or "Ready". */
	FText GetStatusText() const;

	/** The sources, for diagnostics and tests. */
	TConstArrayView<FStageStartupSource> GetSources() const { return Sources; }

	/**
	 * Adds something to wait for. Ignored when a source with that id exists or startup has already settled (a late source
	 * cannot hold the loader back up). Returns whether it was added.
	 */
	bool RegisterSource(FName Id, const FText& Label, float Weight);

	/** Progress of a source, clamped to [0, 1]; never moves it backwards. Ignored for an unknown or settled source. */
	void ReportProgress(FName Id, float Fraction);
	void ReportReady(FName Id);
	void ReportFailed(FName Id, const FText& Reason);

	FOnStageStartupProgress OnProgress;

	/** Fires once, when the state leaves Loading. */
	FOnStageStartupStateChanged OnSettled;

private:
	void BindCatalogSources(FSubsystemCollectionBase& Collection);
	void UnbindCatalogSources();
	FStageStartupSource* FindSource(FName Id);
	void Update();
	void HandleTimeout();

	UFUNCTION()
	void HandleLibraryLoaded();

	UFUNCTION()
	void HandleShopLoaded();

	/** Seconds after which a loader still waiting offers to continue. */
	UPROPERTY(Config)
	float StartupTimeoutSeconds = 30.f;

	TArray<FStageStartupSource> Sources;
	EStageStartupState State = EStageStartupState::Loading;
	bool bTimedOut = false;
	FTimerHandle TimeoutHandle;

	TWeakObjectPtr<class UStageItemSubsystem> ItemSubsystem;
	TWeakObjectPtr<class UStageEconomySubsystem> EconomySubsystem;
	TWeakObjectPtr<class UStageWorkspaceSubsystem> WorkspaceSubsystem;
	TWeakObjectPtr<class UStageAtmosphereSubsystem> AtmosphereSubsystem;
	TArray<TPair<FName, FDelegateHandle>> ProgressBindings;
	TArray<TPair<FName, FDelegateHandle>> LoadedBindings;
};
