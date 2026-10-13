// Copyright Epic Games, Inc. All Rights Reserved.

// Unit tests for span geometry (Docs/ADR/0005-spans-outliner-and-bill-of-materials.md §2-3): run layout, the two-point placement
// constraints and the repeated part transforms. No world and no assets.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Span; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Placement/StageSpanMath.h"

namespace StageSpanTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	StageSpanMath::FRules MakeRules(EStageSpanFit Fit, double SegmentLength, int32 MaxSegments)
	{
		StageSpanMath::FRules Rules;
		Rules.Fit = Fit;
		Rules.SegmentLength = SegmentLength;
		Rules.MaxSegments = MaxSegments;
		return Rules;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSpanLayoutTest, "StageCraft.Span.Layout", StageSpanTests::Flags)

bool FStageSpanLayoutTest::RunTest(const FString& Parameters)
{
	using namespace StageSpanTests;
	using StageSpanMath::ComputeLayout;

	// Whole segments: 2 m truss sections, at most 10.
	const StageSpanMath::FRules Truss = MakeRules(EStageSpanFit::WholeSegments, 200.0, 10);
	FStageSpanLayout Layout = ComputeLayout(Truss, 640.0);
	TestEqual(TEXT("6.4 m rounds to 3 sections"), Layout.SegmentCount, 3);
	TestEqual(TEXT("Whole sections keep their length"), Layout.SegmentPitch, 200.0);
	TestEqual(TEXT("The run is a whole number of sections"), Layout.Length, 600.0);
	TestEqual(TEXT("6.99 m rounds up to 3.5 -> 4 sections"), ComputeLayout(Truss, 700.0).SegmentCount, 4);
	TestEqual(TEXT("Zero length is one section"), ComputeLayout(Truss, 0.0).SegmentCount, 1);
	TestEqual(TEXT("Negative length is one section"), ComputeLayout(Truss, -500.0).SegmentCount, 1);
	TestEqual(TEXT("Longer than the cap stops at the cap"), ComputeLayout(Truss, 1.e9).SegmentCount, 10);
	TestEqual(TEXT("Capped run length"), ComputeLayout(Truss, 1.e9).Length, 2000.0);
	TestEqual(TEXT("NaN is one section, never NaN"), ComputeLayout(Truss, std::numeric_limits<double>::quiet_NaN()).SegmentCount, 1);
	TestEqual(TEXT("Infinity is one section"), ComputeLayout(Truss, std::numeric_limits<double>::infinity()).SegmentCount, 1);
	TestEqual(TEXT("Min length is one section"), StageSpanMath::GetMinLength(Truss), 200.0);
	TestEqual(TEXT("Max length is the cap"), StageSpanMath::GetMaxLength(Truss), 2000.0);

	// Stretch: 1 m railing sections fill the exact length.
	const StageSpanMath::FRules Railing = MakeRules(EStageSpanFit::Stretch, 100.0, 50);
	Layout = ComputeLayout(Railing, 640.0);
	TestEqual(TEXT("6.4 m of railing is 6 sections"), Layout.SegmentCount, 6);
	TestEqual(TEXT("Stretched runs keep the exact length"), Layout.Length, 640.0);
	TestTrue(TEXT("Each section stretches to fill it"), FMath::IsNearlyEqual(Layout.SegmentPitch, 640.0 / 6.0));
	TestEqual(TEXT("A short stretched run is clamped to half a section"), ComputeLayout(Railing, 10.0).Length, 50.0);
	TestEqual(TEXT("Half a section is still one section"), ComputeLayout(Railing, 10.0).SegmentCount, 1);
	TestEqual(TEXT("Stretch min length"), StageSpanMath::GetMinLength(Railing), 50.0);

	// The asset cap can never exceed the absolute cap.
	TestEqual(TEXT("Absolute cap wins over the asset's"), ComputeLayout(MakeRules(EStageSpanFit::WholeSegments, 1.0, 100000), 1.e9).SegmentCount,
		UStageSpanData::AbsoluteMaxSegments);

	// Invalid rules give no run at all.
	TestFalse(TEXT("Zero segment length is invalid"), ComputeLayout(MakeRules(EStageSpanFit::WholeSegments, 0.0, 10), 500.0).IsValid());
	TestFalse(TEXT("NaN segment length is invalid"), ComputeLayout(MakeRules(EStageSpanFit::Stretch, std::numeric_limits<double>::quiet_NaN(), 10), 500.0).IsValid());
	TestFalse(TEXT("No segments allowed is invalid"), ComputeLayout(MakeRules(EStageSpanFit::WholeSegments, 200.0, 0), 500.0).IsValid());
	TestEqual(TEXT("Invalid rules have no min length"), StageSpanMath::GetMinLength(MakeRules(EStageSpanFit::Stretch, -5.0, 10)), 0.0);

	// Deterministic: the same request always gives the same run.
	TestTrue(TEXT("Same input, same layout"), ComputeLayout(Railing, 333.3) == ComputeLayout(Railing, 333.3));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSpanPlacementTest, "StageCraft.Span.Placement", StageSpanTests::Flags)

