// Copyright Epic Games, Inc. All Rights Reserved.

#include "Fixtures/StageZipWriter.h"

#include "Fixtures/StageZipReader.h"
#include "Misc/Compression.h"

namespace StageZip
{
	namespace WriterFormat
	{
		constexpr uint32 LocalHeaderSignature = 0x04034b50;
		constexpr uint32 DirectoryEntrySignature = 0x02014b50;
		constexpr uint32 EndOfDirectorySignature = 0x06054b50;
		constexpr int64 LocalHeaderSize = 30;
		constexpr int64 DirectoryEntrySize = 46;
		constexpr int64 EndOfDirectorySize = 22;
		constexpr uint16 VersionNeeded = 20;
		constexpr uint16 FlagUtf8Names = 0x0800;
		constexpr uint16 MethodStored = 0;
		constexpr uint16 MethodDeflate = 8;
		constexpr uint16 DosTime = 0;
		constexpr uint16 DosDate = (0 << 9) | (1 << 5) | 1;
		constexpr int32 RawDeflateWindow = -15;

		void Write16(TArray<uint8>& Out, uint16 Value)
		{
			Out.Add(uint8(Value & 0xFF));
			Out.Add(uint8(Value >> 8));
		}

		void Write32(TArray<uint8>& Out, uint32 Value)
		{
			Write16(Out, uint16(Value & 0xFFFF));
			Write16(Out, uint16(Value >> 16));
		}

		bool Deflate(TConstArrayView<uint8> Data, TArray<uint8>& OutCompressed)
		{
			const int32 Bound = FCompression::CompressMemoryBound(NAME_Zlib, Data.Num(), COMPRESS_NoFlags, RawDeflateWindow);
			if (Bound <= 0)
			{
				return false;
			}
			OutCompressed.SetNumUninitialized(Bound);
			int32 Size = Bound;
			if (!FCompression::CompressMemory(NAME_Zlib, OutCompressed.GetData(), Size, Data.GetData(), Data.Num(), COMPRESS_NoFlags, RawDeflateWindow))
			{
				return false;
			}
			OutCompressed.SetNum(Size, EAllowShrinking::No);
			return true;
		}
	}

	bool FWriter::AddFile(const FString& Name, TConstArrayView<uint8> Data, EStore Store, FString& OutError)
	{
		if (!IsSafeEntryName(Name) || Name.Contains(TEXT("\\")))
		{
			OutError = FString::Printf(TEXT("\"%s\" is not a safe file name inside an archive."), *Name);
			return false;
		}
		if (Entries.ContainsByPredicate([&Name](const FWritten& Entry) { return Entry.Name.Equals(Name, ESearchCase::IgnoreCase); }))
		{
			OutError = FString::Printf(TEXT("\"%s\" is in the archive already."), *Name);
			return false;
		}
		if (Entries.Num() >= MaxEntries)
		{
			OutError = FString::Printf(TEXT("An archive holds at most %d files."), MaxEntries);
			return false;
		}

		FWritten Entry;
		Entry.Name = Name;
		const FTCHARToUTF8 Utf8(*Name);
		Entry.NameUtf8.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		Entry.Crc32 = StageZip::Crc32(Data);

		TArray<uint8> Compressed;
		const bool bDeflated = Store == EStore::Deflate && Data.Num() > 0 && WriterFormat::Deflate(Data, Compressed) && Compressed.Num() < Data.Num();
		const TConstArrayView<uint8> Payload = bDeflated ? TConstArrayView<uint8>(Compressed) : Data;
		Entry.Method = bDeflated ? WriterFormat::MethodDeflate : WriterFormat::MethodStored;

		const int64 Added = WriterFormat::LocalHeaderSize + Entry.NameUtf8.Num() + Payload.Num();
		const int64 EntryDirectoryBytes = WriterFormat::DirectoryEntrySize + Entry.NameUtf8.Num();
		const int64 DirectoryAfter = DirectoryBytes + EntryDirectoryBytes + WriterFormat::EndOfDirectorySize;
		if (int64(Data.Num()) > MaxBytes || int64(Bytes.Num()) + Added + DirectoryAfter > MaxBytes || int64(Bytes.Num()) + Added > MAX_int32)
		{
			OutError = FString::Printf(TEXT("\"%s\" would make the archive larger than a ZIP without ZIP64 can be."), *Name);
			return false;
		}
		Entry.CompressedSize = uint32(Payload.Num());
		Entry.UncompressedSize = uint32(Data.Num());
		Entry.LocalHeaderOffset = uint32(Bytes.Num());

		Bytes.Reserve(Bytes.Num() + int32(Added));
		WriterFormat::Write32(Bytes, WriterFormat::LocalHeaderSignature);
		WriterFormat::Write16(Bytes, WriterFormat::VersionNeeded);
		WriterFormat::Write16(Bytes, WriterFormat::FlagUtf8Names);
		WriterFormat::Write16(Bytes, Entry.Method);
		WriterFormat::Write16(Bytes, WriterFormat::DosTime);
		WriterFormat::Write16(Bytes, WriterFormat::DosDate);
		WriterFormat::Write32(Bytes, Entry.Crc32);
		WriterFormat::Write32(Bytes, Entry.CompressedSize);
		WriterFormat::Write32(Bytes, Entry.UncompressedSize);
		WriterFormat::Write16(Bytes, uint16(Entry.NameUtf8.Num()));
		WriterFormat::Write16(Bytes, 0);
		Bytes.Append(Entry.NameUtf8);
		Bytes.Append(Payload.GetData(), Payload.Num());
		DirectoryBytes += EntryDirectoryBytes;
		Entries.Add(MoveTemp(Entry));
		return true;
	}

