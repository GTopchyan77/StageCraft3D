// Copyright Epic Games, Inc. All Rights Reserved.

// MV0 probe and the MV2 import commands for scripted runs. MV0 probe (Docs/ADR/0034 §10): reads every .mvr file in a folder with the MV1 parser and logs what it found, the report and the
// time, so a corpus of real files (Vectorworks, grandMA3, GDTF-Share samples) can be checked without importing anything.
// Development only: compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Actors/LightingFixtureActor.h"
#include "Actors/ModularBaseActor.h"
#include "Data/StageParameterTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Mvr/StageMvrImportSubsystem.h"
#include "Mvr/StageMvrParser.h"
#include "Player/ModularPlayerController.h"
#include "Subsystems/StageSessionSubsystem.h"

namespace StageMvrConsoleCommands
{
	/** [folder or file]: default Saved/MVRCorpus. */
	void Probe(const TArray<FString>& Args)
	{
		const FString Target = Args.IsEmpty() ? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MVRCorpus")) : FString::Join(Args, TEXT(" "));
		TArray<FString> Files;
		if (FPaths::FileExists(Target))
		{
			Files.Add(Target);
		}
		else
		{
			IFileManager::Get().FindFiles(Files, *FPaths::Combine(Target, TEXT("*.mvr")), /*Files*/ true, /*Directories*/ false);
			Files.Sort();
			for (FString& File : Files)
			{
				File = FPaths::Combine(Target, File);
			}
		}
		if (Files.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Mvr.Probe: no .mvr files at %s."), *Target);
			return;
		}
		const StageMvr::FLimits Limits;
		int32 Read = 0;
		for (const FString& File : Files)
		{
			const int64 Size = IFileManager::Get().FileSize(*File);
			TArray<uint8> Bytes;
			if (Size < 0 || Size > Limits.Zip.MaxArchiveBytes || !FFileHelper::LoadFileToArray(Bytes, *File))
			{
				UE_LOG(LogStageCraft, Warning, TEXT("StageCraft.Mvr.Probe: %s could not be read or is over %lld MB."), *FPaths::GetCleanFilename(File), Limits.Zip.MaxArchiveBytes >> 20);
				continue;
			}
			const double Start = FPlatformTime::Seconds();
			const FStageMvrParseResult Result = StageMvr::Parse(Bytes, Limits);
			const double Ms = (FPlatformTime::Seconds() - Start) * 1000.0;
			Read += Result.IsValid() ? 1 : 0;
			FString Lines;
			for (const FString& Error : Result.Errors)
			{
				Lines += FString::Printf(TEXT("\n  error: %s"), *Error);
			}
			for (const FString& Warning : Result.Warnings)
			{
				Lines += FString::Printf(TEXT("\n  note: %s"), *Warning);
			}
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Mvr.Probe: %s %s in %.1f ms (%lld KB): %s%s"), Result.IsValid() ? TEXT("OK     ") : TEXT("REFUSED"),
				*FPaths::GetCleanFilename(File), Ms, Size / 1024, Result.IsValid() ? *StageMvr::Describe(Result.Scene) : TEXT("-"), *Lines);
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Mvr.Probe: %d of %d files read."), Read, Files.Num());
	}

	FAutoConsoleCommand ProbeCommand(TEXT("StageCraft.Mvr.Probe"),
		TEXT("Reads .mvr files with the MVR parser and logs the scene, the report and the time: StageCraft.Mvr.Probe [folder or file] (default Saved/MVRCorpus). Nothing is imported."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Probe));

	AModularPlayerController* FindController(UWorld* World)
	{
		return World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
	}

	UStageMvrImportSubsystem* FindImporter(UWorld* World)
	{
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		return GameInstance ? GameInstance->GetSubsystem<UStageMvrImportSubsystem>() : nullptr;
	}

	/** "Import <path>": reads the file for review through the player's request, as File > Import MVR Scene does. */
	void Import(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = FindController(World);
		if (!Controller || Args.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Mvr.Import <path to .mvr>   (needs a StageCraft player)"));
			return;
		}
		const FString Path = FString::Join(Args, TEXT(" "));
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Mvr.Import %s: %s"), *Path, Controller->RequestImportMvr(Path) ? TEXT("reading") : TEXT("refused"));
	}

