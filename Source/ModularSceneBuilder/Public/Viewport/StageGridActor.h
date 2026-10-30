// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Viewport/StageViewMath.h"
#include "StageGridActor.generated.h"

/**
 * The viewport's reference grid (Docs/ADR/0008 §4.4): lines in the floor plane or, in front and side views, in a vertical
 * plane behind the layout. Minor lines at the grid-snap size, major lines every MajorEvery cells, and the world axes in
 * red / green / blue. Drawn as one translucent unlit quad, so the grid reads in Lit, Unlit and the line view alike.
 *
 * The lines are computed per pixel by GridMaterial (Scripts/Content/create_grid_content.py) with analytic anti-aliasing and
 * a fade once cells shrink below a few pixels (STATE.md #48). One-pixel line primitives crawled under TSR's jitter; this does not.
 *
 * Spawned and driven by UStageViewportComponent. It moves the quad and sets material parameters only when the plane, the
 * offset or the cell size changes; it never ticks. Registered as a render helper, so it never appears in a render.
 */
UCLASS(NotPlaceable, Transient, Config = Game)
class MODULARSCENEBUILDER_API AStageGridActor : public AActor
{
	GENERATED_BODY()

public:
	AStageGridActor();

	/** Shows the grid; changes it only when something changed. A non-positive cell size hides it. */
	void ShowGrid(StageViewMath::EGridPlane Plane, double PlaneOffset, double CellCm);

	/** Hides the grid. Idempotent. */
	void HideGrid();

	bool IsGridShown() const { return LastBuild.IsSet(); }

	/** The cell size shown, 0 while hidden; for diagnostics. */
	double GetCellSize() const { return LastBuild.IsSet() ? LastBuild->CellCm : 0.0; }

protected:
	//~ Begin AActor Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End AActor Interface

	/** The grid material (DefaultGame.ini). Without it the grid stays hidden and a warning says why. */
	UPROPERTY(Config, EditDefaultsOnly, Category = "StageCraft|Grid")
	TSoftObjectPtr<class UMaterialInterface> GridMaterial;

	/** How far the grid reaches from the origin on each side, in cm. */
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
	TObjectPtr<class UProceduralMeshComponent> Surface = nullptr;

private:
	/** The material instance on Surface; created on the first ShowGrid. Null when GridMaterial did not load. */
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> GridMaterialInstance = nullptr;

	/** Creates GridMaterialInstance once. False, with a warning the first time, when the material is unavailable. */
	bool EnsureMaterial();

	/** Whether the missing-material warning was logged, so it is said once rather than on every snap change. */
	bool bWarnedMissingMaterial = false;

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

	/** What the grid shows; unset while hidden. */
	TOptional<FGridBuild> LastBuild;
};