bool FStageSpanPlacementTest::RunTest(const FString& Parameters)
{
	using StageSpanMath::ConstrainEndPoint;

	const FVector Start(100.0, 200.0, 50.0);

	// Free direction: the end is the cursor, flattened to the start's height.
	FVector End = ConstrainEndPoint(Start, FVector(400.0, 600.0, 300.0), 0.0);
	TestTrue(TEXT("Free end follows the cursor"), End.Equals(FVector(400.0, 600.0, 50.0)));

	// 15 degree steps: a cursor at 10 degrees snaps to 15 and the length is projected onto that line.
	const double Distance = 500.0;
	const FVector Cursor = Start + FVector(FMath::Cos(FMath::DegreesToRadians(10.0)), FMath::Sin(FMath::DegreesToRadians(10.0)), 0.0) * Distance;
	End = ConstrainEndPoint(Start, Cursor, 15.0);
	TestTrue(TEXT("Direction snapped to 15 degrees"), FMath::IsNearlyEqual(StageSpanMath::GetRunYaw(Start, End, 0.0), 15.0, 1.e-6));
	TestTrue(TEXT("Length projected onto the snapped line"), FMath::IsNearlyEqual(FVector::Dist2D(Start, End), Distance * FMath::Cos(FMath::DegreesToRadians(5.0)), 1.e-6));
	TestEqual(TEXT("Snapped end stays level with the start"), End.Z, Start.Z);

	// 90 degree steps keep runs on the grid axes, including negative directions.
	End = ConstrainEndPoint(Start, Start + FVector(-300.0, 20.0, 0.0), 90.0);
	TestTrue(TEXT("Snapped onto -X"), FMath::IsNearlyEqual(FMath::Abs(StageSpanMath::GetRunYaw(Start, End, 0.0)), 180.0, 1.e-6));
	TestTrue(TEXT("-X run keeps its length along the axis"), FMath::IsNearlyEqual(FVector::Dist2D(Start, End), 300.0, 1.e-6));

	// A cursor on the start point has no direction.
	TestTrue(TEXT("Cursor on start returns start"), ConstrainEndPoint(Start, Start + FVector(0.0, 0.0, 400.0), 15.0).Equals(Start));
	TestEqual(TEXT("No direction uses the fallback yaw"), StageSpanMath::GetRunYaw(Start, Start, 42.0), 42.0);

	// The run's transform: at the start, +X toward the end, level, unit scale.
	const FTransform Run = StageSpanMath::MakeRunTransform(Start, Start + FVector(0.0, 250.0, 0.0), 0.0);
	TestTrue(TEXT("Run starts at the start point"), Run.GetLocation().Equals(Start));
	TestTrue(TEXT("Run +X points at the end"), Run.GetRotation().GetForwardVector().Equals(FVector(0.0, 1.0, 0.0), 1.e-6));
	TestTrue(TEXT("Run is not scaled"), Run.GetScale3D().Equals(FVector::OneVector));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSpanInstancesTest, "StageCraft.Span.Instances", StageSpanTests::Flags)

