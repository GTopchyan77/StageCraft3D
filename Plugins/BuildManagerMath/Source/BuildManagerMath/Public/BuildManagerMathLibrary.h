// Copyright DamenTools internal. Build Manager Math plugin.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "BuildManagerMathLibrary.generated.h"

class APlayerController;

UCLASS()
class BUILDMANAGERMATH_API UBuildManagerMathLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Deprojects the mouse cursor into world space and intersects that ray
	 * with the horizontal placement plane at Z = PlaneZ. Replaces the
	 * 12-node manual ray/plane-intersection chain previously duplicated
	 * every frame in BP_BuildManager.Tick -> UpdateBuildingManagerValues
	 * (#15/#160): DeprojectMousePositionToWorld -> BreakVector x2 -> Abs ->
	 * Subtract/Divide/Less/LessEqual (DoubleDouble) -> Multiply_VectorFloat
	 * -> Add_VectorVector -> BreakVector -> MakeVector.
	 *
	 * Takes PlaneZ/Epsilon as plain parameters and PlayerController as a
	 * plain object pointer — this reads no BP_BuildManager state by name,
	 * so it needs NO reflection bridge (FindFProperty/ProcessEvent), unlike
	 * SpaceCalculator/PreciseTransform's functions. BP_BuildManager still
	 * has no native base class (#152/#160) and this does not change that;
	 * it is called FROM BP_BuildManager's own graph as an ordinary
	 * UBlueprintFunctionLibrary call, not via reflection.
	 *
	 * Fails (returns false, OutLocation left at (0,0,0)) if: PlayerController
	 * is invalid, the mouse can't be deprojected (no viewport/no mouse
	 * position), the ray is near-parallel to the plane (|WorldDirection.Z| <
	 * Epsilon — avoids a divide-by-zero), or the intersection distance
	 * factor T is not meaningfully positive (T <= Epsilon) — this exactly
	 * mirrors the original three Blueprint gates, including the
	 * "do nothing this frame" no-op on failure (the caller must not write
	 * LocationUnderCursor(Camera) when this returns false).
	 *
	 * @param PlayerController  The controller whose mouse cursor to deproject (BP_BuildManager's own PlayerController variable).
	 * @param PlaneZ            World Z of the horizontal placement plane (BP_BuildManager's PlaceablePlaneZ).
	 * @param Epsilon           Tolerance for the near-parallel and near-zero-distance gates (BP_BuildManager's PlaneIntersectionEpsilon).
	 * @param OutLocation       Out: the intersection point, with Z forced exactly to PlaneZ (matches the original's MakeVector(X, Y, PlaceablePlaneZ)).
	 * @return true if a valid intersection was found and OutLocation should be used; false if the caller should skip updating this frame.
	 */
	UFUNCTION(BlueprintCallable, Category = "BuildManagerMath")
	static bool ComputeCursorPlaneIntersection(APlayerController* PlayerController, float PlaneZ, float Epsilon, FVector& OutLocation);
};
