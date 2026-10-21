// Copyright Epic Games, Inc. All Rights Reserved.

#include "Scene/StageSceneComponent.h"

#include "Actors/ModularBaseActor.h"
#include "Async/Async.h"
#include "Components/SelectionComponent.h"
#include "Data/BaseItemData.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Environment/StageEnvironmentSubsystem.h"
#include "History/StageEditHistorySubsystem.h"
#include "TimerManager.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Placement/StagePlacementToolComponent.h"
#include "Scene/StageSceneStore.h"
#include "Subsystems/StageItemSubsystem.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "Tasks/Task.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageSceneComponent)

#define LOCTEXT_NAMESPACE "StageScene"

UStageSceneComponent::UStageSceneComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UStageSceneComponent::BeginPlay()
{
	Super::BeginPlay();

	SpawnSystem = GetOwner()->FindComponentByClass<USpawnSystemComponent>();
	Selection = GetOwner()->FindComponentByClass<USelectionComponent>();
	PlacementTool = GetOwner()->FindComponentByClass<UStagePlacementToolComponent>();
	ensureMsgf(SpawnSystem && Selection, TEXT("%s: UStageSceneComponent needs USpawnSystemComponent and USelectionComponent on the same actor; scenes cannot be loaded."),
		*GetNameSafe(GetOwner()));

	const FString ScenesDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("StageCraft"), TEXT("Scenes"));
	Store = MakeShared<FStageSceneStore, ESPMode::ThreadSafe>(ScenesDirectory);
	// A folder of its own: autosaves never appear among, or overwrite, the scenes the user saved.
	AutosaveStore = MakeShared<FStageSceneStore, ESPMode::ThreadSafe>(FPaths::Combine(ScenesDirectory, TEXT("Autosave")));

	if (UStageSessionSubsystem* Session = GetSession())
	{
		Session->OnDirtyChanged.AddUniqueDynamic(this, &ThisClass::HandleDirtyChanged);
		BoundSession = Session;
		HandleDirtyChanged(Session->IsDirty());
	}
}

void UStageSceneComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// A task still running finds this component gone (weak pointer) and drops its result; a save in progress still completes on disk.
	bOperationInFlight = false;
	bAutosaveInFlight = false;
	if (UStageSessionSubsystem* Session = BoundSession.Get())
	{
		Session->OnDirtyChanged.RemoveDynamic(this, &ThisClass::HandleDirtyChanged);
	}
	BoundSession.Reset();
	if (const UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(AutosaveTimer);
	}
	PlacementEvaluator.Unbind();
	Super::EndPlay(EndPlayReason);
}

UStageSessionSubsystem* UStageSceneComponent::GetSession() const
{
	return UWorld::GetSubsystem<UStageSessionSubsystem>(GetWorld());
}

TArray<FString> UStageSceneComponent::GetSceneNames() const
{
	return Store.IsValid() ? Store->ListScenes() : TArray<FString>();
}

bool UStageSceneComponent::SceneExists(const FString& Name) const
{
	return Store.IsValid() && Store->SceneExists(Name);
}

FString UStageSceneComponent::GetScenesDirectory() const
{
	return Store.IsValid() ? Store->GetDirectory() : FString();
}

// --- Save ---

FStageSceneDocument UStageSceneComponent::CaptureStage(const FString& Name) const
{
	FStageSceneDocument Document;
	Document.Name = Name;
	Document.LevelName = UWorld::RemovePIEPrefix(GetWorld()->GetMapName());
	Document.SavedAtUtc = FDateTime::UtcNow();

	const TArray<AModularBaseActor*> PlacedItems = GetSession()->GetPlacedItems();
	Document.Items.Reserve(PlacedItems.Num());
	for (const AModularBaseActor* Item : PlacedItems)
	{
		// An item with no catalog entry could never be restored; it is not part of a scene.
		if (Item && Item->GetItemData() && !Item->IsActorBeingDestroyed())
		{
			Document.Items.Add(Item->CaptureSnapshot());
		}
	}

	if (const UStageEnvironmentSubsystem* Environment = UWorld::GetSubsystem<UStageEnvironmentSubsystem>(GetWorld()))
	{
		Document.Environment = Environment->GetSettings();
	}
	return Document;
}

