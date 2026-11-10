// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * What an MVR file (Docs/ADR/0034-mvr-scene-exchange-and-product-tiers.md §4–§5) describes, as plain values: the parser's output
 * (MV1). No UObjects and no engine state, so it is built on a worker thread and handed to the importer (MV2) as a whole.
 *
 * Positions stay in MVR space (right-handed, Z up, millimetres) in the matrices; StageMvrUnits::ToUnreal converts them.
 */

/** The object kinds of an MVR ChildList. */
enum class EStageMvrObjectKind : uint8
{
	SceneObject,
	GroupObject,
	FocusPoint,
	Fixture,
	Support,
	Truss,
	VideoScreen,
	Projector,
	Count,
};

/** One DMX address of a fixture: Break is the GDTF DMX break (only 0 is patched in v1), Universe from 1, Address 1–512. */
struct FStageMvrAddress
{
	int32 Break = 0;
	int32 Universe = 1;
	int32 Address = 1;

	bool operator==(const FStageMvrAddress& Other) const = default;
};

/** One object of the scene tree, with its parent and its world matrix resolved. */
struct FStageMvrObject
{
	EStageMvrObjectKind Kind = EStageMvrObjectKind::SceneObject;
	FGuid Id;
	FString Name;

	/** Index into FStageMvrScene::Layers. */
	int32 Layer = INDEX_NONE;
	/** Index into FStageMvrScene::Objects of the object whose ChildList holds this one; INDEX_NONE directly in a layer. */
	int32 Parent = INDEX_NONE;

	/** The Matrix element, in the parent's space (identity when missing). Row vectors u, v, w and the offset o in mm. */
	FMatrix Local = FMatrix::Identity;
	/** Local, then every parent's and the layer's matrix: the object in the file's world, in mm. */
	FMatrix World = FMatrix::Identity;

	/** Geometry3D file names (".3ds" added when a name has no extension) and Symbol references to AUXData Symdefs. */
	TArray<FString> GeometryFiles;
	TArray<FGuid> Symbols;

	// Fixture only.
	/** GDTFSpec, as written (".gdtf" added when it has no extension); empty for a fixture without one. */
	FString GdtfSpec;
	FString GdtfMode;
	TArray<FStageMvrAddress> Addresses;
	/** FixtureID (a string in MVR), FixtureIDNumeric, UnitNumber and CustomId; 0 when not given. */
	FString FixtureId;
	int32 FixtureIdNumeric = 0;
	int32 UnitNumber = 0;
	int32 CustomId = 0;
	/** The FocusPoint this fixture aims at (Focus), and the fixture this one is a multipatch of; invalid when none. */
	FGuid Focus;
	FGuid MultipatchOf;

	/** Line of the element in GeneralSceneDescription.xml, for the report. */
	int32 Line = 0;
};

struct FStageMvrLayer
{
	FGuid Id;
	FString Name;
	FMatrix Matrix = FMatrix::Identity;
};

/** An AUXData Symdef: geometry instanced by Symbol references. */
struct FStageMvrSymdef
{
	FGuid Id;
	FString Name;
	TArray<FString> GeometryFiles;
};

struct FStageMvrScene
{
	int32 VerMajor = 0;
	int32 VerMinor = 0;
	FString Provider;
	FString ProviderVersion;

	TArray<FStageMvrLayer> Layers;
	/** Parents before their children, in document order. */
	TArray<FStageMvrObject> Objects;
	TArray<FStageMvrSymdef> Symdefs;

	/** Every file in the archive, lower case; empty when only the XML was read. */
	TSet<FString> ArchiveFiles;

	/** Objects of Kind. */
	int32 Count(EStageMvrObjectKind Kind) const;

	/** The object with Id, or null. */
	const FStageMvrObject* FindObject(const FGuid& Id) const;

	/** Distinct GDTFSpec file names of the fixtures, in first-use order. */
	TArray<FString> GetGdtfSpecs() const;
};

/** The parser's result. A file with any error is refused as a whole; warnings are for the import report. */
struct FStageMvrParseResult
{
	FStageMvrScene Scene;
	TArray<FString> Errors;
	TArray<FString> Warnings;

	bool IsValid() const { return Errors.IsEmpty(); }
};
