// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Show/StageImagePalette.h"

/** What reading a photo's colours produced: the swatches, or a user-facing reason. */
struct FStageImageLookRead
{
	TArray<FStageImageSwatch> Swatches;
	FText Error;
};

/**
 * Reads a photo's dominant colours off the game thread (STATE.md #86). The file is untrusted input: it goes through the same
 * size, format and dimension checks as the sketch's photo import (StageSketchImageImport::LoadImageFile, Rules.md §55).
 */
namespace StageImageLookReader
{
	/**
	 * Decodes FilePath and extracts up to Count colours on a worker; OnDone runs later on the game thread. The caller makes OnDone
	 * lifetime-safe (bind it weakly): it may run after the caller is gone. Loads the ImageWrapper module, so call it on the game thread.
	 */
	void ReadAsync(const FString& FilePath, int32 Count, TUniqueFunction<void(FStageImageLookRead&&)> OnDone);
}
