// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mvr/StageMvrImportSubsystem.h"

#include "Actors/ModularBaseActor.h"
#include "Async/Async.h"
#include "Components/SpawnSystemComponent.h"
#include "Data/LightingFixtureData.h"
#include "Data/StageSpanData.h"
#include "Engine/AssetManager.h"
#include "Engine/GameInstance.h"
#include "Engine/StreamableManager.h"
#include "Fixtures/StageFixtureLibrarySubsystem.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Mvr/StageMvrArchive.h"
#include "Mvr/StageMvrImportPlan.h"
#include "Mvr/StageMvrPlacement.h"
#include "Data/StageParameterTypes.h"
#include "Tasks/Task.h"
#include "TimerManager.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageMvrImportSubsystem)

#define LOCTEXT_NAMESPACE "StageMvrImport"

namespace StageMvrImportPrivate
{
	FText FileLabel(const FString& File)
	{
		return FText::FromString(FPaths::GetCleanFilename(File));
	}

	/** A library fixture mode as the planner reads it. */
	StageMvrImport::FStageMvrModeTarget MakeTarget(UBaseItemData& Item)
	{
		StageMvrImport::FStageMvrModeTarget Target;
		Target.Item = &Item;
		Target.Label = Item.DisplayName.ToString();
		if (const ULightingFixtureData* Fixture = Cast<ULightingFixtureData>(&Item))
		{
			float Min = 0.f;
			float Max = 0.f;
			const bool bPan = Fixture->GetAttributeRange(StageCraftTags::Attribute_Pan, Min, Max);
			Target.PanRange = FVector2D(Min, Max);
			const bool bTilt = Fixture->GetAttributeRange(StageCraftTags::Attribute_Tilt, Min, Max);
			Target.TiltRange = FVector2D(Min, Max);
			Target.bHasPanTilt = Fixture->bHasPanTilt && bPan && bTilt;
			Target.PivotOffset = Fixture->PanPivotOffset + Fixture->TiltPivotOffset;
		}
		return Target;
	}

	FString TypeLabel(const FStageFixtureImportReview& Review, const FString& File)
	{
		if (Review.Name.IsEmpty())
		{
			return File;
		}
		return Review.Manufacturer.IsEmpty() ? Review.Name : Review.Manufacturer + TEXT(" ") + Review.Name;
	}
}

void UStageMvrImportSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UStageFixtureLibrarySubsystem>();
}

void UStageMvrImportSubsystem::Deinitialize()
{
	if (Job.IsValid() && !Job->IsFinished())
	{
		Job->Abort(LOCTEXT("Closing", "StageCraft is closing."));
	}
	Reset();
	OnChanged.Clear();
	OnFinished.Clear();
	Super::Deinitialize();
}

bool UStageMvrImportSubsystem::IsMvrFile(const FString& File)
{
	return FPaths::GetExtension(File).Equals(TEXT("mvr"), ESearchCase::IgnoreCase);
}

void UStageMvrImportSubsystem::SetState(EStageMvrImportState NewState)
{
	State = NewState;
	OnChanged.Broadcast();
}

void UStageMvrImportSubsystem::Reset()
{
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().ClearTimer(StepTimer);
	}
	if (LoadHandle.IsValid())
	{
		LoadHandle->CancelHandle();
		LoadHandle.Reset();
	}
	Job.Reset();
	Archive.Reset();
	Placement = FPlacementRequest();
	Review = FStageMvrImportReview();
	ReadingFile.Reset();
	++ReadSerial;
	State = EStageMvrImportState::Idle;
}

