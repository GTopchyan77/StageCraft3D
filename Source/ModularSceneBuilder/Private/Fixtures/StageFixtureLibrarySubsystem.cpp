// Copyright Epic Games, Inc. All Rights Reserved.

#include "Fixtures/StageFixtureLibrarySubsystem.h"

#include "Actors/ModularBaseActor.h"
#include "Async/Async.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Fixtures/StageFixtureLibraryRules.h"
#include "Fixtures/StageGdtfFixtureBuilder.h"
#include "Fixtures/StageGdtfMapping.h"
#include "Fixtures/StageGdtfParser.h"
#include "HAL/FileManager.h"
#include "IO/StageAtomicFile.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Subsystems/StageItemSubsystem.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "Tasks/Task.h"
#include "UObject/Package.h"
#include "Engine/World.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageFixtureLibrarySubsystem)

#define LOCTEXT_NAMESPACE "StageFixtureLibrary"

struct UStageFixtureLibrarySubsystem::FParsedFile
{
	FString SourceFile;
	EStageFixtureImportOrigin Origin = EStageFixtureImportOrigin::User;
	TArray<uint8> Bytes;
	bool bRead = false;
	FStageGdtfParseResult Parsed;
	TArray<FStageGdtfModeLayout> Layouts;
	FStageGdtfCapabilities Capabilities;
	FStageGdtfImportReport Report;
	TOptional<EStageFixtureKind> Kind;
	FDateTime ImportedUtc;
	FString SourceFileName;
	double ParseMs = 0.0;
};

namespace StageFixtureLibraryPrivate
{
	using FParsedFile = UStageFixtureLibrarySubsystem::FParsedFile;

	constexpr int32 MaxNamesListed = 12;

	FString NormalizePath(const FString& File)
	{
		FString Path = FPaths::ConvertRelativePathToFull(File.TrimStartAndEnd());
		FPaths::NormalizeFilename(Path);
		FPaths::CollapseRelativeDirectories(Path);
		return Path;
	}

	FText FileLabel(const FString& File)
	{
		return FText::FromString(FPaths::GetCleanFilename(File));
	}

	void ParseInto(FParsedFile& File)
	{
		const double Start = FPlatformTime::Seconds();
		File.Parsed = StageGdtf::Parse(File.Bytes);
		File.Layouts.Reset();
		for (int32 Mode = 0; File.Parsed.IsOk() && Mode < File.Parsed.Type.Modes.Num(); ++Mode)
		{
			File.Layouts.Add(StageGdtfMapping::BuildModeLayout(File.Parsed.Type, Mode));
		}
		File.Capabilities = StageGdtfMapping::DeriveCapabilities(File.Parsed.Type, File.Layouts);
		File.Report = StageGdtfMapping::BuildReport(File.Parsed, File.Layouts, File.Capabilities);
		File.ParseMs = (FPlatformTime::Seconds() - Start) * 1000.0;
	}

	TSharedPtr<FParsedFile> ReadAndParse(const FString& Path, EStageFixtureImportOrigin Origin)
	{
		TSharedPtr<FParsedFile> File = MakeShared<FParsedFile>();
		File->SourceFile = Path;
		File->Origin = Origin;
		File->SourceFileName = FPaths::GetCleanFilename(Path);
		const int64 Size = IFileManager::Get().FileSize(*Path);
		File->bRead = Size >= 0 && Size <= StageFixtureLibrary::MaxFileSizeBytes && FFileHelper::LoadFileToArray(File->Bytes, *Path);
		if (File->bRead)
		{
			ParseInto(*File);
		}
		return File;
	}

	FString ReportText(const FParsedFile& File)
	{
		return File.bRead ? File.Report.ToText() : FString();
	}

	FText TypeLabel(const FStageGdtfFixtureType& Type)
	{
		return FText::FromString(Type.Manufacturer.IsEmpty() ? Type.Name : Type.Manufacturer + TEXT(" ") + Type.Name);
	}

	FText TypeLabel(const ULightingFixtureData& Mode)
	{
		const FString Manufacturer = Mode.Specs.Manufacturer.ToString();
		return FText::FromString(Manufacturer.IsEmpty() ? Mode.SourceTypeName : Manufacturer + TEXT(" ") + Mode.SourceTypeName);
	}

	TArray<UBaseItemData*> ToItems(const TArray<TObjectPtr<ULightingFixtureData>>& Modes)
	{
		TArray<UBaseItemData*> Items;
		Items.Reserve(Modes.Num());
		for (ULightingFixtureData* Mode : Modes)
		{
			if (Mode)
			{
				Items.Add(Mode);
			}
		}
		return Items;
	}

	FText FirstError(const FParsedFile& File)
	{
		if (!File.Report.Errors.IsEmpty())
		{
			return FText::FromString(File.Report.Errors[0]);
		}
		for (const FStageGdtfModeLayout& Layout : File.Layouts)
		{
			if (!Layout.Errors.IsEmpty())
			{
				return FText::FromString(FString::Printf(TEXT("%s: %s"), *Layout.Name, *Layout.Errors[0]));
			}
		}
		return LOCTEXT("NoUsableMode", "no mode StageCraft can use");
	}
}

void UStageFixtureLibrarySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	ItemSubsystem = Collection.InitializeDependency<UStageItemSubsystem>();
	RestoreLibrary();
}

void UStageFixtureLibrarySubsystem::Deinitialize()
{
	Pending.Reset();
	Reviews.Reset();
	ReviewFiles.Reset();
	bRestoring = false;
	OnImportFinished.Clear();
	OnLibraryChanged.Clear();
	OnReviewsChanged.Clear();
	WritePipe.WaitUntilEmpty();

	constexpr ERenameFlags Flags = REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty;
	for (FStageFixtureLibraryType& Type : Types)
	{
		for (ULightingFixtureData* Mode : Type.Modes)
		{
			if (Mode)
			{
				Mode->Rename(nullptr, GetTransientPackage(), Flags);
			}
		}
		if (Type.BaseMesh)
		{
			Type.BaseMesh->Rename(nullptr, GetTransientPackage(), Flags);
		}
	}
	Types.Reset();
	Super::Deinitialize();
}

