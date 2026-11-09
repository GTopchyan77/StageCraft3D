// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/LightingFixtureData.h"
#include "Fixtures/StageGdtfTypes.h"
#include "GameplayTagContainer.h"
#include "Show/StageFixtureCells.h"

/**
 * GDTF to StageCraft (Docs/ADR/0033 §4–§5): which StageCraft attribute a GDTF attribute drives, each mode's DMX layout with its
 * value ranges, the fixture's capabilities, and the import report. Pure: no UObjects are created (GD2's builder does that from
 * these results). Safe on any thread.
 */

/** How a GDTF physical value becomes a StageCraft attribute value. */
enum class EStageGdtfValueKind : uint8
{
	/** 0..1, scaled from the channel's physical span. */
	Normalized,
	/** Degrees, as written (pan, tilt, zoom, index rotations). */
	Degrees,
	/** A wheel or prism slot: 0 is Open (or Out), 1.. the other slots. */
	Slot,
	/** Revolutions per minute, from GDTF's degrees per second. */
	Rpm,
	Kelvin,
	/** StageCraft's Shutter: 0 open, above 0 a strobe rate. */
	Shutter,
};

/** What a GDTF Shutter-family function does on StageCraft's Shutter. */
enum class EStageGdtfShutterRole : uint8
{
	None,
	/** Shutter1: its sets say open or closed. */
	OpenClosed,
	/** Shutter1Strobe, StrobeFrequency: a strobe rate in Hz. */
	Strobe,
};

/** Where a GDTF attribute lands in StageCraft. An invalid Attribute means StageCraft has no control for it. */
struct FStageGdtfAttributeMapping
{
	FGameplayTag Attribute;
	EStageGdtfValueKind Kind = EStageGdtfValueKind::Normalized;
	EStageGdtfShutterRole ShutterRole = EStageGdtfShutterRole::None;
	/** The per-cell control when the attribute sits on a repeated (cell) geometry; Count when cells cannot carry it. */
	EStageCellChannel CellChannel = EStageCellChannel::Count;
	/** StageCraft's model is close but not exact (for example a blade's second edge). Reported. */
	bool bApproximated = false;
};

enum class EStageGdtfChannelStatus : uint8
{
	/** Driven by a StageCraft attribute. */
	Mapped,
	/** Driven, with a difference the report names. */
	Approximated,
	/** No StageCraft control: always sent at its default value. */
	Default,
	/** No DMX slot (Offset "None"). */
	Virtual,
};

/** One DMX range of a channel in StageCraft terms (becomes FStageDMXFunction in GD3). */
struct FStageGdtfRange
{
	int64 DMXFrom = 0;
	int64 DMXTo = 0;
	/** Invalid: StageCraft never selects this range (a closed shutter, a macro, a wheel shake). */
	FGameplayTag Attribute;
	float ValueFrom = 0.f;
	float ValueTo = 0.f;
	/** A slot range (wheel, prism); INDEX_NONE for continuous ranges. */
	int32 Slot = INDEX_NONE;
	/** The GDTF function or set name, for the report. */
	FString Label;
};

/** One channel of a mode as StageCraft will drive it. */
struct FStageGdtfLayoutChannel
{
	/** 1-based slots, most significant first. Empty for a virtual channel. */
	TArray<int32> Offsets;
	/** The StageCraft attribute that owns the channel; invalid when Status is Default or Virtual. */
	FGameplayTag Attribute;
	TArray<FStageGdtfRange> Ranges;
	/** Sent when nothing drives the channel, in the channel's resolution. */
	int64 DefaultDMX = 0;
	EStageGdtfChannelStatus Status = EStageGdtfChannelStatus::Default;
	/** The GDTF attribute of the owning function (or of the first function), for the report. */
	FString GdtfAttribute;
	/** 0-based cell for channels of a repeated geometry; INDEX_NONE otherwise. */
	int32 Cell = INDEX_NONE;
	/** Why the channel is approximated or kept at its default. */
	FString Note;
	/** The owning function's links, for the capabilities: its geometry, wheel and emitter (empty when none). */
	FString Geometry;
	FString Wheel;
	FString Emitter;

	bool Is16Bit() const { return Offsets.Num() == 2; }
};

/** One mode's layout. A mode with Errors cannot be imported; the others still can. */
struct FStageGdtfModeLayout
{
	/** Index into FStageGdtfFixtureType::Modes. */
	int32 ModeIndex = INDEX_NONE;
	FString Name;
	int32 Footprint = 0;
	int32 CellCount = 1;
	TArray<FStageGdtfLayoutChannel> Channels;
	/** Lowest and highest strobe rate of the mode's strobe ranges, in Hz; zero when it has none. */
	FVector2D StrobeRateHz = FVector2D::ZeroVector;
	TArray<FString> Warnings;
	TArray<FString> Errors;

	bool IsUsable() const { return Errors.IsEmpty(); }
	int32 CountStatus(EStageGdtfChannelStatus Status) const;
};

