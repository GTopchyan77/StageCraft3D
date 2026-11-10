// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageImageLookReader.h"

#include "Async/Async.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Sketch/StageSketchImageImport.h"
#include "Tasks/Task.h"

#define LOCTEXT_NAMESPACE "StageImageLookReader"

namespace StageImageLookReader
{
	void ReadAsync(const FString& FilePath, int32 Count, TUniqueFunction<void(FStageImageLookRead&&)> OnDone)
	{
		check(IsInGameThread());
		// Loaded here: the worker may only use it.
		FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		UE::Tasks::Launch(UE_SOURCE_LOCATION, [FilePath, Count, OnDone = MoveTemp(OnDone)]() mutable
		{
			FStageImageLookRead Read;
			TArray<uint8> BGRA;
			int32 Width = 0;
			int32 Height = 0;
			if (!StageSketchImageImport::HasSupportedExtension(FilePath))
			{
				Read.Error = LOCTEXT("Format", "Choose a PNG or JPEG image.");
			}
			else if (StageSketchImageImport::LoadImageFile(FilePath, BGRA, Width, Height, Read.Error))
			{
				Read.Swatches = StageImagePalette::Extract(BGRA, Width, Height, Count);
				if (Read.Swatches.IsEmpty())
				{
					Read.Error = LOCTEXT("NoColours", "No colours could be taken from the image.");
				}
			}
			AsyncTask(ENamedThreads::GameThread, [Read = MoveTemp(Read), OnDone = MoveTemp(OnDone)]() mutable
			{
				OnDone(MoveTemp(Read));
			});
		});
	}
}

#undef LOCTEXT_NAMESPACE
