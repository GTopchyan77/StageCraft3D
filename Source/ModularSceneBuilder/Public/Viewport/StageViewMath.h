// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Viewport/StageViewportTypes.h"

/**
 * Pure rules of the stage viewport (Docs/ADR/0008-viewport-toolbar-projections-and-view-modes.md): projection orientations,
 * orthographic fit / zoom / pan, the screen-size distance used by gizmos in either projection, the grid's line set and the
 * snap presets. No world access; covered by the StageCraft.Viewport.* tests.
 *
 * Screen-space conventions: a cursor offset from the viewport centre is in [-0.5, 0.5] on both axes, X right and Y up.
 * View-plane shifts are (right, up) in world units along the view's right and up axes.
 */
namespace StageViewMath
{
	/** Narrowest and widest orthographic view, in cm (1 m to 500 m across). */
	constexpr double MinOrthoWidth = 100.0;
	constexpr double MaxOrthoWidth = 50000.0;

	/** Width multiplier per wheel notch; a notch towards the screen narrows the view. */
	constexpr double OrthoZoomStep = 1.25;

	/** Defaults of the toolbar's snaps: 50 cm is the grid trusses and spans used before the grid became global. */
	constexpr float DefaultGridSnapCm = 50.f;
	constexpr float DefaultAngleSnapDegrees = 15.f;

	/** The plane a grid is drawn in. */
	enum class EGridPlane : uint8
	{
		XY,
		XZ,
		YZ,
	};

	enum class EGridLineKind : uint8
	{
		Minor,
		/** Every MajorEvery-th line. */
		Major,
		/** The world axis lying in the grid plane through the origin. */
		AxisX,
		AxisY,
		AxisZ,
	};

	struct FGridLine
	{
		FVector Start = FVector::ZeroVector;
		FVector End = FVector::ZeroVector;
		EGridLineKind Kind = EGridLineKind::Minor;
	};

	/** At most this many lines per direction, so a fine grid over a large extent stays bounded (it shrinks the extent instead). */
	constexpr int32 MaxGridLinesPerDirection = 1001;

	MODULARSCENEBUILDER_API bool IsOrthographic(EStageViewProjection Projection);

	/** The fixed view rotation of an orthographic projection (Unreal Editor orientations). Perspective returns ZeroRotator. */
	MODULARSCENEBUILDER_API FRotator GetProjectionRotation(EStageViewProjection Projection);

	/** The plane the grid lies in for a projection: the floor for Perspective and Top, the vertical plane facing the others. */
	MODULARSCENEBUILDER_API EGridPlane GetGridPlane(EStageViewProjection Projection);

	/**
	 * Where along its normal the grid plane sits. The floor is at 0. A vertical plane sits BackDistance beyond the origin as
	 * seen from the camera, so the layout stays in front of it.
	 */
	MODULARSCENEBUILDER_API double GetGridPlaneOffset(EStageViewProjection Projection, double BackDistance);

	/** Orthographic width that shows what a perspective view of FOVDegrees shows at Distance. Clamped; bad input gives the minimum. */
	MODULARSCENEBUILDER_API double FitOrthoWidth(double Distance, double FOVDegrees);

	MODULARSCENEBUILDER_API double ClampOrthoWidth(double Width);

	/** Width after Steps wheel notches (positive = zoom in). Clamped. */
	MODULARSCENEBUILDER_API double StepOrthoWidth(double Width, double Steps);

	/**
	 * Camera shift (right, up) that keeps the world point under the cursor fixed while the width changes from OldWidth to
	 * NewWidth. CursorOffset is that point's offset from the view centre in world units (right, up), measured on screen before
	 * the zoom, so the result holds whichever axis the engine maps OrthoWidth to (ULocalPlayer::AspectRatioAxisConstraint;
	 * the engine default maps it to the height). Zero for bad input.
	 */
	MODULARSCENEBUILDER_API FVector2D ComputeOrthoZoomShift(const FVector2D& CursorOffset, double OldWidth, double NewWidth);

	/**
	 * Camera shift (right, up) for a grab-pan: the scene follows a mouse delta of MouseDelta pixels (X right, Y up).
	 * WorldPerPixel is the measured size of one screen pixel in world units (right, up). Zero for bad input.
	 */
	MODULARSCENEBUILDER_API FVector2D ComputePanShift(const FVector2D& WorldPerPixel, const FVector2D& MouseDelta);

	/**
	 * Distance at which a perspective view would show what this view shows at At: the camera distance in perspective, and
	 * OrthoWidth / (2 tan(FOV / 2)) in orthographic, where distance does not change on-screen size. Gizmos and guides scale by it.
	 */
	MODULARSCENEBUILDER_API double GetScreenScaleDistance(bool bOrthographic, double OrthoWidth, double FOVDegrees, const FVector& ViewLocation, const FVector& At);

	/** The same for a camera's view (APlayerCameraManager::GetCameraCacheView). */
	MODULARSCENEBUILDER_API double GetScreenScaleDistance(const struct FMinimalViewInfo& View, const FVector& At);

	/**
	 * Grid lines in Plane at PlaneOffset, with cells of CellCm, out to HalfExtentCm on both sides of the origin (fewer when
	 * MaxGridLinesPerDirection would be exceeded). The origin lines are axis lines. MajorEvery <= 0 means no major lines.
	 * OutLines is reset; it stays empty for a non-positive or non-finite cell or extent.
	 */
	MODULARSCENEBUILDER_API void BuildGridLines(EGridPlane Plane, double PlaneOffset, double CellCm, double HalfExtentCm, int32 MajorEvery, TArray<FGridLine>& OutLines);

	/** Toolbar choices: Off (0), 10, 50, 100 cm. */
	MODULARSCENEBUILDER_API TConstArrayView<float> GetGridSnapPresets();

	/** Toolbar choices: Off (0), 5, 15, 45 degrees. */
	MODULARSCENEBUILDER_API TConstArrayView<float> GetAngleSnapPresets();

	/** Value if it is one of the presets, else the default. Saved settings and console input pass through these. */
	MODULARSCENEBUILDER_API float SanitizeGridSnap(float Cm);
	MODULARSCENEBUILDER_API float SanitizeAngleSnap(float Degrees);

	MODULARSCENEBUILDER_API bool IsGridSnapPreset(float Cm);
	MODULARSCENEBUILDER_API bool IsAngleSnapPreset(float Degrees);
}
