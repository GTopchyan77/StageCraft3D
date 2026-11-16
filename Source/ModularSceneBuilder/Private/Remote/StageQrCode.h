// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * A QR code encoder (ISO/IEC 18004) for the Web Remote's pairing code (Docs/ADR/0036 §11). The engine has none, and the only text it
 * ever encodes is a short address, so this covers exactly that: byte mode, error correction level M (15 % of the symbol can be
 * damaged or glare-washed and it still reads), versions 1-10 (up to 213 bytes). The mask is chosen by the standard's penalty rules.
 * Pure and deterministic: no UObjects, any thread.
 */
namespace StageQrCode
{
	inline constexpr int32 MinVersion = 1;
	inline constexpr int32 MaxVersion = 10;
	/** The light margin a reader needs around the symbol, in modules (the standard's quiet zone). */
	inline constexpr int32 QuietZone = 4;

	struct FCode
	{
		/** 1-10; 0 when encoding failed. */
		int32 Version = 0;
		/** Modules per side (17 + 4 * Version); 0 when encoding failed. */
		int32 Size = 0;
		/** 0-7, the data mask that was applied. */
		int32 Mask = 0;
		/** Size * Size modules, row by row; 1 is dark. */
		TArray<uint8> Modules;

		bool IsValid() const { return Size > 0 && Modules.Num() == Size * Size; }
		/** False outside the symbol, so callers can draw the quiet zone without bounds checks. */
		bool IsDark(int32 X, int32 Y) const { return IsValid() && X >= 0 && Y >= 0 && X < Size && Y < Size && Modules[Y * Size + X] != 0; }
	};

	/** Encodes Text as UTF-8 in the smallest version that holds it. Invalid (Size 0) when Text is empty or longer than version 10 holds. */
	FCode Encode(const FString& Text);

	/** Encodes raw bytes; ForceMask 0-7 skips the mask choice (tests compare against reference symbols). */
	FCode EncodeBytes(TConstArrayView<uint8> Bytes, int32 ForceMask = -1);

	/**
	 * The symbol as square pixels for a texture: ModulePixels per module (whole pixels, so it stays sharp at any scale step) and the
	 * quiet zone around it, dark modules black on white. Empty for an invalid code; OutSide is the width and height.
	 */
	TArray<FColor> RenderPixels(const FCode& Code, int32 ModulePixels, int32& OutSide);

	/** The most bytes a version holds at level M in byte mode. */
	int32 GetByteCapacity(int32 Version);
}
