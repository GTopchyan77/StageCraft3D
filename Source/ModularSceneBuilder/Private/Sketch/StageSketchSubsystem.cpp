// Copyright Epic Games, Inc. All Rights Reserved.

#include "Sketch/StageSketchSubsystem.h"

#include "Data/LightingFixtureData.h"
#include "Data/StageDeckData.h"
#include "Data/StageSpanData.h"
#include "Engine/AssetManager.h"
#include "Engine/GameInstance.h"
#include "Engine/StreamableManager.h"
#include "Engine/World.h"
#include "ModularSceneBuilder.h"
#include "Sketch/StageSketchGenerator.h"
#include "Sketch/StageSketchMath.h"
#include "Subsystems/StageItemSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageSketchSubsystem)

namespace StageSketchSubsystem
{
	bool IsCandidate(EStageSketchElement Element, const UBaseItemData& Item)
	{
		switch (Element)
		{
		case EStageSketchElement::Deck:		return Item.IsA<UStageDeckData>();
		case EStageSketchElement::Truss:	return Item.IsA<UStageSpanData>() && Item.ItemType == EStageItemType::Truss;
		case EStageSketchElement::Fixture:	return Item.IsA<ULightingFixtureData>();
		default:							return false;
		}
	}
}

bool UStageSketchSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UStageSketchSubsystem::Deinitialize()
{
	if (LoadHandle.IsValid())
	{
		LoadHandle->CancelHandle();
		LoadHandle.Reset();
	}
	PendingPlan.Reset();
	PendingCallback.Unbind();
	OnSketchChanged.Clear();
	OnGenerationFinished.Clear();
	Super::Deinitialize();
}

UStageItemSubsystem* UStageSketchSubsystem::GetCatalog() const
{
	const UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	return GameInstance ? GameInstance->GetSubsystem<UStageItemSubsystem>() : nullptr;
}

bool UStageSketchSubsystem::AddStroke(const FStageSketchStroke& Stroke)
{
	if (Strokes.Num() >= StageSketchMath::MaxStrokes)
	{
		UE_LOG(LogStageCraft, Log, TEXT("Sketch: stroke refused, the sketch already holds %d strokes."), Strokes.Num());
		return false;
	}
	FStageSketchStroke Clean = Stroke;
	if (!StageSketchMath::SanitizeStroke(Clean))
	{
		UE_LOG(LogStageCraft, Verbose, TEXT("Sketch: %s stroke of %d points dropped (too few usable points)."), *UEnum::GetValueAsString(Stroke.Kind), Stroke.Points.Num());
		return false;
	}
	Strokes.Add(MoveTemp(Clean));
	OnSketchChanged.Broadcast();
	return true;
}

bool UStageSketchSubsystem::RemoveStroke(int32 Index)
{
	if (!Strokes.IsValidIndex(Index))
	{
		return false;
	}
	Strokes.RemoveAt(Index);
	OnSketchChanged.Broadcast();
	return true;
}

bool UStageSketchSubsystem::RemoveLastStroke()
{
	return RemoveStroke(Strokes.Num() - 1);
}

void UStageSketchSubsystem::ClearStrokes()
{
	if (!Strokes.IsEmpty())
	{
		Strokes.Reset();
		OnSketchChanged.Broadcast();
	}
}

