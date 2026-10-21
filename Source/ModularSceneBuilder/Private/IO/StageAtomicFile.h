// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Whether a write may replace a file that already exists at the target path. */
enum class EStageFileWrite : uint8
{
	ReplaceExisting,
	/** Fails if the target exists (renders write to paths chosen to be unique). */
	KeepExisting,
};

/**
 * Crash-safe file writes for everything StageCraft saves: scenes, autosaves, layouts, the session, the import list, reports and
 * renders (Docs/ADR/0013-preferences-key-bindings-and-audit-fixes.md §3.3).
 *
 * A write goes to Path.tmp, is flushed to the disk (not just to the OS cache) and only then renamed over Path. After a crash or
 * a power cut, Path therefore holds either its old content or the new content, never a torn or zero-filled file; a stale .tmp
 * may remain and is overwritten by the next write. On failure the temporary file is removed and Path is untouched.
 *
 * Any thread: file system only. Not for concurrent writes to the same path (callers serialise them, e.g. with a pipe).
 */
namespace StageAtomicFile
{
	/** Writes Text as UTF-8 without a byte order mark (a leading U+FEFF in Text is written as one, as the CSV exports want). */
	bool WriteText(const FString& Path, const FString& Text, EStageFileWrite Mode = EStageFileWrite::ReplaceExisting);

	bool WriteBytes(const FString& Path, TConstArrayView64<uint8> Bytes, EStageFileWrite Mode = EStageFileWrite::ReplaceExisting);

	/**
	 * Moves a file that could not be read to Path.bak, so the next save cannot overwrite what the user may want back; an older
	 * .bak is replaced. Returns false if the file could not be moved (it is then still at Path).
	 */
	bool MoveAside(const FString& Path);
}