TOptional<FText> UStageMvrImportSubsystem::RequestRead(const FString& Path)
{
	using namespace StageMvrImportPrivate;

	FString File = FPaths::ConvertRelativePathToFull(Path.TrimStartAndEnd());
	FPaths::NormalizeFilename(File);
	if (!IsMvrFile(File))
	{
		return FText::Format(LOCTEXT("NotMvr", "{0} is not an MVR file (.mvr)."), FileLabel(File));
	}
	if (State != EStageMvrImportState::Idle)
	{
		return LOCTEXT("Busy", "Another MVR file is open. Import or cancel it first.");
	}
	const int64 Size = IFileManager::Get().FileSize(*File);
	const StageMvr::FLimits Limits;
	if (Size < 0)
	{
		return FText::Format(LOCTEXT("NotFound", "{0} was not found."), FileLabel(File));
	}
	if (Size > Limits.Zip.MaxArchiveBytes)
	{
		return FText::Format(LOCTEXT("TooLarge", "{0} is larger than {1} MB, the most an MVR file may be."), FileLabel(File),
			FText::AsNumber(Limits.Zip.MaxArchiveBytes / (1024 * 1024)));
	}

	const uint64 Serial = ++ReadSerial;
	ReadingFile = File;
	SetState(EStageMvrImportState::Reading);
	UE_LOG(LogStageCraft, Log, TEXT("MVR import: reading %s (%lld bytes) on a worker."), *File, Size);
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis = TWeakObjectPtr<UStageMvrImportSubsystem>(this), Serial, File, MaxBytes = Limits.Zip.MaxArchiveBytes]()
	{
		TSharedPtr<FStageMvrArchive> Result = MakeShared<FStageMvrArchive>();
		TArray<uint8> Bytes;
		const int64 OnDisk = IFileManager::Get().FileSize(*File);
		if (OnDisk >= 0 && OnDisk <= MaxBytes && FFileHelper::LoadFileToArray(Bytes, *File))
		{
			*Result = StageMvrArchive::Read(MoveTemp(Bytes));
		}
		else
		{
			Result->Parsed.Errors.Add(TEXT("The file could not be read."));
		}
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Serial, Result = MoveTemp(Result)]() mutable
		{
			if (UStageMvrImportSubsystem* Self = WeakThis.Get())
			{
				Self->FinishRead(Serial, MoveTemp(Result));
			}
		});
	});
	return {};
}

void UStageMvrImportSubsystem::FinishRead(uint64 Serial, TSharedPtr<FStageMvrArchive> InArchive)
{
	if (Serial != ReadSerial || State != EStageMvrImportState::Reading || !InArchive.IsValid())
	{
		return;
	}
	Archive = MoveTemp(InArchive);
	Review = MakeReview(*Archive);
	UE_LOG(LogStageCraft, Log, TEXT("MVR import: %s read in %.1f ms (archive %lld bytes, largest entry %lld bytes): %s; %s."), *Review.SourceFile, Archive->ReadMs,
		Archive->ArchiveBytes, Archive->LargestEntryBytes, *Review.Description, Review.CanImport() ? TEXT("waiting for review") : TEXT("refused"));
	for (const FString& Error : Review.Errors)
	{
		UE_LOG(LogStageCraft, Log, TEXT("MVR import: error: %s"), *Error);
	}
	SetState(EStageMvrImportState::Review);
}

