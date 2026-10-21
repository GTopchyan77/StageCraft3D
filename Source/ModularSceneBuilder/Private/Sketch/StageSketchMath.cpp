// Copyright Epic Games, Inc. All Rights Reserved.

#include "Sketch/StageSketchMath.h"

namespace StageSketchMath
{
	namespace
	{
		/** Distance of Point to the infinite line through A and B (to A when they coincide). */
		double DistanceToLine(const FVector2D& A, const FVector2D& B, const FVector2D& Point)
		{
			const FVector2D Direction = B - A;
			const double Length = Direction.Size();
			if (Length <= UE_KINDA_SMALL_NUMBER)
			{
				return FVector2D::Distance(A, Point);
			}
			return FMath::Abs(Direction.X * (Point.Y - A.Y) - Direction.Y * (Point.X - A.X)) / Length;
		}

		bool IsUsablePoint(const FVector2D& Point)
		{
			return FMath::IsFinite(Point.X) && FMath::IsFinite(Point.Y) && FMath::Abs(Point.X) <= MaxCoordinate && FMath::Abs(Point.Y) <= MaxCoordinate;
		}

		float ClampOrDefault(float Value, float Min, float Max, float Default)
		{
			return FMath::IsFinite(Value) ? FMath::Clamp(Value, Min, Max) : Default;
		}
	}

	FVector2D Stabilize(const FVector2D& Previous, const FVector2D& Raw, double Strength)
	{
		const double Clamped = FMath::IsFinite(Strength) ? FMath::Clamp(Strength, 0.0, double(MaxSmoothing)) : 0.0;
		return Previous + (Raw - Previous) * (1.0 - Clamped);
	}

	TArray<FVector2D> Simplify(TConstArrayView<FVector2D> Points, double Tolerance)
	{
		if (Points.Num() <= 2 || !(Tolerance > 0.0))
		{
			return TArray<FVector2D>(Points);
		}

		// Iterative, so a long stroke cannot exhaust the stack.
		TArray<bool> Keep;
		Keep.Init(false, Points.Num());
		Keep[0] = Keep.Last() = true;
		TArray<TPair<int32, int32>, TInlineAllocator<32>> Ranges;
		Ranges.Emplace(0, Points.Num() - 1);
		while (!Ranges.IsEmpty())
		{
			const TPair<int32, int32> Range = Ranges.Pop(EAllowShrinking::No);
			double Farthest = -1.0;
			int32 FarthestIndex = INDEX_NONE;
			for (int32 Index = Range.Key + 1; Index < Range.Value; ++Index)
			{
				const double Distance = DistanceToLine(Points[Range.Key], Points[Range.Value], Points[Index]);
				if (Distance > Farthest)
				{
					Farthest = Distance;
					FarthestIndex = Index;
				}
			}
			if (FarthestIndex != INDEX_NONE && Farthest > Tolerance)
			{
				Keep[FarthestIndex] = true;
				Ranges.Emplace(Range.Key, FarthestIndex);
				Ranges.Emplace(FarthestIndex, Range.Value);
			}
		}

		TArray<FVector2D> Result;
		for (int32 Index = 0; Index < Points.Num(); ++Index)
		{
			if (Keep[Index])
			{
				Result.Add(Points[Index]);
			}
		}
		return Result;
	}

	FVector2D SnapToGrid(const FVector2D& Point, double Step)
	{
		if (!FMath::IsFinite(Step) || Step <= 0.0)
		{
			return Point;
		}
		return FVector2D(FMath::RoundToDouble(Point.X / Step) * Step, FMath::RoundToDouble(Point.Y / Step) * Step);
	}

	FVector2D ClosestPointOnSegment(const FVector2D& A, const FVector2D& B, const FVector2D& Point, double& OutAlpha)
	{
		const FVector2D Direction = B - A;
		const double LengthSquared = Direction.SizeSquared();
		OutAlpha = LengthSquared > UE_SMALL_NUMBER ? FMath::Clamp(((Point - A) | Direction) / LengthSquared, 0.0, 1.0) : 0.0;
		return A + Direction * OutAlpha;
	}

