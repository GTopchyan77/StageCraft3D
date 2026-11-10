// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Fixtures/StageXmlReader.h"
#include "Fixtures/StageZipReader.h"
#include "Mvr/StageMvrTypes.h"

/**
 * Reads an .mvr file (My Virtual Rig, DIN SPEC 15801; Docs/ADR/0034 §4–§5, phase MV1) into an FStageMvrScene.
 *
 * The file is untrusted. The archive goes through StageZip (stored and deflate, CRC-32, no ZIP64 or encryption, safe names) and
 * GeneralSceneDescription.xml through StageXml with element text kept, both with caps. MVR adds: every file at the archive root,
 * major version 1. Structure and syntax errors (a bad matrix, address or UUID, a duplicate UUID, a cap) refuse the whole file and
 * name the element's line; semantic gaps (a fixture without a mode, a missing GDTF or geometry file, a DMX break other than 0, an
 * unknown focus point) are warnings for the import report. Over a cap the file is refused, never truncated.
 *
 * Tolerated real-world variations (ADR 0034 §4): UUIDs with or without dashes, a GDTFSpec or Geometry3D name without extension.
 *
 * Pure: no files, no UObjects. Safe on any thread.
 */
namespace StageMvr
{
	struct FLimits
	{
		StageZip::FLimits Zip;
		StageXml::FLimits Xml;
		int64 MaxSceneBytes = 32ll * 1024 * 1024;
		int32 MaxObjects = 20000;
		int32 MaxFixtures = 4096;
		/** Nesting of objects in ChildLists (GroupObject in GroupObject...). */
		int32 MaxObjectDepth = 32;
		int32 MaxGdtfFiles = 256;
		int32 MaxLayers = 1024;
		int32 MaxAddressesPerFixture = 64;

		/** ADR 0034 §5's caps: 64 MB archive, 512 entries; the XML reader keeps text and allows the nesting and size those imply. */
		FLimits();
	};

	/** Reads a whole .mvr archive. */
	FStageMvrParseResult Parse(TConstArrayView<uint8> FileBytes, const FLimits& Limits = FLimits());

	/** Reads GeneralSceneDescription.xml text only (no archive): ArchiveFiles stays empty and file references are not checked. */
	FStageMvrParseResult ParseSceneDescription(FStringView Xml, const FLimits& Limits = FLimits());

	/** "{u1,u2,u3}{v1,v2,v3}{w1,w2,w3}{o1,o2,o3}" (white space allowed) as a row-vector matrix in mm. False for anything else. */
	bool ParseMatrix(FStringView Text, FMatrix& OutMatrix);

	/**
	 * "Universe.Address" (universe from 1, address 1–512) or an absolute address from 1 (513 is universe 2, address 1), with its
	 * break. False for anything else.
	 */
	bool ParseAddress(FStringView Text, int32 Break, FStageMvrAddress& OutAddress);

	/** An RFC 4122 UUID with or without dashes (braces allowed). False for anything else, and for the nil UUID. */
	bool ParseUuid(FStringView Text, FGuid& OutId);

	/** One line per object kind with its count, for logs and the probe. */
	FString Describe(const FStageMvrScene& Scene);
}

/**
 * MVR space (right-handed, Z up, millimetres, row vectors) to Unreal (left-handed, Z up, centimetres) and back: Y is mirrored
 * (every element with exactly one Y index changes sign) and the offset is divided by 10. Rotation and scale are kept.
 */
namespace StageMvrUnits
{
	FTransform ToUnreal(const FMatrix& MvrMatrix);
	FMatrix FromUnreal(const FTransform& Transform);
}