EStageSceneResult UStageSceneComponent::SaveScene(const FString& Name)
{
	if (bOperationInFlight)
	{
		return EStageSceneResult::Busy;
	}
	if (!FStageSceneStore::IsValidSceneName(Name))
	{
		return EStageSceneResult::InvalidName;
	}
	const UStageSessionSubsystem* Session = GetSession();
	if (!Session || !Store.IsValid())
	{
		return EStageSceneResult::Unavailable;
	}

	// Captured now, on the game thread: the file holds the stage as it was when the user pressed Save.
	FStageSceneDocument Document = CaptureStage(Name);
	const uint64 EditSerial = Session->GetEditSerial();
	const int32 ItemCount = Document.Items.Num();
	bOperationInFlight = true;

	UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis = TWeakObjectPtr<UStageSceneComponent>(this), SceneStore = Store, Document = MoveTemp(Document), EditSerial, ItemCount]()
	{
		const EStageSceneResult Result = SceneStore->Save(Document);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Result, Name = Document.Name, ItemCount, EditSerial]()
		{
			if (UStageSceneComponent* This = WeakThis.Get())
			{
				This->FinishSave(Result, Name, ItemCount, EditSerial);
			}
		});
	});
	return EStageSceneResult::Success;
}

void UStageSceneComponent::FinishSave(EStageSceneResult Result, const FString& Name, int32 ItemCount, uint64 EditSerialAtCapture)
{
	bOperationInFlight = false;

	FStageSceneOperationResult Outcome;
	Outcome.Operation = EStageSceneOperation::Save;
	Outcome.Result = Result;
	Outcome.SceneName = Name;
	Outcome.ItemCount = ItemCount;

	if (Result != EStageSceneResult::Success)
	{
		Outcome.Message = DescribeFailure(EStageSceneOperation::Save, Result, Name);
		Broadcast(Outcome);
		return;
	}

	if (UStageSessionSubsystem* Session = GetSession())
	{
		Session->SetSceneName(Name);
		// Edits made while the file was being written are not in it, so they stay unsaved.
		if (Session->GetEditSerial() == EditSerialAtCapture)
		{
			Session->MarkClean();
		}
	}

	// The scene file now holds what the autosave held. The old name's autosave goes too (an untitled stage saved under a name).
	DeleteAutosaveFile(Name);
	if (!LastAutosaveName.IsEmpty() && !LastAutosaveName.Equals(Name, ESearchCase::IgnoreCase))
	{
		DeleteAutosaveFile(LastAutosaveName);
	}
	LastAutosaveName.Reset();
	Outcome.Message = FText::Format(LOCTEXT("Saved", "Saved scene \"{0}\" ({1} items)"), FText::FromString(Name), FText::AsNumber(ItemCount));
	Broadcast(Outcome);
}

// --- Load ---

EStageSceneResult UStageSceneComponent::LoadScene(const FString& Name)
{
	return StartLoad(ELoadSource::SavedScene, Name);
}

EStageSceneResult UStageSceneComponent::RecoverAutosave(const FString& Name)
{
	return StartLoad(ELoadSource::Autosave, Name);
}

EStageSceneResult UStageSceneComponent::StartLoad(ELoadSource Source, const FString& Name)
{
	if (bOperationInFlight)
	{
		return EStageSceneResult::Busy;
	}
	if (!FStageSceneStore::IsValidSceneName(Name))
	{
		return EStageSceneResult::InvalidName;
	}
	const TSharedPtr<const FStageSceneStore, ESPMode::ThreadSafe> SourceStore = Source == ELoadSource::Autosave ? AutosaveStore : Store;
	const UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	const UStageItemSubsystem* Items = GameInstance ? GameInstance->GetSubsystem<UStageItemSubsystem>() : nullptr;
	if (!GetSession() || !SpawnSystem || !Selection || !Items || !SourceStore.IsValid())
	{
		return EStageSceneResult::Unavailable;
	}
	// Every item is checked against the catalog when applied; before the catalog exists, every item would be "unknown".
	if (!Items->IsCatalogLoaded())
	{
		return EStageSceneResult::CatalogNotReady;
	}
	if (!SourceStore->SceneExists(Name))
	{
		return EStageSceneResult::NotFound;
	}

	bOperationInFlight = true;
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis = TWeakObjectPtr<UStageSceneComponent>(this), SceneStore = SourceStore, Name, Source]()
	{
		FStageSceneDocument Document;
		FStageSceneParseReport Report;
		const EStageSceneResult Result = SceneStore->Load(Name, Document, Report);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Result, Document = MoveTemp(Document), Report, Source]()
		{
			if (UStageSceneComponent* This = WeakThis.Get())
			{
				This->FinishLoad(Result, Document, Report, Source);
			}
		});
	});
	return EStageSceneResult::Success;
}