bool UStageFixtureLibrarySubsystem::IsFixtureFile(const FString& File)
{
	return FPaths::GetExtension(File).Equals(TEXT("gdtf"), ESearchCase::IgnoreCase);
}

const FStageFixtureLibraryType* UStageFixtureLibrarySubsystem::FindType(const FGuid& FixtureTypeId) const
{
	return Types.FindByPredicate([&FixtureTypeId](const FStageFixtureLibraryType& Type) { return Type.FixtureTypeId == FixtureTypeId; });
}

TSharedRef<UStageFixtureLibrarySubsystem::FParsedFile> UStageFixtureLibrarySubsystem::ParseEmbeddedFile(TArray<uint8> Bytes, const FString& SourceName)
{
	TSharedRef<FParsedFile> File = MakeShared<FParsedFile>();
	File->SourceFile = SourceName;
	File->SourceFileName = FPaths::GetCleanFilename(SourceName);
	File->bRead = Bytes.Num() <= StageFixtureLibrary::MaxFileSizeBytes;
	File->Bytes = MoveTemp(Bytes);
	if (File->bRead)
	{
		StageFixtureLibraryPrivate::ParseInto(*File);
	}
	else
	{
		File->Bytes.Empty();
	}
	return File;
}

bool UStageFixtureLibrarySubsystem::CanAddParsedFile(const FParsedFile& File, const FGuid& ExpectedTypeId)
{
	return File.bRead && File.Parsed.IsOk() && File.Report.bCanImport && ExpectedTypeId.IsValid() && File.Parsed.Type.FixtureTypeId == ExpectedTypeId;
}

FStageFixtureImportOutcome UStageFixtureLibrarySubsystem::RequestImportBytes(TArray<uint8> Bytes, const FString& SourceName)
{
	using namespace StageFixtureLibraryPrivate;

	FStageFixtureImportOutcome Outcome;
	Outcome.SourceFile = SourceName;
	const bool bWaiting = Pending.FindKey(SourceName) != nullptr
		|| Reviews.ContainsByPredicate([&SourceName](const FStageFixtureImportReview& Review) { return Review.SourceFile == SourceName; });
	if (!IsFixtureFile(SourceName) || FPaths::GetCleanFilename(SourceName) != SourceName)
	{
		Outcome.Result = EStageFixtureImportResult::UnsupportedFormat;
		Outcome.Message = FText::Format(LOCTEXT("BytesNotGdtf", "{0} is not a GDTF fixture file (.gdtf)."), FText::FromString(SourceName));
	}
	else if (bWaiting)
	{
		Outcome.Result = EStageFixtureImportResult::AlreadyInLibrary;
		Outcome.Message = FText::Format(LOCTEXT("BytesImporting", "{0} is already being imported."), FText::FromString(SourceName));
	}
	else if (Pending.Num() + Reviews.Num() >= MaxReviews)
	{
		Outcome.Result = EStageFixtureImportResult::Refused;
		Outcome.Message = FText::Format(LOCTEXT("BytesTooManyWaiting", "{0} was not read: {1} fixture files are already waiting. Add or cancel them first."),
			FText::FromString(SourceName), FText::AsNumber(MaxReviews));
	}
	else
	{
		const uint64 Serial = ++NextSerial;
		Pending.Add(Serial, SourceName);
		UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis = TWeakObjectPtr<UStageFixtureLibrarySubsystem>(this), Serial, SourceName, Bytes = MoveTemp(Bytes)]() mutable
		{
			TSharedPtr<FParsedFile> File = ParseEmbeddedFile(MoveTemp(Bytes), SourceName);
			File->Origin = EStageFixtureImportOrigin::User;
			AsyncTask(ENamedThreads::GameThread, [WeakThis, Serial, File = MoveTemp(File)]() mutable
			{
				if (UStageFixtureLibrarySubsystem* Self = WeakThis.Get())
				{
					Self->FinishImport(Serial, MoveTemp(File));
				}
			});
		});
		Outcome.Result = EStageFixtureImportResult::Success;
		return Outcome;
	}
	UE_LOG(LogStageCraft, Log, TEXT("Fixture library: refused %s (%s)."), *SourceName, *UEnum::GetValueAsString(Outcome.Result));
	return Outcome;
}

