// Copyright Epic Games, Inc. All Rights Reserved.

#include "Report/StageReportFiles.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Workspace/StageLayoutStore.h"

namespace StageReportFiles
{
	FString GetOutputDirectory()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("StageCraft") / TEXT("Reports"));
	}

	FString MakeUniqueStemPath(const FString& Directory, const FString& Stem, TConstArrayView<const TCHAR*> Extensions)
	{
		const auto IsTaken = [&Extensions](const FString& Candidate)
		{
			for (const TCHAR* Extension : Extensions)
			{
				if (IFileManager::Get().FileExists(*(Candidate + Extension)))
				{
					return true;
				}
			}
			return false;
		};

		const FString Base = Directory / Stem;
		if (!IsTaken(Base))
		{
			return Base;
		}
		for (int32 Suffix = 2; Suffix < 1000; ++Suffix)
		{
			const FString Candidate = FString::Printf(TEXT("%s_%d"), *Base, Suffix);
			if (!IsTaken(Candidate))
			{
				return Candidate;
			}
		}
		// A thousand exports in one second does not happen; overwriting the first is the safe fallback.
		return Base;
	}

	bool WriteBytesAtomic(const FString& Path, TConstArrayView<uint8> Bytes)
	{
		const FString TempPath = Path + TEXT(".tmp");
		if (!FFileHelper::SaveArrayToFile(Bytes, *TempPath))
		{
			IFileManager::Get().Delete(*TempPath, /*RequireExists*/ false, /*EvenReadOnly*/ false, /*Quiet*/ true);
			return false;
		}
		if (!IFileManager::Get().Move(*Path, *TempPath, /*Replace*/ true, /*EvenIfReadOnly*/ false))
		{
			IFileManager::Get().Delete(*TempPath, /*RequireExists*/ false, /*EvenReadOnly*/ false, /*Quiet*/ true);
			return false;
		}
		return true;
	}

	bool WritePdfAndCsv(const FString& BasePath, TConstArrayView<uint8> Pdf, const FString& Csv)
	{
		const FString PdfPath = BasePath + TEXT(".pdf");
		if (!WriteBytesAtomic(PdfPath, Pdf))
		{
			return false;
		}
		if (!FStageLayoutStore::WriteFileAtomic(BasePath + TEXT(".csv"), Csv))
		{
			IFileManager::Get().Delete(*PdfPath, /*RequireExists*/ false, /*EvenReadOnly*/ false, /*Quiet*/ true);
			return false;
		}
		return true;
	}
}
