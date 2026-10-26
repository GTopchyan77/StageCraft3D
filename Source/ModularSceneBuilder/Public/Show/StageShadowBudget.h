// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "StageShadowBudget.generated.h"

/** One fixture as the shadow budget sees it. */
struct FStageShadowCandidate
{
	/** Light reaching the viewer, roughly: luminous flux at the current dimmer over the squared distance (lm / m²). 0 = dark. */
	float Importance = 0.f;
	/** Whether its beam casts shadows now (it keeps its place against a slightly brighter newcomer, so beams do not flicker). */
	bool bCasting = false;
};

/**
 * Which fixtures may cast shadows (Docs/ADR/0018 §4). Moving shadowed spot lights are what kept 500 crossfading fixtures on the
 * render thread (STATE.md #39): every moved shadowed light re-renders its shadow depths each frame. Unshadowed beams still
 * light the stage; only their shadows go.
 */
namespace StageShadowBudget
{
	/**
	 * Out[i] says whether Candidates[i] casts. The Budget brightest (by Importance, a current caster's importance raised by
	 * Hysteresis, e.g. 0.25 = 25 %) cast; dark candidates never do. Ties keep the lower index. Deterministic; Budget < 0 means
	 * every lit candidate casts.
	 */
	MODULARSCENEBUILDER_API TArray<bool> SelectCasters(TConstArrayView<FStageShadowCandidate> Candidates, int32 Budget, float Hysteresis);

	/** Importance of a beam of Flux lumens seen from DistanceCm away (closer than 1 m counts as 1 m). */
	MODULARSCENEBUILDER_API float GetImportance(float Flux, double DistanceCm);
}

/**
 * Frame-time settings for lighting (Project Settings > Game > StageCraft Performance, saved to DefaultGame.ini). Read by
 * UShowControlSubsystem whenever it re-ranks; every value is clamped by the getters.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "StageCraft Performance"))
class MODULARSCENEBUILDER_API UStageShowPerformanceSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	//~ Begin UDeveloperSettings Interface
	virtual FName GetCategoryName() const override { return TEXT("Game"); }
	//~ End UDeveloperSettings Interface

	/** Fixtures allowed to cast beam shadows at once, 0..2000; -1 when the budget is off (every fixture casts). */
	int32 GetShadowBudget() const;

	/** Seconds between re-rankings while something changes (fades, the camera), 0.05..2. */
	float GetUpdateSeconds() const;

	/** 0..2: how much brighter a newcomer must be to take a caster's place. */
	float GetHysteresis() const;

private:
	/** Off: every fixture casts shadows (the look before STATE.md #45, and its cost). */
	UPROPERTY(Config, EditAnywhere, Category = "Fixture Shadows", meta = (DisplayName = "Limit Fixture Shadows"))
	bool bLimitFixtureShadows = true;

	/** The brightest fixtures nearest the camera cast shadows; the rest light without them. 64 keeps 500 moving heads interactive. */
	UPROPERTY(Config, EditAnywhere, Category = "Fixture Shadows", meta = (EditCondition = "bLimitFixtureShadows", ClampMin = "0", ClampMax = "2000"))
	int32 MaxShadowedFixtures = 64;

	/** How often the choice follows fades and camera moves. Each change rebuilds that light's shadows once. */
	UPROPERTY(Config, EditAnywhere, Category = "Fixture Shadows", meta = (EditCondition = "bLimitFixtureShadows", ClampMin = "0.05", ClampMax = "2.0", Units = "s"))
	float UpdateIntervalSeconds = 0.25f;

	UPROPERTY(Config, EditAnywhere, Category = "Fixture Shadows", meta = (EditCondition = "bLimitFixtureShadows", ClampMin = "0.0", ClampMax = "2.0"))
	float Hysteresis = 0.25f;
};
