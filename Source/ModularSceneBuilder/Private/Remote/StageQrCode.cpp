// Copyright Epic Games, Inc. All Rights Reserved.

#include "Remote/StageQrCode.h"

namespace StageQrCodeImpl
{
	using namespace StageQrCode;

	// Level M rows of the standard's Table 9, versions 1-10 (index 0 unused). Only level M is offered (see the header).
	constexpr int32 EccCodewordsPerBlock[MaxVersion + 1] = { -1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26 };
	constexpr int32 EccBlocks[MaxVersion + 1] = { -1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5 };
	// The two format bits of level M (L=01, M=00, Q=11, H=10).
	constexpr int32 FormatBitsLevelM = 0;

	constexpr int32 PenaltyRun = 3;
	constexpr int32 PenaltyBlock = 3;
	constexpr int32 PenaltyFinderLike = 40;
	constexpr int32 PenaltyBalance = 10;

	int32 RawDataModules(int32 Version)
	{
		int32 Result = (16 * Version + 128) * Version + 64;
		if (Version >= 2)
		{
			const int32 Alignments = Version / 7 + 2;
			Result -= (25 * Alignments - 10) * Alignments - 55;
			if (Version >= 7)
			{
				Result -= 36;
			}
		}
		return Result;
	}

	int32 DataCodewords(int32 Version)
	{
		return RawDataModules(Version) / 8 - EccCodewordsPerBlock[Version] * EccBlocks[Version];
	}

	/** Byte mode's character count is 8 bits up to version 9 and 16 bits from version 10. */
	int32 CountBits(int32 Version)
	{
		return Version <= 9 ? 8 : 16;
	}

	uint8 Multiply(uint8 X, uint8 Y)
	{
		int32 Z = 0;
		for (int32 Bit = 7; Bit >= 0; --Bit)
		{
			Z = (Z << 1) ^ ((Z >> 7) * 0x11D);
			Z ^= ((Y >> Bit) & 1) * X;
		}
		return static_cast<uint8>(Z);
	}

	TArray<uint8> ReedSolomonDivisor(int32 Degree)
	{
		TArray<uint8> Result;
		Result.SetNumZeroed(Degree);
		Result[Degree - 1] = 1;
		uint8 Root = 1;
		for (int32 Index = 0; Index < Degree; ++Index)
		{
			for (int32 Term = 0; Term < Degree; ++Term)
			{
				Result[Term] = Multiply(Result[Term], Root);
				if (Term + 1 < Degree)
				{
					Result[Term] ^= Result[Term + 1];
				}
			}
			Root = Multiply(Root, 0x02);
		}
		return Result;
	}

	TArray<uint8> ReedSolomonRemainder(TConstArrayView<uint8> Data, const TArray<uint8>& Divisor)
	{
		TArray<uint8> Result;
		Result.SetNumZeroed(Divisor.Num());
		for (const uint8 Byte : Data)
		{
			const uint8 Factor = Byte ^ Result[0];
			Result.RemoveAt(0, 1, EAllowShrinking::No);
			Result.Add(0);
			for (int32 Index = 0; Index < Result.Num(); ++Index)
			{
				Result[Index] ^= Multiply(Divisor[Index], Factor);
			}
		}
		return Result;
	}