int32 UStageSketchSubsystem::FindStrokeAt(const FVector2D& Point, double Radius) const
{
	// Newest first, so the stroke drawn on top is the one picked.
	for (int32 Index = Strokes.Num() - 1; Index >= 0; --Index)
	{
		if (StageSketchMath::DistanceToStroke(Strokes[Index], Point) <= Radius)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

int32 UStageSketchSubsystem::CountStrokes(EStageSketchElement Element) const
{
	int32 Count = 0;
	for (const FStageSketchStroke& Stroke : Strokes)
	{
		Count += Stroke.GetElement() == Element ? 1 : 0;
	}
	return Count;
}

void UStageSketchSubsystem::SetSettings(const FStageSketchSettings& NewSettings)
{
	const FStageSketchSettings Clean = StageSketchMath::SanitizeSettings(NewSettings);
	if (!(Clean == Settings))
	{
		Settings = Clean;
		OnSketchChanged.Broadcast();
	}
}

TArray<UBaseItemData*> UStageSketchSubsystem::GetCandidateItems(EStageSketchElement Element) const
{
	TArray<UBaseItemData*> Candidates;
	if (const UStageItemSubsystem* Catalog = GetCatalog())
	{
		for (UBaseItemData* Item : Catalog->GetCatalog())
		{
			if (Item && StageSketchSubsystem::IsCandidate(Element, *Item))
			{
				Candidates.Add(Item);
			}
		}
	}
	return Candidates;
}

UBaseItemData* UStageSketchSubsystem::GetItem(EStageSketchElement Element) const
{
	const TArray<UBaseItemData*> Candidates = GetCandidateItems(Element);
	UBaseItemData* Chosen = ChosenItems.IsValidIndex(int32(Element)) ? ChosenItems[int32(Element)].Get() : nullptr;
	if (Chosen && Candidates.Contains(Chosen))
	{
		return Chosen;
	}
	return Candidates.IsEmpty() ? nullptr : Candidates[0];
}

bool UStageSketchSubsystem::SetItem(EStageSketchElement Element, UBaseItemData* Item)
{
	if (!Item || !GetCandidateItems(Element).Contains(Item))
	{
		return false;
	}
	ChosenItems.SetNum(int32(EStageSketchElement::Count));
	if (ChosenItems[int32(Element)] != Item)
	{
		ChosenItems[int32(Element)] = Item;
		OnSketchChanged.Broadcast();
	}
	return true;
}

FStageSketchBuildPlan UStageSketchSubsystem::MakePlan() const
{
	FStageSketchBuildRules Rules;
	Rules.GridStep = Settings.GridStep;
	Rules.bSnapToGrid = Settings.bSnapToGrid;
	Rules.TrussHeight = Settings.TrussHeight;
	Rules.DeckItem = Cast<UStageDeckData>(GetItem(EStageSketchElement::Deck));
	Rules.TrussItem = Cast<UStageSpanData>(GetItem(EStageSketchElement::Truss));
	Rules.FixtureItem = GetItem(EStageSketchElement::Fixture);
	return StageSketchGenerator::BuildPlan(Strokes, Rules);
}

bool UStageSketchSubsystem::RequestPlanAssets(const FStageSketchBuildPlan& Plan, FSimpleDelegate OnReady)
{
	if (PendingPlan.IsSet() || Plan.IsEmpty())
	{
		return false;
	}

	UAssetManager& AssetManager = UAssetManager::Get();
	TArray<FPrimaryAssetId> AssetIds;
	const auto AddItem = [&AssetIds, &AssetManager](const UBaseItemData* Item, bool bUsed)
	{
		// Items outside the Asset Manager scan (or built at runtime) are already in memory or load on spawn.
		if (Item && bUsed && Item->RequiresAssetStreaming() && AssetManager.GetPrimaryAssetPath(Item->GetPrimaryAssetId()).IsValid())
		{
			AssetIds.AddUnique(Item->GetPrimaryAssetId());
		}
	};
	AddItem(Plan.DeckItem.Get(), !Plan.Decks.IsEmpty());
	AddItem(Plan.TrussItem.Get(), !Plan.Trusses.IsEmpty());
	AddItem(Plan.FixtureItem.Get(), !Plan.Fixtures.IsEmpty());

	PendingPlan = Plan;
	bPendingPlanReady = false;
	PendingCallback = MoveTemp(OnReady);
	if (AssetIds.IsEmpty())
	{
		HandlePlanAssetsLoaded();
		return true;
	}
	// The delegate may run inside this call when everything is loaded already, so the pending state is set first.
	TSharedPtr<FStreamableHandle> Handle = AssetManager.LoadPrimaryAssets(AssetIds, { UBaseItemData::GameBundle },
		FStreamableDelegate::CreateUObject(this, &ThisClass::HandlePlanAssetsLoaded));
	if (PendingPlan.IsSet() && !bPendingPlanReady)
	{
		LoadHandle = MoveTemp(Handle);
	}
	return true;
}

void UStageSketchSubsystem::HandlePlanAssetsLoaded()
{
	LoadHandle.Reset();
	if (!PendingPlan.IsSet())
	{
		return;
	}
	bPendingPlanReady = true;
	FSimpleDelegate Callback = MoveTemp(PendingCallback);
	PendingCallback.Unbind();
	Callback.ExecuteIfBound();
	// A requester that is gone (or did not take the plan) must not block the next generation.
	PendingPlan.Reset();
	bPendingPlanReady = false;
}

TOptional<FStageSketchBuildPlan> UStageSketchSubsystem::TakeReadyPlan()
{
	if (!PendingPlan.IsSet() || !bPendingPlanReady)
	{
		return {};
	}
	TOptional<FStageSketchBuildPlan> Plan = MoveTemp(PendingPlan);
	PendingPlan.Reset();
	bPendingPlanReady = false;
	return Plan;
}

void UStageSketchSubsystem::ReportGeneration(const FStageSketchGenerateResult& Result)
{
	LastResult = Result;
	UE_LOG(LogStageCraft, Log, TEXT("Sketch: generation %s: %s"), Result.bSucceeded ? TEXT("succeeded") : TEXT("refused"), *Result.Message.ToString());
	OnGenerationFinished.Broadcast(Result);
}
