// Copyright Epic Games, Inc. All Rights Reserved.

#include "Vision/StageVisionBomSubsystem.h"

#include "Async/Async.h"
#include "Data/BaseItemData.h"
#include "Data/StageDeckData.h"
#include "Data/StageSpanData.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "IO/StageAtomicFile.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ModularSceneBuilder.h"
#include "Sketch/StageSketchGenerator.h"
#include "Sketch/StageSketchImageImport.h"
#include "Sketch/StageSketchSubsystem.h"
#include "Subsystems/StageItemSubsystem.h"
#include "Tasks/Task.h"
#include "Vision/StageVisionBomBackend.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageVisionBomSubsystem)

#define LOCTEXT_NAMESPACE "StageVisionBomSubsystem"

/** One photo being prepared for the online reading: plain data, touched by the worker until it finishes, then by the game thread. */
struct FStageVisionBomPhotoJob
{
	FString FilePath;
	TArray<uint8> Jpeg;
	FText Error;
};

namespace StageVisionBomPhoto
{
	constexpr int32 JpegQuality = 85;

	/** Reads, checks, resizes and re-encodes the photo. Worker thread; loads nothing that needs the game thread. */
	void Prepare(FStageVisionBomPhotoJob& Job)
	{
		const int64 Size = IFileManager::Get().FileSize(*Job.FilePath);
		if (Size <= 0 || Size > StageSketchImageImport::MaxFileBytes)
		{
			Job.Error = Size <= 0 ? LOCTEXT("Unreadable", "The photo could not be found or read.")
				: FText::Format(LOCTEXT("TooLarge", "The photo is larger than {0} MB."), FText::AsNumber(StageSketchImageImport::MaxFileBytes / (1024 * 1024)));
			return;
		}
		TArray64<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Job.FilePath))
		{
			Job.Error = LOCTEXT("Unreadable", "The photo could not be found or read.");
			return;
		}
		TArray<uint8> BGRA;
		int32 Width = 0;
		int32 Height = 0;
		if (!StageSketchImageImport::DecodeImage(Bytes, BGRA, Width, Height, Job.Error))
		{
			return;
		}
		Bytes.Empty();
		int32 SentWidth = 0;
		int32 SentHeight = 0;
		const TArray<uint8> Sent = StageSketchImageImport::DownscaleBGRA(BGRA, Width, Height, StageSketchImageImport::VisionMaxSide, SentWidth, SentHeight);
		IImageWrapperModule& ImageWrapper = FModuleManager::GetModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const TSharedPtr<IImageWrapper> Jpeg = ImageWrapper.CreateImageWrapper(EImageFormat::JPEG);
		if (!Jpeg.IsValid() || !Jpeg->SetRaw(Sent.GetData(), Sent.Num(), SentWidth, SentHeight, ERGBFormat::BGRA, 8))
		{
			Job.Error = LOCTEXT("EncodeFailed", "The photo could not be prepared for reading.");
			return;
		}
		const TArray64<uint8> Compressed = Jpeg->GetCompressed(JpegQuality);
		Job.Jpeg = TArray<uint8>(Compressed.GetData(), int32(Compressed.Num()));
	}
}

bool UStageVisionBomSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UStageVisionBomSubsystem::Deinitialize()
{
	++Serial;
	if (Backend)
	{
		Backend->Cancel();
		Backend = nullptr;
	}
	if (TestBackend)
	{
		TestBackend->Cancel();
		TestBackend = nullptr;
	}
	OnChanged.Clear();
	Super::Deinitialize();
}

UStageVisionBomBackend* UStageVisionBomSubsystem::GetBackend() const
{
	if (TestBackend)
	{
		return TestBackend;
	}
	if (!Backend)
	{
		UStageVisionBomSubsystem* Self = const_cast<UStageVisionBomSubsystem*>(this);
		Self->Backend = NewObject<UStageClaudeVisionBomBackend>(Self);
	}
	return Backend;
}

bool UStageVisionBomSubsystem::CanInspect(FText& OutReason) const
{
	if (IsBusy())
	{
		OutReason = LOCTEXT("Busy", "A photo is already being read.");
		return false;
	}
	return GetBackend()->IsAvailable(OutReason);
}

bool UStageVisionBomSubsystem::Inspect(const FString& FilePath, FText& OutError)
{
	if (!CanInspect(OutError))
	{
		return false;
	}
	if (!StageSketchImageImport::HasSupportedExtension(FilePath))
	{
		OutError = LOCTEXT("Unsupported", "Choose a PNG or JPEG photo.");
		return false;
	}
	const TSharedRef<FStageVisionBomPhotoJob> Job = MakeShared<FStageVisionBomPhotoJob>();
	Job->FilePath = FilePath;
	// The worker must not load modules; the game thread loads the image codecs first.
	FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

	const uint32 JobSerial = ++Serial;
	State = EStageVisionBomState::Preparing;
	PendingName = FPaths::GetCleanFilename(FilePath);
	Message = FText::Format(LOCTEXT("Preparing", "Preparing {0}…"), FText::FromString(PendingName));
	OnChanged.Broadcast();

	TWeakObjectPtr<UStageVisionBomSubsystem> WeakThis = this;
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis, JobSerial, Job]()
	{
		StageVisionBomPhoto::Prepare(*Job);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, JobSerial, Job]()
		{
			if (UStageVisionBomSubsystem* Self = WeakThis.Get())
			{
				Self->HandlePrepared(JobSerial, Job);
			}
		});
	});
	return true;
}

