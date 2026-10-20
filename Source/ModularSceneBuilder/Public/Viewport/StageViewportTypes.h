// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StageViewportTypes.generated.h"

/**
 * How the stage viewport projects the scene (Docs/ADR/0008-viewport-toolbar-projections-and-view-modes.md §4.1).
 * The orthographic views use the Unreal Editor's orientations: Top looks down (+X up, +Y right), Front looks along -Y
 * (the XZ plane), Back along +Y, Left along +X (the YZ plane), Right along -X.
 */
UENUM(BlueprintType)
enum class EStageViewProjection : uint8
{
	Perspective,
	Top,
	Front,
	Back,
	Left,
	Right,
};

/**
 * How the stage viewport shades the scene (ADR 0008 §4.2, §4.3). Renders are always Lit. Not persisted: every launch
 * starts Lit. The last three are engine lighting diagnostics, unavailable in Shipping (StageViewMath::IsViewModeAvailable).
 */
UENUM(BlueprintType)
enum class EStageViewMode : uint8
{
	/** Full photometric rendering: lights, shadows, fog, post-processing. */
	Lit,

	/** Base colours without lights, shadows or fog: clean placement. */
	Unlit,

	/** CAD line view: silhouettes and hard edges on a flat background, no lights. */
	Lines,

	/** Lighting on neutral grey materials (VMI_LightingOnly): checks fixture coverage without colour or textures. */
	LightingOnly,

	/** Lighting with material normals on neutral materials (VMI_Lit_DetailLighting). */
	DetailLighting,

	/** Every surface as a smooth mirror (VMI_ReflectionOverride): checks reflection capture and Lumen reflections. */
	Reflections,
};
