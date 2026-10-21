// Copyright Epic Games, Inc. All Rights Reserved.

#include "Placement/StageDeckMath.h"

#include "Algo/Reverse.h"

namespace StageDeckMath
{
	namespace
	{
		/** Orientation tolerance in cm²: products below this are treated as collinear. */
		constexpr double OrientationTolerance = 1e-6;

		/** Corners whose edges turn by less than this are collinear and removed (sin of 0.1°). */
		const double CollinearSine = FMath::Sin(FMath::DegreesToRadians(0.1));

		constexpr double CmPerUV = 100.0;

		double Cross(const FVector2D& A, const FVector2D& B)
		{
			return A.X * B.Y - A.Y * B.X;
		}

		/** > 0 when C is left of A -> B. */
		double Orient(const FVector2D& A, const FVector2D& B, const FVector2D& C)
		{
			return Cross(B - A, C - A);
		}

		int32 Sign(double Value)
		{
			return Value > OrientationTolerance ? 1 : (Value < -OrientationTolerance ? -1 : 0);
		}

		/** C lies within the bounding box of A-B (used once C is known to be collinear with them). */
		bool IsWithinBox(const FVector2D& A, const FVector2D& B, const FVector2D& C)
		{
			return C.X >= FMath::Min(A.X, B.X) - UE_KINDA_SMALL_NUMBER && C.X <= FMath::Max(A.X, B.X) + UE_KINDA_SMALL_NUMBER
				&& C.Y >= FMath::Min(A.Y, B.Y) - UE_KINDA_SMALL_NUMBER && C.Y <= FMath::Max(A.Y, B.Y) + UE_KINDA_SMALL_NUMBER;
		}

		/** True when segments A-B and C-D share any point (touching and collinear overlap included). */
		bool SegmentsTouch(const FVector2D& A, const FVector2D& B, const FVector2D& C, const FVector2D& D)
		{
			const int32 O1 = Sign(Orient(A, B, C));
			const int32 O2 = Sign(Orient(A, B, D));
			const int32 O3 = Sign(Orient(C, D, A));
			const int32 O4 = Sign(Orient(C, D, B));
			if (O1 * O2 < 0 && O3 * O4 < 0)
			{
				return true;
			}
			return (O1 == 0 && IsWithinBox(A, B, C)) || (O2 == 0 && IsWithinBox(A, B, D))
				|| (O3 == 0 && IsWithinBox(C, D, A)) || (O4 == 0 && IsWithinBox(C, D, B));
		}

		/** P inside or on triangle A, B, C (counter-clockwise). */
		bool IsInTriangle(const FVector2D& P, const FVector2D& A, const FVector2D& B, const FVector2D& C)
		{
			return Orient(A, B, P) >= -OrientationTolerance && Orient(B, C, P) >= -OrientationTolerance && Orient(C, A, P) >= -OrientationTolerance;
		}

		/** Appends triangle A, B, C (section vertex indices) so that its engine face normal points along Facing. */
		void AddTriangle(FStageDeckMesh::FSection& Section, int32 A, int32 B, int32 C, const FVector& Facing)
		{
			const FVector Normal = GetFaceNormal(Section.Vertices[A], Section.Vertices[B], Section.Vertices[C]);
			if ((Normal | Facing) >= 0.0)
			{
				Section.Triangles.Append({ A, B, C });
			}
			else
			{
				Section.Triangles.Append({ A, C, B });
			}
		}

		/** Removes corners whose two edges are (nearly) collinear, including spikes that fold back. Repeats until none is left. */
		void RemoveCollinear(TArray<FVector2D>& Points)
		{
			bool bRemoved = true;
			while (bRemoved && Points.Num() >= 3)
			{
				bRemoved = false;
				for (int32 Index = 0; Index < Points.Num() && Points.Num() >= 3; ++Index)
				{
					const FVector2D& Previous = Points[(Index + Points.Num() - 1) % Points.Num()];
					const FVector2D& Next = Points[(Index + 1) % Points.Num()];
					const FVector2D In = Points[Index] - Previous;
					const FVector2D Out = Next - Points[Index];
					if (FMath::Abs(Cross(In, Out)) <= CollinearSine * In.Size() * Out.Size())
					{
						Points.RemoveAt(Index);
						bRemoved = true;
						--Index;
					}
				}
			}
		}
	}

	double GetSignedArea(TConstArrayView<FVector2D> Polygon)
	{
		double Twice = 0.0;
		for (int32 Index = 0; Index < Polygon.Num(); ++Index)
		{
			Twice += Cross(Polygon[Index], Polygon[(Index + 1) % Polygon.Num()]);
		}
		return 0.5 * Twice;
	}

