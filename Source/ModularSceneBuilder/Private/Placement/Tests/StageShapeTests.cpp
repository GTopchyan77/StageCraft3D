// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the assistant's stage shapes (Docs/ADR/0018 §5): round and arc riser outlines (accepted by the deck's own sanitizer),
// stairs layout and mesh, and the curved truss layout, mesh and collision. Pure geometry, no world.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Shapes; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Algo/AllOf.h"
#include "Misc/AutomationTest.h"
#include "Placement/StageDeckMath.h"
#include "Placement/StageProceduralShapes.h"

#include <limits>

namespace StageShapeTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	/** Every normal of the section has unit length (a degenerate triangle would leave a zero one). */
	bool HasUnitNormals(const FStageDeckMesh::FSection& Section)
	{
		return Algo::AllOf(Section.Normals, [](const FVector& Normal) { return FMath::IsNearlyEqual(Normal.Size(), 1.0, 1e-3); });
	}

	FBox GetBounds(const FStageDeckMesh::FSection& Section)
	{
		return FBox(Section.Vertices);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShapeOutlineTest, "StageCraft.Shapes.Outlines", StageShapeTests::Flags)

bool FStageShapeOutlineTest::RunTest(const FString& Parameters)
{
	// Segment count: the chord error stays within a centimetre, between the limits.
	TestEqual(TEXT("small circle has the minimum"), StageProceduralShapes::GetCircleSegments(20.0), 16);
	TestEqual(TEXT("huge circle has the maximum"), StageProceduralShapes::GetCircleSegments(100000.0), 96);
	const int32 Segments = StageProceduralShapes::GetCircleSegments(200.0);
	TestTrue(TEXT("2 m radius within the limits"), Segments >= 16 && Segments <= 96);
	TestTrue(TEXT("... and within 1 cm of the curve"), 200.0 * (1.0 - FMath::Cos(PI / Segments)) <= 1.0 + 1e-6 || Segments == 96);

	// A circle: counter-clockwise, on the radius, with the circle's area, and a valid deck outline.
	const TArray<FVector2D> Circle = StageProceduralShapes::MakeCircle(200.0);
	TestTrue(TEXT("circle has corners"), Circle.Num() >= 16);
	TestTrue(TEXT("circle corners on the radius"), Algo::AllOf(Circle, [](const FVector2D& Point) { return FMath::IsNearlyEqual(Point.Size(), 200.0, 1e-6); }));
	const double Area = StageDeckMath::GetSignedArea(Circle);
	TestTrue(TEXT("circle is counter-clockwise"), Area > 0.0);
	TestTrue(TEXT("circle area within 1 % of pi r²"), FMath::Abs(Area - PI * 200.0 * 200.0) < 0.01 * PI * 200.0 * 200.0);
	TArray<FVector2D> Clean;
	TestTrue(TEXT("deck accepts the circle"), StageDeckMath::SanitizeOutline(Circle, Clean) && Clean.Num() == Circle.Num());

	TestTrue(TEXT("no circle for 0"), StageProceduralShapes::MakeCircle(0.0).IsEmpty());
	TestTrue(TEXT("no circle for NaN"), StageProceduralShapes::MakeCircle(std::numeric_limits<double>::quiet_NaN()).IsEmpty());

	// An arc ring: between the radii, symmetric about +X, accepted by the deck.
	const TArray<FVector2D> Ring = StageProceduralShapes::MakeArcRing(300.0, 400.0, 120.0);
	TestTrue(TEXT("ring has corners"), Ring.Num() >= 8);
	TestTrue(TEXT("ring corners between the radii"), Algo::AllOf(Ring, [](const FVector2D& Point) { return Point.Size() > 299.9 && Point.Size() < 400.1; }));
	TestTrue(TEXT("ring is counter-clockwise"), StageDeckMath::GetSignedArea(Ring) > 0.0);
	const double RingArea = PI * (400.0 * 400.0 - 300.0 * 300.0) * (120.0 / 360.0);
	TestTrue(TEXT("ring area within 2 %"), FMath::Abs(StageDeckMath::GetSignedArea(Ring) - RingArea) < 0.02 * RingArea);
	double MinY = 0.0, MaxY = 0.0;
	for (const FVector2D& Point : Ring)
	{
		MinY = FMath::Min(MinY, Point.Y);
		MaxY = FMath::Max(MaxY, Point.Y);
	}
	TestTrue(TEXT("ring symmetric about +X"), FMath::IsNearlyEqual(MinY, -MaxY, 0.01));
	TestTrue(TEXT("deck accepts the ring"), StageDeckMath::SanitizeOutline(Ring, Clean));
	TestTrue(TEXT("pie slice with inner radius 0"), StageDeckMath::SanitizeOutline(StageProceduralShapes::MakeArcRing(0.0, 300.0, 90.0), Clean));
	TestTrue(TEXT("360 degrees is limited to a simple polygon"), StageDeckMath::SanitizeOutline(StageProceduralShapes::MakeArcRing(200.0, 300.0, 360.0), Clean));
	TestTrue(TEXT("inner above outer is refused"), StageProceduralShapes::MakeArcRing(400.0, 300.0, 90.0).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShapeStairsTest, "StageCraft.Shapes.Stairs", StageShapeTests::Flags)

bool FStageShapeStairsTest::RunTest(const FString& Parameters)
{
	using namespace StageShapeTests;

	// Equal steps no taller than the limit.
	const FStageStairsLayout Layout = StageProceduralShapes::LayoutStairs(60.0, 120.0, 30.0, 20.0);
	TestEqual(TEXT("60 cm in 20 cm steps is 3"), Layout.Steps, 3);
	TestTrue(TEXT("step rise 20"), FMath::IsNearlyEqual(Layout.StepRise, 20.0));
	TestTrue(TEXT("run 90"), FMath::IsNearlyEqual(Layout.Run, 90.0));
	const FStageStairsLayout Odd = StageProceduralShapes::LayoutStairs(70.0, 120.0, 30.0, 20.0);
	TestTrue(TEXT("70 cm is 4 equal steps"), Odd.Steps == 4 && FMath::IsNearlyEqual(Odd.StepRise, 17.5));

	// Inputs are clamped to usable sizes.
	const FStageStairsLayout Wild = StageProceduralShapes::LayoutStairs(5000.0, 1.0, 1000.0, 0.0);
	TestTrue(TEXT("rise clamped"), Wild.TotalRise <= 300.0 && Wild.Width >= 40.0 && Wild.Tread <= 60.0 && Wild.StepRise <= 25.0 + 1e-6);
	TestTrue(TEXT("NaN becomes the default"), StageProceduralShapes::LayoutStairs(std::numeric_limits<double>::quiet_NaN(), 120.0, 30.0, 20.0).IsValid());

	// The mesh: steps from the floor to the top, centred, rails only when asked for.
	const FStageShapeMesh Plain = StageProceduralShapes::BuildStairsMesh(Layout, /*bHandrails*/ false);
	TestTrue(TEXT("mesh is valid"), Plain.IsValid());
	TestEqual(TEXT("no rails, one section"), Plain.Sections.Num(), 1);
	const FBox Bounds = GetBounds(Plain.Sections[0]);
	TestTrue(TEXT("steps stand on the floor"), FMath::IsNearlyZero(Bounds.Min.Z));
	TestTrue(TEXT("top is the rise"), FMath::IsNearlyEqual(Bounds.Max.Z, 60.0));
	TestTrue(TEXT("centred along the run"), FMath::IsNearlyEqual(Bounds.Min.X, -45.0) && FMath::IsNearlyEqual(Bounds.Max.X, 45.0));
	TestTrue(TEXT("width"), FMath::IsNearlyEqual(Bounds.GetSize().Y, 120.0));
	TestTrue(TEXT("unit normals"), HasUnitNormals(Plain.Sections[0]));
	TestTrue(TEXT("triangles index vertices"), Algo::AllOf(Plain.Sections[0].Triangles, [&Plain](int32 Index) { return Plain.Sections[0].Vertices.IsValidIndex(Index); }));

	const FStageShapeMesh Railed = StageProceduralShapes::BuildStairsMesh(Layout, /*bHandrails*/ true);
	TestEqual(TEXT("rails are a second section"), Railed.Sections.Num(), 2);
	TestTrue(TEXT("rails reach above the top step"), GetBounds(Railed.Sections[1]).Max.Z > 60.0 + 80.0);
	TestTrue(TEXT("rails inside the width"), GetBounds(Railed.Sections[1]).GetSize().Y <= 120.0);
	TestFalse(TEXT("invalid layout builds nothing"), StageProceduralShapes::BuildStairsMesh(FStageStairsLayout(), true).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShapeArcTrussTest, "StageCraft.Shapes.ArcTruss", StageShapeTests::Flags)

bool FStageShapeArcTrussTest::RunTest(const FString& Parameters)
{
	using namespace StageShapeTests;

	// Sections, bays and samples along the curve.
	const FStageArcTrussLayout Quarter = StageProceduralShapes::LayoutArcTruss(400.0, 90.0, 29.0, 200.0);
	TestTrue(TEXT("arc length"), FMath::IsNearlyEqual(Quarter.ArcLength, 400.0 * UE_DOUBLE_HALF_PI, 1e-6));
	TestEqual(TEXT("sections = ceil(628 / 200)"), Quarter.Sections, 4);
	TestTrue(TEXT("bays about one profile each"), FMath::Abs(Quarter.Bays - FMath::RoundToInt(Quarter.ArcLength / 29.0)) <= 1);
	TestTrue(TEXT("samples every 3 degrees or closer"), Quarter.Samples - 1 >= FMath::CeilToInt(90.0 / 3.0));
	TestFalse(TEXT("a quarter is not a circle"), Quarter.IsFullCircle());
	const FStageArcTrussLayout Exact = StageProceduralShapes::LayoutArcTruss(200.0 / UE_DOUBLE_HALF_PI * 2.0, 90.0, 29.0, 200.0);
	TestEqual(TEXT("exactly two sections stays two"), Exact.Sections, 2);

	// The curve: the apex at the origin heading +X, the ends on the circle around (0, -R).
	TestTrue(TEXT("apex at the origin"), Quarter.GetPoint(0.5).IsNearlyZero(1e-6));
	TestTrue(TEXT("apex outward is +Y"), Quarter.GetOutward(0.5).Equals(FVector2D(0.0, 1.0), 1e-6));
	const FVector2D Centre(0.0, -400.0);
	TestTrue(TEXT("ends on the circle"), FMath::IsNearlyEqual((Quarter.GetPoint(0.0) - Centre).Size(), 400.0, 1e-6)
		&& FMath::IsNearlyEqual((Quarter.GetPoint(1.0) - Centre).Size(), 400.0, 1e-6));
	TestTrue(TEXT("ends symmetric"), FMath::IsNearlyEqual(Quarter.GetPoint(0.0).X, -Quarter.GetPoint(1.0).X, 1e-6));

	// Clamping.
	const FStageArcTrussLayout Wild = StageProceduralShapes::LayoutArcTruss(1.0, 1000.0, 29.0, 200.0);
	TestTrue(TEXT("radius and angle clamped"), Wild.Radius >= 50.0 && Wild.AngleDegrees <= 360.0);
	TestTrue(TEXT("NaN radius becomes the default"), StageProceduralShapes::LayoutArcTruss(std::numeric_limits<double>::quiet_NaN(), 90.0, 29.0, 200.0).IsValid());

	// The mesh stays within the profile around the arc line.
	const FStageShapeMesh Mesh = StageProceduralShapes::BuildArcTrussMesh(Quarter, 5.0, 2.0);
	TestTrue(TEXT("mesh is valid"), Mesh.IsValid());
	TestTrue(TEXT("unit normals"), HasUnitNormals(Mesh.Sections[0]));
	const double Half = Quarter.ProfileWidth * 0.5;
	TestTrue(TEXT("vertices within the profile of the arc line"), Algo::AllOf(Mesh.Sections[0].Vertices, [&](const FVector& Vertex)
	{
		const double Radial = FVector2D(Vertex.X, Vertex.Y + 400.0).Size() - 400.0;
		return FMath::Abs(Radial) <= Half + 0.5 && FMath::Abs(Vertex.Z) <= Half + 0.5;
	}));
	const FStageShapeMesh Circle = StageProceduralShapes::BuildArcTrussMesh(StageProceduralShapes::LayoutArcTruss(300.0, 360.0, 29.0, 200.0), 5.0, 2.0);
	TestTrue(TEXT("a full circle builds"), Circle.IsValid() && Circle.GetTriangleCount() > Mesh.GetTriangleCount());
	TestFalse(TEXT("invalid layout builds nothing"), StageProceduralShapes::BuildArcTrussMesh(FStageArcTrussLayout(), 5.0, 2.0).IsValid());

	// Collision: one hull per rental section, around the profile.
	const TArray<TArray<FVector>> Hulls = StageProceduralShapes::BuildArcTrussCollision(Quarter);
	TestEqual(TEXT("a hull per section"), Hulls.Num(), Quarter.Sections);
	TestTrue(TEXT("each hull has both ends and the middle"), Algo::AllOf(Hulls, [](const TArray<FVector>& Hull) { return Hull.Num() == 12; }));
	TestTrue(TEXT("no hulls for an invalid layout"), StageProceduralShapes::BuildArcTrussCollision(FStageArcTrussLayout()).IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
