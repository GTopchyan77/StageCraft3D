// Copyright Epic Games, Inc. All Rights Reserved.

#include "Placement/StageProceduralShapes.h"

int32 FStageShapeMesh::GetTriangleCount() const
{
	int32 Count = 0;
	for (const FStageDeckMesh::FSection& Section : Sections)
	{
		Count += Section.Triangles.Num() / 3;
	}
	return Count;
}

bool FStageShapeMesh::IsValid() const
{
	return !Sections.IsEmpty() && !Sections[0].IsEmpty();
}

FVector2D FStageArcTrussLayout::GetPoint(double Fraction) const
{
	const FVector2D Out = GetOutward(Fraction);
	return FVector2D(0.0, -Radius) + Out * Radius;
}

FVector2D FStageArcTrussLayout::GetOutward(double Fraction) const
{
	const double Theta = (FMath::Clamp(Fraction, 0.0, 1.0) - 0.5) * FMath::DegreesToRadians(AngleDegrees);
	return FVector2D(FMath::Sin(Theta), FMath::Cos(Theta));
}

namespace StageProceduralShapes
{
	namespace
	{
		/** Texture coordinates in metres, as on decks. */
		constexpr double CmPerUV = 100.0;

		constexpr double MinStairsRise = 10.0;
		constexpr double MaxStairsRise = 300.0;
		constexpr double MinStairsWidth = 40.0;
		constexpr double MaxStairsWidth = 600.0;
		constexpr double MinTread = 20.0;
		constexpr double MaxTread = 60.0;
		constexpr double MinStepRise = 10.0;
		constexpr double MaxStepRise = 25.0;
		constexpr double HandrailSize = 4.0;
		constexpr double HandrailInset = 3.0;

		constexpr double MinArcRadius = 50.0;
		constexpr double MaxArcRadius = 5000.0;
		constexpr double MinArcAngle = 5.0;
		constexpr double MaxArcAngle = 360.0;
		/** Chord samples at least this often along the arc, in degrees. */
		constexpr double ArcSampleDegrees = 3.0;
		constexpr int32 TubeSides = 8;
		constexpr int32 BraceSides = 6;

		double Clean(double Value, double Min, double Max, double Fallback)
		{
			return FMath::IsFinite(Value) ? FMath::Clamp(Value, Min, Max) : Fallback;
		}

		/** Appends a triangle whose engine face normal points along Facing (the deck builder's rule). */
		void AddTriangle(FStageDeckMesh::FSection& Section, int32 A, int32 B, int32 C, const FVector& Facing)
		{
			const FVector Normal = StageDeckMath::GetFaceNormal(Section.Vertices[A], Section.Vertices[B], Section.Vertices[C]);
			if ((Normal | Facing) >= 0.0)
			{
				Section.Triangles.Append({ A, B, C });
			}
			else
			{
				Section.Triangles.Append({ A, C, B });
			}
		}

		/** One flat quad (corners in order around it) facing Normal. */
		void AddQuad(FStageDeckMesh::FSection& Section, const FVector (&Corners)[4], const FVector& Normal)
		{
			const int32 Base = Section.Vertices.Num();
			// Planar texture coordinates along the face's two edges, in metres.
			const FVector U = (Corners[1] - Corners[0]);
			const FVector V = (Corners[3] - Corners[0]);
			const FVector2D Size(U.Size() / CmPerUV, V.Size() / CmPerUV);
			const FVector2D UVs[4] = { FVector2D(0.0, Size.Y), FVector2D(Size.X, Size.Y), FVector2D(Size.X, 0.0), FVector2D(0.0, 0.0) };
			for (int32 Corner = 0; Corner < 4; ++Corner)
			{
				Section.Vertices.Add(Corners[Corner]);
				Section.Normals.Add(Normal);
				Section.UVs.Add(UVs[Corner]);
			}
			AddTriangle(Section, Base, Base + 1, Base + 2, Normal);
			AddTriangle(Section, Base, Base + 2, Base + 3, Normal);
		}