	FVector2D GetCentroid(TConstArrayView<FVector2D> Polygon)
	{
		if (Polygon.IsEmpty())
		{
			return FVector2D::ZeroVector;
		}
		// Relative to the first corner, so large coordinates do not cost precision.
		const FVector2D Origin = Polygon[0];
		double Twice = 0.0;
		FVector2D Weighted = FVector2D::ZeroVector;
		for (int32 Index = 0; Index < Polygon.Num(); ++Index)
		{
			const FVector2D A = Polygon[Index] - Origin;
			const FVector2D B = Polygon[(Index + 1) % Polygon.Num()] - Origin;
			const double Term = Cross(A, B);
			Twice += Term;
			Weighted += (A + B) * Term;
		}
		if (FMath::Abs(Twice) <= OrientationTolerance)
		{
			// Degenerate: the average of the corners.
			FVector2D Sum = FVector2D::ZeroVector;
			for (const FVector2D& Point : Polygon)
			{
				Sum += Point;
			}
			return Sum / Polygon.Num();
		}
		return Origin + Weighted / (3.0 * Twice);
	}

	bool IsSimplePolygon(TConstArrayView<FVector2D> Polygon)
	{
		const int32 Count = Polygon.Num();
		if (Count < 3 || FMath::Abs(GetSignedArea(Polygon)) <= OrientationTolerance)
		{
			return false;
		}
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FVector2D& A = Polygon[Index];
			const FVector2D& B = Polygon[(Index + 1) % Count];
			if (FVector2D::DistSquared(A, B) <= FMath::Square(UE_KINDA_SMALL_NUMBER))
			{
				return false;
			}
			// Adjacent edge folding back over this one (a spike).
			const FVector2D& C = Polygon[(Index + 2) % Count];
			if (Sign(Orient(A, B, C)) == 0 && ((B - A) | (C - B)) < 0.0)
			{
				return false;
			}
			for (int32 Other = Index + 2; Other < Count; ++Other)
			{
				if (Index == 0 && Other == Count - 1)
				{
					continue; // The closing edge is adjacent to the first.
				}
				if (SegmentsTouch(A, B, Polygon[Other], Polygon[(Other + 1) % Count]))
				{
					return false;
				}
			}
		}
		return true;
	}

	bool IsPointInPolygon(TConstArrayView<FVector2D> Polygon, const FVector2D& Point)
	{
		bool bInside = false;
		for (int32 Index = 0, Previous = Polygon.Num() - 1; Index < Polygon.Num(); Previous = Index++)
		{
			const FVector2D& A = Polygon[Index];
			const FVector2D& B = Polygon[Previous];
			if (Sign(Orient(A, B, Point)) == 0 && IsWithinBox(A, B, Point))
			{
				return true;
			}
			if ((A.Y > Point.Y) != (B.Y > Point.Y) && Point.X < (B.X - A.X) * (Point.Y - A.Y) / (B.Y - A.Y) + A.X)
			{
				bInside = !bInside;
			}
		}
		return bInside;
	}

	bool Triangulate(TConstArrayView<FVector2D> Polygon, TArray<int32>& OutTriangles)
	{
		OutTriangles.Reset();
		if (!IsSimplePolygon(Polygon))
		{
			return false;
		}

		TArray<int32> Remaining;
		Remaining.Reserve(Polygon.Num());
		for (int32 Index = 0; Index < Polygon.Num(); ++Index)
		{
			Remaining.Add(Index);
		}
		if (GetSignedArea(Polygon) < 0.0)
		{
			Algo::Reverse(Remaining);
		}

		OutTriangles.Reserve((Polygon.Num() - 2) * 3);
		while (Remaining.Num() > 3)
		{
			bool bClipped = false;
			for (int32 Slot = 0; Slot < Remaining.Num(); ++Slot)
			{
				const int32 Previous = Remaining[(Slot + Remaining.Num() - 1) % Remaining.Num()];
				const int32 Current = Remaining[Slot];
				const int32 Next = Remaining[(Slot + 1) % Remaining.Num()];
				const FVector2D& A = Polygon[Previous];
				const FVector2D& B = Polygon[Current];
				const FVector2D& C = Polygon[Next];
				if (Orient(A, B, C) <= OrientationTolerance)
				{
					continue; // Reflex or flat: not an ear.
				}
				bool bContainsOther = false;
				for (const int32 Other : Remaining)
				{
					if (Other != Previous && Other != Current && Other != Next && IsInTriangle(Polygon[Other], A, B, C))
					{
						bContainsOther = true;
						break;
					}
				}
				if (!bContainsOther)
				{
					OutTriangles.Append({ Previous, Current, Next });
					Remaining.RemoveAt(Slot);
					bClipped = true;
					break;
				}
			}
			if (!bClipped)
			{
				// Only reachable through numeric trouble; a simple polygon always has an ear.
				OutTriangles.Reset();
				return false;
			}
		}
		OutTriangles.Append({ Remaining[0], Remaining[1], Remaining[2] });
		return true;
	}

	bool SanitizeOutline(TConstArrayView<FVector2D> Outline, TArray<FVector2D>& OutOutline)
	{
		OutOutline.Reset();
		for (const FVector2D& Point : Outline)
		{
			if (!FMath::IsFinite(Point.X) || !FMath::IsFinite(Point.Y) || FMath::Abs(Point.X) > MaxExtent || FMath::Abs(Point.Y) > MaxExtent)
			{
				continue;
			}
			if (!OutOutline.IsEmpty() && FVector2D::Distance(OutOutline.Last(), Point) < MinEdgeLength)
			{
				continue;
			}
			OutOutline.Add(Point);
		}
		while (OutOutline.Num() >= 2 && FVector2D::Distance(OutOutline.Last(), OutOutline[0]) < MinEdgeLength)
		{
			OutOutline.Pop();
		}
		RemoveCollinear(OutOutline);
		if (OutOutline.Num() > MaxOutlinePoints)
		{
			OutOutline.SetNum(MaxOutlinePoints);
			RemoveCollinear(OutOutline);
		}
		if (GetSignedArea(OutOutline) < 0.0)
		{
			Algo::Reverse(OutOutline);
		}
		if (!IsSimplePolygon(OutOutline) || GetSignedArea(OutOutline) < MinArea)
		{
			OutOutline.Reset();
			return false;
		}
		return true;
	}

	TArray<FVector2D> MakeRectangle(double Width, double Depth)
	{
		const double HalfX = 0.5 * Width;
		const double HalfY = 0.5 * Depth;
		return { FVector2D(-HalfX, -HalfY), FVector2D(HalfX, -HalfY), FVector2D(HalfX, HalfY), FVector2D(-HalfX, HalfY) };
	}

	int32 GetPanelCount(double AreaCm2, double PanelAreaCm2)
	{
		if (!FMath::IsFinite(AreaCm2) || !FMath::IsFinite(PanelAreaCm2) || AreaCm2 <= 0.0 || PanelAreaCm2 <= 0.0)
		{
			return 0;
		}
		// The small allowance keeps an exact multiple (8 m² of 2 m² panels) from rounding up through float noise.
		const double Panels = FMath::CeilToDouble(AreaCm2 / PanelAreaCm2 - 1e-6);
		return FMath::Clamp(static_cast<int32>(FMath::Min(Panels, double(MAX_int32))), 1, MAX_int32);
	}

	FVector GetFaceNormal(const FVector& P0, const FVector& P1, const FVector& P2)
	{
		return ((P1 - P2) ^ (P0 - P2)).GetSafeNormal();
	}

	FStageDeckMesh BuildMesh(TConstArrayView<FVector2D> Outline, double Height)
	{
		FStageDeckMesh Mesh;
		TArray<int32> Triangles;
		if (!FMath::IsFinite(Height) || Height <= 0.0 || !Triangulate(Outline, Triangles))
		{
			return Mesh;
		}

		// Walk the outline counter-clockwise so each wall's outward side is the right of its edge.
		TArray<FVector2D> Corners(Outline);
		if (GetSignedArea(Corners) < 0.0)
		{
			Algo::Reverse(Corners);
			for (int32& Index : Triangles)
			{
				Index = Corners.Num() - 1 - Index;
			}
		}

		FStageDeckMesh::FSection& Top = Mesh.Top;
		Top.Vertices.Reserve(Corners.Num());
		for (const FVector2D& Corner : Corners)
		{
			Top.Vertices.Add(FVector(Corner.X, Corner.Y, Height));
			Top.Normals.Add(FVector::UpVector);
			Top.UVs.Add(Corner / CmPerUV);
		}
		for (int32 Index = 0; Index + 2 < Triangles.Num(); Index += 3)
		{
			AddTriangle(Top, Triangles[Index], Triangles[Index + 1], Triangles[Index + 2], FVector::UpVector);
		}

		FStageDeckMesh::FSection& Skirt = Mesh.Skirt;
		Skirt.Vertices.Reserve(Corners.Num() * 4);
		double Perimeter = 0.0;
		for (int32 Index = 0; Index < Corners.Num(); ++Index)
		{
			const FVector2D& A = Corners[Index];
			const FVector2D& B = Corners[(Index + 1) % Corners.Num()];
			const FVector2D Edge = B - A;
			const double EdgeLength = Edge.Size();
			const FVector Outward = FVector(Edge.Y, -Edge.X, 0.0).GetSafeNormal();
			const double U0 = Perimeter / CmPerUV;
			const double U1 = (Perimeter + EdgeLength) / CmPerUV;
			const double V = Height / CmPerUV;
			Perimeter += EdgeLength;

			const int32 Base = Skirt.Vertices.Num();
			Skirt.Vertices.Append({ FVector(A.X, A.Y, 0.0), FVector(B.X, B.Y, 0.0), FVector(B.X, B.Y, Height), FVector(A.X, A.Y, Height) });
			Skirt.Normals.Append({ Outward, Outward, Outward, Outward });
			Skirt.UVs.Append({ FVector2D(U0, V), FVector2D(U1, V), FVector2D(U1, 0.0), FVector2D(U0, 0.0) });
			AddTriangle(Skirt, Base, Base + 1, Base + 2, Outward);
			AddTriangle(Skirt, Base, Base + 2, Base + 3, Outward);
		}
		return Mesh;
	}
}
