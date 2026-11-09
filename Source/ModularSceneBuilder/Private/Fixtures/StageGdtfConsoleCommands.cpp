// Copyright Epic Games, Inc. All Rights Reserved.

// Development console command for the GDTF spike (Docs/ADR/0033 §13 GD0): reads every .gdtf in a folder and logs whether it parses,
// each mode's layout, the time it took and which GDTF attributes StageCraft cannot drive. Compiled out of Shipping. It runs on the game
// thread on purpose: it is a measurement tool for a local corpus, not an import path.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Fixtures/StageFixtureLibrarySubsystem.h"
#include "Fixtures/StageGdtfMapping.h"
#include "Fixtures/StageGdtfParser.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Show/StageFixtureCells.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Fixtures/Tests/StageGdtfTestFiles.h"
#endif

namespace StageGdtfConsoleCommands
{
	void Probe(const TArray<FString>& Args)
	{
		const FString Folder = Args.IsEmpty() ? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("GDTFCorpus")) : Args[0];
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *FPaths::Combine(Folder, TEXT("*.gdtf")), /*Files*/ true, /*Directories*/ false);
		Files.Sort();
		if (Files.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Fixtures.GdtfProbe: no .gdtf files in %s."), *Folder);
			return;
		}

		int32 Imported = 0;
		double TotalMs = 0.0;
		double SlowestMs = 0.0;
		FString Slowest;
		TMap<FString, int32> AtDefault;
		TMap<FString, int32> Approximated;
		for (const FString& File : Files)
		{
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *FPaths::Combine(Folder, File)))
			{
				UE_LOG(LogStageCraft, Warning, TEXT("StageCraft.Fixtures.GdtfProbe: %s could not be read."), *File);
				continue;
			}
			const double Start = FPlatformTime::Seconds();
			const FStageGdtfParseResult Parsed = StageGdtf::Parse(Bytes);
			TArray<FStageGdtfModeLayout> Layouts;
			for (int32 Mode = 0; Parsed.IsOk() && Mode < Parsed.Type.Modes.Num(); ++Mode)
			{
				Layouts.Add(StageGdtfMapping::BuildModeLayout(Parsed.Type, Mode));
			}
			const FStageGdtfCapabilities Caps = StageGdtfMapping::DeriveCapabilities(Parsed.Type, Layouts);
			const FStageGdtfImportReport Report = StageGdtfMapping::BuildReport(Parsed, Layouts, Caps);
			const double Ms = (FPlatformTime::Seconds() - Start) * 1000.0;
			TotalMs += Ms;
			if (Ms > SlowestMs)
			{
				SlowestMs = Ms;
				Slowest = File;
			}
			Imported += Report.bCanImport ? 1 : 0;
			for (const FStageGdtfModeLayout& Layout : Layouts)
			{
				for (const FStageGdtfLayoutChannel& Channel : Layout.Channels)
				{
					if (Channel.Status == EStageGdtfChannelStatus::Default)
					{
						++AtDefault.FindOrAdd(Channel.GdtfAttribute);
					}
					else if (Channel.Status == EStageGdtfChannelStatus::Approximated)
					{
						++Approximated.FindOrAdd(Channel.GdtfAttribute);
					}
				}
			}
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Fixtures.GdtfProbe: %s %s in %.2f ms (%d KB, %d cells)\n%s"), Report.bCanImport ? TEXT("OK     ") : TEXT("REFUSED"),
				*File, Ms, Bytes.Num() / 1024, Caps.CellCount, *Report.ToText());
		}

		auto Top = [](TMap<FString, int32>& Counts)
		{
			Counts.ValueSort([](int32 A, int32 B) { return A > B; });
			TArray<FString> Parts;
			for (const TPair<FString, int32>& Pair : Counts)
			{
				if (Parts.Num() == 30)
				{
					break;
				}
				Parts.Add(FString::Printf(TEXT("%s x%d"), *Pair.Key, Pair.Value));
			}
			return FString::Join(Parts, TEXT(", "));
		};
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Fixtures.GdtfProbe: %d of %d files can be imported; %.2f ms on average, slowest %.2f ms (%s).\n  Kept at default: %s\n  Approximated: %s"),
			Imported, Files.Num(), TotalMs / Files.Num(), SlowestMs, *Slowest, *Top(AtDefault), *Top(Approximated));
	}

	FAutoConsoleCommand ProbeCommand(TEXT("StageCraft.Fixtures.GdtfProbe"),
		TEXT("Reads every .gdtf in a folder (default Saved/GDTFCorpus) and logs the import report, timings and unmapped attributes. Development only."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Probe));

#if WITH_DEV_AUTOMATION_TESTS
	void AddTestBar(const TArray<FString>& Args, UWorld* World)
	{
		const int32 Pixels = Args.IsEmpty() ? StageFixtureCells::MaxCells : FCString::Atoi(*Args[0]);
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		UStageFixtureLibrarySubsystem* Library = GameInstance ? GameInstance->GetSubsystem<UStageFixtureLibrarySubsystem>() : nullptr;
		if (!Library || Pixels < 1 || Pixels > StageFixtureCells::MaxCells)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Fixtures.AddTestBar <1..%d>: needs a game world and a pixel count in range."), StageFixtureCells::MaxCells);
			return;
		}
		const FStageFixtureImportOutcome Outcome = Library->AddSessionType(StageGdtfTestFiles::MakeGdtf(StageGdtfTestFiles::BarXml(Pixels)), FString::Printf(TEXT("Test Bar %d"), Pixels));
		const ULightingFixtureData* Mode = Outcome.Modes.IsEmpty() ? nullptr : Outcome.Modes[0].Get();
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Fixtures.AddTestBar: %s %s"), *Outcome.Message.ToString(),
			Mode ? *FString::Printf(TEXT("Bench it with Item=%s (%d cells, %d channels)."), *Mode->GetName(), Mode->CellCount, Mode->GetDMXFootprint()) : TEXT(""));
	}

	FAutoConsoleCommandWithWorldAndArgs AddTestBarCommand(TEXT("StageCraft.Fixtures.AddTestBar"),
		TEXT("Adds the self-authored test pixel bar with N cells (default the maximum) to the Library for this session only; nothing is written. Development only."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&AddTestBar));
#endif
}

#endif