FStageMvrImportReview UStageMvrImportSubsystem::MakeReview(const FStageMvrArchive& Read) const
{
	using namespace StageMvrImportPrivate;

	FStageMvrImportReview Out;
	Out.ReviewId = FGuid::NewGuid();
	Out.SourceFile = ReadingFile;
	Out.Errors = Read.Parsed.Errors;
	Out.Warnings = Read.Parsed.Warnings;
	Out.Warnings.Append(Read.GeometryProblems);
	if (!Read.Parsed.IsValid())
	{
		return Out;
	}
	const FStageMvrScene& Scene = Read.Parsed.Scene;
	Out.Description = StageMvr::Describe(Scene);
	Out.Fixtures = Scene.Count(EStageMvrObjectKind::Fixture);
	Out.Trusses = Scene.Count(EStageMvrObjectKind::Truss);
	Out.FocusPoints = Scene.Count(EStageMvrObjectKind::FocusPoint);
	Out.Layers = Scene.Layers.Num();
	Out.OtherObjects = Scene.Count(EStageMvrObjectKind::SceneObject) + Scene.Count(EStageMvrObjectKind::Support)
		+ Scene.Count(EStageMvrObjectKind::VideoScreen) + Scene.Count(EStageMvrObjectKind::Projector);

	const UStageFixtureLibrarySubsystem* Library = GetGameInstance() ? GetGameInstance()->GetSubsystem<UStageFixtureLibrarySubsystem>() : nullptr;
	TMap<FString, TSet<FString>> UsableModes;
	for (const FString& Spec : Scene.GetGdtfSpecs())
	{
		FStageMvrTypeLine& Line = Out.Types.AddDefaulted_GetRef();
		Line.File = Spec;
		Line.Label = Spec;
		const TSharedPtr<UStageFixtureLibrarySubsystem::FParsedFile>* File = Read.GdtfFiles.Find(Spec.ToLower());
		if (!File || !File->IsValid() || !Library)
		{
			Line.bUsable = false;
			Line.Problem = TEXT("the GDTF file is not in the MVR file");
			continue;
		}
		const FStageFixtureImportReview Gdtf = Library->DescribeParsedFile(**File);
		Line.Label = TypeLabel(Gdtf, Spec);
		Line.bInLibrary = Gdtf.bReplaces;
		Line.bUsable = Gdtf.bCanImport;
		if (!Gdtf.bCanImport)
		{
			Line.Problem = Gdtf.Errors.IsEmpty() ? TEXT("no mode StageCraft can use") : Gdtf.Errors[0];
			continue;
		}
		TSet<FString>& Modes = UsableModes.Add(Spec.ToLower());
		for (const FStageFixtureModeSummary& Mode : Gdtf.Modes)
		{
			if (Mode.bUsable)
			{
				Modes.Add(Mode.Name.ToLower());
			}
		}
	}

	TMap<FString, TSet<FString>> MissingModes;
	for (const FStageMvrObject& Object : Scene.Objects)
	{
		if (Object.Kind != EStageMvrObjectKind::Fixture || Object.MultipatchOf.IsValid())
		{
			continue;
		}
		const FString Spec = Object.GdtfSpec.ToLower();
		if (FStageMvrTypeLine* Line = Out.Types.FindByPredicate([&Spec](const FStageMvrTypeLine& Entry) { return Entry.File.ToLower() == Spec; }))
		{
			++Line->Fixtures;
		}
		const TSet<FString>* Modes = Spec.IsEmpty() ? nullptr : UsableModes.Find(Spec);
		if (!Modes || !Modes->Contains(Object.GdtfMode.ToLower()))
		{
			++Out.GenericFixtures;
			if (Modes)
			{
				MissingModes.FindOrAdd(Spec).Add(Object.GdtfMode);
			}
		}
	}
	for (FStageMvrTypeLine& Line : Out.Types)
	{
		if (const TSet<FString>* Missing = MissingModes.Find(Line.File.ToLower()))
		{
			Line.Problem = FString::Printf(TEXT("no usable mode %s"), *FString::JoinBy(*Missing, TEXT(", "), [](const FString& Mode) { return TEXT("\"") + Mode + TEXT("\""); }));
		}
	}
	if (Out.Fixtures == 0 && Out.Trusses == 0)
	{
		Out.Errors.Add(TEXT("The file has no fixtures or trusses to place."));
	}
	return Out;
}

bool UStageMvrImportSubsystem::CancelReview(const FGuid& ReviewId)
{
	if (State != EStageMvrImportState::Review || Review.ReviewId != ReviewId)
	{
		return false;
	}
	UE_LOG(LogStageCraft, Log, TEXT("MVR import: %s cancelled."), *Review.SourceFile);
	Reset();
	OnChanged.Broadcast();
	return true;
}

void UStageMvrImportSubsystem::GetProgress(int32& OutDone, int32& OutTotal) const
{
	OutDone = Job.IsValid() ? Job->GetDone() : 0;
	OutTotal = Job.IsValid() ? Job->GetTotal() : 0;
}

