// Copyright Epic Games, Inc. All Rights Reserved.

// Unit tests for span geometry (Docs/ADR/0005 §2-3, Docs/ADR/0007 §4): run layout, placement constraints, drawn paths
// (normalisation, corners, closed loops, sag) and the repeated part transforms. No world and no assets.
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

	// A straight one-leg path is exactly the old straight run: three whole 2 m sections, a chord part at the mid-point.
	const FStageSpanPathLayout Whole = StageSpanMath::ComputePathLayout(MakeRules(EStageSpanFit::WholeSegments, 200.0, 10),
		{ FVector::ZeroVector, FVector(600.0, 0.0, 0.0) }, false, 0.0);
	const FTransform Chord(FRotator::ZeroRotator, FVector(100.0, 15.0, 0.0), FVector(2.0, 0.05, 0.05));
	TArray<FTransform> Instances;
	StageSpanMath::MakePathSegmentInstances(Whole, 200.0, Chord, Instances);
	TestEqual(TEXT("One instance per section"), Instances.Num(), 3);
	TestTrue(TEXT("Section 1 at its mid-point"), Instances[0].GetLocation().Equals(FVector(100.0, 15.0, 0.0), 1.e-4));
	TestTrue(TEXT("Section 3 two pitches further"), Instances[2].GetLocation().Equals(FVector(500.0, 15.0, 0.0), 1.e-4));
	TestTrue(TEXT("Whole sections are never stretched"), Instances[1].GetScale3D().Equals(Chord.GetScale3D(), 1.e-4));

	// Stretched: 2.5 m of 1 m sections = 3 sections of 83.3 cm. A cylinder lying along the run (pitched 90 degrees, its
	// height axis Z turned onto X) must get longer, not thicker.
	const FStageSpanPathLayout Stretched = StageSpanMath::ComputePathLayout(MakeRules(EStageSpanFit::Stretch, 100.0, 10),
		{ FVector::ZeroVector, FVector(250.0, 0.0, 0.0) }, false, 0.0);
	const FTransform Cable(FRotator(90.0, 0.0, 0.0), FVector(50.0, 0.0, 0.0), FVector(0.02, 0.02, 1.0));
	StageSpanMath::MakePathSegmentInstances(Stretched, 100.0, Cable, Instances);
	const double Pitch = 250.0 / 3.0;
	const double Stretch = Pitch / 100.0;
	TestEqual(TEXT("Three stretched sections"), Instances.Num(), 3);
	TestTrue(TEXT("Stretched part centre follows the pitch"), Instances[1].GetLocation().Equals(FVector(Pitch + 50.0 * Stretch, 0.0, 0.0), 1.e-4));
	TestTrue(TEXT("The cable gets longer along its own axis"), FMath::IsNearlyEqual(Instances[0].GetScale3D().Z, Stretch, 1.e-4));
	TestTrue(TEXT("The cable keeps its thickness"), FMath::IsNearlyEqual(Instances[0].GetScale3D().X, 0.02, 1.e-4) && FMath::IsNearlyEqual(Instances[0].GetScale3D().Y, 0.02, 1.e-4));
	TestTrue(TEXT("Rotation is kept"), Instances[0].GetRotation().Equals(Cable.GetRotation(), 1.e-4));

	// Joints of an open path: the two ends plus every joint between segments, no corners.
	TArray<FTransform> Joints;
	TArray<FTransform> Corners;
	StageSpanMath::MakePathJointFrames(Stretched, Joints, Corners);
	TestEqual(TEXT("Sections + 1 joints"), Joints.Num(), 4);
	TestEqual(TEXT("A straight run has no corners"), Corners.Num(), 0);
	bool bHasStart = false;
	bool bHasEnd = false;
	for (const FTransform& Joint : Joints)
	{
		bHasStart |= Joint.GetLocation().Equals(FVector::ZeroVector, 1.e-4);
		bHasEnd |= Joint.GetLocation().Equals(FVector(250.0, 0.0, 0.0), 1.e-4);
	}
	TestTrue(TEXT("A joint at the start"), bHasStart);
	TestTrue(TEXT("A joint at the real end"), bHasEnd);

	// No path, no instances.
	StageSpanMath::MakePathSegmentInstances(FStageSpanPathLayout(), 100.0, Chord, Instances);
	TestEqual(TEXT("Invalid layout: no segments"), Instances.Num(), 0);
	StageSpanMath::MakePathJointFrames(FStageSpanPathLayout(), Joints, Corners);
	TestEqual(TEXT("Invalid layout: no joints"), Joints.Num() + Corners.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSpanPathTest, "StageCraft.Span.Path", StageSpanTests::Flags)

