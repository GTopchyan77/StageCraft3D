// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * A small, strict ZIP reader for the GDTF container (Docs/ADR/0033-gdtf-fixture-import-and-device-library.md §8), later MVR.
 *
 * Only what those formats use: stored (method 0) and deflate (method 8) entries in a single-disk archive. ZIP64, encryption and
 * spanned archives are refused. Every offset, size and name comes from untrusted input and is checked before it is used, and each
 * extracted entry is checked against its CRC-32. Deflate goes through the engine's zlib (FCompression with a raw-deflate window).
 *
 * Pure: no files, no UObjects. Safe on any thread.
 */
namespace StageZip
{
	/** Caps applied to an archive and its entries. The defaults are ADR 0033 §8's. */
	struct FLimits
	{
		int64 MaxArchiveBytes = 50ll * 1024 * 1024;
		int32 MaxEntries = 2000;
		/** Largest single entry once extracted. */
		int64 MaxEntryBytes = 64ll * 1024 * 1024;
		/** Sum of every entry's extracted size. */
		int64 MaxTotalBytes = 200ll * 1024 * 1024;
		/** Extracted size over stored size, checked for entries above MinRatioCheckBytes (a "zip bomb" check). */
		int32 MaxCompressionRatio = 100;
		int64 MinRatioCheckBytes = 1024 * 1024;
		int32 MaxNameLength = 260;
	};

	/** One file in the archive (directories are left out). */
	struct FEntry
	{
		/** Forward slashes, no leading slash; checked by IsSafeEntryName and unique ignoring case. */
		FString Name;
		/** 0 stored, 8 deflate. */
		uint16 Method = 0;
		uint32 Crc32 = 0;
		int64 CompressedSize = 0;
		int64 UncompressedSize = 0;
		int64 LocalHeaderOffset = 0;
	};

	/** The archive's central directory. */
	struct FDirectory
	{
		TArray<FEntry> Entries;

		/** The entry named Name (case-insensitive), or null. */
		const FEntry* Find(FStringView Name) const;
	};

	/** Reads and checks the central directory. False, with a reason in OutError, when the archive breaks a rule. */
	bool ReadDirectory(TConstArrayView<uint8> Archive, const FLimits& Limits, FDirectory& OutDirectory, FString& OutError);

	/** Extracts Entry (from ReadDirectory on the same Archive), checking its local header, sizes and CRC-32. */
	bool Extract(TConstArrayView<uint8> Archive, const FEntry& Entry, const FLimits& Limits, TArray<uint8>& OutBytes, FString& OutError);

	/** CRC-32 as ZIP uses it (IEEE 802.3, reflected, initial and final value 0xFFFFFFFF). */
	uint32 Crc32(TConstArrayView<uint8> Data);

	/**
	 * True for a safe relative entry name: not empty, no leading slash or backslash, no drive letter, no "." or ".." segment, no
	 * empty segment and no control character. Backslashes count as separators.
	 */
	bool IsSafeEntryName(FStringView Name);
}
