// Copyright Epic Games, Inc. All Rights Reserved.

#include "Fixtures/StageZipReader.h"

#include "Misc/Compression.h"

namespace StageZip
{
	namespace
	{
		constexpr uint32 EndOfDirectorySignature = 0x06054b50;
		constexpr uint32 DirectoryEntrySignature = 0x02014b50;
		constexpr uint32 LocalHeaderSignature = 0x04034b50;
		constexpr int64 EndOfDirectorySize = 22;
		constexpr int64 DirectoryEntrySize = 46;
		constexpr int64 LocalHeaderSize = 30;
		constexpr int64 MaxCommentSize = 0xFFFF;
		constexpr uint16 FlagEncrypted = 0x0001;
		constexpr uint16 FlagStrongEncryption = 0x0040;
		/** zlib's window for raw deflate (no zlib header), as ZIP stores it. */
		constexpr int32 RawDeflateWindow = -15;

		/** Little-endian reads that never pass the end of Data; false when they would. */
		bool Read16(TConstArrayView<uint8> Data, int64 Offset, uint16& Out)
		{
			if (Offset < 0 || Offset + 2 > Data.Num())
			{
				return false;
			}
			Out = uint16(Data[int32(Offset)] | (Data[int32(Offset + 1)] << 8));
			return true;
		}

		bool Read32(TConstArrayView<uint8> Data, int64 Offset, uint32& Out)
		{
			if (Offset < 0 || Offset + 4 > Data.Num())
			{
				return false;
			}
			const int32 At = int32(Offset);
			Out = uint32(Data[At]) | (uint32(Data[At + 1]) << 8) | (uint32(Data[At + 2]) << 16) | (uint32(Data[At + 3]) << 24);
			return true;
		}

		/** Names are decoded as UTF-8: ASCII names (all that GDTF produces) read the same under the legacy code page. */
		FString DecodeName(TConstArrayView<uint8> Bytes)
		{
			const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
			return FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());
		}