FStageFixtureImportOutcome UStageFixtureLibrarySubsystem::AddEmbeddedFile(FParsedFile& File)
{
	using namespace StageFixtureLibraryPrivate;

	FStageFixtureImportOutcome Outcome;
	Outcome.SourceFile = File.SourceFile;
	Outcome.Report = ReportText(File);
	if (!File.bRead || !File.Parsed.IsOk() || !File.Report.bCanImport)
	{
		Outcome.Result = EStageFixtureImportResult::Refused;
		Outcome.Message = File.bRead
			? FText::Format(LOCTEXT("EmbeddedRefused", "{0} cannot be used: {1}."), FileLabel(File.SourceFile), FirstError(File))
			: FText::Format(LOCTEXT("EmbeddedTooLarge", "{0} is larger than {1} MB, the most a GDTF file may be."), FileLabel(File.SourceFile),
				FText::AsNumber(StageFixtureLibrary::MaxFileSizeBytes / (1024 * 1024)));
		OnImportFinished.Broadcast(Outcome);
		return Outcome;
	}

	Outcome.FixtureTypeId = File.Parsed.Type.FixtureTypeId;
	if (const FStageFixtureLibraryType* Existing = FindType(Outcome.FixtureTypeId))
	{
		Outcome.Result = EStageFixtureImportResult::AlreadyInLibrary;
		Outcome.Modes = Existing->Modes;
		Outcome.Message = FText::Format(LOCTEXT("EmbeddedKnown", "{0} is already in the Library; the Library's copy is used."), TypeLabel(File.Parsed.Type));
		return Outcome;
	}
	if (Types.Num() >= StageFixtureLibrary::MaxTypes)
	{
		Outcome.Result = EStageFixtureImportResult::LibraryFull;
		Outcome.Message = FText::Format(LOCTEXT("EmbeddedFull", "The Library already holds {0} fixture types, the most it can."), FText::AsNumber(StageFixtureLibrary::MaxTypes));
		OnImportFinished.Broadcast(Outcome);
		return Outcome;
	}

	File.ImportedUtc = FDateTime::UtcNow();
	if (!RegisterType(File, /*bSessionOnly*/ false, Outcome))
	{
		Outcome.Result = EStageFixtureImportResult::Refused;
		Outcome.Message = FText::Format(LOCTEXT("EmbeddedNotBuilt", "{0} cannot be used: no mode could be built."), FileLabel(File.SourceFile));
		OnImportFinished.Broadcast(Outcome);
		return Outcome;
	}
	WriteLibraryFile(Outcome.FixtureTypeId, MoveTemp(File.Bytes));
	SaveIndex();
	RefreshReviews();
	Outcome.Result = EStageFixtureImportResult::Success;
	Outcome.Message = FText::Format(LOCTEXT("EmbeddedAdded", "Added {0} ({1} {1}|plural(one=mode,other=modes)) to the Library."), TypeLabel(File.Parsed.Type), FText::AsNumber(Outcome.Modes.Num()));
	UE_LOG(LogStageCraft, Log, TEXT("Fixture library: %s (from %s)"), *Outcome.Message.ToString(), *File.SourceFile);
	OnImportFinished.Broadcast(Outcome);
	OnLibraryChanged.Broadcast();
	return Outcome;
}

TArray<FStageFixtureImportOutcome> UStageFixtureLibrarySubsystem::RequestImport(const TArray<FString>& Files)
{
	using namespace StageFixtureLibraryPrivate;

	TArray<FStageFixtureImportOutcome> Refusals;
	for (const FString& Raw : Files)
	{
		const FString Path = NormalizePath(Raw);
		FStageFixtureImportOutcome Refusal;
		Refusal.SourceFile = Path;
		const int64 Size = IFileManager::Get().FileSize(*Path);
		if (!IsFixtureFile(Path))
		{
			Refusal.Result = EStageFixtureImportResult::UnsupportedFormat;
			Refusal.Message = FText::Format(LOCTEXT("NotGdtf", "{0} is not a GDTF fixture file (.gdtf)."), FileLabel(Path));
		}
		else if (Size < 0)
		{
			Refusal.Result = EStageFixtureImportResult::FileNotFound;
			Refusal.Message = FText::Format(LOCTEXT("NotFound", "{0} was not found."), FileLabel(Path));
		}
		else if (Size > StageFixtureLibrary::MaxFileSizeBytes)
		{
			Refusal.Result = EStageFixtureImportResult::FileTooLarge;
			Refusal.Message = FText::Format(LOCTEXT("TooLarge", "{0} is larger than {1} MB, the most a GDTF file may be."), FileLabel(Path),
				FText::AsNumber(StageFixtureLibrary::MaxFileSizeBytes / (1024 * 1024)));
		}
		else if (Pending.FindKey(Path) != nullptr || Reviews.ContainsByPredicate([&Path](const FStageFixtureImportReview& Review) { return Review.SourceFile == Path; }))
		{
			Refusal.Result = EStageFixtureImportResult::AlreadyInLibrary;
			Refusal.Message = FText::Format(LOCTEXT("Importing", "{0} is already being imported."), FileLabel(Path));
		}
		else if (Pending.Num() + Reviews.Num() >= MaxReviews)
		{
			Refusal.Result = EStageFixtureImportResult::Refused;
			Refusal.Message = FText::Format(LOCTEXT("TooManyWaiting", "{0} was not read: {1} fixture files are already waiting. Add or cancel them first."), FileLabel(Path), FText::AsNumber(MaxReviews));
		}
		else
		{
			const uint64 Serial = ++NextSerial;
			Pending.Add(Serial, Path);
			UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis = TWeakObjectPtr<UStageFixtureLibrarySubsystem>(this), Serial, Path]()
			{
				TSharedPtr<FParsedFile> File = ReadAndParse(Path, EStageFixtureImportOrigin::User);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, Serial, File = MoveTemp(File)]() mutable
				{
					if (UStageFixtureLibrarySubsystem* Self = WeakThis.Get())
					{
						Self->FinishImport(Serial, MoveTemp(File));
					}
				});
			});
			continue;
		}
		UE_LOG(LogStageCraft, Log, TEXT("Fixture library: refused %s (%s)."), *Path, *UEnum::GetValueAsString(Refusal.Result));
		Refusals.Add(MoveTemp(Refusal));
	}
	return Refusals;
}

void UStageFixtureLibrarySubsystem::FinishImport(uint64 Serial, TSharedPtr<FParsedFile> File)
{
	using namespace StageFixtureLibraryPrivate;

	if (Pending.Remove(Serial) == 0 || !File.IsValid())
	{
		return;
	}

	if (!File->bRead)
	{
		FStageFixtureImportOutcome Outcome;
		Outcome.SourceFile = File->SourceFile;
		Outcome.Result = EStageFixtureImportResult::IOError;
		Outcome.Message = FText::Format(LOCTEXT("Unreadable", "{0} could not be read."), FileLabel(File->SourceFile));
		UE_LOG(LogStageCraft, Warning, TEXT("Fixture library: %s"), *Outcome.Message.ToString());
		OnImportFinished.Broadcast(Outcome);
		return;
	}

	FStageFixtureImportReview Review = MakeReview(*File);
	UE_LOG(LogStageCraft, Log, TEXT("Fixture library: %s read in %.2f ms on a worker; waiting for review (%s).\n%s"), *File->SourceFile, File->ParseMs,
		Review.bCanImport ? TEXT("can be added") : TEXT("refused"), *Review.ReportText);
	Reviews.Add(MoveTemp(Review));
	ReviewFiles.Add(MoveTemp(File));
	OnReviewsChanged.Broadcast();
}

