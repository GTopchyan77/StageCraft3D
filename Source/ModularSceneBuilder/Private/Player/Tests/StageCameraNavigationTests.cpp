// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the Unreal Editor camera rules (Docs/ADR/0018 §2): which drag each button combination selects, camera speed steps
// and limits, drag, wheel and orbit maths, and framing a selection with F.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Camera; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Player/StageCameraNavigation.h"

#include <limits>

namespace StageCameraNavigationTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	FStageCameraButtons Buttons(bool bLeft, bool bMiddle, bool bRight, bool bAlt = false)
	{
		FStageCameraButtons Result;
		Result.bLeft = bLeft;
		Result.bMiddle = bMiddle;
		Result.bRight = bRight;
		Result.bAlt = bAlt;
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageCameraDragTest, "StageCraft.Camera.Drags", StageCameraNavigationTests::Flags)

bool FStageCameraDragTest::RunTest(const FString& Parameters)
{
	using namespace StageCameraNavigationTests;
	using StageCameraNavigation::ClassifyDrag;

	TestTrue(TEXT("nothing"), ClassifyDrag(Buttons(false, false, false)) == EStageCameraDrag::None);
	TestTrue(TEXT("Alt alone"), ClassifyDrag(Buttons(false, false, false, true)) == EStageCameraDrag::None);
	TestTrue(TEXT("LMB flies and turns"), ClassifyDrag(Buttons(true, false, false)) == EStageCameraDrag::FlyAndTurn);
	TestTrue(TEXT("RMB looks"), ClassifyDrag(Buttons(false, false, true)) == EStageCameraDrag::Look);
	TestTrue(TEXT("MMB pans"), ClassifyDrag(Buttons(false, true, false)) == EStageCameraDrag::Pan);
	TestTrue(TEXT("LMB + RMB pans vertically"), ClassifyDrag(Buttons(true, false, true)) == EStageCameraDrag::PanVertical);
	TestTrue(TEXT("Alt + LMB orbits"), ClassifyDrag(Buttons(true, false, false, true)) == EStageCameraDrag::Orbit);
	TestTrue(TEXT("Alt + RMB dollies"), ClassifyDrag(Buttons(false, false, true, true)) == EStageCameraDrag::OrbitDolly);
	TestTrue(TEXT("Alt + MMB pans the orbit"), ClassifyDrag(Buttons(false, true, false, true)) == EStageCameraDrag::OrbitPan);
	TestTrue(TEXT("Alt + LMB + RMB dollies"), ClassifyDrag(Buttons(true, false, true, true)) == EStageCameraDrag::OrbitDolly);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageCameraMathTest, "StageCraft.Camera.Math", StageCameraNavigationTests::Flags)

