// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Fixtures/StageGdtfTypes.h"
#include "Fixtures/StageXmlReader.h"
#include "Fixtures/StageZipReader.h"

/**
 * Reads a .gdtf file (Docs/ADR/0033-gdtf-fixture-import-and-device-library.md §8) into an FStageGdtfFixtureType.
 *
 * The file is untrusted. The archive goes through StageZip and description.xml through StageXml, both with caps. Every number is
 * parsed strictly and range-checked. A value that breaks a rule is an error naming its element and line: nothing is clamped
 * silently, and a file with any error is refused as a whole. Parsing checks structure only. What StageCraft can drive is decided by
 * StageGdtfMapping.
 *
 * Pure: no files, no UObjects. Safe on any thread (the import runs it on a worker).
 */
namespace StageGdtf
{
	struct FLimits
	{
		StageZip::FLimits Zip;
		StageXml::FLimits Xml;
		int64 MaxDescriptionBytes = 8ll * 1024 * 1024;
		int32 MaxModes = 64;
		int32 MaxChannelsPerMode = 512;
		int32 MaxLogicalChannelsPerChannel = 16;
		int32 MaxFunctionsPerChannel = 64;
		int32 MaxSetsPerFunction = 256;
		int32 MaxWheels = 16;
		int32 MaxSlotsPerWheel = 64;
		int32 MaxEmitters = 64;
		int32 MaxModels = 256;
		int32 MaxGeometries = 4096;
		int32 MaxGeometryDepth = 32;
		int32 MaxStringLength = 256;
		/** Bytes per DMX value: GDTF allows up to 32-bit channels. */
		int32 MaxValueBytes = 4;
	};

	/** Reads a whole .gdtf archive. */
	FStageGdtfParseResult Parse(TConstArrayView<uint8> FileBytes, const FLimits& Limits = FLimits());

	/** Reads description.xml text only (no archive). ArchiveFiles stays empty, so wheel images are not checked. */
	FStageGdtfParseResult ParseDescription(FStringView Xml, const FLimits& Limits = FLimits());

	/** Parses "128/1", "255/1s" or "0/2". False for anything else, or a value that does not fit its bytes. */
	bool ParseDMXValue(FStringView Text, int32 MaxBytes, FStageGdtfDMXValue& OutValue);

	/**
	 * Value in a channel of ChannelBytes bytes. Fewer bytes are byte-mirrored (255/1 on 16 bits is 65535) or, with "s",
	 * byte-shifted (255/1s on 16 bits is 65280). More bytes keep their most significant bytes.
	 */
	int64 ToChannelValue(const FStageGdtfDMXValue& Value, int32 ChannelBytes);
}
