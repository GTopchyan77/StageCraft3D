// Copyright Epic Games, Inc. All Rights Reserved.

#include "Subsystems/StageSessionSubsystem.h"

#include "Actors/ModularBaseActor.h"
#include "Data/BaseItemData.h"
#include "Engine/World.h"
#include "Interaction/StageParameterInterface.h"
#include "ModularSceneBuilder.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageSessionSubsystem)

#define LOCTEXT_NAMESPACE "StageCraftSession"

bool UStageSessionSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Sessions exist while playing; editor and preview worlds have no player building a stage.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UStageSessionSubsystem::Deinitialize()
{
	for (const TWeakObjectPtr<AModularBaseActor>& Item : Items)
	{
		if (AModularBaseActor* Actor = Item.Get())
		{
			Actor->OnParameterChanged.RemoveDynamic(this, &ThisClass::HandleItemParameterChanged);
		}
	}
	Items.Reset();
	OnStatsChanged.Clear();
	OnDirtyChanged.Clear();

	Super::Deinitialize();
}

void UStageSessionSubsystem::BeginSession(const FStageSessionRules& InRules)
{
	Rules = InRules;
	bSessionStarted = true;
	UE_LOG(LogStageCraft, Log, TEXT("Stage session started in %s (entitlements %s, max items %d, max power %.0f W, max weight %.0f kg)."),
		*GetNameSafe(GetWorld()), Rules.bEnforceEntitlements ? TEXT("enforced") : TEXT("sandbox"),
		Rules.MaxPlacedItems, Rules.MaxTotalPowerWatts, Rules.MaxTotalWeightKg);
}

void UStageSessionSubsystem::RegisterItem(AModularBaseActor* Item)
{
	if (!Item || Items.Contains(Item))
	{
		return;
	}

	Items.Add(Item);
	Item->OnParameterChanged.AddUniqueDynamic(this, &ThisClass::HandleItemParameterChanged);
	RecomputeStats();

	// Items present when the level starts are the loaded stage, not an edit.
	const UWorld* World = GetWorld();
	if (World && World->GetBegunPlay())
	{
		SetDirty(true);
	}
}

void UStageSessionSubsystem::UnregisterItem(AModularBaseActor* Item)
{
	if (!Item || Items.Remove(Item) == 0)
	{
		return;
	}

	Item->OnParameterChanged.RemoveDynamic(this, &ThisClass::HandleItemParameterChanged);
	RecomputeStats();

	const UWorld* World = GetWorld();
	if (World && World->GetBegunPlay() && !World->bIsTearingDown)
	{
		SetDirty(true);
	}
}

TArray<AModularBaseActor*> UStageSessionSubsystem::GetPlacedItems() const
{
	TArray<AModularBaseActor*> Result;
	Result.Reserve(Items.Num());
	for (const TWeakObjectPtr<AModularBaseActor>& Item : Items)
	{
		if (AModularBaseActor* Actor = Item.Get())
		{
			Result.Add(Actor);
		}
	}
	return Result;
}

void UStageSessionSubsystem::HandleItemParameterChanged(AModularBaseActor* Item, FGameplayTag ParameterId)
{
	// An invalid tag means the structure changed (item type swapped), which changes power and weight.
	if (!ParameterId.IsValid())
	{
		RecomputeStats();
		SetDirty(true);
		return;
	}

	// Moves arrive here from the gizmo as well as from the inspector. Attribute changes do not mark
	// the session dirty: cue playback and DMX change them constantly without editing the stage.
	if (ParameterId == StageCraftTags::Param_Transform_Location || ParameterId == StageCraftTags::Param_Transform_Rotation)
	{
		SetDirty(true);
	}
}

void UStageSessionSubsystem::RecomputeStats()
{
	FStageSessionStats NewStats;
	Items.RemoveAll([](const TWeakObjectPtr<AModularBaseActor>& Item) { return !Item.IsValid(); });

	for (const TWeakObjectPtr<AModularBaseActor>& Item : Items)
	{
		const AModularBaseActor* Actor = Item.Get();
		if (!Actor || Actor->IsActorBeingDestroyed())
		{
			continue;
		}
		++NewStats.PlacedItems;
		if (const UBaseItemData* Data = Actor->GetItemData())
		{
			NewStats.TotalPowerWatts += Data->Specs.PowerDrawWatts;
			NewStats.TotalWeightKg += Data->Specs.WeightKg;
		}
	}

	if (NewStats.PlacedItems != Stats.PlacedItems || NewStats.TotalPowerWatts != Stats.TotalPowerWatts || NewStats.TotalWeightKg != Stats.TotalWeightKg)
	{
		Stats = NewStats;
		OnStatsChanged.Broadcast(Stats);
	}
}

FStageEconomyResultInfo UStageSessionSubsystem::EvaluateSessionLimits(const UBaseItemData* Item) const
{
	if (!Item)
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoItem", "No item."));
	}

	if (Rules.MaxPlacedItems > 0 && Stats.PlacedItems + 1 > Rules.MaxPlacedItems)
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::SessionRuleViolation,
			FText::Format(LOCTEXT("ItemLimit", "This stage allows at most {0} items."), FText::AsNumber(Rules.MaxPlacedItems)));
	}
	if (Rules.MaxTotalPowerWatts > 0.f && Stats.TotalPowerWatts + Item->Specs.PowerDrawWatts > Rules.MaxTotalPowerWatts)
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::SessionRuleViolation,
			FText::Format(LOCTEXT("PowerLimit", "Not enough power: {0} W of {1} W in use, {2} needs {3} W."),
				FText::AsNumber(FMath::RoundToInt(Stats.TotalPowerWatts)), FText::AsNumber(FMath::RoundToInt(Rules.MaxTotalPowerWatts)),
				Item->DisplayName, FText::AsNumber(FMath::RoundToInt(Item->Specs.PowerDrawWatts))));
	}
	if (Rules.MaxTotalWeightKg > 0.f && Stats.TotalWeightKg + Item->Specs.WeightKg > Rules.MaxTotalWeightKg)
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::SessionRuleViolation,
			FText::Format(LOCTEXT("WeightLimit", "Too heavy: {0} kg of {1} kg used, {2} weighs {3} kg."),
				FText::AsNumber(FMath::RoundToInt(Stats.TotalWeightKg)), FText::AsNumber(FMath::RoundToInt(Rules.MaxTotalWeightKg)),
				Item->DisplayName, FText::AsNumber(FMath::RoundToInt(Item->Specs.WeightKg))));
	}
	return FStageEconomyResultInfo::Ok();
}

bool UStageSessionSubsystem::ApplyParameterChange(UObject* Target, const FGameplayTag& ParameterId, const FStageParameterValue& Value)
{
	if (!Target || !Target->Implements<UStageParameterInterface>())
	{
		return false;
	}

	const bool bAccepted = IStageParameterInterface::Execute_SetParameterValue(Target, ParameterId, Value);
	if (bAccepted)
	{
		SetDirty(true);
	}
	return bAccepted;
}

void UStageSessionSubsystem::MarkClean()
{
	SetDirty(false);
}

void UStageSessionSubsystem::SetDirty(bool bNewDirty)
{
	if (bDirty != bNewDirty)
	{
		bDirty = bNewDirty;
		OnDirtyChanged.Broadcast(bDirty);
	}
}

#undef LOCTEXT_NAMESPACE