FStageFixtureImportReview UStageFixtureLibrarySubsystem::MakeReview(const FParsedFile& File) const
{
	using namespace StageFixtureLibraryPrivate;

	FStageFixtureImportReview Review;
	Review.ReviewId = FGuid::NewGuid();
	Review.SourceFile = File.SourceFile;
	Review.ReportText = ReportText(File);
	Review.Warnings = File.Report.Warnings;
	Review.Errors = File.Report.Errors;
	Review.bCanImport = File.Report.bCanImport;
	if (!File.Parsed.IsOk())
	{
		if (Review.Errors.IsEmpty())
		{
			Review.Errors.Add(FirstError(File).ToString());
		}
		Review.bCanImport = false;
		return Review;
	}

	const FStageGdtfFixtureType& Type = File.Parsed.Type;
	Review.FixtureTypeId = Type.FixtureTypeId;
	Review.Manufacturer = Type.Manufacturer.Left(StageFixtureLibrary::MaxRefTextLength);
	Review.Name = Type.Name.Left(StageFixtureLibrary::MaxRefTextLength);
	Review.InferredKind = File.Capabilities.Kind;
	Review.bReplaces = FindType(Type.FixtureTypeId) != nullptr;
	for (const FStageGdtfModeLayout& Layout : File.Layouts)
	{
		FStageFixtureModeSummary& Mode = Review.Modes.AddDefaulted_GetRef();
		Mode.Name = Layout.Name.Left(StageFixtureLibrary::MaxRefTextLength);
		Mode.Footprint = Layout.Footprint;
		Mode.bUsable = Layout.IsUsable();
		Mode.Reason = Layout.Errors.IsEmpty() ? FString() : Layout.Errors[0];
		for (const FStageGdtfLayoutChannel& Channel : Layout.Channels)
		{
			switch (Channel.Status)
			{
			case EStageGdtfChannelStatus::Mapped:
				++Mode.Mapped;
				break;
			case EStageGdtfChannelStatus::Approximated:
				++Mode.Approximated;
				if (Mode.ApproximatedNames.Num() < MaxNamesListed && !Mode.ApproximatedNames.Contains(Channel.GdtfAttribute))
				{
					Mode.ApproximatedNames.Add(Channel.GdtfAttribute);
				}
				break;
			case EStageGdtfChannelStatus::Default:
				++Mode.Defaulted;
				if (Mode.DefaultedNames.Num() < MaxNamesListed && !Mode.DefaultedNames.Contains(Channel.GdtfAttribute))
				{
					Mode.DefaultedNames.Add(Channel.GdtfAttribute);
				}
				break;
			default:
				break;
			}
		}
	}
	return Review;
}

const FStageFixtureImportReview* UStageFixtureLibrarySubsystem::FindReview(const FGuid& ReviewId) const
{
	return Reviews.FindByPredicate([&ReviewId](const FStageFixtureImportReview& Review) { return Review.ReviewId == ReviewId; });
}

void UStageFixtureLibrarySubsystem::RefreshReviews()
{
	for (FStageFixtureImportReview& Review : Reviews)
	{
		Review.bReplaces = Review.FixtureTypeId.IsValid() && FindType(Review.FixtureTypeId) != nullptr;
	}
}