		/** A box spanned by Center ± AxisX·Half.X ± AxisY·Half.Y ± AxisZ·Half.Z (unit, orthogonal axes). */
		void AddOrientedBox(FStageDeckMesh::FSection& Section, const FVector& Center, const FVector& AxisX, const FVector& AxisY, const FVector& AxisZ, const FVector& Half)
		{
			const auto P = [&](double SX, double SY, double SZ)
			{
				return Center + AxisX * (SX * Half.X) + AxisY * (SY * Half.Y) + AxisZ * (SZ * Half.Z);
			};
			// Each face: its four corners around the face, and its outward normal.
			AddQuad(Section, { P(-1, -1, 1), P(1, -1, 1), P(1, 1, 1), P(-1, 1, 1) }, AxisZ);
			AddQuad(Section, { P(-1, 1, -1), P(1, 1, -1), P(1, -1, -1), P(-1, -1, -1) }, -AxisZ);
			AddQuad(Section, { P(1, -1, -1), P(1, 1, -1), P(1, 1, 1), P(1, -1, 1) }, AxisX);
			AddQuad(Section, { P(-1, 1, -1), P(-1, -1, -1), P(-1, -1, 1), P(-1, 1, 1) }, -AxisX);
			AddQuad(Section, { P(-1, 1, -1), P(1, 1, -1), P(1, 1, 1), P(-1, 1, 1) }, AxisY);
			AddQuad(Section, { P(1, -1, -1), P(-1, -1, -1), P(-1, -1, 1), P(1, -1, 1) }, -AxisY);
		}

		void AddBox(FStageDeckMesh::FSection& Section, const FVector& Min, const FVector& Max)
		{
			AddOrientedBox(Section, (Min + Max) * 0.5, FVector::ForwardVector, FVector::RightVector, FVector::UpVector, (Max - Min) * 0.5);
		}

		/** Two unit vectors perpendicular to Direction and to each other. */
		void MakeFrame(const FVector& Direction, FVector& OutA, FVector& OutB)
		{
			const FVector Reference = FMath::Abs(Direction.Z) < 0.9 ? FVector::UpVector : FVector::ForwardVector;
			OutA = (Direction ^ Reference).GetSafeNormal();
			OutB = (OutA ^ Direction).GetSafeNormal();
		}

		/** A ring of Sides vertices around Center in the plane of A and B; returns the first index. */
		int32 AddRing(FStageDeckMesh::FSection& Section, const FVector& Center, const FVector& A, const FVector& B, double Radius, int32 Sides, double V)
		{
			const int32 Base = Section.Vertices.Num();
			for (int32 Side = 0; Side < Sides; ++Side)
			{
				const double Angle = 2.0 * UE_DOUBLE_PI * Side / Sides;
				const FVector Normal = A * FMath::Cos(Angle) + B * FMath::Sin(Angle);
				Section.Vertices.Add(Center + Normal * Radius);
				Section.Normals.Add(Normal);
				Section.UVs.Add(FVector2D(double(Side) / Sides, V));
			}
			return Base;
		}

		/** The side wall between two rings of the same size; faces point away from the tube's axis. */
		void ConnectRings(FStageDeckMesh::FSection& Section, int32 First, int32 Second, int32 Sides)
		{
			for (int32 Side = 0; Side < Sides; ++Side)
			{
				const int32 Next = (Side + 1) % Sides;
				const FVector Outward = Section.Normals[First + Side] + Section.Normals[First + Next];
				AddTriangle(Section, First + Side, First + Next, Second + Next, Outward);
				AddTriangle(Section, First + Side, Second + Next, Second + Side, Outward);
			}
		}

		/** A flat cap over a ring, facing Facing (the open end of a chord at the end of an arc). */
		void CapRing(FStageDeckMesh::FSection& Section, int32 Ring, int32 Sides, const FVector& Center, const FVector& Facing)
		{
			const int32 Hub = Section.Vertices.Num();
			Section.Vertices.Add(Center);
			Section.Normals.Add(Facing);
			Section.UVs.Add(FVector2D(0.5, 0.5));
			for (int32 Side = 0; Side < Sides; ++Side)
			{
				AddTriangle(Section, Hub, Ring + Side, Ring + (Side + 1) % Sides, Facing);
			}
		}

		/** A straight tube from A to B, open at both ends (braces end inside the chords). */
		void AddCylinder(FStageDeckMesh::FSection& Section, const FVector& A, const FVector& B, double Radius)
		{
			const FVector Direction = (B - A).GetSafeNormal();
			if (Direction.IsNearlyZero())
			{
				return;
			}
			FVector FrameA, FrameB;
			MakeFrame(Direction, FrameA, FrameB);
			const int32 First = AddRing(Section, A, FrameA, FrameB, Radius, BraceSides, 0.0);
			const int32 Second = AddRing(Section, B, FrameA, FrameB, Radius, BraceSides, FVector::Dist(A, B) / CmPerUV);
			ConnectRings(Section, First, Second, BraceSides);
		}

