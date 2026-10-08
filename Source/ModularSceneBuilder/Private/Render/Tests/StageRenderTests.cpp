// Copyright Epic Games, Inc. All Rights Reserved.

// Unit tests for the pure rules of selection group moves and still renders (Docs/ADR/0004-selection-scenes-and-rendering.md).
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Interaction/StageGroupTransform.h"
#include "Interaction/StageTransformRules.h"
#include "Misc/AutomationTest.h"
#include "Render/StageRenderTypes.h"

namespace StageRenderTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	FStageRenderSettings MakeSettings(EStageRenderResolution Resolution, EStageRenderAntiAliasing AntiAliasing, EStageRenderPostProcess PostProcess = EStageRenderPostProcess::Standard)
	{
		FStageRenderSettings Settings;
		Settings.Resolution = Resolution;
		Settings.AntiAliasing = AntiAliasing;
		Settings.PostProcess = PostProcess;
		return Settings;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageGroupTransformTest, "StageCraft.Selection.GroupTransform", StageRenderTests::Flags)

bool FStageGroupTransformTest::RunTest(const FString& Parameters)
{
	using StageGroupTransform::ApplyLeaderChange;

	const FTransform LeaderStart(FQuat::Identity, FVector(100, 0, 0), FVector::OneVector);
	const FTransform FollowerStart(FRotator(0, 10, 0).Quaternion(), FVector(200, 50, 0), FVector(1, 2, 1));

	// No change: the follower stays exactly where it was.
	const FTransform Unchanged = ApplyLeaderChange(LeaderStart, LeaderStart, FollowerStart);
	TestTrue(TEXT("Identity change leaves the follower"), Unchanged.Equals(FollowerStart, 1.e-6));

	// Move: the same world offset.
	const FTransform Moved = ApplyLeaderChange(LeaderStart, FTransform(FQuat::Identity, FVector(130, -20, 40)), FollowerStart);
	TestTrue(TEXT("Move offsets the follower by the leader's delta"), Moved.GetLocation().Equals(FVector(230, 30, 40), 1.e-6));
	TestTrue(TEXT("Move keeps the follower's rotation"), Moved.GetRotation().Equals(FollowerStart.GetRotation(), 1.e-6));
	TestTrue(TEXT("Move keeps the follower's scale"), Moved.GetScale3D().Equals(FollowerStart.GetScale3D(), 1.e-6));

	// Rotate 90 degrees about Z at the leader's pivot: the follower orbits (offset (100,50) -> (-50,100)) and turns with it.
	const FTransform Rotated = ApplyLeaderChange(LeaderStart, FTransform(FRotator(0, 90, 0).Quaternion(), LeaderStart.GetLocation()), FollowerStart);
	TestTrue(TEXT("Rotate orbits the follower around the leader's pivot"), Rotated.GetLocation().Equals(FVector(50, 100, 0), 1.e-4));
	TestTrue(TEXT("Rotate turns the follower by the same angle"), FMath::IsNearlyEqual(Rotated.Rotator().Yaw, 100.0, 1.e-3));
	TestTrue(TEXT("Rotation keeps the distance to the pivot"),
		FMath::IsNearlyEqual(FVector::Dist(Rotated.GetLocation(), LeaderStart.GetLocation()), FVector::Dist(FollowerStart.GetLocation(), LeaderStart.GetLocation()), 1.e-4));

	// Scale: per-axis ratio applied in place; the follower's position does not spread.
	const FTransform Scaled = ApplyLeaderChange(LeaderStart, FTransform(FQuat::Identity, LeaderStart.GetLocation(), FVector(2, 1, 0.5)), FollowerStart);
	TestTrue(TEXT("Scale multiplies the follower's scale by the leader's ratio"), Scaled.GetScale3D().Equals(FVector(2, 2, 0.5), 1.e-6));
	TestTrue(TEXT("Scale keeps the follower's location"), Scaled.GetLocation().Equals(FollowerStart.GetLocation(), 1.e-6));

	// Scale results go through the shared clamp.
	const FTransform BigFollower(FQuat::Identity, FVector::ZeroVector, FVector(80, 1, 1));
	const FTransform Clamped = ApplyLeaderChange(LeaderStart, FTransform(FQuat::Identity, LeaderStart.GetLocation(), FVector(2, 1, 1)), BigFollower);
	TestEqual(TEXT("Scale is clamped to the transform rules' maximum"), Clamped.GetScale3D().X, StageTransformRules::MaxScale);

	// A leader with a zero scale axis has no ratio on that axis: the follower keeps it.
	const FTransform FlatLeader(FQuat::Identity, LeaderStart.GetLocation(), FVector(1, 0, 1));
	const FTransform FromFlat = ApplyLeaderChange(FlatLeader, FTransform(FQuat::Identity, LeaderStart.GetLocation(), FVector(1, 5, 1)), FollowerStart);
	TestTrue(TEXT("Zero start scale axis leaves the follower's axis unchanged"), FMath::IsNearlyEqual(FromFlat.GetScale3D().Y, FollowerStart.GetScale3D().Y));

	// Results depend only on the start transforms: applying the same leader pose twice gives the same follower pose.
	const FTransform LeaderNow(FRotator(10, 45, 0).Quaternion(), FVector(10, 20, 30), FVector(1.5));
	TestTrue(TEXT("Deterministic: no accumulation across frames"),
		ApplyLeaderChange(LeaderStart, LeaderNow, FollowerStart).Equals(ApplyLeaderChange(LeaderStart, LeaderNow, FollowerStart), 0.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageRenderRulesTest, "StageCraft.Render.Rules", StageRenderTests::Flags)

bool FStageRenderRulesTest::RunTest(const FString& Parameters)
{
	using namespace StageRender;
	using namespace StageRenderTests;

	TestEqual(TEXT("4K preset"), GetOutputSize(EStageRenderResolution::UltraHD4K), FIntPoint(3840, 2160));
	TestEqual(TEXT("Full HD preset"), GetOutputSize(EStageRenderResolution::FullHD), FIntPoint(1920, 1080));
	TestEqual(TEXT("Square preset is 1:1"), GetOutputSize(EStageRenderResolution::Square), FIntPoint(2160, 2160));

	// Only Supersampled renders larger than the output.
	for (const EStageRenderAntiAliasing AA : { EStageRenderAntiAliasing::Off, EStageRenderAntiAliasing::FXAA, EStageRenderAntiAliasing::Temporal })
	{
		const FStageRenderSettings Settings = MakeSettings(EStageRenderResolution::UltraHD4K, AA);
		TestEqual(TEXT("No supersampling outside Supersampled"), GetSupersampleFactor(Settings), 1.0);
		TestEqual(TEXT("Internal size equals output size"), GetInternalSize(Settings), FIntPoint(3840, 2160));
	}

	const FStageRenderSettings HDSuper = MakeSettings(EStageRenderResolution::FullHD, EStageRenderAntiAliasing::Supersampled);
	TestEqual(TEXT("Full HD supersamples at 2x"), GetSupersampleFactor(HDSuper), 2.0);
	TestEqual(TEXT("Full HD supersampled internal size"), GetInternalSize(HDSuper), FIntPoint(3840, 2160));

	// 4K is already one budget's worth of pixels: Supersampled renders it at 1x (the GPU never renders more than a 4K frame).
	const FStageRenderSettings UHDSuper = MakeSettings(EStageRenderResolution::UltraHD4K, EStageRenderAntiAliasing::Supersampled);
	TestEqual(TEXT("4K is at the pixel budget: no extra supersampling"), GetSupersampleFactor(UHDSuper), 1.0);
	TestEqual(TEXT("4K supersampled internal size"), GetInternalSize(UHDSuper), FIntPoint(3840, 2160));

	const FStageRenderSettings SquareSuper = MakeSettings(EStageRenderResolution::Square, EStageRenderAntiAliasing::Supersampled);
	const double SquareFactor = GetSupersampleFactor(SquareSuper);
	TestTrue(TEXT("Square supersamples between 1x and 2x"), SquareFactor > 1.0 && SquareFactor < 2.0);
	const FIntPoint SquareInternal = GetInternalSize(SquareSuper);
	TestEqual(TEXT("Square supersampled internal size"), SquareInternal, FIntPoint(2880, 2880));
	TestTrue(TEXT("Square supersampled stays within the pixel budget"), double(SquareInternal.X) * SquareInternal.Y <= MaxSupersampledPixels * 1.001);
	TestTrue(TEXT("Internal sizes are even"), SquareInternal.X % 2 == 0 && SquareInternal.Y % 2 == 0);

	for (const EStageRenderResolution Resolution : { EStageRenderResolution::UltraHD4K, EStageRenderResolution::FullHD, EStageRenderResolution::Square })
	{
		const FIntPoint Internal = GetInternalSize(MakeSettings(Resolution, EStageRenderAntiAliasing::Supersampled));
		const FIntPoint Output = GetOutputSize(Resolution);
		TestTrue(TEXT("No preset renders more pixels than one 4K frame"), double(Internal.X) * Internal.Y <= MaxSupersampledPixels * 1.001);
		TestTrue(TEXT("Aspect ratio is kept"), FMath::IsNearlyEqual(double(Internal.X) / Internal.Y, double(Output.X) / Output.Y, 0.002));
	}

	TestEqual(TEXT("Non-temporal warm-up"), GetWarmupFrames(MakeSettings(EStageRenderResolution::FullHD, EStageRenderAntiAliasing::FXAA)), 4);
	TestEqual(TEXT("Temporal warm-up"), GetWarmupFrames(MakeSettings(EStageRenderResolution::FullHD, EStageRenderAntiAliasing::Temporal)), 16);
	TestEqual(TEXT("Cinematic doubles the warm-up"),
		GetWarmupFrames(MakeSettings(EStageRenderResolution::FullHD, EStageRenderAntiAliasing::Supersampled, EStageRenderPostProcess::Cinematic)), 32);

	TestEqual(TEXT("File components keep letters, digits, - and _"), SanitizeFileComponent(TEXT(" Main Show (v2)/final ")), FString(TEXT("Main_Show__v2__final")));
	TestEqual(TEXT("Empty name becomes Untitled"), SanitizeFileComponent(TEXT("   ")), FString(TEXT("Untitled")));

	const FDateTime Time(2026, 10, 8, 14, 5, 9);
	TestEqual(TEXT("File name"), MakeFileName(TEXT("My Show"), Time, FIntPoint(3840, 2160)), FString(TEXT("StageCraft_My_Show_20261008-140509_3840x2160.png")));

	const FString Watermark = MakeWatermarkText(TEXT("My Show"), Time, FIntPoint(1920, 1080), 12);
	TestTrue(TEXT("Watermark names the scene"), Watermark.Contains(TEXT("My Show")));
	TestTrue(TEXT("Watermark has the date"), Watermark.Contains(TEXT("2026-10-08 14:05")));
	TestTrue(TEXT("Watermark has the size"), Watermark.Contains(TEXT("1920")) && Watermark.Contains(TEXT("1080")));
	TestTrue(TEXT("Watermark counts items"), Watermark.Contains(TEXT("12 items")));
	TestTrue(TEXT("Singular item"), MakeWatermarkText(TEXT("A"), Time, FIntPoint(1, 1), 1).Contains(TEXT("1 item")) && !MakeWatermarkText(TEXT("A"), Time, FIntPoint(1, 1), 1).Contains(TEXT("1 items")));

	FStageRenderSettings Broken;
	Broken.Resolution = static_cast<EStageRenderResolution>(200);
	Broken.AntiAliasing = static_cast<EStageRenderAntiAliasing>(99);
	Broken.PostProcess = static_cast<EStageRenderPostProcess>(42);
	const FStageRenderSettings Repaired = Sanitize(Broken);
	TestTrue(TEXT("Out-of-range values fall back to the defaults"), Repaired == FStageRenderSettings());
	const FStageRenderSettings Valid = MakeSettings(EStageRenderResolution::Square, EStageRenderAntiAliasing::Off, EStageRenderPostProcess::Clean);
	TestTrue(TEXT("Valid values are kept"), Sanitize(Valid) == Valid);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