FStageFixtureImportOutcome UStageFixtureLibrarySubsystem::ConfirmImport(const FGuid& ReviewId, TOptional<EStageFixtureKind> Kind)
{
	using namespace StageFixtureLibraryPrivate;

	FStageFixtureImportOutcome Outcome;
	const int32 Index = Reviews.IndexOfByPredicate([&ReviewId](const FStageFixtureImportReview& Review) { return Review.ReviewId == ReviewId; });
	if (Index == INDEX_NONE || !ReviewFiles[Index].IsValid())
	{
		Outcome.Result = EStageFixtureImportResult::Unknown;
		Outcome.Message = LOCTEXT("NoReview", "That fixture file is no longer waiting to be added.");
		return Outcome;
	}

	FParsedFile& File = *ReviewFiles[Index];
	Outcome.SourceFile = File.SourceFile;
	Outcome.Report = ReportText(File);
	Outcome.FixtureTypeId = File.Parsed.IsOk() ? File.Parsed.Type.FixtureTypeId : FGuid();
	const int32 Existing = Types.IndexOfByPredicate([&Outcome](const FStageFixtureLibraryType& Type) { return Type.FixtureTypeId == Outcome.FixtureTypeId; });

	bool bRefused = true;
	if (!File.Report.bCanImport || !File.Parsed.IsOk())
	{
		Outcome.Result = EStageFixtureImportResult::Refused;
		Outcome.Message = FText::Format(LOCTEXT("RefusedFile", "{0} cannot be used: {1}."), FileLabel(File.SourceFile), FirstError(File));
	}
	else if (Existing == INDEX_NONE && Types.Num() >= StageFixtureLibrary::MaxTypes)
	{
		Outcome.Result = EStageFixtureImportResult::LibraryFull;
		Outcome.Message = FText::Format(LOCTEXT("Full", "The Library already holds {0} fixture types, the most it can."), FText::AsNumber(StageFixtureLibrary::MaxTypes));
	}
	else if (Existing != INDEX_NONE && Types[Existing].bSessionOnly)
	{
		Outcome.Result = EStageFixtureImportResult::AlreadyInLibrary;
		Outcome.Message = FText::Format(LOCTEXT("SessionType", "{0} is a test type of this session and cannot be replaced."), TypeLabel(File.Parsed.Type));
	}
	else
	{
		bRefused = false;
	}
	if (bRefused)
	{
		UE_LOG(LogStageCraft, Log, TEXT("Fixture library: %s"), *Outcome.Message.ToString());
		OnImportFinished.Broadcast(Outcome);
		return Outcome;
	}

	File.Kind = Kind;
	File.ImportedUtc = FDateTime::UtcNow();
	const bool bReplacing = Existing != INDEX_NONE;
	const bool bBuilt = bReplacing ? ReplaceType(Existing, File, Outcome) : RegisterType(File, false, Outcome) != nullptr;
	if (!bBuilt)
	{
		Outcome.Result = EStageFixtureImportResult::Refused;
		Outcome.Message = FText::Format(LOCTEXT("NotBuilt", "{0} cannot be used: no mode could be built."), FileLabel(File.SourceFile));
		OnImportFinished.Broadcast(Outcome);
		return Outcome;
	}

	WriteLibraryFile(Outcome.FixtureTypeId, MoveTemp(File.Bytes));
	SaveIndex();
	Outcome.Result = EStageFixtureImportResult::Success;
	Outcome.Message = bReplacing
		? FText::Format(LOCTEXT("Replaced", "Replaced {0} in the Library ({1} {1}|plural(one=mode,other=modes))."), TypeLabel(File.Parsed.Type), FText::AsNumber(Outcome.Modes.Num()))
		: FText::Format(LOCTEXT("Added", "Added {0} ({1} {1}|plural(one=mode,other=modes)) to the Library."), TypeLabel(File.Parsed.Type), FText::AsNumber(Outcome.Modes.Num()));
	UE_LOG(LogStageCraft, Log, TEXT("Fixture library: %s"), *Outcome.Message.ToString());

	Reviews.RemoveAt(Index);
	ReviewFiles.RemoveAt(Index);
	RefreshReviews();
	OnLibraryChanged.Broadcast();
	OnReviewsChanged.Broadcast();
	OnImportFinished.Broadcast(Outcome);
	return Outcome;
}

FStageFixtureImportOutcome UStageFixtureLibrarySubsystem::CancelImport(const FGuid& ReviewId)
{
	FStageFixtureImportOutcome Outcome;
	const int32 Index = Reviews.IndexOfByPredicate([&ReviewId](const FStageFixtureImportReview& Review) { return Review.ReviewId == ReviewId; });
	if (Index == INDEX_NONE)
	{
		Outcome.Result = EStageFixtureImportResult::Unknown;
		return Outcome;
	}
	Outcome.Result = EStageFixtureImportResult::Cancelled;
	Outcome.SourceFile = Reviews[Index].SourceFile;
	Outcome.FixtureTypeId = Reviews[Index].FixtureTypeId;
	Reviews.RemoveAt(Index);
	ReviewFiles.RemoveAt(Index);
	OnReviewsChanged.Broadcast();
	return Outcome;
}

FStageFixtureImportOutcome UStageFixtureLibrarySubsystem::RemoveType(const FGuid& FixtureTypeId)
{
	FStageFixtureImportOutcome Outcome;
	Outcome.FixtureTypeId = FixtureTypeId;
	const int32 Index = Types.IndexOfByPredicate([&FixtureTypeId](const FStageFixtureLibraryType& Type) { return Type.FixtureTypeId == FixtureTypeId; });
	if (Index == INDEX_NONE)
	{
		Outcome.Result = EStageFixtureImportResult::Unknown;
		Outcome.Message = LOCTEXT("RemoveUnknown", "That fixture type is not in the Library.");
		OnImportFinished.Broadcast(Outcome);
		return Outcome;
	}

	const FText Label = Types[Index].Modes.IsEmpty() || !Types[Index].Modes[0] ? FText::FromString(Types[Index].SourceFileName) : StageFixtureLibraryPrivate::TypeLabel(*Types[Index].Modes[0].Get());
	const int32 Users = FindPlacedUsers(Types[Index]).Num();
	if (Users > 0)
	{
		Outcome.Result = EStageFixtureImportResult::InUse;
		Outcome.Message = FText::Format(LOCTEXT("InUse", "{0} is used by {1} placed {1}|plural(one=fixture,other=fixtures). Delete or replace them first."), Label, FText::AsNumber(Users));
		OnImportFinished.Broadcast(Outcome);
		return Outcome;
	}

	const FStageFixtureLibraryType Removed = Types[Index];
	Types.RemoveAt(Index);
	UnregisterType(Removed);
	if (!Removed.bSessionOnly)
	{
		WritePipe.Launch(UE_SOURCE_LOCATION, [Path = StageFixtureLibrary::GetLibraryFilePath(FixtureTypeId)]()
		{
			if (IFileManager::Get().FileExists(*Path) && !IFileManager::Get().Delete(*Path, false, true, true))
			{
				UE_LOG(LogStageCraft, Warning, TEXT("Fixture library: could not delete %s; it will be restored at the next start."), *Path);
			}
		});
		SaveIndex();
	}
	Outcome.Result = EStageFixtureImportResult::Success;
	Outcome.Message = FText::Format(LOCTEXT("Removed", "Removed {0} from the Library."), Label);
	UE_LOG(LogStageCraft, Log, TEXT("Fixture library: %s"), *Outcome.Message.ToString());
	RefreshReviews();
	OnLibraryChanged.Broadcast();
	OnReviewsChanged.Broadcast();
	OnImportFinished.Broadcast(Outcome);
	return Outcome;
}