	/** Splits the data codewords into the version's blocks, adds each block's error correction and interleaves them (standard §7.6). */
	TArray<uint8> AddEccAndInterleave(const TArray<uint8>& Data, int32 Version)
	{
		const int32 Blocks = EccBlocks[Version];
		const int32 EccLength = EccCodewordsPerBlock[Version];
		const int32 RawCodewords = RawDataModules(Version) / 8;
		const int32 ShortBlocks = Blocks - RawCodewords % Blocks;
		const int32 ShortBlockLength = RawCodewords / Blocks;
		const TArray<uint8> Divisor = ReedSolomonDivisor(EccLength);

		TArray<TArray<uint8>> AllBlocks;
		int32 Offset = 0;
		for (int32 Block = 0; Block < Blocks; ++Block)
		{
			const int32 DataLength = ShortBlockLength - EccLength + (Block < ShortBlocks ? 0 : 1);
			TArray<uint8> Bytes(Data.GetData() + Offset, DataLength);
			Offset += DataLength;
			const TArray<uint8> Ecc = ReedSolomonRemainder(Bytes, Divisor);
			if (Block < ShortBlocks)
			{
				// A placeholder so every block has the same length; it is skipped when interleaving.
				Bytes.Add(0);
			}
			Bytes.Append(Ecc);
			AllBlocks.Add(MoveTemp(Bytes));
		}

		TArray<uint8> Result;
		Result.Reserve(RawCodewords);
		for (int32 Index = 0; Index < AllBlocks[0].Num(); ++Index)
		{
			for (int32 Block = 0; Block < AllBlocks.Num(); ++Block)
			{
				if (Index != ShortBlockLength - EccLength || Block >= ShortBlocks)
				{
					Result.Add(AllBlocks[Block][Index]);
				}
			}
		}
		return Result;
	}

	struct FQrBits
	{
		TArray<uint8> Bits;

		void Append(uint32 Value, int32 Count)
		{
			for (int32 Bit = Count - 1; Bit >= 0; --Bit)
			{
				Bits.Add(static_cast<uint8>((Value >> Bit) & 1));
			}
		}
	};

	class FBuilder
	{
	public:
		FBuilder(int32 InVersion)
			: Version(InVersion)
			, Size(17 + 4 * InVersion)
		{
			Modules.SetNumZeroed(Size * Size);
			IsFunction.SetNumZeroed(Size * Size);
		}

		bool Get(int32 X, int32 Y) const { return Modules[Y * Size + X] != 0; }

		void DrawFunctionPatterns()
		{
			for (int32 Index = 0; Index < Size; ++Index)
			{
				SetFunction(6, Index, Index % 2 == 0);
				SetFunction(Index, 6, Index % 2 == 0);
			}
			DrawFinder(3, 3);
			DrawFinder(Size - 4, 3);
			DrawFinder(3, Size - 4);

			const TArray<int32> Positions = AlignmentPositions();
			const int32 Count = Positions.Num();
			for (int32 Row = 0; Row < Count; ++Row)
			{
				for (int32 Column = 0; Column < Count; ++Column)
				{
					// The three corners already hold finder patterns.
					if (!((Row == 0 && Column == 0) || (Row == 0 && Column == Count - 1) || (Row == Count - 1 && Column == 0)))
					{
						DrawAlignment(Positions[Row], Positions[Column]);
					}
				}
			}
			// Reserves the format areas; the real bits are drawn once the mask is known.
			DrawFormatBits(0);
			DrawVersion();
		}

		void DrawCodewords(const TArray<uint8>& Codewords)
		{
			const int32 TotalBits = Codewords.Num() * 8;
			int32 BitIndex = 0;
			// Two-module columns, right to left, zig-zagging up and down; the vertical timing column is skipped.
			for (int32 Right = Size - 1; Right >= 1; Right -= 2)
			{
				if (Right == 6)
				{
					Right = 5;
				}
				for (int32 Vertical = 0; Vertical < Size; ++Vertical)
				{
					for (int32 Side = 0; Side < 2; ++Side)
					{
						const int32 X = Right - Side;
						const bool bUpward = ((Right + 1) & 2) == 0;
						const int32 Y = bUpward ? Size - 1 - Vertical : Vertical;
						if (!IsFunction[Y * Size + X] && BitIndex < TotalBits)
						{
							Modules[Y * Size + X] = (Codewords[BitIndex >> 3] >> (7 - (BitIndex & 7))) & 1;
							++BitIndex;
						}
					}
				}
			}
		}

