// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Fixtures/StageFixtureLibrarySubsystem.h"
#include "Mvr/StageMvrParser.h"

/** Everything MV2 needs from an .mvr file, read on a worker: the scene, its GDTF files parsed, and the bounds of its truss geometry. */
struct FStageMvrArchive
{
	FStageMvrParseResult Parsed;

	/** One per distinct GDTFSpec found in the archive, keyed by the spec's lower-case file name (as the planner looks it up). */
	TMap<FString, TSharedPtr<UStageFixtureLibrarySubsystem::FParsedFile>> GdtfFiles;

	/** Lower-case geometry file name -> bounds (MVR mm), for the files trusses use. */
	TMap<FString, FBox> GeometryBounds;

	/** Geometry files that could not be read, user-facing ("truss.glb: it has no mesh positions with bounds"). */
	TArray<FString> GeometryProblems;

	/** Bytes of the .mvr file and the largest extracted entry: the read's peak working set, for the log. */
	int64 ArchiveBytes = 0;
	int64 LargestEntryBytes = 0;
	double ReadMs = 0.0;
};

/**
 * Reads an .mvr file's bytes into an FStageMvrArchive (Docs/ADR/0034 §6, MV2). Pure apart from the GDTF parse it shares with the
 * library; safe on a worker thread. Memory: the archive is taken by value and released before returning; each entry is extracted,
 * used and released in turn (a GDTF keeps its bytes, which the library copies into its folder), so the peak is the archive plus one
 * entry plus the scene's XML tree, all bounded by StageMvr::FLimits.
 */
namespace StageMvrArchive
{
	FStageMvrArchive Read(TArray<uint8> FileBytes, const StageMvr::FLimits& Limits = StageMvr::FLimits());
}
