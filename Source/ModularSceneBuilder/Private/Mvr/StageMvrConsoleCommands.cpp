// Copyright Epic Games, Inc. All Rights Reserved.

// MV0 probe (Docs/ADR/0034 §10): reads every .mvr file in a folder with the MV1 parser and logs what it found, the report and the
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
#include "Mvr/StageMvrParser.h"

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
}

#endif // !UE_BUILD_SHIPPING
