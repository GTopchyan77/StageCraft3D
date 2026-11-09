// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Self-authored GDTF test files (Docs/ADR/0033 §2 G5: committed tests never use manufacturer files) and a small ZIP writer to
 * pack them. Written from the public GDTF specification; the fixtures and their numbers are invented.
 */
namespace StageGdtfTestFiles
{
	/** FixtureTypeID of SpotXml. */
	extern const TCHAR* const SpotId;

	/**
	 * A moving-head spot with three modes:
	 *  - "Standard" (15 ch): 16-bit pan and tilt, dimmer, shutter (closed / open / strobe 1–25 Hz / random), colour wheel, gobo
	 *    wheel, gobo index and rotation sharing one channel, prism, 16-bit zoom 8–40°, focus and control (no StageCraft control),
	 *    and blade 1A.
	 *  - "Basic" (3 ch): 8-bit pan, tilt and dimmer.
	 *  - "Broken": a channel on DMX break 2, which StageCraft cannot patch.
	 */
	FString SpotXml();

	/** A pixel bar: a master dimmer on channel 1, then Pixels repeated RGB cells (GeometryReference with breaks at 2, 5, 8...). */
	FString BarXml(int32 Pixels = 8);

	/** UTF-8 bytes of Text. */
	TArray<uint8> Utf8(const FString& Text);

	/** A ZIP archive of Files (name, bytes), stored or deflated. */
	TArray<uint8> MakeZip(const TArray<TPair<FString, TArray<uint8>>>& Files, bool bDeflate);

	/** A .gdtf archive holding Xml as description.xml and, when bWithWheelImages, small PNG-signature files for the spot's gobos. */
	TArray<uint8> MakeGdtf(const FString& Xml, bool bDeflate = true, bool bWithWheelImages = true);
}

#endif
