// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Fixtures/StageXmlReader.h"
#include "Mvr/StageMvrTypes.h"

/**
 * The MVR XML of a fixture or truss that StageCraft does not use, kept on the placed item and written back on export, so a plot that
 * goes through StageCraft keeps what a console or CAD program put there (Docs/ADR/0034 §18, MV5; the spec's "preserve what is not
 * supported").
 *
 * What is kept, per object: every attribute except uuid, name and multipatch, and every child element except
 *  - the ones the export writes itself (Matrix, Geometries, ChildList; for fixtures GDTFSpec, GDTFMode, Addresses, FixtureID and
 *    FixtureIDNumeric), which would otherwise appear twice, and
 *  - the ones that point at something an export does not carry (DroppedReferences: a focus point, an AUXData class, position or
 *    mapping definition, a gobo image, a connection to another object; for trusses their GDTF file), which would dangle.
 * Everything else (CastShadow, Color, CustomId, UnitNumber, DMX inversion, CustomCommands, Overwrites, Protocols, Alignments,
 * vendor elements) is kept whole, nested children and text included, except a UnitNumber of 0: the export writes that by default.
 * A connection is left out because the object it names may not be exported.
 *
 * The form is canonical XML, "<Keep a=\"...\"><Child>...</Child></Keep>": names checked, text and values escaped, comments and
 * text between child elements (mixed content) not kept. Reading it back (ReadForWriting) treats it as untrusted, because scene
 * files can be edited: it is parsed again with caps and re-serialized, so nothing malformed ever reaches an exported file.
 *
 * Pure: no files, no UObjects. Safe on any thread.
 */
namespace StageMvrPreserved
{
	/** Longest kept fragment, the same as FStageMvrItemOrigin::MaxPreservedLength; a longer one is dropped and reported. */
	constexpr int32 MaxChars = 16 * 1024;

	/** The wrapper element of a kept fragment. */
	extern const TCHAR* const RootName;

	/** Child elements a fixture or truss may carry that point at data an export does not write; they are not kept. */
	TConstArrayView<const TCHAR*> DroppedReferences();

	struct FCaptured
	{
		/** The canonical fragment; empty when nothing is kept (or it was too large). */
		FString Xml;
		int32 Attributes = 0;
		int32 Elements = 0;
		/** Names of child elements left out because they point at data an export does not carry, in document order. */
		TArray<FString> Dropped;
		/** The fragment passed MaxChars and was dropped. */
		bool bTooLarge = false;
	};

	/** What Object (a Fixture or Truss element of Document, read with element text kept) carries that StageCraft does not use. */
	FCaptured Capture(const StageXml::FDocument& Document, int32 ObjectNode, EStageMvrObjectKind Kind);

	/** A kept fragment ready to write inside its object. */
	struct FForWriting
	{
		/** " name=\"value\"" pairs, escaped, to append to the object's start tag. */
		FString Attributes;
		/** One canonical element per entry, escaped, to write after the object's own elements. */
		TArray<FString> Children;
		/** The child elements' names, so the writer leaves out a default it would write (UnitNumber). */
		TSet<FString> ChildNames;
	};

	/**
	 * Reads a kept fragment back (untrusted). False when it is empty, too long, not well-formed, not a <Keep> element, or names an
	 * attribute or element the writer owns; the caller writes the object without it.
	 */
	bool ReadForWriting(FStringView Xml, EStageMvrObjectKind Kind, FForWriting& Out);
}