bool FStageCameraMathTest::RunTest(const FString& Parameters)
{
	using namespace StageCameraNavigation;

	// Speed: 10 % per notch, clamped, NaN is the default.
	TestTrue(TEXT("one notch up"), FMath::IsNearlyEqual(StepCameraSpeed(1.f, 1.f), 1.1f));
	TestTrue(TEXT("one notch down"), FMath::IsNearlyEqual(StepCameraSpeed(1.f, -1.f), 0.9f));
	TestTrue(TEXT("two notches compound"), FMath::IsNearlyEqual(StepCameraSpeed(1.f, 2.f), 1.21f, 1e-4f));
	TestTrue(TEXT("clamped high"), StepCameraSpeed(MaxCameraSpeed, 50.f) == MaxCameraSpeed);
	TestTrue(TEXT("clamped low"), StepCameraSpeed(MinCameraSpeed, -50.f) == MinCameraSpeed);
	TestEqual(TEXT("NaN speed"), SanitizeCameraSpeed(std::numeric_limits<float>::quiet_NaN()), DefaultCameraSpeed);
	TestEqual(TEXT("NaN notches keep the speed"), StepCameraSpeed(2.f, std::numeric_limits<float>::quiet_NaN()), 2.f);
	TestTrue(TEXT("fly speed 2000 cm/s at 1"), FMath::IsNearlyEqual(GetFlySpeed(1.f), 2000.f));

	// LMB drag: forward along the yaw only, turning 0.2 degrees per count.
	const FVector Forward = GetFlyAndTurnTranslation(FVector2D(0.0, 10.0), 90.0, 1.f);
	TestTrue(TEXT("forward at yaw 90 is +Y"), Forward.Equals(FVector(0.0, 10.0, 0.0), 1e-6));
	TestTrue(TEXT("turn 0.2 per count"), FMath::IsNearlyEqual(GetFlyAndTurnYaw(FVector2D(10.0, 0.0)), 2.0, 1e-5));

	// MMB: in the view plane; LMB + RMB: sideways and world up.
	const FVector Pan = GetPanTranslation(FVector2D(10.0, 5.0), FRotator::ZeroRotator, 2.f);
	TestTrue(TEXT("pan right and up, scaled by speed"), Pan.Equals(FVector(0.0, 20.0, 10.0), 1e-6));
	const FVector Vertical = GetVerticalPanTranslation(FVector2D(0.0, 10.0), 45.0, 1.f);
	TestTrue(TEXT("vertical pan is world Z"), Vertical.Equals(FVector(0.0, 0.0, 10.0), 1e-6));

	// Orbit: yaw against the mouse, distance kept, never closer than the minimum.
	const FRotator Orbited = GetOrbitRotation(FRotator(-20.0, 0.0, 0.0), FVector2D(10.0, 5.0));
	TestTrue(TEXT("orbit yaw"), FMath::IsNearlyEqual(Orbited.Yaw, -2.0, 1e-5) && FMath::IsNearlyEqual(Orbited.Pitch, -19.0, 1e-5));
	const FVector Pivot(100.0, 0.0, 0.0);
	TestTrue(TEXT("orbit distance"), FMath::IsNearlyEqual(FVector::Dist(GetOrbitLocation(Pivot, Orbited, 500.0), Pivot), 500.0, 1e-6));
	TestTrue(TEXT("dolly closer"), FMath::IsNearlyEqual(GetDollyDistance(500.0, FVector2D(10.0, 10.0), 1.f), 480.0));
	TestTrue(TEXT("dolly floor"), GetDollyDistance(20.0, FVector2D(1000.0, 0.0), 1.f) == MinOrbitDistance);

	// Wheel: 96 cm per notch along the view, whatever the speed.
	TestTrue(TEXT("wheel dolly"), GetWheelDolly(FRotator::ZeroRotator, 2.f).Equals(FVector(192.0, 0.0, 0.0), 1e-6));
	TestTrue(TEXT("wheel NaN"), GetWheelDolly(FRotator::ZeroRotator, std::numeric_limits<float>::quiet_NaN()).IsZero());

	// F: the bounding sphere fills a 90 degree view, at radius / tan(45) back from the centre.
	const FBox Box(FVector(-100.0), FVector(100.0));
	const FVector Focus = GetFocusLocation(Box, FRotator::ZeroRotator, 90.f, 1.f, FVector::ZeroVector);
	TestTrue(TEXT("focus distance"), FMath::IsNearlyEqual(-Focus.X, Box.GetExtent().Size(), 1e-3) && FMath::IsNearlyZero(Focus.Y) && FMath::IsNearlyZero(Focus.Z));
	TestTrue(TEXT("wide screens step back further"), -GetFocusLocation(Box, FRotator::ZeroRotator, 90.f, 2.f, FVector::ZeroVector).X > -Focus.X);
	const FVector Tiny = GetFocusLocation(FBox(FVector::ZeroVector, FVector::ZeroVector), FRotator::ZeroRotator, 90.f, 1.f, FVector::ZeroVector);
	TestTrue(TEXT("a point frames at the minimum radius"), FMath::IsNearlyEqual(-Tiny.X, MinimumFocusRadius, 1e-3));
	TestTrue(TEXT("invalid box keeps the camera"), GetFocusLocation(FBox(ForceInit), FRotator::ZeroRotator, 90.f, 1.f, FVector(5.0)).Equals(FVector(5.0)));

	// The glide: cubic out, clamped.
	TestEqual(TEXT("ease start"), EaseFocus(0.f), 0.f);
	TestEqual(TEXT("ease end"), EaseFocus(1.f), 1.f);
	TestTrue(TEXT("ease is front-loaded"), EaseFocus(0.5f) > 0.5f);
	TestEqual(TEXT("ease clamped"), EaseFocus(2.f), 1.f);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
