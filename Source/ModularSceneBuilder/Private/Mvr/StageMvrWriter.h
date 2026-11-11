// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Writes an .mvr file (My Virtual Rig, DIN SPEC 15801, version 1.6; Docs/ADR/0034 §7, phase MV4) from plain values: the inverse
 * of StageMvr::Parse and StageMvrImport::Build, so a file StageCraft writes reads back to the same fixtures, patch, IDs and
 * transforms (StageCraft.Mvr.Export tests).
 *
 * What is written:
 *  - GeneralSceneDescription.xml: one Layer per FLayer, each with a ChildList of its fixtures (Matrix, GDTFSpec, GDTFMode,
 *    Addresses, FixtureID, FixtureIDNumeric, UnitNumber), trusses and scene objects (Matrix, Geometries / Geometry3D).
 *  - Every GDTF file a fixture names, stored unchanged (the spec asks for the original file). A fixture whose file is not among
 *    the bytes handed in is written without GDTFSpec and GDTFMode (the spec allows that), and named in the notes.
 *  - One .glb box (StageMvrGeometry::WriteBoxGlb) per distinct box size and position, shared by the objects that use it.
 *  Positions are converted with StageMvrUnits::FromUnreal (mm, right-handed). A fixture is turned back to MVR's hanging rest pose
 *  (the inverse of StageMvrImport::FixtureTransform); scale is not written for fixtures, and a box object's scale is applied to
 *  its box instead of its matrix.
 *
 * Text written into the XML is escaped, and characters XML 1.0 does not allow are dropped. Pure: no files, no UObjects; any thread.
 */
namespace StageMvrExport
{
	/** MVR version written. */
	constexpr int32 VerMajor = 1;
	constexpr int32 VerMinor = 6;

	/** The XML file every .mvr holds. */
	extern const TCHAR* const SceneFileName;

	struct FLayer
	{
		FGuid Id;
		FString Name;
	};

	struct FFixture
	{
		FGuid Id;
		FString Name;
		/** Index into FScene::Layers. */
		int32 Layer = 0;
		/** The placed fixture's actor transform (Unreal space, cm). */
		FTransform Transform = FTransform::Identity;
		/** The GDTF file's name inside the .mvr, and the DMX mode; both empty for a fixture without a GDTF type. */
		FString GdtfSpec;
		FString GdtfMode;
		/** 0 when the fixture has none. */
		int32 FixtureId = 0;
		/** Universe from 1 and address 1–512; 0 when unpatched (no Addresses written). */
		int32 Universe = 0;
		int32 Address = 0;
	};

	/** What a box object is written as. */
	enum class EBoxKind : uint8
	{
		Truss,
		SceneObject,
	};

	/** An item written as a box: a truss leg or any other placed item. */
	struct FBoxObject
	{
		EBoxKind Kind = EBoxKind::SceneObject;
		FGuid Id;
		FString Name;
		int32 Layer = 0;
		/** Where the object's origin is (Unreal space, cm). Its scale applies to Bounds. */
		FTransform Transform = FTransform::Identity;
		/** The object's extent in its own space (cm), before Transform's scale. Must be valid. */
		FBox LocalBounds = FBox(ForceInit);
	};

	struct FScene
	{
		FString Provider = TEXT("StageCraft");
		FString ProviderVersion;
		/** Every object's Layer indexes this; at least one layer when anything is written. */
		TArray<FLayer> Layers;
		TArray<FFixture> Fixtures;
		TArray<FBoxObject> Objects;
	};

	struct FWriteResult
	{
		TArray<uint8> Bytes;
		/** Why nothing was written (an invalid scene, an archive limit); empty on success. */
		FString Error;
		/** User-facing notes on what was written differently ("Spot 3: its GDTF file is missing; written without a fixture type."). */
		TArray<FString> Notes;
		int32 GeometryFiles = 0;
		int32 GdtfFiles = 0;

		bool IsValid() const { return Error.IsEmpty(); }
	};

	/**
	 * The whole .mvr archive. GdtfFiles maps a fixture's GdtfSpec (compared ignoring case) to that file's bytes. Refused, with Error,
	 * when the scene has nothing to write, an object names a layer that does not exist, an id is invalid or used twice, a box has no
	 * bounds, or the archive would pass the ZIP limits.
	 */
	FWriteResult Write(const FScene& Scene, const TMap<FString, TArray<uint8>>& GdtfFiles);

	/** The MVR world matrix (mm) of a fixture whose actor stands at ActorTransform: the inverse of StageMvrImport::FixtureTransform. */
	FMatrix FixtureMatrix(const FTransform& ActorTransform);

	/** "{u}{v}{w}{o}" as MVR writes a matrix: row vectors and the offset in mm, numbers with at most six decimals. */
	FString FormatMatrix(const FMatrix& Matrix);

	/** Text made safe for an XML attribute or element: & < > " ' escaped, characters XML 1.0 does not allow dropped. */
	FString EscapeXml(FStringView Text);

	/** A file name for a GDTF type inside the .mvr: Preferred reduced to safe characters with ".gdtf", else Fallback.gdtf. */
	FString MakeGdtfFileName(const FString& Preferred, const FString& Fallback);
}