bool UStageFixtureLibrarySubsystem::IsTypeInUse(const FGuid& FixtureTypeId) const
{
	const FStageFixtureLibraryType* Type = FindType(FixtureTypeId);
	return Type && !FindPlacedUsers(*Type).IsEmpty();
}

const FStageFixtureLibraryType* UStageFixtureLibrarySubsystem::FindTypeOfItem(const UBaseItemData* Item) const
{
	const ULightingFixtureData* Fixture = Cast<ULightingFixtureData>(Item);
	if (!Fixture || !Fixture->IsLibraryType())
	{
		return nullptr;
	}
	const FStageFixtureLibraryType* Type = FindType(Fixture->FixtureTypeId);
	return Type && Type->Modes.Contains(Fixture) ? Type : nullptr;
}

TArray<AModularBaseActor*> UStageFixtureLibrarySubsystem::FindPlacedUsers(const FStageFixtureLibraryType& Type) const
{
	TArray<AModularBaseActor*> Users;
	const UGameInstance* GameInstance = GetGameInstance();
	const UStageSessionSubsystem* Session = GameInstance ? UWorld::GetSubsystem<UStageSessionSubsystem>(GameInstance->GetWorld()) : nullptr;
	if (!Session)
	{
		return Users;
	}
	for (AModularBaseActor* Item : Session->GetPlacedItems())
	{
		const ULightingFixtureData* Data = Item ? Cast<ULightingFixtureData>(Item->GetItemData()) : nullptr;
		if (Data && Data->FixtureTypeId == Type.FixtureTypeId && Data->IsLibraryType())
		{
			Users.Add(Item);
		}
	}
	return Users;
}

void UStageFixtureLibrarySubsystem::UnregisterType(const FStageFixtureLibraryType& Type)
{
	TArray<UBaseItemData*> Items;
	for (ULightingFixtureData* Mode : Type.Modes)
	{
		if (Mode)
		{
			Items.Add(Mode);
		}
	}
	if (UStageItemSubsystem* Catalog = ItemSubsystem.Get())
	{
		Catalog->RemoveRuntimeItems(Items);
	}
	constexpr ERenameFlags Flags = REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty;
	for (UBaseItemData* Item : Items)
	{
		Item->Rename(nullptr, GetTransientPackage(), Flags);
	}
	if (Type.BaseMesh)
	{
		Type.BaseMesh->Rename(nullptr, GetTransientPackage(), Flags);
	}
}

bool UStageFixtureLibrarySubsystem::ReplaceType(int32 Index, const FParsedFile& File, FStageFixtureImportOutcome& Outcome)
{
	const FStageFixtureLibraryType Old = Types[Index];
	const TArray<AModularBaseActor*> Users = FindPlacedUsers(Old);
	TMap<const UBaseItemData*, FName> OldSlugs;
	for (const ULightingFixtureData* Mode : Old.Modes)
	{
		if (Mode)
		{
			OldSlugs.Add(Mode, Mode->GetFName());
		}
	}

	UnregisterType(Old);
	TOptional<FStageFixtureLibraryType> Built = BuildType(File, false, Outcome);
	if (!Built.IsSet())
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Fixture library: the new file of %s built no mode; the old type stays for this session."), *Old.SourceFileName);
		Outcome.Modes.Reset();
		return false;
	}

	Types[Index] = MoveTemp(Built.GetValue());
	const FStageFixtureLibraryType& New = Types[Index];
	if (UStageItemSubsystem* Catalog = ItemSubsystem.Get())
	{
		Catalog->AddRuntimeItems(StageFixtureLibraryPrivate::ToItems(New.Modes));
	}

	int32 Moved = 0;
	int32 Orphaned = 0;
	for (AModularBaseActor* User : Users)
	{
		const FName* Slug = IsValid(User) ? OldSlugs.Find(User->GetItemData()) : nullptr;
		const TObjectPtr<ULightingFixtureData>* Match = Slug ? New.Modes.FindByPredicate([Slug](const ULightingFixtureData* Mode) { return Mode && Mode->GetFName() == *Slug; }) : nullptr;
		if (Match)
		{
			User->InitializeFromItemData(*Match);
			++Moved;
		}
		else
		{
			++Orphaned;
		}
	}
	if (Orphaned > 0)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Fixture library: %d placed fixtures use a mode the new file no longer has; they keep the old mode until the stage is reloaded."), Orphaned);
	}
	UE_LOG(LogStageCraft, Log, TEXT("Fixture library: replaced %s; %d placed fixtures moved to the new modes."), *New.FixtureTypeId.ToString(EGuidFormats::DigitsWithHyphens), Moved);
	return true;
}

void UStageFixtureLibrarySubsystem::WriteLibraryFile(const FGuid& FixtureTypeId, TArray<uint8> Bytes)
{
	WritePipe.Launch(UE_SOURCE_LOCATION, [Bytes = MoveTemp(Bytes), Target = StageFixtureLibrary::GetLibraryFilePath(FixtureTypeId)]()
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target), true);
		if (!StageAtomicFile::WriteBytes(Target, Bytes))
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Fixture library: could not write %s; the fixture type is available until the app closes."), *Target);
		}
	});
}

FStageFixtureLibraryType* UStageFixtureLibrarySubsystem::RegisterType(const FParsedFile& File, bool bSessionOnly, FStageFixtureImportOutcome& Outcome)
{
	TOptional<FStageFixtureLibraryType> Built = BuildType(File, bSessionOnly, Outcome);
	if (!Built.IsSet())
	{
		return nullptr;
	}
	FStageFixtureLibraryType& Added = Types.Add_GetRef(MoveTemp(Built.GetValue()));
	if (UStageItemSubsystem* Catalog = ItemSubsystem.Get())
	{
		Catalog->AddRuntimeItems(StageFixtureLibraryPrivate::ToItems(Added.Modes));
	}
	return &Added;
}

