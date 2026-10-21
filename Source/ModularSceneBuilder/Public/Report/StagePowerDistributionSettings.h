// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "StagePowerDistributionSettings.generated.h"

UENUM()
enum class EStagePowerPhases : uint8
{
	Single UMETA(DisplayName = "Single phase"),
	Three UMETA(DisplayName = "3-phase")
};

/**
 * The electrical supply the cable schedule plans for (Project Settings > Game > StageCraft Power Distribution, saved to
 * DefaultGame.ini; Docs/ADR/0012-production-reports-and-fixture-benchmark.md §2.3). Defaults are a European 3-phase supply with
 * 16 A circuits loaded to 80 %. The editor ranges are a convenience only: StageCableSchedule::Sanitize clamps every value again,
 * so a hand-edited ini cannot produce a division by zero or a negative current.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "StageCraft Power Distribution"))
class MODULARSCENEBUILDER_API UStagePowerDistributionSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	//~ Begin UDeveloperSettings Interface
	virtual FName GetCategoryName() const override { return TEXT("Game"); }
	//~ End UDeveloperSettings Interface

	/** The settings as the planner's value type. */
	struct FStagePowerDistribution ToDistribution() const;

	/** Phase-to-neutral voltage. */
	UPROPERTY(Config, EditAnywhere, Category = "Supply", meta = (ClampMin = "100", ClampMax = "400", DisplayName = "Phase Voltage (V)"))
	float PhaseVoltage = 230.f;

	UPROPERTY(Config, EditAnywhere, Category = "Supply")
	EStagePowerPhases Phases = EStagePowerPhases::Three;

	/** Breaker rating of one distro circuit. */
	UPROPERTY(Config, EditAnywhere, Category = "Circuits", meta = (ClampMin = "1", ClampMax = "125", DisplayName = "Circuit Rating (A)"))
	float CircuitRatingAmps = 16.f;

	/** Share of the rating a circuit may carry continuously (80 % is the usual planning margin). */
	UPROPERTY(Config, EditAnywhere, Category = "Circuits", meta = (ClampMin = "10", ClampMax = "100", Units = "Percent"))
	float UsablePercent = 80.f;

	/** Real power / apparent power of the loads. Catalog specs give watts; 1 treats them as VA. */
	UPROPERTY(Config, EditAnywhere, Category = "Circuits", meta = (ClampMin = "0.5", ClampMax = "1.0"))
	float PowerFactor = 1.f;
};