FStageEconomyResultInfo UStageMvrImportSubsystem::StartImport(const FGuid& ReviewId, FPlacementRequest Request)
{
	using namespace StageMvrImportPrivate;

	if (State != EStageMvrImportState::Review || Review.ReviewId != ReviewId || !Archive.IsValid())
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoReview", "That MVR file is no longer waiting to be imported."));
	}
	if (!Review.CanImport())
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, FText::Format(LOCTEXT("Refused", "{0} cannot be imported: {1}"),
			FileLabel(Review.SourceFile), FText::FromString(Review.Errors.IsEmpty() ? FString() : Review.Errors[0])));
	}
	USpawnSystemComponent* Spawner = Request.Spawner.Get();
	if (!Spawner)
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoSpawner", "Placing is not available in this level."));
	}

	// Fixture types first: a type the library has is reused, a new one is added (outside the stage's undo, as any GDTF import).
	StageMvrImport::FInputs Inputs;
	UStageFixtureLibrarySubsystem* Library = GetGameInstance() ? GetGameInstance()->GetSubsystem<UStageFixtureLibrarySubsystem>() : nullptr;
	for (TPair<FString, TSharedPtr<UStageFixtureLibrarySubsystem::FParsedFile>>& File : Archive->GdtfFiles)
	{
		if (!Library || !File.Value.IsValid())
		{
			continue;
		}
		const FStageFixtureImportOutcome Added = Library->AddEmbeddedFile(*File.Value);
		if (!Added.IsSuccess() && Added.Result != EStageFixtureImportResult::AlreadyInLibrary)
		{
			UE_LOG(LogStageCraft, Log, TEXT("MVR import: %s not added: %s"), *File.Key, *Added.Message.ToString());
			continue;
		}
		TMap<FString, StageMvrImport::FStageMvrModeTarget>& Modes = Inputs.Modes.Add(File.Key);
		for (ULightingFixtureData* Mode : Added.Modes)
		{
			if (Mode)
			{
				Modes.Add(Mode->DMXModeName.ToString().ToLower(), MakeTarget(*Mode));
			}
		}
	}
	if (UBaseItemData* Generic = Request.GenericFixture.Get())
	{
		Inputs.Fallback = MakeTarget(*Generic);
	}
	Inputs.TrussItem = Request.TrussItem;
	Inputs.GeometryBounds = Archive->GeometryBounds;
	Inputs.UsedFixtureIds = Request.UsedFixtureIds;
	Inputs.ExistingMvrIds = Request.ExistingMvrIds;

	StageMvrImport::FPlan Plan = StageMvrImport::Build(Archive->Parsed.Scene, Inputs);
	UE_LOG(LogStageCraft, Log, TEXT("MVR import: plan for %s: %d fixtures (%d generic, %d renumbered, %d aimed, %d clamped), %d truss runs; skipped %d fixtures, %d trusses, %d already on the stage; %d other objects not placed."),
		*Review.SourceFile, Plan.Fixtures.Num(), Plan.FallbackFixtures, Plan.RenumberedFixtures, Plan.AimedFixtures, Plan.ClampedAims, Plan.Trusses.Num(),
		Plan.SkippedFixtures, Plan.SkippedTrusses, Plan.SkippedExisting, Plan.NotPlacedObjects);
	if (Plan.Num() == 0)
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, Plan.SkippedExisting > 0
			? LOCTEXT("AllThere", "Everything in this file is already on the stage.")
			: LOCTEXT("NothingToPlace", "Nothing in this file can be placed."));
	}

	// The scene, its GDTF bytes (now in the library) and the geometry are no longer needed: only the plan is kept while placing.
	Archive.Reset();
	Review.Warnings.Append(Plan.Notes);

	TArray<FPrimaryAssetId> AssetIds;
	UAssetManager& AssetManager = UAssetManager::Get();
	const auto AddItem = [&AssetIds, &AssetManager](const UBaseItemData* Item)
	{
		if (Item && Item->RequiresAssetStreaming() && AssetManager.GetPrimaryAssetPath(Item->GetPrimaryAssetId()).IsValid())
		{
			AssetIds.AddUnique(Item->GetPrimaryAssetId());
		}
	};
	if (!Plan.Trusses.IsEmpty())
	{
		AddItem(Plan.TrussItem.Get());
	}
	for (const StageMvrImport::FFixturePlacement& Fixture : Plan.Fixtures)
	{
		if (Fixture.bFallback)
		{
			AddItem(Fixture.Item.Get());
			break;
		}
	}

	FStageMvrPlacementJob::FCheck Check = MoveTemp(Request.Check);
	Job = MakeShared<FStageMvrPlacementJob>(MoveTemp(Plan), *Spawner, MoveTemp(Check));
	Placement = MoveTemp(Request);
	SetState(EStageMvrImportState::Placing);
	if (AssetIds.IsEmpty())
	{
		ScheduleStep();
	}
	else
	{
		TSharedPtr<FStreamableHandle> Handle = AssetManager.LoadPrimaryAssets(AssetIds, { UBaseItemData::GameBundle },
			FStreamableDelegate::CreateWeakLambda(this, [this]() { LoadHandle.Reset(); ScheduleStep(); }));
		if (State == EStageMvrImportState::Placing && !StepTimer.IsValid())
		{
			LoadHandle = MoveTemp(Handle);
		}
	}
	return FStageEconomyResultInfo::Ok();
}