	double DistanceToStroke(const FStageSketchStroke& Stroke, const FVector2D& Point)
	{
		const TArray<FVector2D>& Points = Stroke.Points;
		if (Points.IsEmpty())
		{
			return TNumericLimits<double>::Max();
		}
		if (Points.Num() == 1)
		{
			return FVector2D::Distance(Points[0], Point);
		}
		double Nearest = TNumericLimits<double>::Max();
		const int32 EdgeCount = Stroke.bClosed && Points.Num() >= 3 ? Points.Num() : Points.Num() - 1;
		for (int32 Edge = 0; Edge < EdgeCount; ++Edge)
		{
			double Alpha = 0.0;
			const FVector2D Closest = ClosestPointOnSegment(Points[Edge], Points[(Edge + 1) % Points.Num()], Point, Alpha);
			Nearest = FMath::Min(Nearest, FVector2D::Distance(Closest, Point));
		}
		return Nearest;
	}

	bool IsClosedGesture(TConstArrayView<FVector2D> Points)
	{
		return Points.Num() >= 3 && FVector2D::Distance(Points[0], Points.Last()) <= CloseDistance;
	}

	bool SanitizeStroke(FStageSketchStroke& Stroke)
	{
		TArray<FVector2D> Clean;
		Clean.Reserve(FMath::Min(Stroke.Points.Num(), MaxStrokePoints));
		for (const FVector2D& Point : Stroke.Points)
		{
			if (Clean.Num() >= MaxStrokePoints)
			{
				break;
			}
			if (IsUsablePoint(Point) && (Clean.IsEmpty() || FVector2D::Distance(Clean.Last(), Point) >= MinPointSpacing))
			{
				Clean.Add(Point);
			}
		}
		Stroke.DeckHeight = ClampOrDefault(Stroke.DeckHeight, MinDeckHeight, MaxDeckHeight, FStageSketchSettings().DeckHeight);

		switch (Stroke.Kind)
		{
		case EStageSketchStrokeKind::Freehand:
			Clean = Simplify(Clean, FreehandTolerance);
			Stroke.bClosed = IsClosedGesture(Clean);
			if (Stroke.bClosed)
			{
				Clean.Pop(); // The end meets the start.
			}
			break;

		case EStageSketchStrokeKind::Shape:
			Stroke.bClosed = true;
			break;

		case EStageSketchStrokeKind::Truss:
			if (Clean.Num() >= 3 && FVector2D::Distance(Clean[0], Clean.Last()) < MinPointSpacing)
			{
				Clean.Pop();
				Stroke.bClosed = true;
			}
			break;

		case EStageSketchStrokeKind::Fixture:
			Clean.SetNum(FMath::Min(Clean.Num(), 1));
			Stroke.bClosed = false;
			break;

		default:
			return false;
		}

		// A closed stroke repeats no point; three corners are the least that encloses anything.
		while (Stroke.bClosed && Clean.Num() >= 2 && FVector2D::Distance(Clean[0], Clean.Last()) < MinPointSpacing)
		{
			Clean.Pop();
		}
		if (Stroke.bClosed && Clean.Num() < 3)
		{
			Stroke.bClosed = false;
			if (Stroke.Kind == EStageSketchStrokeKind::Shape)
			{
				return false;
			}
		}

		const int32 Required = Stroke.Kind == EStageSketchStrokeKind::Fixture ? 1 : 2;
		Stroke.Points = MoveTemp(Clean);
		return Stroke.Points.Num() >= Required;
	}

	FStageSketchSettings SanitizeSettings(const FStageSketchSettings& Settings)
	{
		const FStageSketchSettings Defaults;
		FStageSketchSettings Clean = Settings;
		Clean.DeckHeight = ClampOrDefault(Settings.DeckHeight, MinDeckHeight, MaxDeckHeight, Defaults.DeckHeight);
		Clean.TrussHeight = ClampOrDefault(Settings.TrussHeight, MinTrussHeight, MaxTrussHeight, Defaults.TrussHeight);
		Clean.GridStep = ClampOrDefault(Settings.GridStep, MinGridStep, MaxGridStep, Defaults.GridStep);
		Clean.Smoothing = ClampOrDefault(Settings.Smoothing, 0.f, MaxSmoothing, Defaults.Smoothing);
		return Clean;
	}
}
