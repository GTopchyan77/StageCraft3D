// Copyright Epic Games, Inc. All Rights Reserved.

// Unit tests for Sketch to Stage (Docs/ADR/0015-sketch-to-stage.md): stroke helpers (StageSketchMath), deck geometry
// (StageDeckMath) and the plan (StageSketchGenerator). No world; items are transient data assets.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Sketch; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/ModularBaseActor.h"
#include "Data/BaseItemData.h"
#include "Data/StageDeckData.h"
#include "Data/StageSpanData.h"
#include "Misc/AutomationTest.h"
#include "Placement/StageDeckMath.h"
#include "Sketch/StageSketchGenerator.h"
#include "Sketch/StageSketchMath.h"
#include "UObject/Package.h"

#include <limits>

namespace StageSketchTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	FStageSketchStroke MakeStroke(EStageSketchStrokeKind Kind, TArray<FVector2D> Points, bool bClosed = false, float DeckHeight = 60.f)
	{
		FStageSketchStroke Stroke;
		Stroke.Kind = Kind;
		Stroke.Points = MoveTemp(Points);
		Stroke.bClosed = bClosed;
		Stroke.DeckHeight = DeckHeight;
		return Stroke;
	}

	/** Transient catalog items with known rules: a 2 x 1 m deck panel system (20-200 cm), 2 m whole-section truss, a fixture. */
	struct FItems
	{
		UStageDeckData* Deck = nullptr;
		UStageSpanData* Truss = nullptr;
		UBaseItemData* Fixture = nullptr;

		FItems()
		{
			Deck = NewObject<UStageDeckData>(GetTransientPackage());
			Deck->DisplayName = FText::FromString(TEXT("Test Deck"));
			Truss = NewObject<UStageSpanData>(GetTransientPackage());
			Truss->DisplayName = FText::FromString(TEXT("Test Truss"));
			Truss->Fit = EStageSpanFit::WholeSegments;
			Truss->SegmentLength = 200.f;
			Truss->MaxSegments = 100;
			Fixture = NewObject<UBaseItemData>(GetTransientPackage());
			Fixture->DisplayName = FText::FromString(TEXT("Test Fixture"));
			Fixture->ActorClass = AModularBaseActor::StaticClass();
		}

		FStageSketchBuildRules MakeRules() const
		{
			FStageSketchBuildRules Rules;
			Rules.GridStep = 50.0;
			Rules.bSnapToGrid = true;
			Rules.TrussHeight = 600.0;
			Rules.DeckItem = Deck;
			Rules.TrussItem = Truss;
			Rules.FixtureItem = Fixture;
			return Rules;
		}
	};

	double SumTriangleArea(TConstArrayView<FVector2D> Polygon, TConstArrayView<int32> Triangles, bool& bAllCounterClockwise)
	{
		double Sum = 0.0;
		bAllCounterClockwise = true;
		for (int32 Index = 0; Index + 2 < Triangles.Num(); Index += 3)
		{
			const double Area = StageDeckMath::GetSignedArea({ Polygon[Triangles[Index]], Polygon[Triangles[Index + 1]], Polygon[Triangles[Index + 2]] });
			bAllCounterClockwise &= Area > 0.0;
			Sum += Area;
		}
		return Sum;
	}

	/** Every triangle of Section faces (by the engine's face normal) the way its vertices' normals say. */
	bool FacesMatchNormals(const FStageDeckMesh::FSection& Section)
	{
		for (int32 Index = 0; Index + 2 < Section.Triangles.Num(); Index += 3)
		{
			const int32 A = Section.Triangles[Index], B = Section.Triangles[Index + 1], C = Section.Triangles[Index + 2];
			const FVector Face = StageDeckMath::GetFaceNormal(Section.Vertices[A], Section.Vertices[B], Section.Vertices[C]);
			if ((Face | Section.Normals[A]) < 0.99)
			{
				return false;
			}
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSketchMathTest, "StageCraft.Sketch.Math", StageSketchTests::Flags)

bool FStageSketchMathTest::RunTest(const FString& Parameters)
{
	using namespace StageSketchTests;
	using namespace StageSketchMath;

	// Stabiliser.
	TestEqual(TEXT("Stabilize 0 jumps to the cursor"), Stabilize(FVector2D(0, 0), FVector2D(100, 0), 0.0), FVector2D(100, 0));
	TestEqual(TEXT("Stabilize 0.5 goes half way"), Stabilize(FVector2D(0, 0), FVector2D(100, 0), 0.5), FVector2D(50, 0));
	TestTrue(TEXT("Stabilize is clamped below 1 (never stuck)"), Stabilize(FVector2D(0, 0), FVector2D(100, 0), 5.0).X > 0.0);
	TestEqual(TEXT("Stabilize treats NaN as raw"), Stabilize(FVector2D(0, 0), FVector2D(100, 0), std::numeric_limits<double>::quiet_NaN()), FVector2D(100, 0));

	// Simplification keeps the ends and real corners.
	TArray<FVector2D> Line;
	for (int32 Index = 0; Index <= 10; ++Index)
	{
		Line.Add(FVector2D(Index * 100.0, (Index % 2) * 2.0));
	}
	const TArray<FVector2D> Straight = Simplify(Line, 15.0);
	TestEqual(TEXT("A wobbly straight line simplifies to its two ends"), Straight.Num(), 2);
	TestEqual(TEXT("...the first end"), Straight[0], Line[0]);
	TestEqual(TEXT("...the last end"), Straight.Last(), Line.Last());
	const TArray<FVector2D> Corner = Simplify({ FVector2D(0, 0), FVector2D(250, 1), FVector2D(500, 0), FVector2D(500, 250), FVector2D(500, 500) }, 15.0);
	TestEqual(TEXT("An L keeps its corner"), Corner.Num(), 3);
	TestEqual(TEXT("...at the bend"), Corner[1], FVector2D(500, 0));

	// Grid, segments, distances.
	TestEqual(TEXT("SnapToGrid"), SnapToGrid(FVector2D(74, -26), 50.0), FVector2D(50, -50));
	TestEqual(TEXT("SnapToGrid with no step"), SnapToGrid(FVector2D(74, -26), 0.0), FVector2D(74, -26));
	double Alpha = 0.0;
	TestEqual(TEXT("Closest point on a segment"), ClosestPointOnSegment(FVector2D(0, 0), FVector2D(100, 0), FVector2D(30, 40), Alpha), FVector2D(30, 0));
	TestEqual(TEXT("...at 30 %"), Alpha, 0.3);
	ClosestPointOnSegment(FVector2D(0, 0), FVector2D(100, 0), FVector2D(300, 40), Alpha);
	TestEqual(TEXT("...clamped to the end"), Alpha, 1.0);

	const FStageSketchStroke Square = MakeStroke(EStageSketchStrokeKind::Shape, { FVector2D(0, 0), FVector2D(100, 0), FVector2D(100, 100), FVector2D(0, 100) }, true);
	TestEqual(TEXT("Distance to a closed stroke includes its closing edge"), DistanceToStroke(Square, FVector2D(-10, 50)), 10.0);
	FStageSketchStroke Open = Square;
	Open.bClosed = false;
	TestEqual(TEXT("...an open one does not"), DistanceToStroke(Open, FVector2D(-10, 50)), FVector2D::Distance(FVector2D(-10, 50), FVector2D(0, 100)));
	TestEqual(TEXT("Distance to a fixture marker"), DistanceToStroke(MakeStroke(EStageSketchStrokeKind::Fixture, { FVector2D(3, 4) }), FVector2D(0, 0)), 5.0);

	// Sanitizing strokes.
	{
		TArray<FVector2D> Loop;
		for (int32 Step = 0; Step < 24; ++Step)
		{
			const double Angle = 2.0 * UE_DOUBLE_PI * Step / 24.0;
			Loop.Add(FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * 300.0);
		}
		Loop.Add(FVector2D(290.0, 20.0)); // Ends near the start.
		FStageSketchStroke Freehand = MakeStroke(EStageSketchStrokeKind::Freehand, Loop);
		TestTrue(TEXT("A freehand loop is usable"), SanitizeStroke(Freehand));
		TestTrue(TEXT("...closed"), Freehand.bClosed);
		TestTrue(TEXT("...a deck"), Freehand.GetElement() == EStageSketchElement::Deck);
		TestTrue(TEXT("...simplified"), Freehand.Points.Num() < 24 && Freehand.Points.Num() >= 3);

		FStageSketchStroke Curve = MakeStroke(EStageSketchStrokeKind::Freehand, { FVector2D(0, 0), FVector2D(300, 200), FVector2D(600, 0) });
		TestTrue(TEXT("An open freehand line is usable"), SanitizeStroke(Curve));
		TestFalse(TEXT("...open"), Curve.bClosed);
		TestTrue(TEXT("...a truss"), Curve.GetElement() == EStageSketchElement::Truss);
	}
	{
		FStageSketchStroke TwoCorners = MakeStroke(EStageSketchStrokeKind::Shape, { FVector2D(0, 0), FVector2D(100, 0) });
		TestFalse(TEXT("A shape needs three corners"), SanitizeStroke(TwoCorners));

		const double NaN = std::numeric_limits<double>::quiet_NaN();
		FStageSketchStroke Dirty = MakeStroke(EStageSketchStrokeKind::Shape,
			{ FVector2D(0, 0), FVector2D(NaN, 5), FVector2D(0.2, 0.2), FVector2D(100, 0), FVector2D(1e9, 0), FVector2D(100, 100) }, false, 9999.f);
		TestTrue(TEXT("A shape with bad points keeps the good ones"), SanitizeStroke(Dirty));
		TestEqual(TEXT("...NaN, far-away and duplicate points dropped"), Dirty.Points.Num(), 3);
		TestTrue(TEXT("...always closed"), Dirty.bClosed);
		TestEqual(TEXT("...height clamped"), Dirty.DeckHeight, MaxDeckHeight);

		FStageSketchStroke Loop = MakeStroke(EStageSketchStrokeKind::Truss, { FVector2D(0, 0), FVector2D(400, 0), FVector2D(400, 400), FVector2D(0, 0) });
		TestTrue(TEXT("A truss line back on its start"), SanitizeStroke(Loop));
		TestTrue(TEXT("...is a loop"), Loop.bClosed);
		TestEqual(TEXT("...without the repeated point"), Loop.Points.Num(), 3);

		FStageSketchStroke Fixture = MakeStroke(EStageSketchStrokeKind::Fixture, { FVector2D(10, 10), FVector2D(20, 20) });
		TestTrue(TEXT("A fixture"), SanitizeStroke(Fixture));
		TestEqual(TEXT("...keeps one point"), Fixture.Points.Num(), 1);

		TArray<FVector2D> Many;
		for (int32 Index = 0; Index < MaxStrokePoints + 50; ++Index)
		{
			Many.Add(FVector2D(Index * 10.0, (Index % 3) * 10.0));
		}
		FStageSketchStroke Long = MakeStroke(EStageSketchStrokeKind::Truss, Many);
		TestTrue(TEXT("A very long stroke"), SanitizeStroke(Long));
		TestTrue(TEXT("...is capped"), Long.Points.Num() <= MaxStrokePoints);
	}

	// Settings.
	FStageSketchSettings Wild;
	Wild.DeckHeight = -5.f;
	Wild.TrussHeight = 1e6f;
	Wild.GridStep = std::numeric_limits<float>::quiet_NaN();
	Wild.Smoothing = 2.f;
	const FStageSketchSettings Clean = SanitizeSettings(Wild);
	TestEqual(TEXT("Settings: deck height clamped"), Clean.DeckHeight, MinDeckHeight);
	TestEqual(TEXT("Settings: truss height clamped"), Clean.TrussHeight, MaxTrussHeight);
	TestEqual(TEXT("Settings: NaN grid takes the default"), Clean.GridStep, FStageSketchSettings().GridStep);
	TestEqual(TEXT("Settings: smoothing clamped"), Clean.Smoothing, MaxSmoothing);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSketchDeckTest, "StageCraft.Sketch.Deck", StageSketchTests::Flags)

bool FStageSketchDeckTest::RunTest(const FString& Parameters)
{
	using namespace StageSketchTests;
	using namespace StageDeckMath;

	const TArray<FVector2D> Square = { FVector2D(0, 0), FVector2D(200, 0), FVector2D(200, 200), FVector2D(0, 200) };
	TArray<FVector2D> Clockwise = Square;
	Algo::Reverse(Clockwise);
	TestEqual(TEXT("Counter-clockwise area is positive"), GetSignedArea(Square), 40000.0);
	TestEqual(TEXT("Clockwise area is negative"), GetSignedArea(Clockwise), -40000.0);
	TestEqual(TEXT("Centroid"), GetCentroid(Square), FVector2D(100, 100));

	const TArray<FVector2D> Bowtie = { FVector2D(0, 0), FVector2D(200, 200), FVector2D(200, 0), FVector2D(0, 200) };
	TestTrue(TEXT("A square is simple"), IsSimplePolygon(Square));
	TestFalse(TEXT("A bow tie is not"), IsSimplePolygon(Bowtie));
	TestFalse(TEXT("A spike is not"), IsSimplePolygon({ FVector2D(0, 0), FVector2D(200, 0), FVector2D(100, 0), FVector2D(100, 100) }));
	TestTrue(TEXT("Inside"), IsPointInPolygon(Square, FVector2D(50, 50)));
	TestTrue(TEXT("On an edge counts as inside"), IsPointInPolygon(Square, FVector2D(200, 50)));
	TestFalse(TEXT("Outside"), IsPointInPolygon(Square, FVector2D(250, 50)));

	// Triangulation: convex, concave (an L), either orientation; a bow tie fails.
	TArray<int32> Triangles;
	bool bCounterClockwise = false;
	TestTrue(TEXT("Square triangulates"), Triangulate(Square, Triangles));
	TestEqual(TEXT("...into two triangles"), Triangles.Num(), 6);
	const TArray<FVector2D> L = { FVector2D(0, 0), FVector2D(400, 0), FVector2D(400, 100), FVector2D(100, 100), FVector2D(100, 300), FVector2D(0, 300) };
	TestTrue(TEXT("An L triangulates"), Triangulate(L, Triangles));
	TestEqual(TEXT("...into four triangles"), Triangles.Num(), 12);
	TestEqual(TEXT("...covering exactly its area"), SumTriangleArea(L, Triangles, bCounterClockwise), GetSignedArea(L), 1e-6);
	TestTrue(TEXT("...all counter-clockwise"), bCounterClockwise);
	TArray<FVector2D> ReversedL = L;
	Algo::Reverse(ReversedL);
	TestTrue(TEXT("A clockwise L triangulates"), Triangulate(ReversedL, Triangles));
	TestEqual(TEXT("...covering its area"), SumTriangleArea(ReversedL, Triangles, bCounterClockwise), -GetSignedArea(ReversedL), 1e-6);
	TestTrue(TEXT("...all counter-clockwise"), bCounterClockwise);
	TestFalse(TEXT("A bow tie does not triangulate"), Triangulate(Bowtie, Triangles));
	TestTrue(TEXT("...and leaves no triangles"), Triangles.IsEmpty());

	// Outline repair.
	const double NaN = std::numeric_limits<double>::quiet_NaN();
	TArray<FVector2D> Clean;
	TestTrue(TEXT("A dirty clockwise outline is repaired"), SanitizeOutline(
		{ FVector2D(0, 0), FVector2D(0, 200), FVector2D(0, 200.5), FVector2D(NaN, 3), FVector2D(100, 200), FVector2D(200, 200), FVector2D(200, 0), FVector2D(0.3, 0) }, Clean));
	TestEqual(TEXT("...to its four real corners"), Clean.Num(), 4);
	TestTrue(TEXT("...counter-clockwise"), GetSignedArea(Clean) > 0.0);
	TestFalse(TEXT("A bow tie is refused"), SanitizeOutline(Bowtie, Clean));
	TestFalse(TEXT("A 40 x 40 cm outline is too small"), SanitizeOutline({ FVector2D(0, 0), FVector2D(40, 0), FVector2D(40, 40), FVector2D(0, 40) }, Clean));
	TestFalse(TEXT("Corners 1 km out are dropped (nothing left)"), SanitizeOutline({ FVector2D(1e5, 0), FVector2D(1e5, 100), FVector2D(1e5 + 100, 100) }, Clean));
	TestEqual(TEXT("MakeRectangle is centred and counter-clockwise"), GetSignedArea(MakeRectangle(200, 100)), 20000.0);

	// Panels.
	TestEqual(TEXT("8 m2 of 2 m2 panels"), GetPanelCount(80000.0, 20000.0), 4);
	TestEqual(TEXT("A little over rounds up"), GetPanelCount(80100.0, 20000.0), 5);
	TestEqual(TEXT("Tiny decks need one panel"), GetPanelCount(10.0, 20000.0), 1);
	TestEqual(TEXT("Invalid input needs none"), GetPanelCount(NaN, 20000.0), 0);

	// The mesh: faces point where their normals say, by the engine's own face-normal formula.
	const TArray<FVector2D>* Outlines[] = { &L, &ReversedL };
	for (const TArray<FVector2D>* Outline : Outlines)
	{
		const FStageDeckMesh Mesh = BuildMesh(*Outline, 60.0);
		TestTrue(TEXT("Deck mesh is valid"), Mesh.IsValid());
		TestEqual(TEXT("...top has one triangle per corner minus two"), Mesh.Top.Triangles.Num(), (Outline->Num() - 2) * 3);
		TestEqual(TEXT("...one wall (two triangles) per edge"), Mesh.Skirt.Triangles.Num(), Outline->Num() * 6);
		TestTrue(TEXT("...top faces up"), FacesMatchNormals(Mesh.Top) && Mesh.Top.Normals[0] == FVector::UpVector);
		TestTrue(TEXT("...walls face out"), FacesMatchNormals(Mesh.Skirt));
		// The wall along Y = 0 of the L faces -Y (away from the deck).
		bool bFoundFront = false;
		for (int32 Index = 0; Index < Mesh.Skirt.Vertices.Num(); Index += 4)
		{
			if (FMath::IsNearlyZero(Mesh.Skirt.Vertices[Index].Y) && FMath::IsNearlyZero(Mesh.Skirt.Vertices[Index + 1].Y))
			{
				bFoundFront = true;
				TestTrue(TEXT("...the Y = 0 wall faces -Y"), Mesh.Skirt.Normals[Index].Equals(-FVector::RightVector));
			}
		}
		TestTrue(TEXT("...has a Y = 0 wall"), bFoundFront);
		TestTrue(TEXT("...top at the height"), FMath::IsNearlyEqual(Mesh.Top.Vertices[0].Z, 60.0));
	}
	TestFalse(TEXT("No mesh at zero height"), BuildMesh(Square, 0.0).IsValid());
	TestFalse(TEXT("No mesh for a bow tie"), BuildMesh(Bowtie, 60.0).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSketchPlanTest, "StageCraft.Sketch.Plan", StageSketchTests::Flags)

bool FStageSketchPlanTest::RunTest(const FString& Parameters)
{
	using namespace StageSketchTests;
	const FItems Items;
	const FStageSketchBuildRules Rules = Items.MakeRules();

	TArray<FStageSketchStroke> Strokes;
	// 1. A 4 x 8 m deck drawn slightly off the grid.
	Strokes.Add(MakeStroke(EStageSketchStrokeKind::Shape, { FVector2D(10, -10), FVector2D(410, 5), FVector2D(395, 790), FVector2D(-5, 805) }, true, 60.f));
	// 2. A closed 8 x 6 m truss loop drawn off the section grid.
	Strokes.Add(MakeStroke(EStageSketchStrokeKind::Truss, { FVector2D(5, -1005), FVector2D(795, -990), FVector2D(810, -410), FVector2D(-10, -395) }, true));
	// 3. A fixture near the truss's first leg, 4. one on the deck, 5. one on the floor, 6. a duplicate of 4.
	Strokes.Add(MakeStroke(EStageSketchStrokeKind::Fixture, { FVector2D(410, -975) }));
	Strokes.Add(MakeStroke(EStageSketchStrokeKind::Fixture, { FVector2D(210, 390) }));
	Strokes.Add(MakeStroke(EStageSketchStrokeKind::Fixture, { FVector2D(2010, 1990) }));
	Strokes.Add(MakeStroke(EStageSketchStrokeKind::Fixture, { FVector2D(195, 405) }));
	// 7. A self-crossing shape, 8. a deck taller than the item allows.
	Strokes.Add(MakeStroke(EStageSketchStrokeKind::Shape, { FVector2D(3000, 0), FVector2D(3400, 400), FVector2D(3400, 0), FVector2D(3000, 400) }, true));
	Strokes.Add(MakeStroke(EStageSketchStrokeKind::Shape, { FVector2D(-2000, 0), FVector2D(-1800, 0), FVector2D(-1800, 200), FVector2D(-2000, 200) }, true, 450.f));

	const FStageSketchBuildPlan Plan = StageSketchGenerator::BuildPlan(Strokes, Rules);
	TestEqual(TEXT("Two decks (the bow tie skipped)"), Plan.Decks.Num(), 2);
	TestEqual(TEXT("One truss run"), Plan.Trusses.Num(), 1);
	TestEqual(TEXT("Three fixtures (the duplicate skipped)"), Plan.Fixtures.Num(), 3);
	TestEqual(TEXT("Warnings: bow tie, clamped height, duplicate fixture"), Plan.Warnings.Num(), 3);
	TestTrue(TEXT("Plan keeps the items"), Plan.DeckItem.Get() == Items.Deck && Plan.TrussItem.Get() == Items.Truss && Plan.FixtureItem.Get() == Items.Fixture);

	if (Plan.Decks.Num() == 2 && Plan.Trusses.Num() == 1 && Plan.Fixtures.Num() == 3)
	{
		const FStageSketchDeckPlan& Deck = Plan.Decks[0];
		TestEqual(TEXT("Deck: corners snapped to the 50 cm grid, 32 m2"), Deck.Area, 320000.0, 1e-6);
		TestEqual(TEXT("Deck: 16 panels of 2 m2"), Deck.Panels, 16);
		TestTrue(TEXT("Deck: origin at the centroid on the floor"), Deck.Transform.GetLocation().Equals(FVector(200, 400, 0)));
		TestEqual(TEXT("Deck: its stroke's height"), Deck.Height, 60.0);
		TestEqual(TEXT("Deck: four corners"), Deck.LocalOutline.Num(), 4);
		TestEqual(TEXT("Tall deck: clamped to the item's 2 m"), Plan.Decks[1].Height, 200.0);

		const FStageSketchTrussPlan& Truss = Plan.Trusses[0];
		TestTrue(TEXT("Truss: snapped to the 2 m section grid, so the loop closes"), Truss.bClosed);
		TestEqual(TEXT("Truss: 4 + 3 + 4 + 3 sections"), Truss.SegmentCount, 14);
		TestTrue(TEXT("Truss: at the truss height, from the first corner"), Truss.Transform.GetLocation().Equals(FVector(0, -1000, 600)));

		const FStageSketchFixturePlan& Hung = Plan.Fixtures[0];
		TestTrue(TEXT("Fixture near the truss hangs from it"), Hung.Mount == EStageSketchFixtureMount::Truss);
		TestTrue(TEXT("...projected onto the leg, its position along it on the grid"), Hung.Transform.GetLocation().Equals(FVector(400, -1000, 600)));
		TestTrue(TEXT("...upside down"), FMath::IsNearlyEqual(FMath::Abs(Hung.Transform.Rotator().Roll), 180.0, 0.01));
		const FStageSketchFixturePlan& OnDeck = Plan.Fixtures[1];
		TestTrue(TEXT("Fixture inside the deck stands on it"), OnDeck.Mount == EStageSketchFixtureMount::Deck);
		TestTrue(TEXT("...on the deck top, snapped"), OnDeck.Transform.GetLocation().Equals(FVector(200, 400, 60)));
		const FStageSketchFixturePlan& OnFloor = Plan.Fixtures[2];
		TestTrue(TEXT("Fixture in the open stands on the floor"), OnFloor.Mount == EStageSketchFixtureMount::Floor);
		TestTrue(TEXT("...snapped"), OnFloor.Transform.GetLocation().Equals(FVector(2000, 2000, 0)));
	}

	// Deterministic.
	const FStageSketchBuildPlan Again = StageSketchGenerator::BuildPlan(Strokes, Rules);
	bool bSame = Again.Num() == Plan.Num() && Again.Warnings.Num() == Plan.Warnings.Num();
	for (int32 Index = 0; bSame && Index < Plan.Fixtures.Num(); ++Index)
	{
		bSame = Again.Fixtures[Index].Transform.Equals(Plan.Fixtures[Index].Transform, 0.0);
	}
	TestTrue(TEXT("The same strokes give the same plan"), bSame);

	// An off-grid loop of a whole-section truss with snapping off cannot close.
	{
		FStageSketchBuildRules Free = Rules;
		Free.bSnapToGrid = false;
		const FStageSketchBuildPlan Open = StageSketchGenerator::BuildPlan({ MakeStroke(EStageSketchStrokeKind::Truss,
			{ FVector2D(0, 0), FVector2D(730, 0), FVector2D(730, 470) }, true) }, Free);
		TestEqual(TEXT("Unsnapped loop: still one run"), Open.Trusses.Num(), 1);
		TestTrue(TEXT("...built open"), Open.Trusses.Num() == 1 && !Open.Trusses[0].bClosed);
		TestEqual(TEXT("...with a warning"), Open.Warnings.Num(), 1);
	}

	// Missing items skip their strokes with one warning per element.
	{
		FStageSketchBuildRules NoItems = Rules;
		NoItems.DeckItem = nullptr;
		NoItems.FixtureItem = nullptr;
		const FStageSketchBuildPlan Partial = StageSketchGenerator::BuildPlan(Strokes, NoItems);
		TestEqual(TEXT("No deck or fixture item: only the truss"), Partial.Num(), 1);
		TestEqual(TEXT("...two warnings (decks, fixtures)"), Partial.Warnings.Num(), 2);
		TestTrue(TEXT("...the fixture on the truss still needs a fixture item"), Partial.Fixtures.IsEmpty());
	}

	TestEqual(TEXT("An empty sketch plans nothing"), StageSketchGenerator::BuildPlan({}, Rules).Num(), 0);
	TestFalse(TEXT("DescribePlan names what is built"), StageSketchGenerator::DescribePlan(Plan).IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