		const uint32* GetCrcTable()
		{
			static const TStaticArray<uint32, 256> Table = []()
			{
				TStaticArray<uint32, 256> Result;
				for (uint32 Index = 0; Index < 256; ++Index)
				{
					uint32 Value = Index;
					for (int32 Bit = 0; Bit < 8; ++Bit)
					{
						Value = (Value & 1) ? (0xEDB88320u ^ (Value >> 1)) : (Value >> 1);
					}
					Result[Index] = Value;
				}
				return Result;
			}();
			return Table.GetData();
		}
	}

	uint32 Crc32(TConstArrayView<uint8> Data)
	{
		const uint32* Table = GetCrcTable();
		uint32 Crc = 0xFFFFFFFFu;
		for (const uint8 Byte : Data)
		{
			Crc = Table[(Crc ^ Byte) & 0xFF] ^ (Crc >> 8);
		}
		return Crc ^ 0xFFFFFFFFu;
	}

	bool IsSafeEntryName(FStringView Name)
	{
		if (Name.IsEmpty() || Name[0] == TEXT('/') || Name[0] == TEXT('\\'))
		{
			return false;
		}
		for (const TCHAR Char : Name)
		{
			if (Char < 0x20 || Char == TEXT(':'))
			{
				return false; // Control characters, and drive letters or alternate data streams.
			}
		}
		int32 SegmentStart = 0;
		for (int32 Index = 0; Index <= Name.Len(); ++Index)
		{
			if (Index == Name.Len() || Name[Index] == TEXT('/') || Name[Index] == TEXT('\\'))
			{
				const FStringView Segment = Name.Mid(SegmentStart, Index - SegmentStart);
				const bool bTrailingSlash = Index == Name.Len() && Segment.IsEmpty();
				if (!bTrailingSlash && (Segment.IsEmpty() || Segment == TEXT(".") || Segment == TEXT("..")))
				{
					return false;
				}
				SegmentStart = Index + 1;
			}
		}
		return true;
	}

	const FEntry* FDirectory::Find(FStringView Name) const
	{
		return Entries.FindByPredicate([Name](const FEntry& Entry) { return FStringView(Entry.Name).Equals(Name, ESearchCase::IgnoreCase); });
	}

	bool ReadDirectory(TConstArrayView<uint8> Archive, const FLimits& Limits, FDirectory& OutDirectory, FString& OutError)
	{
		OutDirectory.Entries.Reset();
		if (Archive.Num() > Limits.MaxArchiveBytes)
		{
			OutError = FString::Printf(TEXT("The file is %lld bytes; the limit is %lld."), int64(Archive.Num()), Limits.MaxArchiveBytes);
			return false;
		}
		if (Archive.Num() < EndOfDirectorySize)
		{
			OutError = TEXT("The file is not a ZIP archive (too short).");
			return false;
		}

		// The end-of-directory record is the last thing in the file, followed only by its comment.
		int64 EndOffset = INDEX_NONE;
		const int64 SearchStop = FMath::Max<int64>(0, Archive.Num() - EndOfDirectorySize - MaxCommentSize);
		for (int64 Offset = Archive.Num() - EndOfDirectorySize; Offset >= SearchStop; --Offset)
		{
			uint32 Signature = 0;
			uint16 CommentLength = 0;
			if (Read32(Archive, Offset, Signature) && Signature == EndOfDirectorySignature
				&& Read16(Archive, Offset + 20, CommentLength) && Offset + EndOfDirectorySize + CommentLength == Archive.Num())
			{
				EndOffset = Offset;
				break;
			}
		}
		if (EndOffset == INDEX_NONE)
		{
			OutError = TEXT("The file is not a ZIP archive (no end of central directory).");
			return false;
		}

		uint16 DiskNumber = 0, DirectoryDisk = 0, EntriesOnDisk = 0, EntryCount = 0;
		uint32 DirectorySize = 0, DirectoryOffset = 0;
		Read16(Archive, EndOffset + 4, DiskNumber);
		Read16(Archive, EndOffset + 6, DirectoryDisk);
		Read16(Archive, EndOffset + 8, EntriesOnDisk);
		Read16(Archive, EndOffset + 10, EntryCount);
		Read32(Archive, EndOffset + 12, DirectorySize);
		Read32(Archive, EndOffset + 16, DirectoryOffset);
		if (EntryCount == 0xFFFF || DirectorySize == 0xFFFFFFFFu || DirectoryOffset == 0xFFFFFFFFu)
		{
			OutError = TEXT("ZIP64 archives are not supported.");
			return false;
		}
		if (DiskNumber != 0 || DirectoryDisk != 0 || EntriesOnDisk != EntryCount)
		{
			OutError = TEXT("Archives split over several disks are not supported.");
			return false;
		}
		if (EntryCount > Limits.MaxEntries)
		{
			OutError = FString::Printf(TEXT("The archive has %d entries; the limit is %d."), int32(EntryCount), Limits.MaxEntries);
			return false;
		}
		if (int64(DirectoryOffset) + DirectorySize > EndOffset)
		{
			OutError = TEXT("The central directory lies outside the file.");
			return false;
		}

		OutDirectory.Entries.Reserve(EntryCount);
		int64 Offset = DirectoryOffset;
		int64 TotalBytes = 0;
		for (int32 Index = 0; Index < EntryCount; ++Index)
		{
			uint32 Signature = 0;
			uint16 Flags = 0, Method = 0, NameLength = 0, ExtraLength = 0, CommentLength = 0;
			uint32 Crc = 0, CompressedSize = 0, UncompressedSize = 0, LocalOffset = 0;
			const bool bRead = Read32(Archive, Offset, Signature) && Read16(Archive, Offset + 8, Flags) && Read16(Archive, Offset + 10, Method)
				&& Read32(Archive, Offset + 16, Crc) && Read32(Archive, Offset + 20, CompressedSize) && Read32(Archive, Offset + 24, UncompressedSize)
				&& Read16(Archive, Offset + 28, NameLength) && Read16(Archive, Offset + 30, ExtraLength) && Read16(Archive, Offset + 32, CommentLength)
				&& Read32(Archive, Offset + 42, LocalOffset);
			const int64 RecordEnd = Offset + DirectoryEntrySize + NameLength + ExtraLength + CommentLength;
			if (!bRead || Signature != DirectoryEntrySignature || RecordEnd > int64(DirectoryOffset) + DirectorySize)
			{
				OutError = FString::Printf(TEXT("Central directory entry %d is damaged."), Index + 1);
				return false;
			}
			if (NameLength == 0 || NameLength > Limits.MaxNameLength)
			{
				OutError = FString::Printf(TEXT("Entry %d has a name of %d bytes; the limit is %d."), Index + 1, int32(NameLength), Limits.MaxNameLength);
				return false;
			}

			FString Name = DecodeName(Archive.Slice(int32(Offset + DirectoryEntrySize), NameLength));
			Name.ReplaceCharInline(TEXT('\\'), TEXT('/'));
			Offset = RecordEnd;
			if (!IsSafeEntryName(Name))
			{
				OutError = FString::Printf(TEXT("Entry \"%s\" has an unsafe name."), *Name.Left(Limits.MaxNameLength));
				return false;
			}
			if (Name.EndsWith(TEXT("/")))
			{
				continue; // A directory: nothing to extract.
			}
			if (Flags & (FlagEncrypted | FlagStrongEncryption))
			{
				OutError = FString::Printf(TEXT("Entry \"%s\" is encrypted."), *Name);
				return false;
			}
			if (Method != 0 && Method != 8)
			{
				OutError = FString::Printf(TEXT("Entry \"%s\" uses compression method %d; only stored and deflate are supported."), *Name, int32(Method));
				return false;
			}
			if (CompressedSize == 0xFFFFFFFFu || UncompressedSize == 0xFFFFFFFFu || LocalOffset == 0xFFFFFFFFu)
			{
				OutError = TEXT("ZIP64 archives are not supported.");
				return false;
			}
			if (Method == 0 && CompressedSize != UncompressedSize)
			{
				OutError = FString::Printf(TEXT("Stored entry \"%s\" has different stored and extracted sizes."), *Name);
				return false;
			}
			if (UncompressedSize > Limits.MaxEntryBytes)
			{
				OutError = FString::Printf(TEXT("Entry \"%s\" is %u bytes extracted; the limit is %lld."), *Name, UncompressedSize, Limits.MaxEntryBytes);
				return false;
			}
			if (UncompressedSize > Limits.MinRatioCheckBytes && int64(UncompressedSize) > int64(CompressedSize) * Limits.MaxCompressionRatio)
			{
				OutError = FString::Printf(TEXT("Entry \"%s\" expands more than %d times; it is refused as a possible zip bomb."), *Name, Limits.MaxCompressionRatio);
				return false;
			}
			TotalBytes += UncompressedSize;
			if (TotalBytes > Limits.MaxTotalBytes)
			{
				OutError = FString::Printf(TEXT("The archive extracts to more than %lld bytes."), Limits.MaxTotalBytes);
				return false;
			}
			if (int64(LocalOffset) + LocalHeaderSize + CompressedSize > DirectoryOffset)
			{
				OutError = FString::Printf(TEXT("Entry \"%s\" lies outside the archive's data."), *Name);
				return false;
			}
			if (OutDirectory.Find(Name))
			{
				OutError = FString::Printf(TEXT("The archive has two entries named \"%s\"."), *Name);
				return false;
			}

			FEntry& Entry = OutDirectory.Entries.AddDefaulted_GetRef();
			Entry.Name = MoveTemp(Name);
			Entry.Method = Method;
			Entry.Crc32 = Crc;
			Entry.CompressedSize = CompressedSize;
			Entry.UncompressedSize = UncompressedSize;
			Entry.LocalHeaderOffset = LocalOffset;
		}
		return true;
	}

	bool Extract(TConstArrayView<uint8> Archive, const FEntry& Entry, const FLimits& Limits, TArray<uint8>& OutBytes, FString& OutError)
	{
		OutBytes.Reset();
		uint32 Signature = 0;
		uint16 NameLength = 0, ExtraLength = 0;
		if (!Read32(Archive, Entry.LocalHeaderOffset, Signature) || Signature != LocalHeaderSignature
			|| !Read16(Archive, Entry.LocalHeaderOffset + 26, NameLength) || !Read16(Archive, Entry.LocalHeaderOffset + 28, ExtraLength))
		{
			OutError = FString::Printf(TEXT("Entry \"%s\" has a damaged local header."), *Entry.Name);
			return false;
		}
		// The central directory's sizes are authoritative: the local header may defer them to a data descriptor.
		const int64 DataOffset = Entry.LocalHeaderOffset + LocalHeaderSize + NameLength + ExtraLength;
		if (DataOffset + Entry.CompressedSize > Archive.Num() || Entry.UncompressedSize > Limits.MaxEntryBytes)
		{
			OutError = FString::Printf(TEXT("Entry \"%s\" lies outside the file."), *Entry.Name);
			return false;
		}
		const TConstArrayView<uint8> Data = Archive.Slice(int32(DataOffset), int32(Entry.CompressedSize));

		if (Entry.Method == 0)
		{
			OutBytes.Append(Data.GetData(), Data.Num());
		}
		else if (Entry.UncompressedSize > 0)
		{
			OutBytes.SetNumUninitialized(int32(Entry.UncompressedSize));
			const bool bInflated = FCompression::UncompressMemory(NAME_Zlib, OutBytes.GetData(), OutBytes.Num(), Data.GetData(), Data.Num(),
				COMPRESS_NoFlags, static_cast<uintptr_t>(static_cast<intptr_t>(RawDeflateWindow)));
			if (!bInflated)
			{
				OutBytes.Reset();
				OutError = FString::Printf(TEXT("Entry \"%s\" could not be decompressed."), *Entry.Name);
				return false;
			}
		}

		if (Crc32(OutBytes) != Entry.Crc32)
		{
			OutBytes.Reset();
			OutError = FString::Printf(TEXT("Entry \"%s\" is damaged (CRC mismatch)."), *Entry.Name);
			return false;
		}
		return true;
	}
}