	/** "Confirm" / "Cancel": the review's IMPORT or CANCEL. */
	void Answer(UWorld* World, bool bConfirm)
	{
		AModularPlayerController* Controller = FindController(World);
		const UStageMvrImportSubsystem* Importer = FindImporter(World);
		const FStageMvrImportReview* Review = Importer ? Importer->GetReview() : nullptr;
		if (!Controller || !Review || Importer->GetState() != EStageMvrImportState::Review)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Mvr: no MVR review is open."));
			return;
		}
		if (bConfirm)
		{
			const FStageEconomyResultInfo Started = Controller->RequestConfirmMvrImport(Review->ReviewId);
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Mvr.Confirm: %s"), Started.IsSuccess() ? TEXT("placing") : *Started.Message.ToString());
		}
		else
		{
			Controller->RequestCancelMvrImport(Review->ReviewId);
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Mvr.Cancel: done."));
		}
	}

	/** "Status": the importer's state and review, and every fixture on the stage with its ID, patch, aim and MVR UUID. */
	void Status(const TArray<FString>& Args, UWorld* World)
	{
		const UStageMvrImportSubsystem* Importer = FindImporter(World);
		if (Importer)
		{
			int32 Done = 0;
			int32 Total = 0;
			Importer->GetProgress(Done, Total);
			const FStageMvrImportReview* Review = Importer->GetReview();
			static const TCHAR* const StateNames[] = { TEXT("Idle"), TEXT("Reading"), TEXT("Review"), TEXT("Placing") };
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Mvr.Status: %s, %d/%d placed%s%s"), StateNames[int32(Importer->GetState())],
				Done, Total, Review ? TEXT(", review: ") : TEXT(""), Review ? *Review->Description : TEXT(""));
		}
		const UStageSessionSubsystem* Session = World ? UWorld::GetSubsystem<UStageSessionSubsystem>(World) : nullptr;
		int32 Imported = 0;
		for (const AModularBaseActor* Item : Session ? Session->GetPlacedItems() : TArray<AModularBaseActor*>())
		{
			if (!Item || !Item->GetMvrId().IsValid())
			{
				continue;
			}
			++Imported;
			const ALightingFixtureActor* Fixture = Cast<ALightingFixtureActor>(Item);
			const FVector Location = Item->GetActorLocation();
			if (Fixture)
			{
				UE_LOG(LogStageCraft, Display, TEXT("  fixture %d \"%s\" %s at (%.0f, %.0f, %.0f) rot %s patch %d.%03d pan %.1f tilt %.1f mvr %s"), Fixture->GetFixtureId(),
					*Item->GetInstanceLabel().ToString(), *GetNameSafe(Fixture->GetFixtureData()), Location.X, Location.Y, Location.Z, *Item->GetActorRotation().ToCompactString(),
					Fixture->GetPatch().Universe, Fixture->GetPatch().Address, Fixture->GetAttribute(StageCraftTags::Attribute_Pan), Fixture->GetAttribute(StageCraftTags::Attribute_Tilt),
					*Item->GetMvrId().ToString(EGuidFormats::DigitsWithHyphens));
			}
			else
			{
				UE_LOG(LogStageCraft, Display, TEXT("  item \"%s\" %s at (%.0f, %.0f, %.0f) mvr %s"), *Item->GetInstanceLabel().ToString(), *Item->GetClass()->GetName(),
					Location.X, Location.Y, Location.Z, *Item->GetMvrId().ToString(EGuidFormats::DigitsWithHyphens));
			}
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Mvr.Status: %d imported items on the stage."), Imported);
	}

	FAutoConsoleCommandWithWorldAndArgs ImportCommand(TEXT("StageCraft.Mvr.Import"),
		TEXT("Reads an .mvr file for review through the player's request: StageCraft.Mvr.Import <path>. Then StageCraft.Mvr.Confirm or StageCraft.Mvr.Cancel."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Import));
	FAutoConsoleCommandWithWorld ConfirmCommand(TEXT("StageCraft.Mvr.Confirm"), TEXT("Imports the MVR file under review (the review's IMPORT)."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World) { Answer(World, true); }));
	FAutoConsoleCommandWithWorld CancelCommand(TEXT("StageCraft.Mvr.Cancel"), TEXT("Drops the MVR file under review (the review's CANCEL)."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World) { Answer(World, false); }));
	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.Mvr.Status"), TEXT("Logs the MVR importer's state and every imported item on the stage."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
}

#endif // !UE_BUILD_SHIPPING
