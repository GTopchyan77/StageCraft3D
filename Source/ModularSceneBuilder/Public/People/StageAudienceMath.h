// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "People/StageFigureRig.h"
#include "StageAudienceMath.generated.h"

/** What an audience area is doing (Docs/ADR/0031 §4). Saved as a byte: append new moods at the end. */
UENUM(BlueprintType)
enum class EStageCrowdMood : uint8
{
	/** Mostly standing and swaying, some clapping: a support act, a theatre, a conference. */
	Calm		UMETA(DisplayName = "Calm"),
	/** Mostly bouncing on the beat. */
	Dancing		UMETA(DisplayName = "Dancing"),
	/** Arms up and cheering. */
	Cheering	UMETA(DisplayName = "Cheering"),

	Count		UMETA(Hidden)
};

/**
 * Pure maths of an audience area (AStageAudienceActor): how many people a floor area holds at a density, where they stand and
 * what each one does. Deterministic for a seed, so a saved area shows the same crowd when it is opened again. No world, no
 * UObjects; covered by StageCraft.People.AudienceMath.
 *
 * Space: the area's local space. The area spans X in [0, Depth] (away from the stage) and Y in [-Width / 2, Width / 2], so the
 * actor's origin is the middle of its front edge, and everyone faces -X (towards the stage).
 */
namespace StageAudienceMath
{
	/** Densest crowd placed, in people per m² (a packed standing crowd is about 4). */
	constexpr double MaxDensity = 5.0;

	/** One person: where they stand and what they look like. Colours and tones in 0..1. */
	struct FPerson
	{
		FTransform Transform;
		EStageFigureClip Clip = EStageFigureClip::CrowdIdle;
		float Phase = 0.f;
		float CyclesPerSecond = 1.f;
		FLinearColor TopColor = FLinearColor::Gray;
		float SkinTone = 0.f;
		float BottomShade = 0.f;
	};

	/**
	 * People in an area of Width x Depth cm at Density people per m², at most MaxPeople. 0 for a non-finite, empty or negative area
	 * or density.
	 */
	MODULARSCENEBUILDER_API int32 GetPeopleCount(double Width, double Depth, double Density, int32 MaxPeople);

	/** The clips of a mood and their shares (summing to 1), in a fixed order. */
	MODULARSCENEBUILDER_API TArray<TPair<EStageFigureClip, float>> GetMoodMix(EStageCrowdMood Mood);

	/**
	 * Places Count people in the area on a jittered grid (never two in one cell, so nobody overlaps), with random clips from the
	 * mood's mix, phases, small speed and height differences, a slight turn either way, shirt colours from TopColors (a mid grey
	 * when empty), skin tones and trousers. The same arguments give the same people.
	 */
	MODULARSCENEBUILDER_API void PlacePeople(double Width, double Depth, int32 Count, EStageCrowdMood Mood, int32 Seed,
		TConstArrayView<FLinearColor> TopColors, TArray<FPerson>& OutPeople);
}