void UStageVisionBomSubsystem::HandlePrepared(uint32 JobSerial, TSharedRef<FStageVisionBomPhotoJob> Job)
{
	if (JobSerial != Serial || State != EStageVisionBomState::Preparing)
	{
		return;
	}
	if (Job->Jpeg.IsEmpty())
	{
		Fail(Job->Error.IsEmpty() ? LOCTEXT("PrepareFailed", "The photo could not be read.") : Job->Error);
		return;
	}
	if (!SendJpeg(Job->Jpeg, JobSerial))
	{
		Fail(LOCTEXT("SendFailed", "The photo could not be sent to the online model."));
	}
}

bool UStageVisionBomSubsystem::SendJpeg(const TArray<uint8>& Jpeg, uint32 JobSerial)
{
	State = EStageVisionBomState::Reading;
	Message = FText::Format(LOCTEXT("Reading", "Reading {0} with {1}…"), FText::FromString(PendingName), GetBackend()->GetDisplayName());
	TWeakObjectPtr<UStageVisionBomSubsystem> WeakThis = this;
	const bool bStarted = GetBackend()->Read(Jpeg, FOnStageVisionBomRead::CreateWeakLambda(this, [WeakThis, JobSerial](const FStageVisionBomResult& Result)
	{
		if (UStageVisionBomSubsystem* Self = WeakThis.Get())
		{
			Self->HandleRead(Result, JobSerial);
		}
	}));
	if (bStarted && State == EStageVisionBomState::Reading)
	{
		// A backend may answer inside Read (tests); only announce Reading when it has not.
		OnChanged.Broadcast();
	}
	return bStarted;
}

void UStageVisionBomSubsystem::HandleRead(const FStageVisionBomResult& Result, uint32 JobSerial)
{
	if (JobSerial != Serial || State != EStageVisionBomState::Reading)
	{
		return;
	}
	if (Result.Status != FStageVisionBomResult::EStatus::Inventory)
	{
		Fail(Result.Status == FStageVisionBomResult::EStatus::Text
			? FText::Format(LOCTEXT("ModelSaid", "No equipment list: {0}"), FText::FromString(Result.Text))
			: Result.Error);
		return;
	}

	Lines = Result.Lines;
	StageEstimate = Result.Stage;
	ModelNotes = Result.Text;
	SourceName = PendingName;
	TArray<const UBaseItemData*> Catalog;
	const UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	if (const UStageItemSubsystem* Items = GameInstance ? GameInstance->GetSubsystem<UStageItemSubsystem>() : nullptr)
	{
		Catalog.Append(Items->GetCatalog());
	}
	StageVisionBom::MatchCatalog(Lines, Catalog);

	const FStageVisionBomTotals Totals = StageVisionBom::ComputeTotals(Lines);
	State = EStageVisionBomState::Ready;
	Message = FText::Format(LOCTEXT("Read", "Read {0}: {1} {1}|plural(one=line,other=lines), {2} in the catalog, {3} to check{4}."),
		FText::FromString(SourceName), FText::AsNumber(Lines.Num()), FText::AsNumber(Totals.MatchedLines), FText::AsNumber(Totals.LinesToReview),
		Result.Dropped > 0 ? FText::Format(LOCTEXT("Dropped", "; {0} unreadable left out"), FText::AsNumber(Result.Dropped)) : FText::GetEmpty());
	UE_LOG(LogStageCraft, Log, TEXT("Vision Inspector: %s: %d lines (%d matched, %d to check, %d dropped)."), *SourceName, Lines.Num(),
		Totals.MatchedLines, Totals.LinesToReview, Result.Dropped);
	OnChanged.Broadcast();
}

void UStageVisionBomSubsystem::Fail(const FText& Reason)
{
	State = EStageVisionBomState::Failed;
	Message = Reason;
	OnChanged.Broadcast();
}

void UStageVisionBomSubsystem::Cancel()
{
	if (!IsBusy())
	{
		return;
	}
	++Serial;
	GetBackend()->Cancel();
	State = Lines.IsEmpty() ? EStageVisionBomState::Idle : EStageVisionBomState::Ready;
	Message = LOCTEXT("Cancelled", "Reading cancelled.");
	OnChanged.Broadcast();
}

void UStageVisionBomSubsystem::Clear()
{
	++Serial;
	GetBackend()->Cancel();
	Lines.Reset();
	StageEstimate = FStageVisionBomStage();
	ModelNotes.Reset();
	SourceName.Reset();
	State = EStageVisionBomState::Idle;
	Message = FText::GetEmpty();
	OnChanged.Broadcast();
}

