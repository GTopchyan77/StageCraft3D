// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Startup/StageStartupTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "UObject/PrimaryAssetId.h"
#include "StageAtmosphereSubsystem.generated.h"

/**
 * The catalog of atmosphere presets (Docs/ADR/0024-startup-atmospheres-starter-stages.md §3): every UStageAtmospherePreset
 * the Asset Manager found, loaded asynchronously at startup and kept for the run.
 *
 * Read-only for everyone. It owns no environment state: applying a preset is the controller's environment request, and the
 * environment subsystem is the single writer of the settings. Views read the list once and bind to OnAtmospheresLoaded.
 *
 * Game instance subsystem: content that lives for the whole run, like the item catalog.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageAtmosphereSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	bool IsLoaded() const { return bLoaded; }

	/** Every preset in SortOrder (then name). Empty until loaded. */
	const TArray<class UStageAtmospherePreset*>& GetAtmospheres() const { return ObjectPtrDecay(Atmospheres); }

	class UStageAtmospherePreset* FindAtmosphere(const FPrimaryAssetId& Id) const;

	/** The preset whose settings Settings equal (StageEnvironment's tolerance), or null: the panel lights that card. */
	class UStageAtmospherePreset* FindMatchingAtmosphere(const struct FStageEnvironmentSettings& Settings) const;

	/** Load progress in [0, 1]: 1 once loaded. For the startup loader. */
	float GetLoadProgress() const;

	FOnStageLoadProgress OnLoadProgress;

	/** Fires once, when the presets have loaded (also when there are none). */
	FSimpleMulticastDelegate OnAtmospheresLoaded;

private:
	void LoadAtmospheres();
	void HandleAtmospheresLoaded();

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageAtmospherePreset>> Atmospheres;

	TSharedPtr<struct FStreamableHandle> LoadHandle;
	bool bLoaded = false;
};