void UStageSceneComponent::FinishLoad(EStageSceneResult Result, const FStageSceneDocument& Document, const FStageSceneParseReport& Report, ELoadSource Source)
{
	bOperationInFlight = false;

	if (Result != EStageSceneResult::Success)
	{
		FStageSceneOperationResult Outcome;
		Outcome.Operation = EStageSceneOperation::Load;
		Outcome.Result = Result;
		Outcome.SceneName = Document.Name;
		Outcome.Message = DescribeFailure(EStageSceneOperation::Load, Result, Document.Name);
		Broadcast(Outcome);
		return;
	}
	Broadcast(ApplyDocument(Document, Report, Source));
}

FStageSceneOperationResult UStageSceneComponent::ApplyDocument(const FStageSceneDocument& Document, const FStageSceneParseReport& Report, ELoadSource Source)
{
	FStageSceneOperationResult Outcome;
	Outcome.Operation = EStageSceneOperation::Load;
	Outcome.SceneName = Document.Name;

	// Re-checked: the world may have changed while the file was being read.
	UStageSessionSubsystem* Session = GetSession();
	const UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	const UStageItemSubsystem* ItemSubsystem = GameInstance ? GameInstance->GetSubsystem<UStageItemSubsystem>() : nullptr;
	if (!Session || !SpawnSystem || !Selection || !ItemSubsystem || !ItemSubsystem->IsCatalogLoaded())
	{
		Outcome.Result = EStageSceneResult::Unavailable;
		Outcome.Message = DescribeFailure(EStageSceneOperation::Load, Outcome.Result, Document.Name);
		return Outcome;
	}

	const FString CurrentLevel = UWorld::RemovePIEPrefix(GetWorld()->GetMapName());
	if (!Document.LevelName.IsEmpty() && Document.LevelName != CurrentLevel)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Scene \"%s\" was saved in level %s and is being loaded into %s; item positions refer to the original level."),
			*Document.Name, *Document.LevelName, *CurrentLevel);
	}

	// Catalog-only: a scene may name only items the Library offers. Anything else in the file is not spawned.
	TMap<FSoftObjectPath, UBaseItemData*> Catalog;
	for (UBaseItemData* Item : ItemSubsystem->GetCatalog())
	{
		if (Item)
		{
			Catalog.Add(FSoftObjectPath(Item), Item);
		}
	}

	// Replace the stage. Not recorded: the history is cleared below, because its steps name the items being removed.
	if (PlacementTool)
	{
		PlacementTool->EnterSelectMode();
	}
	Selection->ClearSelection();
	for (AModularBaseActor* Existing : Session->GetPlacedItems())
	{
		SpawnSystem->TryDeleteActor(Existing);
	}

	int32 UnknownItems = 0;
	int32 RefusedItems = 0;
	for (const FStageItemSnapshot& Snapshot : Document.Items)
	{
		UBaseItemData* const* Item = Catalog.Find(Snapshot.Item.ToSoftObjectPath());
		if (!Item)
		{
			++UnknownItems;
			continue;
		}
		// A pre-check of the item itself (ownership, item count). A span's full quantity is only known once its saved state is
		// applied, so the spawn system's validator still checks that, and reports it, when the item is restored.
		if (PlacementEvaluator.IsBound() && !PlacementEvaluator.Execute(**Item, 1).IsSuccess())
		{
			++RefusedItems;
			continue;
		}
		if (SpawnSystem->RestoreItem(Snapshot))
		{
			++Outcome.ItemCount;
		}
		else
		{
			++RefusedItems;
		}
	}

	// Before MarkClean: setting the environment marks the session dirty like any edit, and it is part of what was loaded.
	if (UStageEnvironmentSubsystem* Environment = UWorld::GetSubsystem<UStageEnvironmentSubsystem>(GetWorld()))
	{
		Environment->SetSettings(Document.Environment);
	}

	if (UStageEditHistorySubsystem* History = UWorld::GetSubsystem<UStageEditHistorySubsystem>(GetWorld()))
	{
		History->ClearHistory();
	}
	LastAutosaveSerial = Session->GetEditSerial();

	if (Source == ELoadSource::Autosave)
	{
		// The autosave is named after its scene; the scene file still lacks these changes, so the stage stays unsaved.
		const bool bUntitled = Document.Name.Equals(UntitledAutosaveName, ESearchCase::IgnoreCase);
		Session->SetSceneName(bUntitled ? FString() : Document.Name);
		LastAutosaveName = Document.Name;
		Outcome.Result = EStageSceneResult::Success;
		Outcome.SkippedCount = Report.InvalidItems + UnknownItems + RefusedItems;
		Outcome.Message = FText::Format(LOCTEXT("Recovered", "Recovered the autosave of \"{0}\" ({1} items, {2} skipped). Save it to keep the changes."),
			bUntitled ? LOCTEXT("UntitledStage", "Untitled stage") : FText::FromString(Document.Name), FText::AsNumber(Outcome.ItemCount), FText::AsNumber(Outcome.SkippedCount));
		UE_LOG(LogStageCraft, Log, TEXT("Autosave \"%s\" recovered: %d placed, %d refused, %d unknown, %d invalid."), *Document.Name,
			Outcome.ItemCount, RefusedItems, UnknownItems, Report.InvalidItems);
		return Outcome;
	}

	Session->SetSceneName(Document.Name);
	Session->MarkClean();

	Outcome.Result = EStageSceneResult::Success;
	Outcome.SkippedCount = Report.InvalidItems + UnknownItems + RefusedItems;
	Outcome.Message = Outcome.SkippedCount == 0
		? FText::Format(LOCTEXT("Loaded", "Loaded scene \"{0}\" ({1} items)"), FText::FromString(Document.Name), FText::AsNumber(Outcome.ItemCount))
		: FText::Format(LOCTEXT("LoadedSkipped", "Loaded scene \"{0}\": {1} items placed, {2} skipped ({3} not allowed now, {4} not in the Library, {5} damaged)"),
			FText::FromString(Document.Name), FText::AsNumber(Outcome.ItemCount), FText::AsNumber(Outcome.SkippedCount),
			FText::AsNumber(RefusedItems), FText::AsNumber(UnknownItems), FText::AsNumber(Report.InvalidItems));

	UE_LOG(LogStageCraft, Log, TEXT("Scene \"%s\" loaded: %d placed, %d refused, %d unknown, %d invalid, %d ids repaired."), *Document.Name,
		Outcome.ItemCount, RefusedItems, UnknownItems, Report.InvalidItems, Report.RepairedIds);
	return Outcome;
}

