// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * File helpers shared by the report exports (bill of materials, patch sheet; Docs/ADR/0011-cue-list-and-patch-sheet-export.md
 * §3.2). Any thread: they touch only the file system.
 */
namespace StageReportFiles
{
	/** Saved/StageCraft/Reports, absolute. */
	FString GetOutputDirectory();

	/**
	 * Directory/Stem, or Directory/Stem_2, _3... when a file of that stem exists with any of Extensions (two exports within
	 * one second), so files written together (a PDF and its CSV) keep the same name. Returns the path without an extension.
	 */
	FString MakeUniqueStemPath(const FString& Directory, const FString& Stem, TConstArrayView<const TCHAR*> Extensions);

	/** Writes Bytes to Path.tmp, then moves it over Path, so a reader never sees half a file. False (and no file) on failure. */
	bool WriteBytesAtomic(const FString& Path, TConstArrayView<uint8> Bytes);
}