		/** The four chord centre lines' offsets from the arc line: (outward, up), counter-clockwise around the profile. */
		const FVector2D ChordOffsets[4] = { FVector2D(-0.5, -0.5), FVector2D(0.5, -0.5), FVector2D(0.5, 0.5), FVector2D(-0.5, 0.5) };

		FVector GetChordPoint(const FStageArcTrussLayout& Layout, int32 Chord, double Fraction)
		{
			const FVector2D Point = Layout.GetPoint(Fraction) + Layout.GetOutward(Fraction) * (ChordOffsets[Chord].X * Layout.ProfileWidth);
			return FVector(Point.X, Point.Y, ChordOffsets[Chord].Y * Layout.ProfileWidth);
		}

		FVector GetTangent(const FStageArcTrussLayout& Layout, double Fraction)
		{
			const FVector2D Out = Layout.GetOutward(Fraction);
			return FVector(Out.Y, -Out.X, 0.0);
		}
	}

	int32 GetCircleSegments(double Radius, double MaxSagittaCm)
	{
		if (!FMath::IsFinite(Radius) || Radius <= 0.0)
		{
			return 0;
		}
		const double Sagitta = FMath::Clamp(FMath::IsFinite(MaxSagittaCm) ? MaxSagittaCm : 1.0, 0.01, Radius);
		// The chord of an n-gon strays R·(1 − cos(π/n)) from the circle.
		const double Needed = UE_DOUBLE_PI / FMath::Acos(FMath::Clamp(1.0 - Sagitta / Radius, -1.0, 1.0));
		return FMath::Clamp(FMath::CeilToInt(Needed), 16, 96);
	}

	TArray<FVector2D> MakeCircle(double Radius)
	{
		TArray<FVector2D> Points;
		const int32 Segments = GetCircleSegments(Radius);
		Points.Reserve(Segments);
		for (int32 Index = 0; Index < Segments; ++Index)
		{
			const double Angle = 2.0 * UE_DOUBLE_PI * Index / Segments;
			Points.Add(FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * Radius);
		}
		return Points;
	}

	TArray<FVector2D> MakeArcRing(double InnerRadius, double OuterRadius, double AngleDegrees)
	{
		TArray<FVector2D> Points;
		if (!FMath::IsFinite(InnerRadius) || !FMath::IsFinite(OuterRadius) || !FMath::IsFinite(AngleDegrees)
			|| OuterRadius <= 0.0 || InnerRadius < 0.0 || InnerRadius >= OuterRadius)
		{
			return Points;
		}
		const double Angle = FMath::DegreesToRadians(FMath::Clamp(AngleDegrees, 5.0, 359.0));
		const int32 OuterSteps = FMath::Max(4, FMath::CeilToInt(GetCircleSegments(OuterRadius) * Angle / (2.0 * UE_DOUBLE_PI)));
		const int32 InnerSteps = InnerRadius > 0.0 ? FMath::Max(2, FMath::CeilToInt(GetCircleSegments(InnerRadius) * Angle / (2.0 * UE_DOUBLE_PI))) : 0;
		Points.Reserve(OuterSteps + InnerSteps + 2);
		for (int32 Step = 0; Step <= OuterSteps; ++Step)
		{
			const double A = -0.5 * Angle + Angle * Step / OuterSteps;
			Points.Add(FVector2D(FMath::Cos(A), FMath::Sin(A)) * OuterRadius);
		}
		if (InnerSteps == 0)
		{
			Points.Add(FVector2D::ZeroVector);
			return Points;
		}
		for (int32 Step = InnerSteps; Step >= 0; --Step)
		{
			const double A = -0.5 * Angle + Angle * Step / InnerSteps;
			Points.Add(FVector2D(FMath::Cos(A), FMath::Sin(A)) * InnerRadius);
		}
		return Points;
	}

