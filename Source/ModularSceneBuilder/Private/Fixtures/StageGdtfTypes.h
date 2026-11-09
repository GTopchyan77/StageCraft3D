// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * A GDTF fixture type as read from its description.xml (Docs/ADR/0033-gdtf-fixture-import-and-device-library.md). Plain values in
 * GDTF's own terms and units (metres, degrees, CIE xyY, DMX values as written). StageGdtfMapping turns them into StageCraft terms.
 * Built by StageGdtf::Parse; safe to copy between threads.
 */

/** A colour as GDTF writes it: CIE 1931 x, y and luminance Y. */
struct FStageGdtfColorCIE
{
	double X = 0.3127;
	double Y = 0.3290;
	double Luminance = 100.0;
	bool bSet = false;
};

/** A DMX value as written ("128/1", "255/1s"): the value, its byte count and whether it is byte-shifted rather than mirrored. */
struct FStageGdtfDMXValue
{
	int64 Value = 0;
	int32 Bytes = 1;
	bool bShift = false;
	bool bSet = false;
};

struct FStageGdtfWheelSlot
{
	FString Name;
	FStageGdtfColorCIE Color;
	/** A PNG under ./wheels, without extension; empty when none. */
	FString MediaFileName;
	/** PrismFacet children: a prism slot when above 0. */
	int32 PrismFacets = 0;
};

struct FStageGdtfWheel
{
	FString Name;
	TArray<FStageGdtfWheelSlot> Slots;
};

struct FStageGdtfEmitter
{
	FString Name;
	FStageGdtfColorCIE Color;
	float DominantWaveLengthNm = 0.f;
};

enum class EStageGdtfPrimitive : uint8
{
	Undefined, Cube, Cylinder, Sphere, Base, Yoke, Head, Scanner, Conventional, Pigtail, Base1_1, Scanner1_1, Conventional1_1,
};

struct FStageGdtfModel
{
	FString Name;
	/** Length, Width, Height in metres. */
	FVector SizeM = FVector::ZeroVector;
	EStageGdtfPrimitive Primitive = EStageGdtfPrimitive::Undefined;
	FString File;
};

enum class EStageGdtfGeometryType : uint8
{
	Geometry, Axis, FilterBeam, FilterColor, FilterGobo, FilterShaper, Beam, MediaServerLayer, MediaServerCamera, MediaServerMaster,
	Display, GeometryReference, Laser, WiringObject, Inventory, Structure, Support, Magnet,
};

enum class EStageGdtfBeamType : uint8
{
	Wash, Spot, None, Rectangle, PC, Fresnel, Glow,
};

/** A Beam geometry's light. Defaults are the spec's. */
struct FStageGdtfBeam
{
	FString LampType = TEXT("Discharge");
	float PowerConsumptionW = 1000.f;
	float LuminousFlux = 10000.f;
	float ColorTemperatureK = 6000.f;
	float BeamAngleDeg = 25.f;
	float FieldAngleDeg = 25.f;
	float BeamRadiusM = 0.05f;
	EStageGdtfBeamType BeamType = EStageGdtfBeamType::Wash;
};

/** A GeometryReference's Break: where that instance's channels start. */
struct FStageGdtfBreak
{
	int32 DMXOffset = 1;
	int32 DMXBreak = 1;
};

/** One node of the geometry tree. Geometries are stored flat; Parent and Children index into FStageGdtfFixtureType::Geometries. */
struct FStageGdtfGeometry
{
	FString Name;
	EStageGdtfGeometryType Type = EStageGdtfGeometryType::Geometry;
	FString Model;
	/** The Position matrix, row by row, as written: translation in the fourth column, in metres. Identity by default. */
	double Position[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	int32 Parent = INDEX_NONE;
	TArray<int32> Children;
	/** Type == Beam only. */
	FStageGdtfBeam Beam;
	/** Type == GeometryReference only: the top-level geometry it instantiates, and its breaks. */
	FString ReferencedGeometry;
	TArray<FStageGdtfBreak> Breaks;
};

struct FStageGdtfChannelSet
{
	FString Name;
	FStageGdtfDMXValue DMXFrom;
	TOptional<float> PhysicalFrom;
	TOptional<float> PhysicalTo;
	/** 1-based; 0 when the set names no wheel slot. */
	int32 WheelSlotIndex = 0;
};

struct FStageGdtfChannelFunction
{
	FString Name;
	/** The attribute this range controls ("NoFeature" when none). */
	FString Attribute;
	FString OriginalAttribute;
	FStageGdtfDMXValue DMXFrom;
	FStageGdtfDMXValue Default;
	float PhysicalFrom = 0.f;
	float PhysicalTo = 1.f;
	FString Wheel;
	FString Emitter;
	/** Set when another channel gates this function (mode dependencies). Read for the report only. */
	FString ModeMaster;
	TArray<FStageGdtfChannelSet> Sets;
};

struct FStageGdtfLogicalChannel
{
	FString Attribute;
	TArray<FStageGdtfChannelFunction> Functions;
};

struct FStageGdtfDMXChannel
{
	int32 DMXBreak = 1;
	/** DMXBreak="Overwrite": a template channel, instantiated for every GeometryReference to its geometry. */
	bool bBreakOverwrite = false;
	/** 1-based, most significant first ("1,2" is a 16-bit channel). Empty: a virtual channel with no DMX slot. */
	TArray<int32> Offsets;
	FString Geometry;
	/** Node path of the initial channel function, as written. */
	FString InitialFunction;
	TArray<FStageGdtfLogicalChannel> LogicalChannels;
	/** Line in description.xml, for messages. */
	int32 Line = 0;
};

struct FStageGdtfMode
{
	FString Name;
	FString Description;
	/** The top-level geometry this mode drives. */
	FString Geometry;
	TArray<FStageGdtfDMXChannel> Channels;
};

struct FStageGdtfProperties
{
	float WeightKg = 0.f;
	/** Sum of every PowerConsumption connector, in watts. */
	float PowerW = 0.f;
	float LegHeightM = 0.f;
};

struct FStageGdtfFixtureType
{
	FString DataVersion;
	FString Name;
	FString ShortName;
	FString LongName;
	FString Manufacturer;
	FString Description;
	FGuid FixtureTypeId;
	/** Attribute names defined in AttributeDefinitions. */
	TSet<FString> DefinedAttributes;
	TArray<FStageGdtfWheel> Wheels;
	TArray<FStageGdtfEmitter> Emitters;
	TArray<FStageGdtfModel> Models;
	TArray<FStageGdtfGeometry> Geometries;
	TArray<FStageGdtfMode> Modes;
	FStageGdtfProperties Properties;
	/** Every file name in the archive (for checking that wheel images exist). */
	TSet<FString> ArchiveFiles;

	const FStageGdtfWheel* FindWheel(FStringView WheelName) const;
	const FStageGdtfEmitter* FindEmitter(FStringView EmitterName) const;
	const FStageGdtfModel* FindModel(FStringView ModelName) const;
	/** Index of the geometry named GeometryName, or INDEX_NONE. Names are unique within a fixture type. */
	int32 FindGeometry(FStringView GeometryName) const;
	/** True when Geometry is Ancestor or lies under it. */
	bool IsInSubtree(int32 Geometry, int32 Ancestor) const;
};

/** What Parse returns: a type when Errors is empty. Warnings never refuse the file. */
struct FStageGdtfParseResult
{
	FStageGdtfFixtureType Type;
	TArray<FString> Errors;
	TArray<FString> Warnings;

	bool IsOk() const { return Errors.IsEmpty(); }
};
