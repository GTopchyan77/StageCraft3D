// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/BaseItemData.h"
#include "StageTrussData.generated.h"

UENUM(BlueprintType)
enum class EStageTrussProfile : uint8
{
	Ladder		UMETA(DisplayName = "Ladder (2-chord)"),
	Triangle	UMETA(DisplayName = "Triangle (3-chord)"),
	Box			UMETA(DisplayName = "Box (4-chord)"),
};

UENUM(BlueprintType)
enum class EStageTrussPieceKind : uint8
{
	Straight	UMETA(DisplayName = "Straight"),
	Corner		UMETA(DisplayName = "Corner"),
	Junction	UMETA(DisplayName = "T / Cross Junction"),
	Tower		UMETA(DisplayName = "Tower / Ground Support"),
	BasePlate	UMETA(DisplayName = "Base Plate"),
	Hoist		UMETA(DisplayName = "Chain Hoist"),
};

/** A point where a load (fixture clamp, hoist hook, bridle) may be attached. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageRiggingPoint
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rigging")
	FName Name;

	/** Actor-local transform. +Z of the rotation is the direction a hung load pulls away from (usually down = -Z). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rigging")
	FTransform LocalTransform;

	/** Safe working load at this point. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rigging", meta = (ClampMin = "0.0", Units = "kg"))
	float SafeWorkingLoadKg = 250.f;
};

/** A face where another truss piece connects (for snap-to-truss in a later phase). */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageTrussConnector
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rigging")
	FName Name;

	/** Actor-local transform; +X points out of the connection face. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rigging")
	FTransform LocalTransform;
};

/**
 * Catalog entry for truss pieces and rigging hardware. Each physical length is its own asset
 * (as in a rental inventory: "F34 2.0 m", "F34 3.0 m"), so truss instances have no length
 * parameter to edit and loads stay traceable to a real part number.
 */
UCLASS(BlueprintType, Const)
class MODULARSCENEBUILDER_API UStageTrussData : public UBaseItemData
{
	GENERATED_BODY()

public:
	UStageTrussData();

	//~ Begin UObject Interface
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
	//~ End UObject Interface

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Truss")
	EStageTrussPieceKind PieceKind = EStageTrussPieceKind::Straight;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Truss", meta = (EditCondition = "PieceKind != EStageTrussPieceKind::Hoist"))
	EStageTrussProfile Profile = EStageTrussProfile::Box;

	/** Outer chord-to-chord width, e.g. 29 cm for F34-class truss. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Truss", meta = (ClampMin = "0.0", Units = "cm"))
	float ProfileWidth = 29.f;

	/** Length along the actor's local +X. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Truss", meta = (ClampMin = "0.0", Units = "cm"))
	float Length = 200.f;

	/** Maximum centre point load for this span (hoist: rated capacity). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Truss", meta = (ClampMin = "0.0", Units = "kg"))
	float MaxPointLoadKg = 500.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rigging", meta = (TitleProperty = "Name"))
	TArray<FStageRiggingPoint> RiggingPoints;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rigging", meta = (TitleProperty = "Name"))
	TArray<FStageTrussConnector> Connectors;
};