	FStageStairsLayout LayoutStairs(double TotalRise, double Width, double Tread, double MaxRise)
	{
		FStageStairsLayout Layout;
		Layout.TotalRise = Clean(TotalRise, MinStairsRise, MaxStairsRise, 60.0);
		Layout.Width = Clean(Width, MinStairsWidth, MaxStairsWidth, 120.0);
		Layout.Tread = Clean(Tread, MinTread, MaxTread, 30.0);
		const double StepLimit = Clean(MaxRise, MinStepRise, MaxStepRise, 20.0);
		// A whisker of tolerance, so 60 cm in 20 cm steps is 3 steps and not 4.
		Layout.Steps = FMath::Max(1, FMath::CeilToInt(Layout.TotalRise / StepLimit - 1e-6));
		Layout.StepRise = Layout.TotalRise / Layout.Steps;
		Layout.Run = Layout.Steps * Layout.Tread;
		return Layout;
	}

	FStageShapeMesh BuildStairsMesh(const FStageStairsLayout& Layout, bool bHandrails, double HandrailHeight)
	{
		FStageShapeMesh Mesh;
		if (!Layout.IsValid())
		{
			return Mesh;
		}
		const double HalfWidth = Layout.Width * 0.5;
		const double Start = -Layout.Run * 0.5;
		FStageDeckMesh::FSection& Steps = Mesh.Sections.AddDefaulted_GetRef();
		for (int32 Step = 0; Step < Layout.Steps; ++Step)
		{
			// Solid from the floor: the blocks share their inner walls, so only the visible stair surface shows.
			AddBox(Steps, FVector(Start + Step * Layout.Tread, -HalfWidth, 0.0), FVector(Start + (Step + 1) * Layout.Tread, HalfWidth, Layout.StepRise * (Step + 1)));
		}

		if (!bHandrails)
		{
			return Mesh;
		}
		FStageDeckMesh::FSection& Rails = Mesh.Sections.AddDefaulted_GetRef();
		const double Height = Clean(HandrailHeight, 60.0, 120.0, 90.0);
		const double Half = HandrailSize * 0.5;
		const FVector PostHalf(Half, Half, Height * 0.5);
		for (const double Side : { -1.0, 1.0 })
		{
			const double Y = Side * (HalfWidth - HandrailInset - Half);
			// Posts stand on the first and the last tread, at the middle of each.
			const FVector Bottom(Start + Layout.Tread * 0.5, Y, Layout.StepRise);
			const FVector Top(Start + Layout.Run - Layout.Tread * 0.5, Y, Layout.TotalRise);
			AddOrientedBox(Rails, Bottom + FVector(0.0, 0.0, Height * 0.5), FVector::ForwardVector, FVector::RightVector, FVector::UpVector, PostHalf);
			if (Layout.Steps > 1)
			{
				AddOrientedBox(Rails, Top + FVector(0.0, 0.0, Height * 0.5), FVector::ForwardVector, FVector::RightVector, FVector::UpVector, PostHalf);
				// The rail runs over both posts along the pitch line.
				const FVector From = Bottom + FVector(0.0, 0.0, Height);
				const FVector To = Top + FVector(0.0, 0.0, Height);
				const FVector Along = (To - From).GetSafeNormal();
				AddOrientedBox(Rails, (From + To) * 0.5, Along, FVector::RightVector, (Along ^ FVector::RightVector).GetSafeNormal(),
					FVector(FVector::Dist(From, To) * 0.5 + Half, Half, Half));
			}
		}
		return Mesh;
	}

	FStageArcTrussLayout LayoutArcTruss(double Radius, double AngleDegrees, double ProfileWidth, double SectionLength)
	{
		FStageArcTrussLayout Layout;
		Layout.Radius = Clean(Radius, MinArcRadius, MaxArcRadius, 400.0);
		Layout.AngleDegrees = Clean(AngleDegrees, MinArcAngle, MaxArcAngle, 90.0);
		Layout.ProfileWidth = Clean(ProfileWidth, 10.0, 100.0, 29.0);
		const double Section = Clean(SectionLength, 50.0, 1000.0, 200.0);
		Layout.ArcLength = Layout.Radius * FMath::DegreesToRadians(Layout.AngleDegrees);
		Layout.Sections = FMath::Max(1, FMath::CeilToInt(Layout.ArcLength / Section - 1e-6));
		Layout.Bays = FMath::Max(1, FMath::RoundToInt(Layout.ArcLength / Layout.ProfileWidth));
		// Chords are sampled at every bay boundary and in between, at least every ArcSampleDegrees, so the curve is smooth.
		const int32 PerBay = FMath::Max(1, FMath::CeilToInt(Layout.AngleDegrees / Layout.Bays / ArcSampleDegrees));
		Layout.Samples = Layout.Bays * PerBay + 1;
		return Layout;
	}