// --- Autosave ---

const TCHAR* UStageSceneComponent::UntitledAutosaveName = TEXT("(Untitled)");

TArray<FString> UStageSceneComponent::GetAutosaveNames() const
{
	return AutosaveStore.IsValid() ? AutosaveStore->ListScenes() : TArray<FString>();
}

FString UStageSceneComponent::GetAutosaveName() const
{
	const UStageSessionSubsystem* Session = GetSession();
	return Session && !Session->GetSceneName().IsEmpty() ? Session->GetSceneName() : FString(UntitledAutosaveName);
}

void UStageSceneComponent::HandleDirtyChanged(bool bIsDirty)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	// A looping timer only while there is something to protect: a clean stage costs nothing.
	if (bIsDirty && AutosaveIntervalSeconds > 0.f)
	{
		if (!World->GetTimerManager().IsTimerActive(AutosaveTimer))
		{
			World->GetTimerManager().SetTimer(AutosaveTimer, this, &ThisClass::Autosave, FMath::Max(AutosaveIntervalSeconds, 10.f), /*bLoop*/ true);
		}
	}
	else
	{
		World->GetTimerManager().ClearTimer(AutosaveTimer);
	}
}

void UStageSceneComponent::Autosave()
{
	const UStageSessionSubsystem* Session = GetSession();
	// A user save or load in progress owns the stage; the next interval tries again.
	if (!Session || !Session->IsDirty() || bOperationInFlight || bAutosaveInFlight || !AutosaveStore.IsValid()
		|| Session->GetEditSerial() == LastAutosaveSerial)
	{
		return;
	}

	const FString FileName = GetAutosaveName();
	FStageSceneDocument Document = CaptureStage(FileName);
	const uint64 EditSerial = Session->GetEditSerial();
	bAutosaveInFlight = true;

	UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis = TWeakObjectPtr<UStageSceneComponent>(this), SceneStore = AutosaveStore, Document = MoveTemp(Document), EditSerial]()
	{
		const EStageSceneResult Result = SceneStore->Save(Document);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Result, FileName = Document.Name, EditSerial]()
		{
			if (UStageSceneComponent* This = WeakThis.Get())
			{
				This->FinishAutosave(Result, FileName, EditSerial);
			}
		});
	});
}

