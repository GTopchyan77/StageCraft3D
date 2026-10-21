// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "StageCablingSettings.generated.h"

UENUM()
enum class EStageCableAnchorSetting : uint8
{
	/** Under the equipment it serves, on the floor. */
	Automatic,
	/** At the location given below. */
	Fixed
};

/**
 * How the shopping and assembly list measures cables and scales hardware (Project Settings > Game > StageCraft Cabling & Rigging,
 * saved to DefaultGame.ini; Docs/ADR/0014-shopping-and-assembly-list.md). Defaults: +10 % on every run, stock 2/5/10/15/20 m,
 * 12-way distros on a 63 A feed, rigged from 2 m, one clamp per 20 kg, bonds of 10/25/50/100 kg. The editor ranges are a
 * convenience only: StageShoppingList::Sanitize clamps every value again, so a hand-edited ini cannot break planning.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "StageCraft Cabling & Rigging"))
class MODULARSCENEBUILDER_API UStageCablingSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	//~ Begin UDeveloperSettings Interface
	virtual FName GetCategoryName() const override { return TEXT("Game"); }
	//~ End UDeveloperSettings Interface

	/** The settings as the planner's value type, sanitized. */
	struct FStageCablingRules ToRules() const;

	/** Added to every measured route before it is rounded up to a stock length. */
	UPROPERTY(Config, EditAnywhere, Category = "Cables", meta = (ClampMin = "0", ClampMax = "100", Units = "Percent"))
	float SafetyMarginPercent = 10.f;

	/** The cable lengths in stock, in metres. A run longer than the longest is made of joined cables. */
	UPROPERTY(Config, EditAnywhere, Category = "Cables", meta = (DisplayName = "Stock Lengths (m)"))
	TArray<float> StockLengthsMeters = { 2.f, 5.f, 10.f, 15.f, 20.f };

	UPROPERTY(Config, EditAnywhere, Category = "Cables")
	FString PowerCableName = TEXT("PowerCON TRUE1");

	UPROPERTY(Config, EditAnywhere, Category = "Cables")
	FString DmxCableName = TEXT("XLR 5-pin DMX");

	/** Where the distros stand. Automatic: the centre of the powered equipment, on the floor. */
	UPROPERTY(Config, EditAnywhere, Category = "Positions")
	EStageCableAnchorSetting DistroPosition = EStageCableAnchorSetting::Automatic;

	UPROPERTY(Config, EditAnywhere, Category = "Positions", meta = (EditCondition = "DistroPosition == EStageCableAnchorSetting::Fixed", DisplayName = "Distro Location (m)"))
	FVector DistroLocationMeters = FVector::ZeroVector;

	/** Where the DMX node feeding the universes stands. Automatic: the centre of the patched fixtures, on the floor. */
	UPROPERTY(Config, EditAnywhere, Category = "Positions")
	EStageCableAnchorSetting ControlNodePosition = EStageCableAnchorSetting::Automatic;

	UPROPERTY(Config, EditAnywhere, Category = "Positions", meta = (EditCondition = "ControlNodePosition == EStageCableAnchorSetting::Fixed", DisplayName = "Control Node Location (m)"))
	FVector ControlNodeLocationMeters = FVector::ZeroVector;

	/** Circuit outputs on one distro. */
	UPROPERTY(Config, EditAnywhere, Category = "Distros", meta = (ClampMin = "1", ClampMax = "96"))
	int32 DistroOutputs = 12;

	/** Current one distro may take on each phase of its feed. */
	UPROPERTY(Config, EditAnywhere, Category = "Distros", meta = (ClampMin = "1", ClampMax = "1000", DisplayName = "Distro Feed (A)"))
	float DistroFeedAmps = 63.f;

	/** A fixture at least this high (world Z) is rigged and gets clamps and a safety bond; so is one tilted over 30°. */
	UPROPERTY(Config, EditAnywhere, Category = "Rigging", meta = (ClampMin = "0", ClampMax = "100", DisplayName = "Rigged Above (m)"))
	float RiggedAboveMeters = 2.f;

	/** Fixture weight one clamp may carry; heavier fixtures get more clamps. */
	UPROPERTY(Config, EditAnywhere, Category = "Rigging", meta = (ClampMin = "1", ClampMax = "1000", DisplayName = "Weight per Clamp (kg)"))
	float WeightPerClampKg = 20.f;

	/** Working load limits of the safety bonds in stock; each rigged fixture gets the lightest one rated for its weight. */
	UPROPERTY(Config, EditAnywhere, Category = "Rigging", meta = (DisplayName = "Safety Bond Ratings (kg)"))
	TArray<float> SafetyBondRatingsKg = { 10.f, 25.f, 50.f, 100.f };
};