		/** XOR-ing twice undoes a mask, which is how each candidate is tried. */
		void ApplyMask(int32 Mask)
		{
			for (int32 Y = 0; Y < Size; ++Y)
			{
				for (int32 X = 0; X < Size; ++X)
				{
					bool bInvert = false;
					switch (Mask)
					{
					case 0: bInvert = (X + Y) % 2 == 0; break;
					case 1: bInvert = Y % 2 == 0; break;
					case 2: bInvert = X % 3 == 0; break;
					case 3: bInvert = (X + Y) % 3 == 0; break;
					case 4: bInvert = (X / 3 + Y / 2) % 2 == 0; break;
					case 5: bInvert = X * Y % 2 + X * Y % 3 == 0; break;
					case 6: bInvert = (X * Y % 2 + X * Y % 3) % 2 == 0; break;
					default: bInvert = ((X + Y) % 2 + X * Y % 3) % 2 == 0; break;
					}
					if (bInvert && !IsFunction[Y * Size + X])
					{
						Modules[Y * Size + X] ^= 1;
					}
				}
			}
		}

		void DrawFormatBits(int32 Mask)
		{
			const int32 Data = (FormatBitsLevelM << 3) | Mask;
			int32 Remainder = Data;
			for (int32 Index = 0; Index < 10; ++Index)
			{
				Remainder = (Remainder << 1) ^ ((Remainder >> 9) * 0x537);
			}
			const int32 Bits = ((Data << 10) | Remainder) ^ 0x5412;
			auto Bit = [Bits](int32 Index) { return ((Bits >> Index) & 1) != 0; };

			for (int32 Index = 0; Index <= 5; ++Index)
			{
				SetFunction(8, Index, Bit(Index));
			}
			SetFunction(8, 7, Bit(6));
			SetFunction(8, 8, Bit(7));
			SetFunction(7, 8, Bit(8));
			for (int32 Index = 9; Index < 15; ++Index)
			{
				SetFunction(14 - Index, 8, Bit(Index));
			}
			for (int32 Index = 0; Index < 8; ++Index)
			{
				SetFunction(Size - 1 - Index, 8, Bit(Index));
			}
			for (int32 Index = 8; Index < 15; ++Index)
			{
				SetFunction(8, Size - 15 + Index, Bit(Index));
			}
			// The standard's always-dark module beside the lower-left finder.
			SetFunction(8, Size - 8, true);
		}

		int32 Penalty() const
		{
			int32 Result = 0;
			for (int32 Line = 0; Line < Size; ++Line)
			{
				Result += LinePenalty(Line, /*bRow*/ true);
				Result += LinePenalty(Line, /*bRow*/ false);
			}
			for (int32 Y = 0; Y < Size - 1; ++Y)
			{
				for (int32 X = 0; X < Size - 1; ++X)
				{
					const bool bColor = Get(X, Y);
					if (bColor == Get(X + 1, Y) && bColor == Get(X, Y + 1) && bColor == Get(X + 1, Y + 1))
					{
						Result += PenaltyBlock;
					}
				}
			}
			int32 Dark = 0;
			for (const uint8 Module : Modules)
			{
				Dark += Module;
			}
			const int32 Total = Size * Size;
			// Each full 5 % that the dark share is away from 50 % costs PenaltyBalance.
			const int32 Steps = (FMath::Abs(Dark * 20 - Total * 10) + Total - 1) / Total - 1;
			Result += Steps * PenaltyBalance;
			return Result;
		}

		TArray<uint8> TakeModules() { return MoveTemp(Modules); }

		const int32 Version;
		const int32 Size;

	private:
		void SetFunction(int32 X, int32 Y, bool bDark)
		{
			Modules[Y * Size + X] = bDark ? 1 : 0;
			IsFunction[Y * Size + X] = 1;
		}

