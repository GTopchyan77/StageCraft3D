// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * A small ZIP writer, the counterpart of StageZip's reader, for the containers StageCraft writes (MVR export, Docs/ADR/0034 §7).
 *
 * Writes what the reader accepts and what GDTF / MVR require (DIN SPEC 15800 / 15801): a single-disk archive of stored (method 0)
 * or deflate (method 8) entries, no ZIP64, no encryption, no extra fields, names in UTF-8 (general purpose flag bit 11). Each entry
 * is deflated through the engine's zlib with a raw-deflate window and stored instead when deflate would not make it smaller, or
 * when the caller asks to store it (files that are already compressed, such as a .gdtf, which is itself a ZIP).
 *
 * Every entry is dated 1980-01-01 00:00, the earliest DOS time, so the same files always give the same bytes (tests compare them).
 *
 * Pure: no files, no UObjects. Safe on any thread; one writer per thread.
 */
namespace StageZip
{
	/** How AddFile writes an entry. */
	enum class EStore : uint8
	{
		/** Deflate, kept only when it is smaller than the data. */
		Deflate,
		/** Stored as it is: for data that is already compressed. */
		Store,
	};

	class FWriter
	{
	public:
		/** The ZIP limits without ZIP64: entry count and every size and offset fit in their 16- and 32-bit fields. */
		static constexpr int32 MaxEntries = 0xFFFF;
		static constexpr int64 MaxBytes = 0xFFFFFFFFll;

		/**
		 * Adds a file. False, with OutError, when Name is not a safe entry name (StageZip::IsSafeEntryName), repeats an earlier name
		 * (compared ignoring case, as the reader does), or the archive would pass MaxEntries or MaxBytes. Nothing is added then.
		 */
		bool AddFile(const FString& Name, TConstArrayView<uint8> Data, EStore Store, FString& OutError);

		/** Ends the archive (central directory and its end record) and hands over its bytes. The writer is empty afterwards. */
		TArray<uint8> Finish();

		int32 Num() const { return Entries.Num(); }

	private:
		struct FWritten
		{
			FString Name;
			TArray<uint8> NameUtf8;
			uint16 Method = 0;
			uint32 Crc32 = 0;
			uint32 CompressedSize = 0;
			uint32 UncompressedSize = 0;
			uint32 LocalHeaderOffset = 0;
		};

		TArray<FWritten> Entries;
		TArray<uint8> Bytes;
		/** Size the central directory will have for Entries. */
		int64 DirectoryBytes = 0;
	};
}
