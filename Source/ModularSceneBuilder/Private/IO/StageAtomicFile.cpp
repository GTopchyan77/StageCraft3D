// Copyright Epic Games, Inc. All Rights Reserved.

#include "IO/StageAtomicFile.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

namespace StageAtomicFile
{
	namespace
	{
		bool WriteFlushed(const FString& TempPath, TConstArrayView64<uint8> Bytes)
		{
			IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
			PlatformFile.CreateDirectoryTree(*FPaths::GetPath(TempPath));
			TUniquePtr<IFileHandle> Handle(PlatformFile.OpenWrite(*TempPath, /*bAppend*/ false, /*bAllowRead*/ false));
			if (!Handle)
			{
				return false;
			}
			// A full flush reaches the disk before the rename, so the journal can never keep the rename without the data.
			return Handle->Write(Bytes.GetData(), Bytes.Num()) && Handle->Flush(/*bFullFlush*/ true);
		}
	}

	bool WriteBytes(const FString& Path, TConstArrayView64<uint8> Bytes, EStageFileWrite Mode)
	{
		const FString TempPath = Path + TEXT(".tmp");
		IFileManager& FileManager = IFileManager::Get();
		const bool bReplace = Mode == EStageFileWrite::ReplaceExisting;
		if (!WriteFlushed(TempPath, Bytes)
			|| !FileManager.Move(*Path, *TempPath, bReplace, /*bEvenIfReadOnly*/ false, /*bAttributes*/ false, /*bDoNotRetryOrError*/ true))
		{
			FileManager.Delete(*TempPath, /*RequireExists*/ false, /*EvenReadOnly*/ false, /*Quiet*/ true);
			return false;
		}
		return true;
	}

	bool WriteText(const FString& Path, const FString& Text, EStageFileWrite Mode)
	{
		const FTCHARToUTF8 Utf8(*Text, Text.Len());
		return WriteBytes(Path, TConstArrayView64<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), Mode);
	}

	bool MoveAside(const FString& Path)
	{
		return IFileManager::Get().Move(*(Path + TEXT(".bak")), *Path, /*bReplace*/ true, /*bEvenIfReadOnly*/ false, /*bAttributes*/ false, /*bDoNotRetryOrError*/ true);
	}
}