TOptional<FStageFixtureLibraryType> UStageFixtureLibrarySubsystem::BuildType(const FParsedFile& File, bool bSessionOnly, FStageFixtureImportOutcome& Outcome)
{
	const FStageGdtfFixtureType& Type = File.Parsed.Type;
	const FGuid Id = Type.FixtureTypeId;
	const FString Folder = StageFixtureLibrary::MakeTypeFolder(Id);

	TArray<FString> ModeNames;
	ModeNames.Reserve(Type.Modes.Num());
	for (const FStageGdtfMode& Mode : Type.Modes)
	{
		ModeNames.Add(Mode.Name);
	}
	const TArray<FString> Slugs = StageFixtureLibrary::MakeModeSlugs(ModeNames);

	constexpr ERenameFlags Flags = REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty;
	const auto ClaimName = [](UPackage& Package, const FString& Name)
	{
		if (UObject* Stale = StaticFindObjectFast(UObject::StaticClass(), &Package, FName(*Name)))
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Fixture library: %s was still in memory from an earlier session; it was moved aside."), *Stale->GetPathName());
			Stale->Rename(nullptr, GetTransientPackage(), Flags);
		}
	};

	FStageFixtureLibraryType Entry;
	Entry.FixtureTypeId = Id;
	Entry.SourceFileName = bSessionOnly ? FString() : File.SourceFileName;
	Entry.ImportedUtc = File.ImportedUtc;
	Entry.bSessionOnly = bSessionOnly;
	Entry.bKindCorrected = File.Kind.IsSet();
	Entry.CorrectedKind = File.Kind.Get(EStageFixtureKind::MovingHeadSpot);

	const EStageFixtureKind Kind = File.Kind.Get(File.Capabilities.Kind);
	const StageGdtfFixtureBuilder::FBodyLayout Body = StageGdtfFixtureBuilder::MakeBodyLayout(File.Capabilities, Kind);
	UPackage* BodyPackage = CreatePackage(*(Folder + TEXT("/Body")));
	ClaimName(*BodyPackage, TEXT("Base"));
	Entry.BaseMesh = StageGdtfFixtureBuilder::BuildBaseMesh(*BodyPackage, TEXT("Base"), Body.BaseSizeCm);
	if (!Entry.BaseMesh)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Fixture library: the base of %s could not be built; its fixtures show no base."), *Type.Name);
	}

	Outcome.Modes.Reset();
	for (const FStageGdtfModeLayout& Layout : File.Layouts)
	{
		if (!Layout.IsUsable() || !Slugs.IsValidIndex(Layout.ModeIndex))
		{
			continue;
		}
		const FString& Slug = Slugs[Layout.ModeIndex];
		UPackage* Package = CreatePackage(*(Folder + TEXT("/") + Slug));
		ClaimName(*Package, Slug);
		ULightingFixtureData* Data = NewObject<ULightingFixtureData>(Package, FName(*Slug), RF_Transient);
		StageGdtfFixtureBuilder::Fill(*Data, Type, File.Capabilities, Layout, File.Kind, Entry.BaseMesh);
		Entry.Modes.Add(Data);
		Outcome.Modes.Add(Data);
	}
	if (Entry.Modes.IsEmpty())
	{
		return {};
	}
	Outcome.FixtureTypeId = Id;
	return Entry;
}

void UStageFixtureLibrarySubsystem::RestoreLibrary()
{
	using namespace StageFixtureLibraryPrivate;

	bRestoring = true;
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis = TWeakObjectPtr<UStageFixtureLibrarySubsystem>(this), Directory = StageFixtureLibrary::GetLibraryDirectory(),
		IndexPath = StageFixtureLibrary::GetIndexPath()]()
	{
		const double Start = FPlatformTime::Seconds();
		TArray<StageFixtureLibrary::FIndexEntry> Index;
		int32 Skipped = 0;
		FString IndexJson;
		const bool bIndexRead = IFileManager::Get().FileExists(*IndexPath) && FFileHelper::LoadFileToString(IndexJson, *IndexPath)
			&& StageFixtureLibrary::ReadIndex(IndexJson, Index, Skipped);
		if (IFileManager::Get().FileExists(*IndexPath) && !bIndexRead)
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Fixture library: %s is not a valid index; fixture types load with their inferred kind."), *IndexPath);
		}
		else if (Skipped > 0)
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Fixture library: %d damaged entries of %s were skipped."), Skipped, *IndexPath);
		}

		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *FPaths::Combine(Directory, TEXT("*.gdtf")), /*Files*/ true, /*Directories*/ false);
		Names.Sort();
		TArray<TSharedPtr<FParsedFile>> Files;
		for (const FString& Name : Names)
		{
			FGuid NameId;
			if (Files.Num() >= StageFixtureLibrary::MaxTypes || !StageFixtureLibrary::ParseLibraryFileName(Name, NameId))
			{
				continue;
			}
			TSharedPtr<FParsedFile> File = ReadAndParse(FPaths::Combine(Directory, Name), EStageFixtureImportOrigin::Restore);
			if (File->bRead && File->Parsed.IsOk() && File->Parsed.Type.FixtureTypeId != NameId)
			{
				UE_LOG(LogStageCraft, Warning, TEXT("Fixture library: %s holds FixtureTypeID %s, not the one in its name; it is skipped."), *Name,
					*File->Parsed.Type.FixtureTypeId.ToString(EGuidFormats::DigitsWithHyphens));
				continue;
			}
			if (const StageFixtureLibrary::FIndexEntry* Entry = Index.FindByPredicate([&NameId](const StageFixtureLibrary::FIndexEntry& Candidate) { return Candidate.FixtureTypeId == NameId; }))
			{
				File->Kind = Entry->Kind;
				File->ImportedUtc = Entry->ImportedUtc;
				File->SourceFileName = Entry->SourceFileName.IsEmpty() ? Name : Entry->SourceFileName;
			}
			Files.Add(MoveTemp(File));
		}
		const double WorkerMs = (FPlatformTime::Seconds() - Start) * 1000.0;
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Files = MoveTemp(Files), WorkerMs]() mutable
		{
			if (UStageFixtureLibrarySubsystem* Self = WeakThis.Get())
			{
				Self->HandleLibraryRestored(MoveTemp(Files), WorkerMs);
			}
		});
	});
}

