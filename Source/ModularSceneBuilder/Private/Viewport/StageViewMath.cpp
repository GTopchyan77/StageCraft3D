// Copyright Epic Games, Inc. All Rights Reserved.

#include "Viewport/StageViewMath.h"

#include "Camera/CameraTypes.h"
#include "ShowFlags.h"

namespace StageViewMath
{
	namespace
	{
		constexpr float GridSnapPresets[] = { 0.f, 10.f, 50.f, 100.f };
		constexpr float AngleSnapPresets[] = { 0.f, 5.f, 15.f, 45.f };

		bool IsPreset(TConstArrayView<float> Presets, float Value)
		{
			return FMath::IsFinite(Value) && Presets.ContainsByPredicate([Value](float Preset) { return FMath::IsNearlyEqual(Preset, Value, 1.e-3f); });
		}

		/** A point in Plane at PlaneOffset, from coordinates along the plane's first and second in-plane axes. */
		FVector PlanePoint(EGridPlane Plane, double PlaneOffset, double First, double Second)
		{
			switch (Plane)
			{
			case EGridPlane::XZ:	return FVector(First, PlaneOffset, Second);
			case EGridPlane::YZ:	return FVector(PlaneOffset, First, Second);
			case EGridPlane::XY:
			default:				return FVector(First, Second, PlaneOffset);
			}
		}

		/** The axis drawn along the plane's first axis (at Second == 0) and along its second axis (at First == 0). */
		void GetPlaneAxes(EGridPlane Plane, EGridLineKind& OutAlongFirst, EGridLineKind& OutAlongSecond)
		{
			switch (Plane)
			{
			case EGridPlane::XZ:	OutAlongFirst = EGridLineKind::AxisX; OutAlongSecond = EGridLineKind::AxisZ; return;
			case EGridPlane::YZ:	OutAlongFirst = EGridLineKind::AxisY; OutAlongSecond = EGridLineKind::AxisZ; return;
			case EGridPlane::XY:
			default:				OutAlongFirst = EGridLineKind::AxisX; OutAlongSecond = EGridLineKind::AxisY; return;
			}
		}
	}

	bool IsOrthographic(EStageViewProjection Projection)
	{
		return Projection != EStageViewProjection::Perspective;
	}

	FRotator GetProjectionRotation(EStageViewProjection Projection)
	{
		switch (Projection)
		{
		case EStageViewProjection::Top:		return FRotator(-90.0, 0.0, 0.0);
		case EStageViewProjection::Front:	return FRotator(0.0, -90.0, 0.0);
		case EStageViewProjection::Back:	return FRotator(0.0, 90.0, 0.0);
		case EStageViewProjection::Left:	return FRotator(0.0, 0.0, 0.0);
		case EStageViewProjection::Right:	return FRotator(0.0, 180.0, 0.0);
		case EStageViewProjection::Perspective:
		default:							return FRotator::ZeroRotator;
		}
	}

	EGridPlane GetGridPlane(EStageViewProjection Projection)
	{
		switch (Projection)
		{
		case EStageViewProjection::Front:
		case EStageViewProjection::Back:	return EGridPlane::XZ;
		case EStageViewProjection::Left:
		case EStageViewProjection::Right:	return EGridPlane::YZ;
		default:							return EGridPlane::XY;
		}
	}

	double GetGridPlaneOffset(EStageViewProjection Projection, double BackDistance)
	{
		if (GetGridPlane(Projection) == EGridPlane::XY)
		{
			return 0.0;
		}
		// Along the view direction, past the origin: Front looks along -Y, so "behind" is negative Y.
		const FVector Forward = GetProjectionRotation(Projection).Vector();
		const double Sign = GetGridPlane(Projection) == EGridPlane::XZ ? FMath::Sign(Forward.Y) : FMath::Sign(Forward.X);
		return Sign * FMath::Max(0.0, BackDistance);
	}

	double ClampOrthoWidth(double Width)
	{
		return FMath::IsFinite(Width) ? FMath::Clamp(Width, MinOrthoWidth, MaxOrthoWidth) : MinOrthoWidth;
	}

	double FitOrthoWidth(double Distance, double FOVDegrees)
	{
		if (!FMath::IsFinite(Distance) || !FMath::IsFinite(FOVDegrees) || Distance <= 0.0 || FOVDegrees <= 0.0 || FOVDegrees >= 180.0)
		{
			return MinOrthoWidth;
		}
		return ClampOrthoWidth(2.0 * Distance * FMath::Tan(FMath::DegreesToRadians(FOVDegrees) * 0.5));
	}

	double StepOrthoWidth(double Width, double Steps)
	{
		if (!FMath::IsFinite(Steps))
		{
			return ClampOrthoWidth(Width);
		}
		return ClampOrthoWidth(ClampOrthoWidth(Width) / FMath::Pow(OrthoZoomStep, Steps));
	}

	FVector2D ComputeOrthoZoomShift(const FVector2D& CursorOffset, double OldWidth, double NewWidth)
	{
		if (!FMath::IsFinite(OldWidth) || !FMath::IsFinite(NewWidth) || OldWidth <= 0.0 || NewWidth <= 0.0 || CursorOffset.ContainsNaN())
		{
			return FVector2D::ZeroVector;
		}
		// Every on-screen offset scales with the width; the centre moves by what the cursor point's offset loses.
		return CursorOffset * (1.0 - NewWidth / OldWidth);
	}

