// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Sketch/StageSketchImageTrace.h"

/**
 * One photo or scan being read for the sketch (Docs/ADR/0017 §2.6): the inputs, set on the game thread, and everything the worker
 * thread makes from the file. Plain data only, so it can cross threads; shared between the worker and the game thread through a
 * TSharedRef, and touched by one thread at a time (the worker until it finishes, then the game thread).
 */
struct FStageSketchImportJob
{
	// --- Inputs ---
	FString FilePath;
	FStageSketchTraceOptions Options;
	/** Also encode a JPEG for the online reading. */
	bool bWantVisionImage = false;

	// --- Outputs ---
	bool bDecoded = false;
	FText Error;
	int32 SourceWidth = 0;
	int32 SourceHeight = 0;

	/** The photo for the canvas background, BGRA8, alpha flattened onto white. */
	TArray<uint8> Preview;
	int32 PreviewWidth = 0;
	int32 PreviewHeight = 0;

	/** The photo for the online reading (JPEG), when asked for. */
	TArray<uint8> VisionJpeg;
	int32 VisionWidth = 0;
	int32 VisionHeight = 0;

	/** The local reading, always made: the result, or the fallback when the online reading fails. */
	FStageSketchTraceResult Trace;
};

/** Reading image files for the sketch. Validation is part of decoding: the file is untrusted input (Rules.md §55). */
namespace StageSketchImageImport
{
	/** Larger files are refused before they are read. */
	constexpr int64 MaxFileBytes = 64ll * 1024 * 1024;

	/** Larger images are refused from their header, before anything is decompressed. */
	constexpr int32 MaxDimension = 12000;
	constexpr int64 MaxPixels = 60ll * 1000 * 1000;

	/** Longest side of the canvas background and of the image sent for the online reading. */
	constexpr int32 PreviewMaxSide = 1024;
	constexpr int32 VisionMaxSide = 1568;

	/** .png, .jpg or .jpeg. */
	bool HasSupportedExtension(const FString& FilePath);

	/** The file dialog filter for the supported formats. */
	FString GetFileDialogFilter();

	/**
	 * Decodes PNG or JPEG bytes into BGRA8 (alpha flattened onto white) after checking the format and the header's dimensions.
	 * False, with a user-facing reason, otherwise. Thread-safe once the ImageWrapper module is loaded.
	 */
	bool DecodeImage(TConstArrayView64<uint8> FileBytes, TArray<uint8>& OutBGRA, int32& OutWidth, int32& OutHeight, FText& OutError);

	/** Rec. 601 luma of BGRA8 pixels. */
	FStageSketchGrayImage ToGray(TConstArrayView<uint8> BGRA, int32 Width, int32 Height);

	/** BGRA8 scaled down (box filter) so the longest side is at most MaxSide; unchanged when it fits. */
	TArray<uint8> DownscaleBGRA(TConstArrayView<uint8> BGRA, int32 Width, int32 Height, int32 MaxSide, int32& OutWidth, int32& OutHeight);

	/** Reads, decodes and traces Job's file. Runs on a worker thread; loads nothing that needs the game thread. */
	void Run(FStageSketchImportJob& Job);
}
