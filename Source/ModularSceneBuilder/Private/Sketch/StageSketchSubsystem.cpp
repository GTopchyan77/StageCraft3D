// Copyright Epic Games, Inc. All Rights Reserved.

#include "Sketch/StageSketchSubsystem.h"

#include "Async/Async.h"
#include "Data/LightingFixtureData.h"
#include "Data/StageDeckData.h"
#include "Data/StageSpanData.h"
#include "Engine/AssetManager.h"
#include "Engine/GameInstance.h"
#include "Engine/StreamableManager.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "IImageWrapperModule.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ModularSceneBuilder.h"
#include "Sketch/StageSketchGenerator.h"
#include "Sketch/StageSketchImageImport.h"
#include "Sketch/StageSketchMath.h"
#include "Sketch/StageSketchVisionBackend.h"
#include "Subsystems/StageItemSubsystem.h"
#include "Tasks/Task.h"

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
	++ImportSerial;
	if (VisionBackend)
	{
		VisionBackend->Cancel();
		VisionBackend = nullptr;
	}
	OnlineJob.Reset();
	ImportState = EStageSketchImportState::Idle;
	ReferenceImage = nullptr;
	OnImportChanged.Clear();
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

// --- Photo / scan import (Docs/ADR/0017 §2.6-2.7) ---

#define LOCTEXT_NAMESPACE "StageSketchSubsystem"

namespace StageSketchSubsystem
{
	/** "2 decks, 3 truss lines and 6 fixtures" for what was added. */
	FText DescribeImported(TConstArrayView<FStageSketchStroke> Strokes)
	{
		int32 Counts[int32(EStageSketchElement::Count)] = { 0, 0, 0 };
		for (const FStageSketchStroke& Stroke : Strokes)
		{
			if (Stroke.GetElement() != EStageSketchElement::Count)
			{
				++Counts[int32(Stroke.GetElement())];
			}
		}
		return FText::Format(LOCTEXT("ImportedCounts", "{0} {0}|plural(one=deck,other=decks), {1} truss {1}|plural(one=line,other=lines) and {2} {2}|plural(one=fixture,other=fixtures)"),
			FText::AsNumber(Counts[int32(EStageSketchElement::Deck)]), FText::AsNumber(Counts[int32(EStageSketchElement::Truss)]), FText::AsNumber(Counts[int32(EStageSketchElement::Fixture)]));
	}
}

bool UStageSketchSubsystem::CanReadOnline(FText& OutReason) const
{
	// The settings and the key are read now, so turning the online model on applies without reloading the level.
	return VisionBackend ? VisionBackend->IsAvailable(OutReason) : GetDefault<UStageClaudeSketchVisionBackend>()->IsAvailable(OutReason);
}

bool UStageSketchSubsystem::ImportImage(const FString& FilePath, EStageSketchReader Reader, FText& OutError)
{
	if (IsImportPending())
	{
		OutError = LOCTEXT("ImportBusy", "A photo is already being read.");
		return false;
	}
	const FString FullPath = FPaths::ConvertRelativePathToFull(FilePath);
	if (!StageSketchImageImport::HasSupportedExtension(FullPath))
	{
		OutError = LOCTEXT("ImportFormat", "Choose a PNG or JPEG image.");
		return false;
	}
	if (!FPaths::FileExists(FullPath))
	{
		OutError = FText::Format(LOCTEXT("ImportMissing", "{0} was not found."), FText::FromString(FPaths::GetCleanFilename(FullPath)));
		return false;
	}
	const bool bOnline = Reader == EStageSketchReader::Claude;
	if (bOnline && !CanReadOnline(OutError))
	{
		return false;
	}
	if (bOnline && !VisionBackend)
	{
		VisionBackend = NewObject<UStageClaudeSketchVisionBackend>(this);
	}

	// Loaded here on the game thread; the worker only uses it.
	FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const TSharedRef<FStageSketchImportJob> Job = MakeShared<FStageSketchImportJob>();
	Job->FilePath = FullPath;
	Job->Options.WidthCm = Settings.PhotoWidth;
	Job->Options.DeckHeight = Settings.DeckHeight;
	Job->bWantVisionImage = bOnline;

	const uint32 Serial = ++ImportSerial;
	ImportState = EStageSketchImportState::Reading;
	bImportOnline = bOnline;
	bImportSucceeded = false;
	ImportMessage = FText::Format(LOCTEXT("ImportReading", "Reading {0}…"), FText::FromString(FPaths::GetCleanFilename(FullPath)));
	UE_LOG(LogStageCraft, Log, TEXT("Sketch: importing %s (%s reading, %.0f m wide)."), *FullPath, bOnline ? TEXT("online") : TEXT("local"), Settings.PhotoWidth * 0.01f);
	OnImportChanged.Broadcast();

	// The worker touches only the job; the result comes back to the game thread, where a stale or orphaned one is dropped.
	TWeakObjectPtr<UStageSketchSubsystem> WeakThis = this;
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis, Serial, Job]()
	{
		StageSketchImageImport::Run(*Job);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Serial, Job]()
		{
			if (UStageSketchSubsystem* Sketch = WeakThis.Get())
			{
				Sketch->HandleImageRead(Serial, Job);
			}
		});
	});
	return true;
}