void UStageFixtureLibrarySubsystem::HandleLibraryRestored(TArray<TSharedPtr<FParsedFile>> Files, double WorkerMs)
{
	using namespace StageFixtureLibraryPrivate;

	if (!bRestoring)
	{
		return;
	}
	bRestoring = false;

	const double Start = FPlatformTime::Seconds();
	int32 Restored = 0;
	double SlowestMs = 0.0;
	for (const TSharedPtr<FParsedFile>& File : Files)
	{
		SlowestMs = FMath::Max(SlowestMs, File->ParseMs);
		FStageFixtureImportOutcome Outcome;
		Outcome.SourceFile = File->SourceFile;
		Outcome.Origin = EStageFixtureImportOrigin::Restore;
		Outcome.Report = ReportText(*File);
		if (!File->bRead || !File->Report.bCanImport)
		{
			Outcome.Result = File->bRead ? EStageFixtureImportResult::Refused : EStageFixtureImportResult::IOError;
			Outcome.Message = File->bRead
				? FText::Format(LOCTEXT("RestoreRefused", "{0} in the Library cannot be used: {1}."), FileLabel(File->SourceFile), FirstError(*File))
				: FText::Format(LOCTEXT("RestoreUnreadable", "{0} in the Library could not be read."), FileLabel(File->SourceFile));
		}
		else if (FindType(File->Parsed.Type.FixtureTypeId))
		{
			continue;
		}
		else if (RegisterType(*File, /*bSessionOnly*/ false, Outcome))
		{
			++Restored;
			continue;
		}
		else
		{
			Outcome.Result = EStageFixtureImportResult::Refused;
			Outcome.Message = FText::Format(LOCTEXT("RestoreNotBuilt", "{0} in the Library cannot be used: no mode could be built."), FileLabel(File->SourceFile));
		}
		UE_LOG(LogStageCraft, Warning, TEXT("Fixture library: %s\n%s"), *Outcome.Message.ToString(), *Outcome.Report);
		OnImportFinished.Broadcast(Outcome);
	}

	UE_LOG(LogStageCraft, Log, TEXT("Fixture library: %d of %d fixture types restored from %s; read and parsed in %.1f ms on a worker (slowest file %.2f ms), built in %.1f ms on the game thread."),
		Restored, Files.Num(), *StageFixtureLibrary::GetLibraryDirectory(), WorkerMs, SlowestMs, (FPlatformTime::Seconds() - Start) * 1000.0);
	if (Restored > 0)
	{
		RefreshReviews();
		OnLibraryChanged.Broadcast();
	}
}

void UStageFixtureLibrarySubsystem::SaveIndex()
{
	TArray<StageFixtureLibrary::FIndexEntry> Entries;
	Entries.Reserve(Types.Num());
	for (const FStageFixtureLibraryType& Type : Types)
	{
		if (Type.bSessionOnly)
		{
			continue;
		}
		StageFixtureLibrary::FIndexEntry& Entry = Entries.AddDefaulted_GetRef();
		Entry.FixtureTypeId = Type.FixtureTypeId;
		Entry.SourceFileName = Type.SourceFileName;
		Entry.ImportedUtc = Type.ImportedUtc;
		if (Type.bKindCorrected)
		{
			Entry.Kind = Type.CorrectedKind;
		}
	}
	WritePipe.Launch(UE_SOURCE_LOCATION, [Json = StageFixtureLibrary::WriteIndex(Entries), Path = StageFixtureLibrary::GetIndexPath()]()
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
		if (!StageAtomicFile::WriteText(Path, Json))
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Fixture library: could not write %s."), *Path);
		}
	});
}

#if !UE_BUILD_SHIPPING
FStageFixtureImportOutcome UStageFixtureLibrarySubsystem::AddSessionType(TConstArrayView<uint8> Bytes, const FString& Label)
{
	using namespace StageFixtureLibraryPrivate;

	FParsedFile File;
	File.SourceFile = Label;
	File.SourceFileName = Label;
	File.Bytes = TArray<uint8>(Bytes.GetData(), Bytes.Num());
	File.bRead = true;
	ParseInto(File);

	FStageFixtureImportOutcome Outcome;
	Outcome.SourceFile = Label;
	Outcome.Report = ReportText(File);
	if (!File.Report.bCanImport)
	{
		Outcome.Result = EStageFixtureImportResult::Refused;
		Outcome.Message = FText::Format(LOCTEXT("SessionRefused", "{0} cannot be used: {1}."), FText::FromString(Label), FirstError(File));
	}
	else if (FindType(File.Parsed.Type.FixtureTypeId))
	{
		Outcome.Result = EStageFixtureImportResult::AlreadyInLibrary;
		Outcome.FixtureTypeId = File.Parsed.Type.FixtureTypeId;
		Outcome.Message = FText::Format(LOCTEXT("SessionAlreadyIn", "{0} is already in the Library."), TypeLabel(File.Parsed.Type));
	}
	else if (RegisterType(File, /*bSessionOnly*/ true, Outcome))
	{
		Outcome.Result = EStageFixtureImportResult::Success;
		Outcome.Message = FText::Format(LOCTEXT("SessionAdded", "Added {0} for this session."), TypeLabel(File.Parsed.Type));
		OnLibraryChanged.Broadcast();
	}
	return Outcome;
}
#endif

#undef LOCTEXT_NAMESPACE