bool UStageVisionBomSubsystem::SetLineQuantity(int32 Index, int32 Quantity)
{
	if (IsBusy() || !Lines.IsValidIndex(Index))
	{
		return false;
	}
	const int32 Clamped = FMath::Clamp(Quantity, 1, StageVisionBom::MaxQuantity);
	if (Lines[Index].Quantity != Clamped)
	{
		Lines[Index].Quantity = Clamped;
		OnChanged.Broadcast();
	}
	return true;
}

bool UStageVisionBomSubsystem::RemoveLine(int32 Index)
{
	if (IsBusy() || !Lines.IsValidIndex(Index))
	{
		return false;
	}
	Lines.RemoveAt(Index);
	OnChanged.Broadcast();
	return true;
}

FStageVisionBomTotals UStageVisionBomSubsystem::GetTotals() const
{
	return StageVisionBom::ComputeTotals(Lines);
}

FStageVisionBomLayout UStageVisionBomSubsystem::GetLayout() const
{
	return StageVisionBom::MakeLayout(StageEstimate, Lines.ContainsByPredicate([](const FStageVisionBomLine& Line) { return Line.Kind == EStageVisionBomKind::Deck; }));
}

FString UStageVisionBomSubsystem::MakeCsv() const
{
	return StageVisionBom::ToCsv(Lines);
}

bool UStageVisionBomSubsystem::SaveCsv(const FString& Path, FText& OutError) const
{
	if (Lines.IsEmpty())
	{
		OutError = LOCTEXT("NothingToSave", "There is no bill of materials to save.");
		return false;
	}
	if (!StageAtomicFile::WriteText(Path, MakeCsv()))
	{
		OutError = FText::Format(LOCTEXT("SaveFailed", "Could not write {0}."), FText::FromString(FPaths::GetCleanFilename(Path)));
		return false;
	}
	return true;
}

bool UStageVisionBomSubsystem::MakeBuildPlan(FStageSketchBuildPlan& OutPlan, FText& OutReason) const
{
	OutPlan = FStageSketchBuildPlan();
	if (Lines.IsEmpty())
	{
		OutReason = LOCTEXT("NoBill", "Read a stage photo first.");
		return false;
	}
	const UStageSketchSubsystem* Sketch = UWorld::GetSubsystem<UStageSketchSubsystem>(GetWorld());
	if (!Sketch)
	{
		OutReason = LOCTEXT("NoSketch", "Building is not available in this level.");
		return false;
	}
	const FStageVisionBomLayout Layout = GetLayout();
	const FStageVisionBomBuildInput Input = StageVisionBom::MakeBuildInput(Lines, Layout);

	FStageSketchBuildRules Rules;
	Rules.GridStep = 50.0;
	Rules.bSnapToGrid = true;
	Rules.FloorHeight = 0.0;
	Rules.TrussHeight = Layout.TrussHeightCm;
	Rules.DeckItem = Cast<UStageDeckData>(Sketch->GetItem(EStageSketchElement::Deck));
	Rules.TrussItem = Cast<UStageSpanData>(Sketch->GetItem(EStageSketchElement::Truss));
	OutPlan = StageSketchGenerator::BuildPlan(Input.Strokes, Rules);
	for (const FStageVisionBomPlacement& Placement : Input.Items)
	{
		OutPlan.Items.Add({ Placement.Item, Placement.Transform });
	}
	OutPlan.Warnings.Append(Input.Warnings);
	if (OutPlan.IsEmpty())
	{
		OutReason = OutPlan.Warnings.IsEmpty() ? LOCTEXT("NothingToBuild", "Nothing on this bill is in the catalog.")
			: FText::Format(LOCTEXT("NothingUsable", "Nothing to build. {0}"), OutPlan.Warnings[0]);
		return false;
	}
	return true;
}

void UStageVisionBomSubsystem::ReportBuild(const FStageSketchGenerateResult& Result)
{
	Message = Result.Message;
	OnChanged.Broadcast();
}

#if WITH_DEV_AUTOMATION_TESTS
void UStageVisionBomSubsystem::SetBackendForTesting(UStageVisionBomBackend* InBackend)
{
	if (TestBackend)
	{
		TestBackend->Cancel();
	}
	TestBackend = InBackend;
}

bool UStageVisionBomSubsystem::InspectJpegForTesting(const TArray<uint8>& Jpeg, const FString& Name, FText& OutError)
{
	if (!CanInspect(OutError))
	{
		return false;
	}
	PendingName = Name;
	if (!SendJpeg(Jpeg, ++Serial))
	{
		Fail(LOCTEXT("SendFailed", "The photo could not be sent to the online model."));
		return false;
	}
	return true;
}

void UStageVisionBomSubsystem::MatchCatalogForTesting(TConstArrayView<const UBaseItemData*> Catalog)
{
	StageVisionBom::MatchCatalog(Lines, Catalog);
	OnChanged.Broadcast();
}
#endif

#undef LOCTEXT_NAMESPACE