void UStageSketchSubsystem::HandleImageRead(uint32 Serial, TSharedRef<FStageSketchImportJob> Job)
{
	if (Serial != ImportSerial || ImportState != EStageSketchImportState::Reading)
	{
		return;
	}
	const FText FileName = FText::FromString(FPaths::GetCleanFilename(Job->FilePath));
	if (!Job->bDecoded)
	{
		FinishImport(false, FText::Format(LOCTEXT("ImportUnreadable", "{0} could not be read: {1}"), FileName, Job->Error));
		return;
	}
	ShowReference(*Job);
	if (bImportOnline && VisionBackend && !Job->VisionJpeg.IsEmpty())
	{
		if (VisionBackend->Read(Job->VisionJpeg, FOnStageSketchVisionFinished::CreateUObject(this, &ThisClass::HandleOnlineReading, Serial)))
		{
			OnlineJob = Job;
			ImportState = EStageSketchImportState::ReadingOnline;
			ImportMessage = FText::Format(LOCTEXT("ImportOnline", "{0} is reading {1}…"), VisionBackend->GetDisplayName(), FileName);
			OnImportChanged.Broadcast();
			return;
		}
		ApplyLocalReading(*Job, LOCTEXT("OnlineNotStarted", "the online request could not start"));
		return;
	}
	ApplyLocalReading(*Job, bImportOnline ? LOCTEXT("OnlineNoImage", "the image could not be prepared for it") : FText::GetEmpty());
}

void UStageSketchSubsystem::HandleOnlineReading(const FStageSketchVisionResult& Result, uint32 Serial)
{
	if (Serial != ImportSerial || ImportState != EStageSketchImportState::ReadingOnline || !OnlineJob.IsValid())
	{
		return;
	}
	const TSharedPtr<FStageSketchImportJob> Job = MoveTemp(OnlineJob);
	OnlineJob.Reset();
	if (Result.Status != FStageSketchVisionResult::EStatus::Plan || Result.Strokes.IsEmpty())
	{
		const FText Reason = Result.Status == FStageSketchVisionResult::EStatus::Text ? FText::FromString(Result.Text)
			: Result.Status == FStageSketchVisionResult::EStatus::Plan ? LOCTEXT("OnlineNothing", "it found no stage elements")
			: Result.Error;
		ApplyLocalReading(*Job, Reason);
		return;
	}
	const TArray<FStageSketchStroke> Added = AddImportedStrokes(StageSketchVision::ToPlan(Result.Strokes, Job->VisionWidth, Job->VisionHeight,
		Job->Options.WidthCm, Job->Options.DeckHeight));
	const FText FileName = FText::FromString(FPaths::GetCleanFilename(Job->FilePath));
	FText Message = FText::Format(LOCTEXT("ImportedOnline", "Claude read {0}: added {1}. Check them against the photo, then GENERATE 3D."), FileName,
		StageSketchSubsystem::DescribeImported(Added));
	if (Result.Dropped > 0)
	{
		Message = FText::Format(LOCTEXT("ImportedOnlineDropped", "{0} {1} {1}|plural(one=element was,other=elements were) out of range and left out."), Message, FText::AsNumber(Result.Dropped));
	}
	if (!Result.Text.IsEmpty())
	{
		Message = FText::Format(LOCTEXT("ImportedOnlineNote", "{0} Note: {1}"), Message, FText::FromString(Result.Text));
	}
	FinishImport(!Added.IsEmpty(), Added.IsEmpty() ? LOCTEXT("ImportFull", "Nothing was added: the sketch is full. Clear some strokes first.") : Message);
}

