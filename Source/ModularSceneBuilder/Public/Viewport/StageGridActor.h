// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Viewport/StageViewMath.h"
#include "StageGridActor.generated.h"

/**
 * The viewport's reference grid (Docs/ADR/0008 §4.4): 1-pixel lines in the floor plane or, in front and side views, in a
 * vertical plane behind the layout. Minor lines at the grid-snap size, major lines every MajorEvery cells, and the world axes
 * in red / green / blue. Lines are drawn into scene colour, so the grid reads in Lit, Unlit and the line view alike.
 *
 * Spawned and driven by UStageViewportComponent. It rebuilds its lines only when the plane, the offset or the cell size
 * changes (no tick: the line batch's own tick is switched off, as these lines never expire). Registered as a render
 * helper, so it never appears in a render.
 */
UCLASS(NotPlaceable, Transient)
class MODULARSCENEBUILDER_API AStageGridActor : public AActor
{
	GENERATED_BODY()

public:
	AStageGridActor();

	/** Shows the grid with these lines; rebuilds only when something changed. A non-positive cell size hides it. */
	void ShowGrid(StageViewMath::EGridPlane Plane, double PlaneOffset, double CellCm);

	/** Hides the grid and drops its lines. Idempotent. */
	void HideGrid();

	bool IsGridShown() const { return LastBuild.IsSet(); }

	/** Lines currently drawn, for diagnostics. */
	int32 GetLineCount() const;

protected:
	//~ Begin AActor Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End AActor Interface

	/** How far the grid reaches from the origin on each side, in cm (fewer cells when the line cap would be exceeded). */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Grid", meta = (ClampMin = "100.0", Units = "cm"))
	double HalfExtentCm = 5000.0;

	/** A major line every this many cells. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Grid", meta = (ClampMin = "1"))
	int32 MajorEvery = 10;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Grid")
	FLinearColor MinorColor = FLinearColor(0.26f, 0.27f, 0.29f);

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Grid")
	FLinearColor MajorColor = FLinearColor(0.42f, 0.43f, 0.46f);

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Grid")
	FLinearColor AxisXColor = FLinearColor(0.60f, 0.12f, 0.10f);

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Grid")
	FLinearColor AxisYColor = FLinearColor(0.15f, 0.50f, 0.12f);

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Grid")
	FLinearColor AxisZColor = FLinearColor(0.12f, 0.25f, 0.65f);

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<class ULineBatchComponent> Lines = nullptr;

private:
	struct FGridBuild
	{
		StageViewMath::EGridPlane Plane = StageViewMath::EGridPlane::XY;
		double PlaneOffset = 0.0;
		double CellCm = 0.0;

		bool operator==(const FGridBuild& Other) const
		{
			return Plane == Other.Plane && PlaneOffset == Other.PlaneOffset && CellCm == Other.CellCm;
		}
	};

	FLinearColor GetLineColor(StageViewMath::EGridLineKind Kind) const;

	/** What the lines show; unset while hidden. */
	TOptional<FGridBuild> LastBuild;

	/** Reused between rebuilds. */
	TArray<StageViewMath::FGridLine> ScratchLines;
};