	TArray<uint8> FWriter::Finish()
	{
		const uint32 DirectoryOffset = uint32(Bytes.Num());
		for (const FWritten& Entry : Entries)
		{
			WriterFormat::Write32(Bytes, WriterFormat::DirectoryEntrySignature);
			WriterFormat::Write16(Bytes, WriterFormat::VersionNeeded);
			WriterFormat::Write16(Bytes, WriterFormat::VersionNeeded);
			WriterFormat::Write16(Bytes, WriterFormat::FlagUtf8Names);
			WriterFormat::Write16(Bytes, Entry.Method);
			WriterFormat::Write16(Bytes, WriterFormat::DosTime);
			WriterFormat::Write16(Bytes, WriterFormat::DosDate);
			WriterFormat::Write32(Bytes, Entry.Crc32);
			WriterFormat::Write32(Bytes, Entry.CompressedSize);
			WriterFormat::Write32(Bytes, Entry.UncompressedSize);
			WriterFormat::Write16(Bytes, uint16(Entry.NameUtf8.Num()));
			WriterFormat::Write16(Bytes, 0);
			WriterFormat::Write16(Bytes, 0);
			WriterFormat::Write16(Bytes, 0);
			WriterFormat::Write16(Bytes, 0);
			WriterFormat::Write32(Bytes, 0);
			WriterFormat::Write32(Bytes, Entry.LocalHeaderOffset);
			Bytes.Append(Entry.NameUtf8);
		}
		const uint32 DirectorySize = uint32(Bytes.Num()) - DirectoryOffset;
		WriterFormat::Write32(Bytes, WriterFormat::EndOfDirectorySignature);
		WriterFormat::Write16(Bytes, 0);
		WriterFormat::Write16(Bytes, 0);
		WriterFormat::Write16(Bytes, uint16(Entries.Num()));
		WriterFormat::Write16(Bytes, uint16(Entries.Num()));
		WriterFormat::Write32(Bytes, DirectorySize);
		WriterFormat::Write32(Bytes, DirectoryOffset);
		WriterFormat::Write16(Bytes, 0);

		Entries.Reset();
		DirectoryBytes = 0;
		return MoveTemp(Bytes);
	}
}