		void DrawFinder(int32 CenterX, int32 CenterY)
		{
			for (int32 DY = -4; DY <= 4; ++DY)
			{
				for (int32 DX = -4; DX <= 4; ++DX)
				{
					const int32 Distance = FMath::Max(FMath::Abs(DX), FMath::Abs(DY));
					const int32 X = CenterX + DX;
					const int32 Y = CenterY + DY;
					if (X >= 0 && X < Size && Y >= 0 && Y < Size)
					{
						SetFunction(X, Y, Distance != 2 && Distance != 4);
					}
				}
			}
		}

		void DrawAlignment(int32 CenterX, int32 CenterY)
		{
			for (int32 DY = -2; DY <= 2; ++DY)
			{
				for (int32 DX = -2; DX <= 2; ++DX)
				{
					SetFunction(CenterX + DX, CenterY + DY, FMath::Max(FMath::Abs(DX), FMath::Abs(DY)) != 1);
				}
			}
		}

		TArray<int32> AlignmentPositions() const
		{
			TArray<int32> Result;
			if (Version == 1)
			{
				return Result;
			}
			const int32 Count = Version / 7 + 2;
			const int32 Step = (Version * 8 + Count * 3 + 5) / (Count * 4 - 4) * 2;
			Result.Add(6);
			for (int32 Position = Size - 7; Result.Num() < Count; Position -= Step)
			{
				Result.Insert(Position, 1);
			}
			return Result;
		}

		void DrawVersion()
		{
			if (Version < 7)
			{
				return;
			}
			int32 Remainder = Version;
			for (int32 Index = 0; Index < 12; ++Index)
			{
				Remainder = (Remainder << 1) ^ ((Remainder >> 11) * 0x1F25);
			}
			const int32 Bits = (Version << 12) | Remainder;
			for (int32 Index = 0; Index < 18; ++Index)
			{
				const bool bDark = ((Bits >> Index) & 1) != 0;
				const int32 A = Size - 11 + Index % 3;
				const int32 B = Index / 3;
				SetFunction(A, B, bDark);
				SetFunction(B, A, bDark);
			}
		}

		/** Runs of five or more (PenaltyRun and one more per extra module) and finder-like 1:1:3:1:1 patterns with a light side. */
		int32 LinePenalty(int32 Line, bool bRow) const
		{
			int32 Result = 0;
			bool bRunColor = false;
			int32 RunLength = 0;
			int32 History[7] = { 0, 0, 0, 0, 0, 0, 0 };
			for (int32 Along = 0; Along < Size; ++Along)
			{
				const bool bColor = bRow ? Get(Along, Line) : Get(Line, Along);
				if (bColor == bRunColor)
				{
					++RunLength;
					if (RunLength == 5)
					{
						Result += PenaltyRun;
					}
					else if (RunLength > 5)
					{
						++Result;
					}
				}
				else
				{
					AddHistory(RunLength, History);
					if (!bRunColor)
					{
						Result += CountFinderLike(History) * PenaltyFinderLike;
					}
					bRunColor = bColor;
					RunLength = 1;
				}
			}
			if (bRunColor)
			{
				AddHistory(RunLength, History);
				RunLength = 0;
			}
			// The quiet zone past the edge counts as a light run.
			RunLength += Size;
			AddHistory(RunLength, History);
			Result += CountFinderLike(History) * PenaltyFinderLike;
			return Result;
		}

		void AddHistory(int32 RunLength, int32 (&History)[7]) const
		{
			if (History[0] == 0)
			{
				// The light quiet zone before the first run.
				RunLength += Size;
			}
			for (int32 Index = 6; Index > 0; --Index)
			{
				History[Index] = History[Index - 1];
			}
			History[0] = RunLength;
		}

		static int32 CountFinderLike(const int32 (&History)[7])
		{
			const int32 N = History[1];
			const bool bCore = N > 0 && History[2] == N && History[3] == N * 3 && History[4] == N && History[5] == N;
			return (bCore && History[0] >= N * 4 && History[6] >= N ? 1 : 0) + (bCore && History[6] >= N * 4 && History[0] >= N ? 1 : 0);
		}