bool FStageSpanInstancesTest::RunTest(const FString& Parameters)
{
	using namespace StageSpanTests;

	// Three whole 2 m sections; a chord part authored at the segment's mid-point.
	const FStageSpanLayout Whole = StageSpanMath::ComputeLayout(MakeRules(EStageSpanFit::WholeSegments, 200.0, 10), 600.0);
	const FTransform Chord(FRotator::ZeroRotator, FVector(100.0, 15.0, 0.0), FVector(2.0, 0.05, 0.05));
	TArray<FTransform> Instances;
	StageSpanMath::MakeSegmentInstances(Whole, 200.0, Chord, Instances);
	TestEqual(TEXT("One instance per section"), Instances.Num(), 3);
	TestTrue(TEXT("Section 1 at its mid-point"), Instances[0].GetLocation().Equals(FVector(100.0, 15.0, 0.0)));
	TestTrue(TEXT("Section 3 two pitches further"), Instances[2].GetLocation().Equals(FVector(500.0, 15.0, 0.0)));
	TestTrue(TEXT("Whole sections are never stretched"), Instances[1].GetScale3D().Equals(Chord.GetScale3D()));

	// Stretched: 2.5 m of 1 m sections = 3 sections of 83.3 cm. A cylinder lying along the run (pitched 90 degrees, its
	// height axis Z turned onto X) must get longer, not thicker.
	const FStageSpanLayout Stretched = StageSpanMath::ComputeLayout(MakeRules(EStageSpanFit::Stretch, 100.0, 10), 250.0);
	const FTransform Cable(FRotator(90.0, 0.0, 0.0), FVector(50.0, 0.0, 0.0), FVector(0.02, 0.02, 1.0));
	StageSpanMath::MakeSegmentInstances(Stretched, 100.0, Cable, Instances);
	const double Stretch = Stretched.SegmentPitch / 100.0;
	TestEqual(TEXT("Three stretched sections"), Instances.Num(), 3);
	TestTrue(TEXT("Stretched part centre follows the pitch"), Instances[1].GetLocation().Equals(FVector(Stretched.SegmentPitch + 50.0 * Stretch, 0.0, 0.0), 1.e-4));
	TestTrue(TEXT("The cable gets longer along its own axis"), FMath::IsNearlyEqual(Instances[0].GetScale3D().Z, Stretch, 1.e-4));
	TestTrue(TEXT("The cable keeps its thickness"), FMath::IsNearlyEqual(Instances[0].GetScale3D().X, 0.02, 1.e-4) && FMath::IsNearlyEqual(Instances[0].GetScale3D().Y, 0.02, 1.e-4));
	TestTrue(TEXT("Rotation is kept"), Instances[0].GetRotation().Equals(Cable.GetRotation(), 1.e-4));

	// Joints: both ends included, never stretched.
	const FTransform Post(FRotator::ZeroRotator, FVector(0.0, 0.0, 50.0), FVector(0.05, 0.05, 1.0));
	StageSpanMath::MakeJointInstances(Stretched, Post, Instances);
	TestEqual(TEXT("Sections + 1 joints"), Instances.Num(), 4);
	TestTrue(TEXT("First joint at the start"), Instances[0].GetLocation().Equals(FVector(0.0, 0.0, 50.0)));
	TestTrue(TEXT("Last joint at the real end"), Instances.Last().GetLocation().Equals(FVector(250.0, 0.0, 50.0), 1.e-4));
	TestTrue(TEXT("Joints are not stretched"), Instances[2].GetScale3D().Equals(Post.GetScale3D()));

	// No run, no instances.
	StageSpanMath::MakeSegmentInstances(FStageSpanLayout(), 100.0, Chord, Instances);
	TestEqual(TEXT("Invalid layout: no segments"), Instances.Num(), 0);
	StageSpanMath::MakeJointInstances(FStageSpanLayout(), Post, Instances);
	TestEqual(TEXT("Invalid layout: no joints"), Instances.Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