/** One non-open slot of a wheel as StageCraft lists it (slot 0, Open or Out, is implicit). */
struct FStageGdtfSlotOut
{
	FString Name;
	/** Colour wheels: the filter's linear transmission. */
	FLinearColor Filter = FLinearColor::White;
	/** Gobo wheels: the PNG's base name under ./wheels; empty when none. */
	FString MediaFileName;
	/** Prisms: facets (3 when the file does not say). */
	int32 Facets = 3;
};

/** What the fixture can do, from every usable mode together (ADR 0033 §4.2: capabilities are the fixture's). */
struct FStageGdtfCapabilities
{
	EStageFixtureKind Kind = EStageFixtureKind::LEDPar;
	FGameplayTag Category;
	EStageFixtureColorSystem ColorSystem = EStageFixtureColorSystem::None;
	/** Emitter colours in StageCraft attribute order (R, G, B, W, A); empty for the colour system's defaults. */
	TArray<FLinearColor> Emitters;
	bool bAcceptsGel = false;
	bool bHasShutter = false;
	FVector2D StrobeRateHz = FVector2D(1.0, 20.0);
	bool bHasCTO = false;
	bool bHasColorTemperatureControl = false;
	FVector2D ColorTemperatureRangeK = FVector2D(2700.0, 8000.0);
	TArray<FStageGdtfSlotOut> ColorWheel;
	TArray<FStageGdtfSlotOut> GoboWheel;
	TArray<FStageGdtfSlotOut> StaticGoboWheel;
	TArray<FStageGdtfSlotOut> Prisms;
	bool bGoboSpin = false;
	float MaxRotationRpm = 60.f;
	bool bHasFrost = false;
	bool bHasIris = false;
	EStageBeamShaper BeamShaper = EStageBeamShaper::None;
	int32 CellCount = 1;
	EStageCellControl CellControl = EStageCellControl::Color;
	bool bHasPanTilt = false;
	FVector2D PanLimits = FVector2D(-270.0, 270.0);
	FVector2D TiltLimits = FVector2D(-135.0, 135.0);
	float LuminousFlux = 10000.f;
	float ColorTemperatureK = 6000.f;
	float BeamAngleMin = 25.f;
	float BeamAngleMax = 25.f;
	float FieldAngleRatio = 1.f;
	/** Body sizes and pivots in centimetres, Unreal axes; zero where the file gives nothing. */
	FVector BaseSizeCm = FVector::ZeroVector;
	FVector YokeSizeCm = FVector::ZeroVector;
	FVector HeadSizeCm = FVector::ZeroVector;
	float LensDiameterCm = 0.f;
	FVector PanPivotOffsetCm = FVector::ZeroVector;
	FVector TiltPivotOffsetCm = FVector::ZeroVector;
	FVector BeamOriginOffsetCm = FVector::ZeroVector;
	float WeightKg = 0.f;
	float PowerW = 0.f;
	FVector DimensionsCm = FVector::ZeroVector;
};

/** What the user sees before adding the fixture (ADR 0033 §8). */
struct FStageGdtfImportReport
{
	FString Title;
	TArray<FString> Lines;
	TArray<FString> Warnings;
	TArray<FString> Errors;
	/** At least one mode can be imported and the file itself had no errors. */
	bool bCanImport = false;

	FString ToText() const;
};

namespace StageGdtfMapping
{
	/** Values a strobe range spans on StageCraft's Shutter (the lowest strobe rate is just above open). */
	constexpr float StrobeFloor = 0.01f;

	/** Where a GDTF attribute ("Dimmer", "ColorAdd_R", "Gobo1Pos"...) lands. Unknown names map to an invalid attribute. */
	FStageGdtfAttributeMapping MapAttribute(FStringView GdtfAttribute);

	/** The StageCraft slot (0 = Open/Out) of a 1-based GDTF wheel slot index; INDEX_NONE when the wheel has no such slot. */
	int32 MapWheelSlot(const FStageGdtfWheel& Wheel, int32 WheelSlotIndex);

	/** The slots StageCraft lists for a wheel: every slot except the first open (or out) one. */
	TArray<FStageGdtfSlotOut> ListSlots(const FStageGdtfWheel& Wheel);

	/** Builds one mode's layout: offsets, cells, ranges, defaults and statuses, with errors for what StageCraft cannot represent. */
	FStageGdtfModeLayout BuildModeLayout(const FStageGdtfFixtureType& Type, int32 ModeIndex);

	/** The fixture's capabilities from the usable layouts (and the geometry, beam, wheels and emitters they use). */
	FStageGdtfCapabilities DeriveCapabilities(const FStageGdtfFixtureType& Type, TConstArrayView<FStageGdtfModeLayout> Layouts);

	/** The Library category of a fixture kind (ADR 0033 §5.6). */
	FGameplayTag CategoryForKind(EStageFixtureKind Kind);

	/** The report for a parsed file and its layouts. */
	FStageGdtfImportReport BuildReport(const FStageGdtfParseResult& Parsed, TConstArrayView<FStageGdtfModeLayout> Layouts, const FStageGdtfCapabilities& Capabilities);
}