void UStageSceneComponent::FinishAutosave(EStageSceneResult Result, const FString& FileName, uint64 EditSerialAtCapture)
{
	bAutosaveInFlight = false;
	if (Result != EStageSceneResult::Success)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Autosave of \"%s\" failed (%s); the next one tries again."), *FileName, *UEnum::GetValueAsString(Result));
		return;
	}

	// Saved or discarded while this was being written: the file it just wrote is already obsolete.
	const UStageSessionSubsystem* Session = GetSession();
	if (!Session || !Session->IsDirty())
	{
		DeleteAutosaveFile(FileName);
		return;
	}

	LastAutosaveSerial = EditSerialAtCapture;
	LastAutosaveName = FileName;
	UE_LOG(LogStageCraft, Log, TEXT("Autosaved the stage to %s."), *AutosaveStore->GetScenePath(FileName));
}

void UStageSceneComponent::DiscardAutosave()
{
	DeleteAutosaveFile(GetAutosaveName());
	if (!LastAutosaveName.IsEmpty())
	{
		DeleteAutosaveFile(LastAutosaveName);
	}
	LastAutosaveName.Reset();
}

void UStageSceneComponent::DeleteAutosaveFile(const FString& FileName) const
{
	if (AutosaveStore.IsValid() && AutosaveStore->SceneExists(FileName))
	{
		AutosaveStore->Delete(FileName);
	}
}

// --- Delete ---

EStageSceneResult UStageSceneComponent::DeleteScene(const FString& Name)
{
	if (bOperationInFlight)
	{
		return EStageSceneResult::Busy;
	}
	if (!Store.IsValid())
	{
		return EStageSceneResult::Unavailable;
	}

	// A single small file: deleting it inline costs less than a round trip through a task.
	FStageSceneOperationResult Outcome;
	Outcome.Operation = EStageSceneOperation::Delete;
	Outcome.SceneName = Name;
	Outcome.Result = Store->Delete(Name);
	Outcome.Message = Outcome.IsSuccess()
		? FText::Format(LOCTEXT("Deleted", "Deleted scene \"{0}\""), FText::FromString(Name))
		: DescribeFailure(EStageSceneOperation::Delete, Outcome.Result, Name);
	Broadcast(Outcome);
	return Outcome.Result;
}

// --- Reporting ---

void UStageSceneComponent::Broadcast(const FStageSceneOperationResult& Result)
{
	UE_LOG(LogStageCraft, Log, TEXT("Scene %s \"%s\": %s. %s"), *UEnum::GetValueAsString(Result.Operation), *Result.SceneName,
		*UEnum::GetValueAsString(Result.Result), *Result.Message.ToString());
	OnSceneOperationFinished.Broadcast(Result);
}

FText UStageSceneComponent::DescribeFailure(EStageSceneOperation Operation, EStageSceneResult Result, const FString& Name)
{
	const FText SceneName = FText::FromString(Name);
	switch (Result)
	{
	case EStageSceneResult::InvalidName:
		return LOCTEXT("InvalidName", "Use 1-64 letters, digits, spaces, '-', '_' or brackets for a scene name.");
	case EStageSceneResult::NotFound:
		return FText::Format(LOCTEXT("NotFound", "There is no saved scene \"{0}\"."), SceneName);
	case EStageSceneResult::Corrupt:
		return FText::Format(LOCTEXT("Corrupt", "Scene \"{0}\" could not be read: the file is damaged or not a StageCraft scene. It was left unchanged."), SceneName);
	case EStageSceneResult::VersionMismatch:
		return FText::Format(LOCTEXT("Version", "Scene \"{0}\" was saved by an incompatible version of StageCraft."), SceneName);
	case EStageSceneResult::WriteFailed:
		return Operation == EStageSceneOperation::Delete
			? FText::Format(LOCTEXT("DeleteFailed", "Scene \"{0}\" could not be deleted."), SceneName)
			: FText::Format(LOCTEXT("WriteFailed", "Scene \"{0}\" could not be written to disk."), SceneName);
	case EStageSceneResult::Busy:
		return LOCTEXT("Busy", "Another scene is still being saved or loaded. Try again in a moment.");
	case EStageSceneResult::CatalogNotReady:
		return LOCTEXT("CatalogNotReady", "The Library is still loading. Try again in a moment.");
	case EStageSceneResult::Unavailable:
		return LOCTEXT("Unavailable", "Scenes are not available in this level.");
	case EStageSceneResult::Success:
	default:
		return FText::GetEmpty();
	}
}

#undef LOCTEXT_NAMESPACE
