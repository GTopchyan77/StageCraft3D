// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * The Preferences panel's Quality row: the engine's scalability levels 0–3 as one choice. Cinematic is not offered (it is
 * meant for offline renders). Custom means the saved per-group levels are not one single level, e.g. a hand-edited
 * GameUserSettings.ini; picking another row keeps those groups as they are.
 */
enum class EStageGraphicsQuality : uint8
{
	Low,
	Medium,
	High,
	Epic,
	Custom
};

/** The graphics preferences of this PC as the Preferences panel shows them (stored by the engine in GameUserSettings.ini). */
struct FStageGraphicsChoices
{
	EStageGraphicsQuality Quality = EStageGraphicsQuality::Epic;
	/** Render resolution in percent of the viewport; 0 = the engine's default for this display. */
	int32 ResolutionPercent = 0;
	/** Frames per second; 0 = no cap. */
	int32 FrameCap = 0;
	bool bVSync = false;

	bool operator==(const FStageGraphicsChoices& Other) const
	{
		return Quality == Other.Quality && ResolutionPercent == Other.ResolutionPercent && FrameCap == Other.FrameCap && bVSync == Other.bVSync;
	}
	bool operator!=(const FStageGraphicsChoices& Other) const { return !(*this == Other); }
};

/**
 * Pure rules for the graphics preferences (STATE.md #111). The engine's UGameUserSettings stores and applies them;
 * UStageGraphicsSubsystem is the only writer.
 */
namespace StageGraphics
{
	/** Below this a stage is unreadable; the engine itself allows down to 0 %. */
	constexpr int32 MinResolutionPercent = 25;
	constexpr int32 MaxResolutionPercent = 100;
	/** A cap outside this range is a typo, not a choice (a cap of 1–14 fps would make the app unusable). */
	constexpr int32 MinFrameCap = 15;
	constexpr int32 MaxFrameCap = 360;

	/** The Resolution row: 0 (Auto), then percentages. */
	MODULARSCENEBUILDER_API TConstArrayView<int32> GetResolutionPresets();

	/** The Frame cap row: 0 (Off), then frames per second. */
	MODULARSCENEBUILDER_API TConstArrayView<int32> GetFrameCapPresets();

	/**
	 * Values the engine can be given:
	 *  - an unknown quality becomes Custom;
	 *  - a resolution of 0 or less becomes 0 (Auto), anything else is clamped to MinResolutionPercent..MaxResolutionPercent;
	 *  - a frame cap of 0 or less becomes 0 (Off), anything else is clamped to MinFrameCap..MaxFrameCap.
	 */
	MODULARSCENEBUILDER_API FStageGraphicsChoices Sanitize(const FStageGraphicsChoices& Choices);

	/** The engine's single scalability level for Quality, or INDEX_NONE for Custom. */
	MODULARSCENEBUILDER_API int32 ToScalabilityLevel(EStageGraphicsQuality Quality);

	/**
	 * The Quality the engine's per-group levels (shadows, textures, effects, ...) amount to: their common level 0–3, or Custom
	 * when they differ, when one is outside 0–3 (Cinematic) or when there are none.
	 * The render resolution is deliberately not part of it: the engine's own single level only counts when the resolution
	 * equals that level's default (BaseScalability.ini "50 71 87 100"), but here Resolution is a row of its own.
	 */
	MODULARSCENEBUILDER_API EStageGraphicsQuality FromGroupLevels(TConstArrayView<int32> Levels);
}