		TArray<uint8> Modules;
		TArray<uint8> IsFunction;
	};
}

int32 StageQrCode::GetByteCapacity(int32 Version)
{
	if (Version < MinVersion || Version > MaxVersion)
	{
		return 0;
	}
	using namespace StageQrCodeImpl;
	return (DataCodewords(Version) * 8 - 4 - CountBits(Version)) / 8;
}

StageQrCode::FCode StageQrCode::EncodeBytes(TConstArrayView<uint8> Bytes, int32 ForceMask)
{
	using namespace StageQrCodeImpl;
	FCode Code;
	if (Bytes.IsEmpty() || ForceMask > 7)
	{
		return Code;
	}
	int32 Version = MinVersion;
	while (Version <= MaxVersion && Bytes.Num() > GetByteCapacity(Version))
	{
		++Version;
	}
	if (Version > MaxVersion)
	{
		return Code;
	}

	const int32 CapacityBits = DataCodewords(Version) * 8;
	FQrBits Writer;
	Writer.Append(0x4, 4);
	Writer.Append(static_cast<uint32>(Bytes.Num()), CountBits(Version));
	for (const uint8 Byte : Bytes)
	{
		Writer.Append(Byte, 8);
	}
	Writer.Append(0, FMath::Min(4, CapacityBits - Writer.Bits.Num()));
	Writer.Append(0, (8 - Writer.Bits.Num() % 8) % 8);
	for (uint32 Pad = 0xEC; Writer.Bits.Num() < CapacityBits; Pad ^= 0xEC ^ 0x11)
	{
		Writer.Append(Pad, 8);
	}

	TArray<uint8> Data;
	Data.SetNumZeroed(Writer.Bits.Num() / 8);
	for (int32 Index = 0; Index < Writer.Bits.Num(); ++Index)
	{
		Data[Index >> 3] |= Writer.Bits[Index] << (7 - (Index & 7));
	}

	FBuilder Builder(Version);
	Builder.DrawFunctionPatterns();
	Builder.DrawCodewords(AddEccAndInterleave(Data, Version));

	int32 Mask = ForceMask;
	if (Mask < 0)
	{
		int32 BestPenalty = TNumericLimits<int32>::Max();
		for (int32 Candidate = 0; Candidate < 8; ++Candidate)
		{
			Builder.ApplyMask(Candidate);
			Builder.DrawFormatBits(Candidate);
			const int32 Penalty = Builder.Penalty();
			if (Penalty < BestPenalty)
			{
				BestPenalty = Penalty;
				Mask = Candidate;
			}
			Builder.ApplyMask(Candidate);
		}
	}
	Builder.ApplyMask(Mask);
	Builder.DrawFormatBits(Mask);

	Code.Version = Version;
	Code.Size = Builder.Size;
	Code.Mask = Mask;
	Code.Modules = Builder.TakeModules();
	return Code;
}

StageQrCode::FCode StageQrCode::Encode(const FString& Text)
{
	const FTCHARToUTF8 Utf8(*Text, Text.Len());
	return EncodeBytes(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
}

TArray<FColor> StageQrCode::RenderPixels(const FCode& Code, int32 ModulePixels, int32& OutSide)
{
	OutSide = 0;
	TArray<FColor> Pixels;
	if (!Code.IsValid() || ModulePixels < 1 || ModulePixels > 32)
	{
		return Pixels;
	}
	const int32 Modules = Code.Size + 2 * QuietZone;
	OutSide = Modules * ModulePixels;
	Pixels.Init(FColor::White, OutSide * OutSide);
	for (int32 Y = 0; Y < OutSide; ++Y)
	{
		for (int32 X = 0; X < OutSide; ++X)
		{
			if (Code.IsDark(X / ModulePixels - QuietZone, Y / ModulePixels - QuietZone))
			{
				Pixels[Y * OutSide + X] = FColor::Black;
			}
		}
	}
	return Pixels;
}
