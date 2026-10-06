// Copyright Epic Games, Inc. All Rights Reserved.

// Unit tests for object-to-object snapping (Docs/ADR/0003-undo-redo-and-object-snapping.md).
// No world: StageSnapMath is pure box geometry.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Snap; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Placement/StageSnapMath.h"

namespace StageSnapTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;
	constexpr double Tolerance = 1.e-6;
	constexpr double Threshold = 20.0;

	FBox Box(double MinX, double MinY, double MinZ, double MaxX, double MaxY, double MaxZ)
	{
		return FBox(FVector(MinX, MinY, MinZ), FVector(MaxX, MaxY, MaxZ));
	}

	// A 1 m crate on the floor at the origin.
	const FBox Crate = Box(0, 0, 0, 100, 100, 100);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSnapMathTest, "StageCraft.Snap.Math", StageSnapTests::Flags)

bool FStageSnapMathTest::RunTest(const FString& Parameters)
{
	using namespace StageSnapMath;
	using namespace StageSnapTests;

	// Side by side: 12 cm gap on X, 3 cm off on Y. X closes the gap, Y aligns the edges, Z is already level.
	{
		const FBox Moving = Box(112, 3, 0, 212, 103, 100);
		const FSnapResult Snap = ComputeSnap(Moving, { Crate }, Threshold, AllAxes);
		TestTrue(TEXT("X snaps flush"), Snap.Axes[0].bSnapped && Snap.Axes[0].Anchor == ESnapAnchor::FlushAfter);
		TestEqual(TEXT("X closes the 12 cm gap"), Snap.Axes[0].Delta, -12.0, Tolerance);
		TestEqual(TEXT("X guide on the shared face"), Snap.Axes[0].Plane, 100.0, Tolerance);
		TestTrue(TEXT("Y aligns the min edges (first of equal candidates)"), Snap.Axes[1].bSnapped && Snap.Axes[1].Anchor == ESnapAnchor::AlignMin);
		TestEqual(TEXT("Y moves 3 cm"), Snap.Axes[1].Delta, -3.0, Tolerance);
		TestTrue(TEXT("Z is already aligned (zero delta still counts, for the guide)"), Snap.Axes[2].bSnapped && FMath::IsNearlyZero(Snap.Axes[2].Delta));
		TestTrue(TEXT("Offset combines the axes"), Snap.GetOffset().Equals(FVector(-12.0, -3.0, 0.0), Tolerance));
	}

	// Axis mask: placement snaps sideways only.
	{
		const FSnapResult Snap = ComputeSnap(Box(112, 3, 0, 212, 103, 100), { Crate }, Threshold, HorizontalAxes);
		TestFalse(TEXT("Masked-out Z never snaps"), Snap.Axes[2].bSnapped);
		TestTrue(TEXT("X still snaps"), Snap.Axes[0].bSnapped);
	}

	// Threshold is inclusive; beyond it nothing pulls.
	{
		TestTrue(TEXT("Exactly at the threshold snaps"), ComputeSnap(Box(120, 0, 0, 220, 100, 100), { Crate }, Threshold, AxisBitX).Axes[0].bSnapped);
		TestFalse(TEXT("Just beyond the threshold does not"), ComputeSnap(Box(120.5, 0, 0, 220.5, 100, 100), { Crate }, Threshold, AxisBitX).Axes[0].bSnapped);
	}

	// Relevance: a neighbour far away on another axis never pulls, even when this axis lines up.
	{
		const FBox FarAway = Box(112, 500, 0, 212, 600, 100);
		TestFalse(TEXT("Not relevant across the stage"), IsNeighbourRelevant(FarAway, Crate, 0, Threshold));
		TestFalse(TEXT("No snap to a far item"), ComputeSnap(FarAway, { Crate }, Threshold, AllAxes).AnySnapped());
		TestTrue(TEXT("Within the threshold on the other axes counts"), IsNeighbourRelevant(Box(112, 115, 0, 212, 215, 100), Crate, 0, Threshold));
	}

	// Stacking: 3 cm above the crate, the Z axis lands flush on its top face.
	{
		const FSnapResult Snap = ComputeSnap(Box(5, 0, 103, 105, 100, 203), { Crate }, Threshold, AxisBitZ);
		TestTrue(TEXT("Z snaps flush on top"), Snap.Axes[2].bSnapped && Snap.Axes[2].Anchor == ESnapAnchor::FlushAfter);
		TestEqual(TEXT("Z drops 3 cm"), Snap.Axes[2].Delta, -3.0, Tolerance);
		TestEqual(TEXT("Z plane is the top face"), Snap.Axes[2].Plane, 100.0, Tolerance);
	}

	// Closest neighbour wins; on a tie the earlier neighbour wins (deterministic for a given order).
	{
		const FBox Right = Box(230, 0, 0, 330, 100, 100);
		const FSnapResult Closest = ComputeSnap(Box(112, 0, 0, 212, 100, 100), { Crate, Right }, Threshold, AxisBitX);
		TestEqual(TEXT("12 cm beats 18 cm"), Closest.Axes[0].Neighbour, 0);

		const FBox TiedRight = Box(236, 0, 0, 336, 100, 100);
		const FSnapResult Tie = ComputeSnap(Box(118, 0, 0, 218, 100, 100), { Crate, TiedRight }, Threshold, AxisBitX);
		TestEqual(TEXT("Tie keeps the first neighbour"), Tie.Axes[0].Neighbour, 0);
		TestEqual(TEXT("Tie delta"), Tie.Axes[0].Delta, -18.0, Tolerance);
		const FSnapResult Swapped = ComputeSnap(Box(118, 0, 0, 218, 100, 100), { TiedRight, Crate }, Threshold, AxisBitX);
		TestEqual(TEXT("Order decides a tie"), Swapped.Axes[0].Delta, 18.0, Tolerance);
	}

	// Degenerate input.
	{
		TestFalse(TEXT("Invalid neighbour boxes are skipped"), ComputeSnap(Box(112, 0, 0, 212, 100, 100), { FBox(ForceInit) }, Threshold, AllAxes).AnySnapped());
		TestFalse(TEXT("Zero threshold snaps nothing"), ComputeSnap(Box(100, 0, 0, 200, 100, 100), { Crate }, 0.0, AllAxes).AnySnapped());
		TestFalse(TEXT("Invalid moving box snaps nothing"), ComputeSnap(FBox(ForceInit), { Crate }, Threshold, AllAxes).AnySnapped());
		TestFalse(TEXT("No neighbours, no snap"), ComputeSnap(Box(100, 0, 0, 200, 100, 100), {}, Threshold, AllAxes).AnySnapped());
	}

	// Grid merge: snapped axes come from the free position, the rest from the grid.
	{
		FSnapResult Snap;
		Snap.Axes[0].bSnapped = true;
		Snap.Axes[0].Delta = 5.0;
		const FVector Merged = MergeWithGrid(FVector(112.0, 53.0, 7.0), FVector(100.0, 0.0, 0.0), Snap);
		TestTrue(TEXT("X = free + delta, Y and Z = grid"), Merged.Equals(FVector(117.0, 0.0, 0.0), Tolerance));
	}

	// Guides.
	{
		FVector Start, End;
		MakeGuide(0, 100.0, Box(100, 0, 0, 200, 100, 100), Crate, Start, End);
		TestTrue(TEXT("Side guide starts on the floor at the shared face"), Start.Equals(FVector(100.0, 0.0, 0.0), Tolerance));
		TestTrue(TEXT("Side guide spans both boxes along Y"), End.Equals(FVector(100.0, 100.0, 0.0), Tolerance));

		MakeGuide(2, 100.0, Box(5, 0, 100, 105, 100, 200), Crate, Start, End);
		TestTrue(TEXT("Stack guide on the top face, centred in Y"), Start.Equals(FVector(0.0, 50.0, 100.0), Tolerance));
		TestTrue(TEXT("Stack guide spans both boxes along X"), End.Equals(FVector(105.0, 50.0, 100.0), Tolerance));
	}

	// Signature: zero when idle, stable for the same faces, different for different faces.
	{
		TestEqual(TEXT("Nothing snapped -> zero"), ComputeSnap(Box(500, 500, 0, 600, 600, 100), { Crate }, Threshold, AllAxes).GetSignature(), 0u);
		const uint32 Near = ComputeSnap(Box(112, 0, 0, 212, 100, 100), { Crate }, Threshold, AxisBitX).GetSignature();
		const uint32 Nearer = ComputeSnap(Box(105, 0, 0, 205, 100, 100), { Crate }, Threshold, AxisBitX).GetSignature();
		const uint32 OtherFace = ComputeSnap(Box(-112, 0, 0, -12, 100, 100), { Crate }, Threshold, AxisBitX).GetSignature();
		TestTrue(TEXT("Snapped -> non-zero"), Near != 0u);
		TestEqual(TEXT("Same face at another distance -> same signature"), Near, Nearer);
		TestNotEqual(TEXT("Other face -> new signature"), Near, OtherFace);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
