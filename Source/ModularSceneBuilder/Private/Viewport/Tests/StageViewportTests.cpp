// Copyright Epic Games, Inc. All Rights Reserved.

// Unit tests for the pure viewport rules (Docs/ADR/0008-viewport-toolbar-projections-and-view-modes.md): projection
// orientations, orthographic fit / zoom / pan, screen-scale distance, grid lines, snap presets and the global placement grid.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Placement/StagePlacementMath.h"
#include "ShowFlags.h"
#include "Viewport/StageViewMath.h"

#include <limits>

namespace StageViewportTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageViewProjectionTest, "StageCraft.Viewport.Projection", StageViewportTests::Flags)

bool FStageViewProjectionTest::RunTest(const FString& Parameters)
{
	using namespace StageViewMath;

	TestFalse(TEXT("Perspective is not orthographic"), IsOrthographic(EStageViewProjection::Perspective));
	TestTrue(TEXT("Top is orthographic"), IsOrthographic(EStageViewProjection::Top));

	// Unreal Editor orientations: forward, screen right and screen up of each view.
	struct FExpected { EStageViewProjection Projection; FVector Forward; FVector Right; FVector Up; EGridPlane Plane; };
	const FExpected Cases[] = {
		{ EStageViewProjection::Top,	FVector(0, 0, -1),	FVector(0, 1, 0),	FVector(1, 0, 0),	EGridPlane::XY },
		{ EStageViewProjection::Front,	FVector(0, -1, 0),	FVector(1, 0, 0),	FVector(0, 0, 1),	EGridPlane::XZ },
		{ EStageViewProjection::Back,	FVector(0, 1, 0),	FVector(-1, 0, 0),	FVector(0, 0, 1),	EGridPlane::XZ },
		{ EStageViewProjection::Left,	FVector(1, 0, 0),	FVector(0, 1, 0),	FVector(0, 0, 1),	EGridPlane::YZ },
		{ EStageViewProjection::Right,	FVector(-1, 0, 0),	FVector(0, -1, 0),	FVector(0, 0, 1),	EGridPlane::YZ },
	};
	for (const FExpected& Case : Cases)
	{
		const FString Name = UEnum::GetValueAsString(Case.Projection);
		const FRotationMatrix Axes(GetProjectionRotation(Case.Projection));
		TestTrue(Name + TEXT(" forward"), Axes.GetUnitAxis(EAxis::X).Equals(Case.Forward, 1.e-6));
		TestTrue(Name + TEXT(" screen right"), Axes.GetUnitAxis(EAxis::Y).Equals(Case.Right, 1.e-6));
		TestTrue(Name + TEXT(" screen up"), Axes.GetUnitAxis(EAxis::Z).Equals(Case.Up, 1.e-6));
		TestEqual(Name + TEXT(" grid plane"), GetGridPlane(Case.Projection), Case.Plane);
	}
	TestEqual(TEXT("Perspective draws the floor grid"), GetGridPlane(EStageViewProjection::Perspective), EGridPlane::XY);

	// The floor stays at 0; a vertical grid stands beyond the origin as seen from the camera.
	TestEqual(TEXT("Top grid on the floor"), GetGridPlaneOffset(EStageViewProjection::Top, 5000.0), 0.0);
	TestEqual(TEXT("Front (looking -Y) grid behind at -Y"), GetGridPlaneOffset(EStageViewProjection::Front, 5000.0), -5000.0);
	TestEqual(TEXT("Back (looking +Y) grid behind at +Y"), GetGridPlaneOffset(EStageViewProjection::Back, 5000.0), 5000.0);
	TestEqual(TEXT("Left (looking +X) grid behind at +X"), GetGridPlaneOffset(EStageViewProjection::Left, 5000.0), 5000.0);
	TestEqual(TEXT("Right (looking -X) grid behind at -X"), GetGridPlaneOffset(EStageViewProjection::Right, 5000.0), -5000.0);
	TestEqual(TEXT("Negative back distance clamps to 0"), GetGridPlaneOffset(EStageViewProjection::Front, -10.0), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageViewOrthoMathTest, "StageCraft.Viewport.OrthoMath", StageViewportTests::Flags)

bool FStageViewOrthoMathTest::RunTest(const FString& Parameters)
{
	using namespace StageViewMath;

	// Fit: what a 90 degree perspective view shows at 10 m is 20 m across.
	TestTrue(TEXT("Fit 90 deg at 1000 cm"), FMath::IsNearlyEqual(FitOrthoWidth(1000.0, 90.0), 2000.0, 1.e-6));
	TestEqual(TEXT("Fit clamps to the minimum"), FitOrthoWidth(10.0, 90.0), MinOrthoWidth);
	TestEqual(TEXT("Fit clamps to the maximum"), FitOrthoWidth(1.e7, 90.0), MaxOrthoWidth);
	TestEqual(TEXT("Fit with NaN distance"), FitOrthoWidth(std::numeric_limits<double>::quiet_NaN(), 90.0), MinOrthoWidth);
	TestEqual(TEXT("Fit with a 180 deg FOV"), FitOrthoWidth(1000.0, 180.0), MinOrthoWidth);
	TestEqual(TEXT("Clamp NaN width"), ClampOrthoWidth(std::numeric_limits<double>::quiet_NaN()), MinOrthoWidth);

	// Wheel steps: in narrows by 1.25, out widens by 1.25, limits hold.
	TestTrue(TEXT("One notch in"), FMath::IsNearlyEqual(StepOrthoWidth(2000.0, 1.0), 1600.0, 1.e-6));
	TestTrue(TEXT("One notch out"), FMath::IsNearlyEqual(StepOrthoWidth(2000.0, -1.0), 2500.0, 1.e-6));
	TestEqual(TEXT("Zoom in stops at the minimum"), StepOrthoWidth(MinOrthoWidth, 5.0), MinOrthoWidth);
	TestEqual(TEXT("Zoom out stops at the maximum"), StepOrthoWidth(MaxOrthoWidth, -5.0), MaxOrthoWidth);
	TestEqual(TEXT("NaN steps keep the width"), StepOrthoWidth(2000.0, std::numeric_limits<double>::quiet_NaN()), 2000.0);

	// Zoom anchor: the world point under the cursor stays put, wherever the cursor is. On-screen offsets scale with the
	// width whatever axis it spans, so a point at Offset from the centre shows at Offset * New / Old after the zoom.
	for (const FVector2D& Offset : { FVector2D(0.0, 0.0), FVector2D(1600.0, 900.0), FVector2D(-700.0, 120.0), FVector2D(450.0, -880.0) })
	{
		const FVector2D Center(300.0, -120.0);
		const double OldWidth = 4000.0;
		const double NewWidth = StepOrthoWidth(OldWidth, 3.0);
		const FVector2D Shift = ComputeOrthoZoomShift(Offset, OldWidth, NewWidth);
		const FVector2D Before = Center + Offset;
		const FVector2D After = Center + Shift + Offset * (NewWidth / OldWidth);
		TestTrue(FString::Printf(TEXT("Anchor fixed at offset (%.0f, %.0f)"), Offset.X, Offset.Y), Before.Equals(After, 1.e-6));
	}
	TestTrue(TEXT("Centre zoom does not shift"), ComputeOrthoZoomShift(FVector2D::ZeroVector, 4000.0, 2000.0).IsZero());
	TestTrue(TEXT("Zero old width gives no shift"), ComputeOrthoZoomShift(FVector2D(300.0, 300.0), 0.0, 2000.0).IsZero());
	TestTrue(TEXT("NaN offset gives no shift"), ComputeOrthoZoomShift(FVector2D(std::numeric_limits<double>::quiet_NaN(), 0.0), 4000.0, 2000.0).IsZero());

	// Grab-pan: the scene follows the mouse, 1:1 in measured pixels (here 2 cm wide, 3 cm tall).
	const FVector2D Pan = ComputePanShift(FVector2D(2.0, 3.0), FVector2D(10.0, -5.0));
	TestTrue(TEXT("Pan moves the camera against the mouse"), Pan.Equals(FVector2D(-20.0, 15.0), 1.e-9));
	TestTrue(TEXT("Pan with a negative pixel size"), ComputePanShift(FVector2D(-1.0, 1.0), FVector2D(10.0, 0.0)).IsZero());

	// Screen-size distance: the camera distance in perspective, width / (2 tan(FOV / 2)) in ortho.
	TestTrue(TEXT("Perspective uses the camera distance"),
		FMath::IsNearlyEqual(GetScreenScaleDistance(false, 0.0, 90.0, FVector(0, 0, 0), FVector(300, 400, 0)), 500.0, 1.e-9));
	TestTrue(TEXT("Ortho 2000 cm at 90 deg reads as 1000 cm"),
		FMath::IsNearlyEqual(GetScreenScaleDistance(true, 2000.0, 90.0, FVector(0, 0, 50000), FVector::ZeroVector), 1000.0, 1.e-6));
	TestTrue(TEXT("Ortho ignores the camera distance"),
		FMath::IsNearlyEqual(GetScreenScaleDistance(true, 2000.0, 90.0, FVector(0, 0, 50000), FVector::ZeroVector),
			GetScreenScaleDistance(true, 2000.0, 90.0, FVector(0, 0, 900), FVector::ZeroVector), 1.e-9));
	TestTrue(TEXT("Ortho and fit agree"), FMath::IsNearlyEqual(GetScreenScaleDistance(true, FitOrthoWidth(1500.0, 70.0), 70.0, FVector::ZeroVector, FVector::ZeroVector), 1500.0, 1.e-6));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageViewGridTest, "StageCraft.Viewport.Grid", StageViewportTests::Flags)

bool FStageViewGridTest::RunTest(const FString& Parameters)
{
	using namespace StageViewMath;
	TArray<FGridLine> Lines;

	// 1 m cells out to 5 m: 11 lines each way.
	BuildGridLines(EGridPlane::XY, 0.0, 100.0, 500.0, 10, Lines);
	TestEqual(TEXT("11 + 11 lines"), Lines.Num(), 22);
	int32 AxisX = 0, AxisY = 0, Majors = 0;
	for (const FGridLine& Line : Lines)
	{
		AxisX += Line.Kind == EGridLineKind::AxisX;
		AxisY += Line.Kind == EGridLineKind::AxisY;
		Majors += Line.Kind == EGridLineKind::Major;
		TestEqual(TEXT("Floor lines lie at Z = 0"), Line.Start.Z, 0.0);
	}
	TestEqual(TEXT("One X axis line"), AxisX, 1);
	TestEqual(TEXT("One Y axis line"), AxisY, 1);
	TestEqual(TEXT("No major lines inside 5 cells (index 0 is the axis)"), Majors, 0);
	const FGridLine* XAxis = Lines.FindByPredicate([](const FGridLine& Line) { return Line.Kind == EGridLineKind::AxisX; });
	TestTrue(TEXT("The X axis line runs along X through the origin"), XAxis && XAxis->Start.Equals(FVector(-500, 0, 0)) && XAxis->End.Equals(FVector(500, 0, 0)));

	// Majors every 10 cells.
	BuildGridLines(EGridPlane::XY, 0.0, 10.0, 250.0, 10, Lines);
	Majors = 0;
	for (const FGridLine& Line : Lines)
	{
		Majors += Line.Kind == EGridLineKind::Major;
	}
	TestEqual(TEXT("Major lines at +-100 and +-200 cm, both directions"), Majors, 8);

	// A front view's grid stands in its plane, with the X and Z axes.
	BuildGridLines(EGridPlane::XZ, -5000.0, 100.0, 300.0, 10, Lines);
	bool bAllInPlane = true;
	int32 AxisZ = 0;
	AxisX = 0;
	for (const FGridLine& Line : Lines)
	{
		bAllInPlane &= Line.Start.Y == -5000.0 && Line.End.Y == -5000.0;
		AxisX += Line.Kind == EGridLineKind::AxisX;
		AxisZ += Line.Kind == EGridLineKind::AxisZ;
	}
	TestTrue(TEXT("XZ lines lie in Y = offset"), bAllInPlane);
	TestTrue(TEXT("XZ grid has X and Z axes"), AxisX == 1 && AxisZ == 1);

	BuildGridLines(EGridPlane::YZ, 2000.0, 100.0, 300.0, 10, Lines);
	TestTrue(TEXT("YZ lines lie in X = offset"), Lines.Num() > 0 && Lines[0].Start.X == 2000.0);

	// The line cap shrinks the extent instead of exploding.
	BuildGridLines(EGridPlane::XY, 0.0, 1.0, 5000.0, 10, Lines);
	TestEqual(TEXT("Capped at 1001 lines per direction"), Lines.Num(), 2 * MaxGridLinesPerDirection);

	// Invalid input draws nothing.
	BuildGridLines(EGridPlane::XY, 0.0, 0.0, 500.0, 10, Lines);
	TestEqual(TEXT("Zero cell"), Lines.Num(), 0);
	BuildGridLines(EGridPlane::XY, 0.0, -10.0, 500.0, 10, Lines);
	TestEqual(TEXT("Negative cell"), Lines.Num(), 0);
	BuildGridLines(EGridPlane::XY, 0.0, std::numeric_limits<double>::quiet_NaN(), 500.0, 10, Lines);
	TestEqual(TEXT("NaN cell"), Lines.Num(), 0);
	BuildGridLines(EGridPlane::XY, 0.0, 100.0, 50.0, 10, Lines);
	TestEqual(TEXT("Extent smaller than a cell"), Lines.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageViewSnapTest, "StageCraft.Viewport.Snap", StageViewportTests::Flags)

bool FStageViewSnapTest::RunTest(const FString& Parameters)
{
	using namespace StageViewMath;

	TestEqual(TEXT("Grid presets"), GetGridSnapPresets().Num(), 4);
	TestEqual(TEXT("Angle presets"), GetAngleSnapPresets().Num(), 4);
	TestTrue(TEXT("Off is a grid preset"), IsGridSnapPreset(0.f));
	TestTrue(TEXT("50 cm is a grid preset"), IsGridSnapPreset(50.f));
	TestFalse(TEXT("25 cm is not"), IsGridSnapPreset(25.f));
	TestFalse(TEXT("NaN is not"), IsGridSnapPreset(std::numeric_limits<float>::quiet_NaN()));
	TestTrue(TEXT("15 deg is an angle preset"), IsAngleSnapPreset(15.f));
	TestFalse(TEXT("90 deg is not"), IsAngleSnapPreset(90.f));
	TestEqual(TEXT("A hand-edited grid falls back to the default"), SanitizeGridSnap(33.f), DefaultGridSnapCm);
	TestEqual(TEXT("A valid grid is kept"), SanitizeGridSnap(10.f), 10.f);
	TestEqual(TEXT("A hand-edited angle falls back to the default"), SanitizeAngleSnap(-5.f), DefaultAngleSnapDegrees);
	TestEqual(TEXT("Off is kept"), SanitizeAngleSnap(0.f), 0.f);
	TestTrue(TEXT("Defaults are presets"), IsGridSnapPreset(DefaultGridSnapCm) && IsAngleSnapPreset(DefaultAngleSnapDegrees));

	// Toolbar clicks toggle: the active step turns off, Off while off restores the last step (or the fallback).
	TestEqual(TEXT("Picking another step selects it"), ResolveSnapPick(50.f, 10.f, 0.f, 50.f), 10.f);
	TestEqual(TEXT("Picking the active step turns snap off"), ResolveSnapPick(50.f, 50.f, 0.f, 50.f), 0.f);
	TestEqual(TEXT("Picking Off while on turns snap off"), ResolveSnapPick(100.f, 0.f, 10.f, 50.f), 0.f);
	TestEqual(TEXT("Picking Off while off restores the last step"), ResolveSnapPick(0.f, 0.f, 100.f, 50.f), 100.f);
	TestEqual(TEXT("Picking Off while off without a last step uses the fallback"), ResolveSnapPick(0.f, 0.f, 0.f, 15.f), 15.f);
	TestEqual(TEXT("A NaN last step uses the fallback"), ResolveSnapPick(0.f, 0.f, std::numeric_limits<float>::quiet_NaN(), 15.f), 15.f);
	TestEqual(TEXT("Picking a step while off selects it"), ResolveSnapPick(0.f, 45.f, 5.f, 15.f), 45.f);
	float Value = 50.f;
	float LastOn = 0.f;
	for (int32 Click = 0; Click < 4; ++Click)
	{
		// Clicking the same step repeatedly alternates on / off and never sticks.
		const float Next = ResolveSnapPick(Value, 50.f, LastOn, 50.f);
		LastOn = Value > 0.f ? Value : LastOn;
		Value = Next;
		TestEqual(*FString::Printf(TEXT("Repeated click %d alternates"), Click), Value, Click % 2 == 0 ? 0.f : 50.f);
	}

	// Everyday modes always; the engine lighting diagnostics only where their show flags exist.
	TestTrue(TEXT("Lit is available"), IsViewModeAvailable(EStageViewMode::Lit));
	TestTrue(TEXT("Unlit is available"), IsViewModeAvailable(EStageViewMode::Unlit));
	TestTrue(TEXT("Lines is available"), IsViewModeAvailable(EStageViewMode::Lines));
	for (const EStageViewMode Mode : { EStageViewMode::LightingOnly, EStageViewMode::DetailLighting, EStageViewMode::Reflections })
	{
		TestEqual(*FString::Printf(TEXT("%s follows the build's show flags"), *UEnum::GetValueAsString(Mode)), IsViewModeAvailable(Mode), !UE_BUILD_OPTIMIZED_SHOWFLAGS);
	}

	// The global grid replaces the item's X/Y grid and keeps everything else.
	FStageItemPlacementRules Rules;
	Rules.GridSize = FVector(100.0, 100.0, 25.0);
	Rules.PlacementOffset = FVector(0, 0, 50);
	Rules.bAlignToSurfaceNormal = true;
	const FStageItemPlacementRules Global = StagePlacementMath::WithGlobalGrid(Rules, 50.0);
	TestTrue(TEXT("X/Y take the global grid, Z is kept"), Global.GridSize.Equals(FVector(50.0, 50.0, 25.0)));
	TestTrue(TEXT("Offset and alignment are kept"), Global.PlacementOffset.Equals(Rules.PlacementOffset) && Global.bAlignToSurfaceNormal);
	TestTrue(TEXT("Off frees X/Y"), StagePlacementMath::WithGlobalGrid(Rules, 0.0).GridSize.Equals(FVector(0.0, 0.0, 25.0)));
	TestTrue(TEXT("NaN counts as Off"), StagePlacementMath::WithGlobalGrid(Rules, std::numeric_limits<double>::quiet_NaN()).GridSize.Equals(FVector(0.0, 0.0, 25.0)));

	FStageItemPlacementRules Flat;
	Flat.GridSize = FVector(100.0, 100.0, 0.0);
	const FTransform Landing = StagePlacementMath::ComputePlacementTransform(StagePlacementMath::WithGlobalGrid(Flat, 50.0), FVector(137.0, -61.0, 12.0), FVector::UpVector);
	TestTrue(TEXT("137 / -61 land on the 50 cm grid"), Landing.GetLocation().Equals(FVector(150.0, -50.0, 12.0)));
	const FTransform Free = StagePlacementMath::ComputePlacementTransform(StagePlacementMath::WithGlobalGrid(Flat, 0.0), FVector(137.0, -61.0, 12.0), FVector::UpVector);
	TestTrue(TEXT("Off lands where the cursor is"), Free.GetLocation().Equals(FVector(137.0, -61.0, 12.0)));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