void UStageSketchSubsystem::ApplyLocalReading(const FStageSketchImportJob& Job, const FText& OnlineFailure)
{
	const FText FileName = FText::FromString(FPaths::GetCleanFilename(Job.FilePath));
	const FStageSketchTraceResult& Trace = Job.Trace;
	const TArray<FStageSketchStroke> Added = AddImportedStrokes(Trace.Strokes);
	FText Message;
	if (Added.IsEmpty())
	{
		Message = Trace.Strokes.IsEmpty()
			? FText::Format(LOCTEXT("ImportNothing", "Nothing usable was found in {0}."), FileName)
			: LOCTEXT("ImportFull", "Nothing was added: the sketch is full. Clear some strokes first.");
	}
	else
	{
		Message = FText::Format(LOCTEXT("ImportedLocal", "Traced {0}: added {1}. Check them against the photo, then GENERATE 3D."), FileName,
			StageSketchSubsystem::DescribeImported(Added));
	}
	if (!Trace.Warnings.IsEmpty())
	{
		Message = FText::Format(LOCTEXT("ImportWarnings", "{0} {1}"), Message, FText::Join(FText::FromString(TEXT(" ")), Trace.Warnings));
	}
	if (!OnlineFailure.IsEmpty())
	{
		Message = FText::Format(LOCTEXT("ImportOnlineFailed", "{0} (Claude could not read it: {1}; it was traced on this computer instead.)"), Message, OnlineFailure);
	}
	FinishImport(!Added.IsEmpty(), Message);
}

TArray<FStageSketchStroke> UStageSketchSubsystem::AddImportedStrokes(TConstArrayView<FStageSketchStroke> NewStrokes)
{
	TArray<FStageSketchStroke> Added;
	for (const FStageSketchStroke& Stroke : NewStrokes)
	{
		if (Strokes.Num() >= StageSketchMath::MaxStrokes)
		{
			UE_LOG(LogStageCraft, Log, TEXT("Sketch: import stopped at %d strokes, the sketch's limit."), Strokes.Num());
			break;
		}
		FStageSketchStroke Clean = Stroke;
		if (StageSketchMath::SanitizeStroke(Clean))
		{
			Strokes.Add(Clean);
			Added.Add(MoveTemp(Clean));
		}
	}
	if (!Added.IsEmpty())
	{
		OnSketchChanged.Broadcast();
	}
	return Added;
}

void UStageSketchSubsystem::ShowReference(const FStageSketchImportJob& Job)
{
	if (Job.PreviewWidth <= 0 || Job.PreviewHeight <= 0 || Job.Preview.Num() != Job.PreviewWidth * Job.PreviewHeight * 4)
	{
		return;
	}
	UTexture2D* Texture = UTexture2D::CreateTransient(Job.PreviewWidth, Job.PreviewHeight, PF_B8G8R8A8, NAME_None,
		TConstArrayView64<uint8>(Job.Preview.GetData(), Job.Preview.Num()));
	if (!Texture)
	{
		return;
	}
	Texture->SRGB = true;
	Texture->UpdateResource();
	ReferenceImage = Texture;
	// The same placement as the traced strokes: centred on the origin, Settings.PhotoWidth wide, image up = plan +X.
	const double Width = Job.Options.WidthCm;
	const double Height = Width * double(Job.PreviewHeight) / double(Job.PreviewWidth);
	ReferenceBounds = FBox2D(FVector2D(-Height * 0.5, -Width * 0.5), FVector2D(Height * 0.5, Width * 0.5));
	bReferenceVisible = true;
}

void UStageSketchSubsystem::FinishImport(bool bSucceeded, const FText& Message)
{
	++ImportsFinished;
	ImportState = EStageSketchImportState::Idle;
	bImportSucceeded = bSucceeded;
	ImportMessage = Message;
	UE_LOG(LogStageCraft, Log, TEXT("Sketch: import %s: %s"), bSucceeded ? TEXT("finished") : TEXT("failed"), *Message.ToString());
	OnImportChanged.Broadcast();
}

void UStageSketchSubsystem::CancelImport()
{
	++ImportSerial;
	if (VisionBackend)
	{
		VisionBackend->Cancel();
	}
	OnlineJob.Reset();
	if (IsImportPending())
	{
		FinishImport(false, LOCTEXT("ImportCancelled", "Import cancelled."));
	}
}

void UStageSketchSubsystem::SetReferenceVisible(bool bVisible)
{
	if (bReferenceVisible != bVisible)
	{
		bReferenceVisible = bVisible;
		OnImportChanged.Broadcast();
	}
}

void UStageSketchSubsystem::ClearReference()
{
	if (ReferenceImage)
	{
		ReferenceImage = nullptr;
		ReferenceBounds = FBox2D(ForceInit);
		OnImportChanged.Broadcast();
	}
}

#undef LOCTEXT_NAMESPACE
