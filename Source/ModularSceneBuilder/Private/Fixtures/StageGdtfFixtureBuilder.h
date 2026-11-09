// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/LightingFixtureData.h"
#include "Fixtures/StageGdtfMapping.h"
#include "Fixtures/StageGdtfTypes.h"

/**
 * Turns a parsed GDTF fixture type into StageCraft fixture data (Docs/ADR/0033 §4–§6): one ULightingFixtureData per usable DMX
 * mode, sharing the fixture's capabilities and body.
 *
 * Fill and the body rules are deterministic: the same file gives the same data in every run. Unmapped channels keep their slots
 * as channels without an attribute and with the GDTF attribute as their label, so the footprint is the mode's real footprint.
 * Body part sizes come from the file's models where it has them, otherwise from kind defaults; pivots and the beam origin
 * follow the built-in fixtures' layout rule from those sizes (GDTF positions are adopted once real files confirm them, §14).
 *
 * Fill only writes properties of the object it is given; BuildBaseMesh creates an engine object. Both run on the game thread.
 * UStaticMesh is declared at global scope by LightingFixtureData.h, so the declarations below name the engine class.
 */
namespace StageGdtfFixtureBuilder
{
	/** Body part sizes and offsets in centimetres, in the layout ULightingFixtureData uses. */
	struct FBodyLayout
	{
		FVector BaseSizeCm = FVector::ZeroVector;
		FVector YokeSizeCm = FVector::ZeroVector;
		FVector HeadSizeCm = FVector::ZeroVector;
		float LensDiameterCm = 0.f;
		FVector2D CellStripSizeCm = FVector2D::ZeroVector;
		FVector PanPivotOffset = FVector::ZeroVector;
		FVector TiltPivotOffset = FVector::ZeroVector;
		FVector BeamOriginOffset = FVector::ZeroVector;
		/** The head is a box (bars, strobes, blinders) rather than a cylinder. */
		bool bBoxHead = false;
	};

	/** The body a fixture with these capabilities gets. Pure. */
	FBodyLayout MakeBodyLayout(const FStageGdtfCapabilities& Capabilities, EStageFixtureKind Kind);

	/**
	 * Fills a freshly created ULightingFixtureData with one mode of the type. Kind overrides the inferred kind when set (the
	 * user's correction). BaseMesh is the fixture's base, shared by every mode; null leaves the base empty.
	 */
	void Fill(ULightingFixtureData& Data, const FStageGdtfFixtureType& Type, const FStageGdtfCapabilities& Capabilities, const FStageGdtfModeLayout& Layout,
		TOptional<EStageFixtureKind> Kind, UStaticMesh* BaseMesh);

	/**
	 * DMX channels of a layout in StageCraft's form (Docs/ADR/0033 §4.3): mapped channels with their attribute (16-bit when the fine
	 * byte follows the coarse one) and their ranges as functions; everything else as channels without an attribute that keep
	 * their slots. Every channel carries its GDTF default, split per byte where its bytes become separate channels. Strobe ranges
	 * are scaled over StrobeRateHz, the fixture's strobe rates in Hz. A channel with more than StageDMXFunctions::MaxFunctions
	 * ranges is kept at its default. Sorted by channel. Pure.
	 */
	TArray<FStageFixtureDMXChannel> MakeDMXChannels(const FStageGdtfModeLayout& Layout, const FVector2D& StrobeRateHz);

	/** The Shutter value of a strobe rate on a fixture whose strobe spans StrobeRateHz: StageGdtfMapping::StrobeFloor..1. Pure. */
	float StrobeHzToShutter(float Hz, const FVector2D& StrobeRateHz);

	/**
	 * A box of SizeCm centred on its origin, with the engine's basic-shape material and a simple box collision so the fixture can be
	 * picked. Transient, in Outer. Null when the size is not positive or the mesh could not be built.
	 */
	UStaticMesh* BuildBaseMesh(UObject& Outer, FName Name, const FVector& SizeCm);
}
