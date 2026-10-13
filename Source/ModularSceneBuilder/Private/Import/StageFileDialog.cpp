// Copyright Epic Games, Inc. All Rights Reserved.

#include "Import/StageFileDialog.h"

#include "Framework/Application/SlateApplication.h"
#include "Misc/Paths.h"

#if WITH_STAGECRAFT_FILE_DIALOG
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#endif

namespace StageFileDialog
{
	bool IsAvailable()
	{
#if WITH_STAGECRAFT_FILE_DIALOG
		return FDesktopPlatformModule::Get() != nullptr;
#else
		return false;
#endif
	}

	bool OpenFiles(const FText& Title, const FString& DefaultDirectory, const FString& Filter, TArray<FString>& OutFiles)
	{
		OutFiles.Reset();
#if WITH_STAGECRAFT_FILE_DIALOG
		IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
		if (!DesktopPlatform || !FSlateApplication::IsInitialized())
		{
			return false;
		}

		// Parenting to the active app window keeps the dialog on top of it and modal to it.
		const void* ParentWindow = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
		TArray<FString> Picked;
		if (!DesktopPlatform->OpenFileDialog(ParentWindow, Title.ToString(), DefaultDirectory, FString(), Filter, EFileDialogFlags::Multiple, Picked))
		{
			return false;
		}
		for (const FString& File : Picked)
		{
			OutFiles.Add(FPaths::ConvertRelativePathToFull(File));
		}
		return !OutFiles.IsEmpty();
#else
		return false;
#endif
	}
}