	FVector2D ComputePanShift(const FVector2D& WorldPerPixel, const FVector2D& MouseDelta)
	{
		if (WorldPerPixel.ContainsNaN() || MouseDelta.ContainsNaN() || WorldPerPixel.X < 0.0 || WorldPerPixel.Y < 0.0)
		{
			return FVector2D::ZeroVector;
		}
		// Grab the scene: it follows the mouse, so the camera moves the other way.
		return -MouseDelta * WorldPerPixel;
	}

	double GetScreenScaleDistance(bool bOrthographic, double OrthoWidth, double FOVDegrees, const FVector& ViewLocation, const FVector& At)
	{
		if (!bOrthographic)
		{
			return FVector::Dist(ViewLocation, At);
		}
		const double HalfAngle = FMath::DegreesToRadians(FMath::Clamp(FOVDegrees, 1.0, 170.0)) * 0.5;
		return FMath::Max(OrthoWidth, 0.0) / (2.0 * FMath::Tan(HalfAngle));
	}

	double GetScreenScaleDistance(const FMinimalViewInfo& View, const FVector& At)
	{
		return GetScreenScaleDistance(View.ProjectionMode == ECameraProjectionMode::Orthographic, View.OrthoWidth, View.FOV, View.Location, At);
	}

	void BuildGridLines(EGridPlane Plane, double PlaneOffset, double CellCm, double HalfExtentCm, int32 MajorEvery, TArray<FGridLine>& OutLines)
	{
		OutLines.Reset();
		if (!FMath::IsFinite(CellCm) || !FMath::IsFinite(HalfExtentCm) || !FMath::IsFinite(PlaneOffset) || CellCm <= 0.0 || HalfExtentCm <= 0.0)
		{
			return;
		}

		const int32 MaxHalfCount = (MaxGridLinesPerDirection - 1) / 2;
		const int32 HalfCount = static_cast<int32>(FMath::Min<double>(FMath::FloorToDouble(HalfExtentCm / CellCm), MaxHalfCount));
		const double Extent = HalfCount * CellCm;
		if (HalfCount <= 0)
		{
			return;
		}

		EGridLineKind AlongFirst;
		EGridLineKind AlongSecond;
		GetPlaneAxes(Plane, AlongFirst, AlongSecond);

		OutLines.Reserve(2 * (2 * HalfCount + 1));
		for (int32 Index = -HalfCount; Index <= HalfCount; ++Index)
		{
			const double At = Index * CellCm;
			const EGridLineKind Regular = MajorEvery > 0 && Index % MajorEvery == 0 ? EGridLineKind::Major : EGridLineKind::Minor;

			// A line along the first axis at Second == At, and one along the second axis at First == At.
			OutLines.Add({ PlanePoint(Plane, PlaneOffset, -Extent, At), PlanePoint(Plane, PlaneOffset, Extent, At), Index == 0 ? AlongFirst : Regular });
			OutLines.Add({ PlanePoint(Plane, PlaneOffset, At, -Extent), PlanePoint(Plane, PlaneOffset, At, Extent), Index == 0 ? AlongSecond : Regular });
		}
	}

	TConstArrayView<float> GetGridSnapPresets()
	{
		return GridSnapPresets;
	}

	TConstArrayView<float> GetAngleSnapPresets()
	{
		return AngleSnapPresets;
	}

	bool IsGridSnapPreset(float Cm)
	{
		return IsPreset(GridSnapPresets, Cm);
	}

	bool IsAngleSnapPreset(float Degrees)
	{
		return IsPreset(AngleSnapPresets, Degrees);
	}

	float SanitizeGridSnap(float Cm)
	{
		return IsGridSnapPreset(Cm) ? Cm : DefaultGridSnapCm;
	}

	float SanitizeAngleSnap(float Degrees)
	{
		return IsAngleSnapPreset(Degrees) ? Degrees : DefaultAngleSnapDegrees;
	}

	float ResolveSnapPick(float Current, float Picked, float LastOn, float Fallback)
	{
		constexpr float Tolerance = 1.e-3f;
		const bool bPickedOff = FMath::IsNearlyZero(Picked, Tolerance);
		if (!bPickedOff)
		{
			return FMath::IsNearlyEqual(Current, Picked, Tolerance) ? 0.f : Picked;
		}
		if (!FMath::IsNearlyZero(Current, Tolerance))
		{
			return 0.f;
		}
		return FMath::IsFinite(LastOn) && LastOn > Tolerance ? LastOn : Fallback;
	}

	bool IsViewModeAvailable(EStageViewMode Mode)
	{
		switch (Mode)
		{
		case EStageViewMode::LightingOnly:
		case EStageViewMode::DetailLighting:
		case EStageViewMode::Reflections:
			return !UE_BUILD_OPTIMIZED_SHOWFLAGS;

		case EStageViewMode::Lit:
		case EStageViewMode::Unlit:
		case EStageViewMode::Lines:
			return true;

		default:
			return false;
		}
	}
}