bool FStageSpanPathTest::RunTest(const FString& Parameters)
{
	using namespace StageSpanTests;
	using StageSpanMath::ComputePathLayout;
	const StageSpanMath::FRules Truss = MakeRules(EStageSpanFit::WholeSegments, 200.0, 20);

	// Normalisation keeps each leg's direction and shifts later points with it: 5.4 m -> 6 m (3 sections), then 3.7 m -> 4 m.
	const FStageSpanPathLayout L = ComputePathLayout(Truss, { FVector::ZeroVector, FVector(540.0, 0.0, 0.0), FVector(540.0, 370.0, 0.0) }, false, 0.0);
	TestEqual(TEXT("Two legs"), L.Legs.Num(), 2);
	TestTrue(TEXT("First leg snapped to 6 m"), L.Points[1].Equals(FVector(600.0, 0.0, 0.0), 1.e-6));
	TestTrue(TEXT("Second leg starts where the first ended and keeps its direction"), L.Points[2].Equals(FVector(600.0, 400.0, 0.0), 1.e-6));
	TestEqual(TEXT("Segments across legs"), L.GetSegmentCount(), 5);
	TestNearlyEqual(TEXT("Length across legs"), L.GetLength(), 1000.0, 1.e-6);

	// Corner: the bend vertex is a corner, turned to the bisector (45 degrees between +X and +Y); ends and in-leg joints are joints.
	TArray<FTransform> Joints;
	TArray<FTransform> Corners;
	StageSpanMath::MakePathJointFrames(L, Joints, Corners);
	TestEqual(TEXT("One corner"), Corners.Num(), 1);
	TestTrue(TEXT("Corner at the bend"), Corners[0].GetLocation().Equals(FVector(600.0, 0.0, 0.0), 1.e-6));
	TestNearlyEqual(TEXT("Corner bisects the legs"), Corners[0].Rotator().Yaw, 45.0, 1.e-4);
	// 2 ends + (3 - 1) + (2 - 1) in-leg joints.
	TestEqual(TEXT("Joints without the corner"), Joints.Num(), 5);

	// Segments of consecutive legs meet: section 3 ends exactly where section 4 (first of leg 2) starts.
	TArray<FTransform> Sections;
	const FTransform Unit(FRotator::ZeroRotator, FVector::ZeroVector, FVector::OneVector);
	StageSpanMath::MakePathSegmentInstances(L, 200.0, Unit, Sections);
	TestEqual(TEXT("Five sections"), Sections.Num(), 5);
	TestTrue(TEXT("Leg 2 starts at the corner"), Sections[3].GetLocation().Equals(FVector(600.0, 0.0, 0.0), 1.e-4));
	TestNearlyEqual(TEXT("Leg 2 sections face +Y"), Sections[3].Rotator().Yaw, 90.0, 1.e-4);
	TestTrue(TEXT("Section 3 ends at the corner"), Sections[2].TransformPosition(FVector(200.0, 0.0, 0.0)).Equals(FVector(600.0, 0.0, 0.0), 1.e-4));

	// Closed rectangle 6 x 4 m: four legs, four corners, no end joints, the start not doubled.
	const TArray<FVector> Rectangle = { FVector::ZeroVector, FVector(600.0, 0.0, 0.0), FVector(600.0, 400.0, 0.0), FVector(0.0, 400.0, 0.0) };
	const FStageSpanPathLayout Loop = ComputePathLayout(Truss, Rectangle, true, 0.0);
	TestTrue(TEXT("Rectangle closes"), Loop.bClosed);
	TestEqual(TEXT("Four legs"), Loop.Legs.Num(), 4);
	TestEqual(TEXT("Four vertices"), Loop.Points.Num(), 4);
	TestEqual(TEXT("Ten sections"), Loop.GetSegmentCount(), 10);
	StageSpanMath::MakePathJointFrames(Loop, Joints, Corners);
	TestEqual(TEXT("Four corners"), Corners.Num(), 4);
	TestEqual(TEXT("In-leg joints only"), Joints.Num(), 2 + 1 + 2 + 1);
	TestTrue(TEXT("CanClosePath agrees"), StageSpanMath::CanClosePath(Truss, Rectangle));

	// A closing distance that is not whole stays open for whole-segment items, but closes when stretched. The third leg (4.5 m)
	// normalises to 4 m, so the closing side runs from (200, 400) to the start: 4.47 m, not a whole number of 2 m sections.
	const TArray<FVector> Skewed = { FVector::ZeroVector, FVector(600.0, 0.0, 0.0), FVector(600.0, 400.0, 0.0), FVector(150.0, 400.0, 0.0) };
	TestFalse(TEXT("Non-whole closing leg stays open"), ComputePathLayout(Truss, Skewed, true, 0.0).bClosed);
	TestFalse(TEXT("CanClosePath agrees"), StageSpanMath::CanClosePath(Truss, Skewed));
	const StageSpanMath::FRules Railing = MakeRules(EStageSpanFit::Stretch, 100.0, 100);
	TestTrue(TEXT("A stretched railing closes any loop"), ComputePathLayout(Railing, Skewed, true, 0.0).bClosed);
	TestFalse(TEXT("Two points never close"), ComputePathLayout(Truss, { FVector::ZeroVector, FVector(600.0, 0.0, 0.0) }, true, 0.0).bClosed);

	// Untrusted input: NaN, duplicates and too many points are dropped; the segment cap stops the path.
	const double NaN = std::numeric_limits<double>::quiet_NaN();
	const FStageSpanPathLayout Cleaned = ComputePathLayout(Truss, { FVector::ZeroVector, FVector(NaN, 0.0, 0.0), FVector(400.0, 0.0, 0.0), FVector(400.2, 0.0, 0.0) }, false, 0.0);
	TestEqual(TEXT("NaN and duplicate dropped"), Cleaned.Points.Num(), 2);
	const StageSpanMath::FRules Short = MakeRules(EStageSpanFit::WholeSegments, 200.0, 4);
	const FStageSpanPathLayout Capped = ComputePathLayout(Short, { FVector::ZeroVector, FVector(600.0, 0.0, 0.0), FVector(600.0, 600.0, 0.0) }, false, 0.0);
	TestEqual(TEXT("Cap shared by the legs"), Capped.GetSegmentCount(), 4);
	TestTrue(TEXT("Second leg shortened to what is left"), Capped.Points.Last().Equals(FVector(600.0, 200.0, 0.0), 1.e-6));
	TArray<FVector> Many;
	for (int32 Index = 0; Index < 100; ++Index)
	{
		Many.Add(FVector(Index * 100.0, (Index % 2) * 100.0, 0.0));
	}
	TestTrue(TEXT("At most MaxPathPoints vertices"), ComputePathLayout(Railing, Many, false, 0.0).Points.Num() <= StageSpanMath::MaxPathPoints);
	TestFalse(TEXT("One point is no path"), ComputePathLayout(Truss, { FVector::ZeroVector }, false, 0.0).IsValid());
	TestFalse(TEXT("Invalid rules give no path"), ComputePathLayout(MakeRules(EStageSpanFit::Stretch, 0.0, 10), Rectangle, false, 0.0).IsValid());

	// ConstrainPathPoint: the vertex the user sees is the stored one (whole segments from the last point).
	const FVector Next = StageSpanMath::ConstrainPathPoint(Truss, FVector(0.0, 0.0, 10.0), FVector(530.0, 20.0, 50.0), 15.0);
	TestTrue(TEXT("Polar snapped, level and whole"), Next.Equals(FVector(600.0, 0.0, 10.0), 1.e-6));

	// Determinism.
	TestTrue(TEXT("Same input, same path"), ComputePathLayout(Truss, Rectangle, true, 0.0) == Loop);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSpanSagTest, "StageCraft.Span.Sag", StageSpanTests::Flags)

