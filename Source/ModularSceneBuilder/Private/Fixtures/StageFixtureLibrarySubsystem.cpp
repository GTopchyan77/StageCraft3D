// Copyright Epic Games, Inc. All Rights Reserved.

#include "Fixtures/StageFixtureLibrarySubsystem.h"

#include "Async/Async.h"
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
#include "Tasks/Task.h"
#include "UObject/Package.h"

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
	bRestoring = false;
	OnImportFinished.Clear();
	OnLibraryChanged.Clear();
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
		else if (Pending.FindKey(Path) != nullptr)
		{
			Refusal.Result = EStageFixtureImportResult::AlreadyInLibrary;
			Refusal.Message = FText::Format(LOCTEXT("Importing", "{0} is already being imported."), FileLabel(Path));
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

	FStageFixtureImportOutcome Outcome;
	Outcome.SourceFile = File->SourceFile;
	Outcome.Origin = File->Origin;
	Outcome.Report = ReportText(*File);
	Outcome.FixtureTypeId = File->bRead ? File->Parsed.Type.FixtureTypeId : FGuid();

	if (!File->bRead)
	{
		Outcome.Result = EStageFixtureImportResult::IOError;
		Outcome.Message = FText::Format(LOCTEXT("Unreadable", "{0} could not be read."), FileLabel(File->SourceFile));
	}
	else if (!File->Report.bCanImport)
	{
		Outcome.Result = EStageFixtureImportResult::Refused;
		Outcome.Message = FText::Format(LOCTEXT("RefusedFile", "{0} cannot be used: {1}."), FileLabel(File->SourceFile), FirstError(*File));
	}
	else if (FindType(File->Parsed.Type.FixtureTypeId))
	{
		Outcome.Result = EStageFixtureImportResult::AlreadyInLibrary;
		Outcome.Message = FText::Format(LOCTEXT("AlreadyIn", "{0} is already in the Library."), TypeLabel(File->Parsed.Type));
	}
	else if (Types.Num() >= StageFixtureLibrary::MaxTypes)
	{
		Outcome.Result = EStageFixtureImportResult::LibraryFull;
		Outcome.Message = FText::Format(LOCTEXT("Full", "The Library already holds {0} fixture types, the most it can."), FText::AsNumber(StageFixtureLibrary::MaxTypes));
	}
	else
	{
		File->ImportedUtc = FDateTime::UtcNow();
		if (RegisterType(*File, /*bSessionOnly*/ false, Outcome))
		{
			const FString Target = StageFixtureLibrary::GetLibraryFilePath(Outcome.FixtureTypeId);
			WritePipe.Launch(UE_SOURCE_LOCATION, [Bytes = MoveTemp(File->Bytes), Target]()
			{
				IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target), /*Tree*/ true);
				if (!StageAtomicFile::WriteBytes(Target, Bytes))
				{
					UE_LOG(LogStageCraft, Warning, TEXT("Fixture library: could not write %s; the fixture type is available until the app closes."), *Target);
				}
			});
			SaveIndex();
			Outcome.Result = EStageFixtureImportResult::Success;
			Outcome.Message = FText::Format(LOCTEXT("Added", "Added {0} ({1} {1}|plural(one=mode,other=modes)) to the Library."), TypeLabel(File->Parsed.Type), FText::AsNumber(Outcome.Modes.Num()));
			OnLibraryChanged.Broadcast();
		}
		else
		{
			Outcome.Result = EStageFixtureImportResult::Refused;
			Outcome.Message = FText::Format(LOCTEXT("NotBuilt", "{0} cannot be used: no mode could be built."), FileLabel(File->SourceFile));
		}
	}

	UE_LOG(LogStageCraft, Log, TEXT("Fixture library: %s %s in %.2f ms on a worker: %s\n%s"), *UEnum::GetValueAsString(Outcome.Result), *Outcome.SourceFile,
		File->ParseMs, *Outcome.Message.ToString(), *Outcome.Report);
	OnImportFinished.Broadcast(Outcome);
}

FStageFixtureLibraryType* UStageFixtureLibrarySubsystem::RegisterType(const FParsedFile& File, bool bSessionOnly, FStageFixtureImportOutcome& Outcome)
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
		return nullptr;
	}

	FStageFixtureLibraryType& Added = Types.Add_GetRef(MoveTemp(Entry));
	if (UStageItemSubsystem* Items = ItemSubsystem.Get())
	{
		for (ULightingFixtureData* Mode : Added.Modes)
		{
			Items->AddRuntimeItem(*Mode);
		}
	}
	Outcome.FixtureTypeId = Id;
	return &Added;
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
