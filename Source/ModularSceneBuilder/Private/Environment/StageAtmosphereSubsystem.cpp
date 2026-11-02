// Copyright Epic Games, Inc. All Rights Reserved.

#include "Environment/StageAtmosphereSubsystem.h"

#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "Environment/StageAtmospherePreset.h"
#include "ModularSceneBuilder.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageAtmosphereSubsystem)

const FPrimaryAssetType UStageAtmospherePreset::AtmosphereAssetType(TEXT("StageAtmosphere"));

FPrimaryAssetId UStageAtmospherePreset::GetPrimaryAssetId() const
{
	return FPrimaryAssetId(AtmosphereAssetType, GetFName());
}

void UStageAtmosphereSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// As the item catalog: in the editor the registry may still be scanning when PIE starts.
	UAssetManager::CallOrRegister_OnCompletedInitialScan(FSimpleMulticastDelegate::FDelegate::CreateUObject(this, &ThisClass::LoadAtmospheres));
}

void UStageAtmosphereSubsystem::Deinitialize()
{
	OnLoadProgress.Clear();
	OnAtmospheresLoaded.Clear();
	if (LoadHandle.IsValid())
	{
		LoadHandle->CancelHandle();
		LoadHandle.Reset();
	}
	Atmospheres.Reset();
	bLoaded = false;
	Super::Deinitialize();
}

void UStageAtmosphereSubsystem::LoadAtmospheres()
{
	UAssetManager& AssetManager = UAssetManager::Get();
	TArray<FPrimaryAssetId> Ids;
	AssetManager.GetPrimaryAssetIdList(UStageAtmospherePreset::AtmosphereAssetType, Ids);
	if (Ids.IsEmpty())
	{
		UE_LOG(LogStageCraft, Warning, TEXT("No atmosphere presets found. Check the StageAtmosphere entry under AssetManagerSettings in DefaultGame.ini."));
		HandleAtmospheresLoaded();
		return;
	}

	LoadHandle = AssetManager.LoadPrimaryAssets(Ids, {}, FStreamableDelegate::CreateUObject(this, &ThisClass::HandleAtmospheresLoaded));
	if (bLoaded)
	{
		LoadHandle.Reset(); // Already in memory: the delegate ran inside LoadPrimaryAssets.
	}
	else if (LoadHandle.IsValid())
	{
		LoadHandle->BindUpdateDelegate(FStreamableUpdateDelegate::CreateWeakLambda(this, [this](TSharedRef<FStreamableHandle> Handle)
		{
			OnLoadProgress.Broadcast(Handle->GetLoadProgress());
		}));
	}
}

void UStageAtmosphereSubsystem::HandleAtmospheresLoaded()
{
	TArray<UObject*> Loaded;
	UAssetManager::Get().GetPrimaryAssetObjectList(UStageAtmospherePreset::AtmosphereAssetType, Loaded);

	Atmospheres.Reset(Loaded.Num());
	for (UObject* Object : Loaded)
	{
		if (UStageAtmospherePreset* Preset = Cast<UStageAtmospherePreset>(Object))
		{
			Atmospheres.Add(Preset);
		}
	}
	Atmospheres.Sort([](const UStageAtmospherePreset& A, const UStageAtmospherePreset& B)
	{
		return A.SortOrder != B.SortOrder ? A.SortOrder < B.SortOrder : A.GetFName().LexicalLess(B.GetFName());
	});

	LoadHandle.Reset(); // The Asset Manager keeps the loaded presets; the handle was only for progress.
	bLoaded = true;
	UE_LOG(LogStageCraft, Log, TEXT("Atmosphere presets loaded: %d."), Atmospheres.Num());
	OnAtmospheresLoaded.Broadcast();
}

UStageAtmospherePreset* UStageAtmosphereSubsystem::FindAtmosphere(const FPrimaryAssetId& Id) const
{
	for (UStageAtmospherePreset* Preset : Atmospheres)
	{
		if (Preset && Preset->GetPrimaryAssetId() == Id)
		{
			return Preset;
		}
	}
	return nullptr;
}

UStageAtmospherePreset* UStageAtmosphereSubsystem::FindMatchingAtmosphere(const FStageEnvironmentSettings& Settings) const
{
	for (UStageAtmospherePreset* Preset : Atmospheres)
	{
		// Compared sanitized: the stored settings always are, the asset's may not be.
		if (Preset && StageEnvironment::Sanitize(Preset->Settings).Equals(Settings))
		{
			return Preset;
		}
	}
	return nullptr;
}

float UStageAtmosphereSubsystem::GetLoadProgress() const
{
	if (bLoaded)
	{
		return 1.f;
	}
	return LoadHandle.IsValid() ? LoadHandle->GetLoadProgress() : 0.f;
}