void UStageMvrImportSubsystem::ScheduleStep()
{
	UGameInstance* GameInstance = GetGameInstance();
	if (!GameInstance || State != EStageMvrImportState::Placing)
	{
		return;
	}
	StepTimer = GameInstance->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ThisClass::StepPlacement));
}

void UStageMvrImportSubsystem::StepPlacement()
{
	using namespace StageMvrImportPrivate;

	StepTimer.Invalidate();
	if (State != EStageMvrImportState::Placing || !Job.IsValid())
	{
		return;
	}
	if (!Job->Step(PlacementBudgetSeconds))
	{
		OnChanged.Broadcast();
		ScheduleStep();
		return;
	}

	FStageMvrImportOutcome Outcome;
	Outcome.SourceFile = Review.SourceFile;
	Outcome.Notes = Job->GetPlan().Notes;
	UE_LOG(LogStageCraft, Log, TEXT("MVR import: %s placing %s after %d of %d items in %d frames (%.1f ms placing, %.2f ms per item)."), *Review.SourceFile,
		Job->Succeeded() ? TEXT("finished") : TEXT("refused"), Job->GetDone(), Job->GetTotal(), Job->GetSteps(), Job->GetPlacingSeconds() * 1000.0,
		Job->GetDone() > 0 ? Job->GetPlacingSeconds() * 1000.0 / Job->GetDone() : 0.0);
	if (Job->Succeeded())
	{
		TArray<AModularBaseActor*> Items;
		Items.Reserve(Job->GetPlaced().Num());
		for (const TWeakObjectPtr<AModularBaseActor>& Item : Job->GetPlaced())
		{
			if (AModularBaseActor* Actor = Item.Get())
			{
				Items.Add(Actor);
			}
		}
		TArray<TPair<FString, TArray<int32>>> LayerGroups;
		const StageMvrImport::FPlan& Plan = Job->GetPlan();
		for (TPair<int32, TArray<int32>>& Layer : Job->GetFixtureIdsByLayer())
		{
			if (Plan.LayerNames.IsValidIndex(Layer.Key))
			{
				LayerGroups.Emplace(Plan.LayerNames[Layer.Key], MoveTemp(Layer.Value));
			}
		}
		LayerGroups.Sort([](const TPair<FString, TArray<int32>>& A, const TPair<FString, TArray<int32>>& B) { return A.Key < B.Key; });
		const FText StepName = FText::Format(LOCTEXT("StepName", "Import {0}"), FileLabel(Review.SourceFile));
		Outcome.Groups = Placement.OnPlaced ? Placement.OnPlaced(Items, LayerGroups, StepName) : 0;
		Outcome.bSucceeded = true;
		Outcome.Fixtures = Plan.Fixtures.Num();
		Outcome.Trusses = Plan.Trusses.Num();
		Outcome.Message = FText::Format(LOCTEXT("Imported", "Imported {0}: {1} {1}|plural(one=fixture,other=fixtures), {2} truss {2}|plural(one=run,other=runs), {3} {3}|plural(one=group,other=groups)."),
			FileLabel(Review.SourceFile), FText::AsNumber(Outcome.Fixtures), FText::AsNumber(Outcome.Trusses), FText::AsNumber(Outcome.Groups));
	}
	else
	{
		Outcome.Message = FText::Format(LOCTEXT("NotImported", "Nothing from {0} was placed: {1}"), FileLabel(Review.SourceFile), Job->GetResult().Message);
	}
	FinishImport(Outcome);
}

void UStageMvrImportSubsystem::FinishImport(const FStageMvrImportOutcome& Outcome)
{
	UE_LOG(LogStageCraft, Log, TEXT("MVR import: %s"), *Outcome.Message.ToString());
	Reset();
	OnFinished.Broadcast(Outcome);
	OnChanged.Broadcast();
}

#undef LOCTEXT_NAMESPACE