	FStageShapeMesh BuildArcTrussMesh(const FStageArcTrussLayout& Layout, double ChordDiameter, double BraceDiameter)
	{
		FStageShapeMesh Mesh;
		if (!Layout.IsValid())
		{
			return Mesh;
		}
		const double ChordRadius = Clean(ChordDiameter, 1.0, 20.0, 5.0) * 0.5;
		const double BraceRadius = Clean(BraceDiameter, 0.5, 10.0, 2.0) * 0.5;
		// ProfileWidth is the outside width (F34: 29 cm), so the chord centre lines sit one chord radius inside it.
		FStageArcTrussLayout Centres = Layout;
		Centres.ProfileWidth = FMath::Max(Layout.ProfileWidth - 2.0 * ChordRadius, 2.0 * ChordRadius);
		FStageDeckMesh::FSection& Truss = Mesh.Sections.AddDefaulted_GetRef();

		for (int32 Chord = 0; Chord < 4; ++Chord)
		{
			int32 Previous = INDEX_NONE;
			for (int32 Sample = 0; Sample < Layout.Samples; ++Sample)
			{
				const double Fraction = double(Sample) / (Layout.Samples - 1);
				const FVector Tangent = GetTangent(Layout, Fraction);
				const FVector2D Out = Layout.GetOutward(Fraction);
				const int32 Ring = AddRing(Truss, GetChordPoint(Centres, Chord, Fraction), FVector(Out.X, Out.Y, 0.0), FVector::UpVector,
					ChordRadius, TubeSides, Fraction * Layout.ArcLength / CmPerUV);
				if (Previous != INDEX_NONE)
				{
					ConnectRings(Truss, Previous, Ring, TubeSides);
				}
				Previous = Ring;
				if (!Layout.IsFullCircle() && (Sample == 0 || Sample == Layout.Samples - 1))
				{
					CapRing(Truss, Ring, TubeSides, GetChordPoint(Centres, Chord, Fraction), Sample == 0 ? -Tangent : Tangent);
				}
			}
		}

		for (int32 Bay = 0; Bay <= Layout.Bays; ++Bay)
		{
			const double Fraction = double(Bay) / Layout.Bays;
			// A full circle's last frame is its first: no second set of rungs on top of the first.
			const bool bRungs = !(Layout.IsFullCircle() && Bay == Layout.Bays);
			for (int32 Face = 0; Face < 4; ++Face)
			{
				const int32 Next = (Face + 1) % 4;
				if (bRungs)
				{
					AddCylinder(Truss, GetChordPoint(Centres, Face, Fraction), GetChordPoint(Centres, Next, Fraction), BraceRadius);
				}
				if (Bay < Layout.Bays)
				{
					// Zigzag: alternate bays lean the other way, as on a welded truss.
					const double NextFraction = double(Bay + 1) / Layout.Bays;
					const bool bLean = (Bay % 2) == 0;
					AddCylinder(Truss, GetChordPoint(Centres, bLean ? Face : Next, Fraction), GetChordPoint(Centres, bLean ? Next : Face, NextFraction), BraceRadius);
				}
			}
		}
		return Mesh;
	}

	TArray<TArray<FVector>> BuildArcTrussCollision(const FStageArcTrussLayout& Layout)
	{
		TArray<TArray<FVector>> Hulls;
		if (!Layout.IsValid())
		{
			return Hulls;
		}
		Hulls.Reserve(Layout.Sections);
		for (int32 Section = 0; Section < Layout.Sections; ++Section)
		{
			TArray<FVector>& Hull = Hulls.AddDefaulted_GetRef();
			// Both ends and the middle: an arc section bulges outward between its ends.
			for (const double Step : { 0.0, 0.5, 1.0 })
			{
				const double Fraction = (Section + Step) / Layout.Sections;
				for (int32 Chord = 0; Chord < 4; ++Chord)
				{
					Hull.Add(GetChordPoint(Layout, Chord, Fraction));
				}
			}
		}
		return Hulls;
	}
}