bool FStageSpanSagTest::RunTest(const FString& Parameters)
{
	using namespace StageSpanTests;

	StageSpanMath::FRules Cable = MakeRules(EStageSpanFit::Stretch, 100.0, 100);
	Cable.MaxSagFraction = 0.1;
	const FStageSpanPathLayout Hung = StageSpanMath::ComputePathLayout(Cable, { FVector::ZeroVector, FVector(800.0, 0.0, 0.0) }, false, 50.0);
	TestNearlyEqual(TEXT("Sag kept"), Hung.SagDepth, 50.0, 1.e-9);
	TestNearlyEqual(TEXT("Mid-span is the lowest point"), StageSpanMath::GetPathLegPoint(Hung, 0, 0.5).Z, -50.0, 1.e-9);
	TestNearlyEqual(TEXT("Ends stay on their supports"), StageSpanMath::GetPathLegPoint(Hung, 0, 1.0).Z, 0.0, 1.e-9);

	// Chords meet: the end of each segment is the start of the next, and every segment is stretched to its chord.
	TArray<FTransform> Segments;
	const FTransform Unit(FRotator::ZeroRotator, FVector::ZeroVector, FVector::OneVector);
	StageSpanMath::MakePathSegmentInstances(Hung, 100.0, Unit, Segments);
	TestEqual(TEXT("Eight segments"), Segments.Num(), 8);
	bool bConnected = true;
	for (int32 Index = 0; Index + 1 < Segments.Num(); ++Index)
	{
		bConnected &= Segments[Index].TransformPosition(FVector(100.0, 0.0, 0.0)).Equals(Segments[Index + 1].GetLocation(), 1.e-3);
	}
	TestTrue(TEXT("Consecutive chords meet at their joints"), bConnected);
	TestTrue(TEXT("The first chord slopes down"), Segments[0].Rotator().Pitch < 0.0);
	TestTrue(TEXT("The last chord slopes up"), Segments.Last().Rotator().Pitch > 0.0);

	// Limits: deeper than the item allows is clamped; negative and NaN mean none; rigid and whole-segment items never sag.
	TestNearlyEqual(TEXT("Clamped to 10 % of the leg"), StageSpanMath::ComputePathLayout(Cable, { FVector::ZeroVector, FVector(800.0, 0.0, 0.0) }, false, 500.0).SagDepth, 80.0, 1.e-9);
	TestEqual(TEXT("Negative sag is none"), StageSpanMath::ComputePathLayout(Cable, { FVector::ZeroVector, FVector(800.0, 0.0, 0.0) }, false, -5.0).SagDepth, 0.0);
	TestEqual(TEXT("NaN sag is none"), StageSpanMath::ComputePathLayout(Cable, { FVector::ZeroVector, FVector(800.0, 0.0, 0.0) }, false, std::numeric_limits<double>::quiet_NaN()).SagDepth, 0.0);
	StageSpanMath::FRules Truss = MakeRules(EStageSpanFit::WholeSegments, 200.0, 10);
	Truss.MaxSagFraction = 0.1;
	TestEqual(TEXT("Whole sections never sag"), StageSpanMath::ComputePathLayout(Truss, { FVector::ZeroVector, FVector(800.0, 0.0, 0.0) }, false, 50.0).SagDepth, 0.0);
	TestEqual(TEXT("Rigid items never sag"), StageSpanMath::ComputePathLayout(MakeRules(EStageSpanFit::Stretch, 100.0, 10), { FVector::ZeroVector, FVector(800.0, 0.0, 0.0) }, false, 50.0).SagDepth, 0.0);

	// A short leg sags less than a long one: each is limited by its own length.
	const FStageSpanPathLayout TwoLegs = StageSpanMath::ComputePathLayout(Cable, { FVector::ZeroVector, FVector(800.0, 0.0, 0.0), FVector(800.0, 200.0, 0.0) }, false, 60.0);
	TestNearlyEqual(TEXT("Long leg sags fully"), TwoLegs.GetLegSag(0), 60.0, 1.e-9);
	TestNearlyEqual(TEXT("Short leg limited to 10 %"), TwoLegs.GetLegSag(1), 20.0, 1.e-9);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
