// Copyright Epic Games, Inc. All Rights Reserved.

#include "Sketch/StageSketchGenerator.h"

#include "Data/BaseItemData.h"
#include "Data/StageDeckData.h"
#include "Data/StageSpanData.h"
#include "Placement/StageDeckMath.h"
#include "Placement/StageSpanMath.h"
#include "Sketch/StageSketchMath.h"

#define LOCTEXT_NAMESPACE "StageSketchGenerator"

namespace StageSketchGenerator
{
	namespace
	{
		/** A truss as the fixture pass needs it: its world vertices at the truss height. */
		struct FPlannedTruss
		{
			TArray<FVector2D> Points;
			bool bClosed = false;
		};

		struct FPlannedDeck
		{
			TArray<FVector2D> Outline;
			double Top = 0.0;
		};

		FText StrokeName(EStageSketchElement Element, int32 StrokeIndex)
		{
			const FText Number = FText::AsNumber(StrokeIndex + 1);
			switch (Element)
			{
			case EStageSketchElement::Deck:		return FText::Format(LOCTEXT("ShapeName", "Stage shape (stroke {0})"), Number);
			case EStageSketchElement::Truss:	return FText::Format(LOCTEXT("TrussName", "Truss line (stroke {0})"), Number);
			default:							return FText::Format(LOCTEXT("FixtureName", "Fixture (stroke {0})"), Number);
			}
		}

		void PlanDeck(const FStageSketchStroke& Stroke, int32 StrokeIndex, const FStageSketchBuildRules& Rules, FStageSketchBuildPlan& Plan, TArray<FPlannedDeck>& OutDecks)
		{
			TArray<FVector2D> Corners;
			Corners.Reserve(Stroke.Points.Num());
			for (const FVector2D& Point : Stroke.Points)
			{
				Corners.Add(Rules.bSnapToGrid ? StageSketchMath::SnapToGrid(Point, Rules.GridStep) : Point);
			}

			TArray<FVector2D> Outline;
			if (!StageDeckMath::SanitizeOutline(Corners, Outline))
			{
				Plan.Warnings.Add(FText::Format(LOCTEXT("BadShape", "{0} crosses itself or is smaller than 0.25 m² after snapping; skipped."),
					StrokeName(EStageSketchElement::Deck, StrokeIndex)));
				return;
			}

			const UStageDeckData& Deck = *Rules.DeckItem;
			const FVector2D Centre = StageDeckMath::GetCentroid(Outline);
			FStageSketchDeckPlan& Planned = Plan.Decks.AddDefaulted_GetRef();
			Planned.StrokeIndex = StrokeIndex;
			Planned.Transform = FTransform(FVector(Centre.X, Centre.Y, Rules.FloorHeight));
			Planned.LocalOutline.Reserve(Outline.Num());
			for (const FVector2D& Corner : Outline)
			{
				Planned.LocalOutline.Add(Corner - Centre);
			}
			const double Requested = FMath::IsFinite(Stroke.DeckHeight) ? double(Stroke.DeckHeight) : double(Deck.GetDefaultHeight());
			Planned.Height = FMath::Clamp(Requested, double(Deck.GetMinHeight()), double(Deck.GetMaxHeight()));
			if (!FMath::IsNearlyEqual(Planned.Height, Requested, 0.5))
			{
				Plan.Warnings.Add(FText::Format(LOCTEXT("HeightClamped", "{0}: height {1} m is outside what {2} allows; built at {3} m."),
					StrokeName(EStageSketchElement::Deck, StrokeIndex), FText::AsNumber(Requested * 0.01), Deck.DisplayName, FText::AsNumber(Planned.Height * 0.01)));
			}
			Planned.Area = FMath::Abs(StageDeckMath::GetSignedArea(Planned.LocalOutline));
			Planned.Panels = StageDeckMath::GetPanelCount(Planned.Area, Deck.GetPanelArea());

			OutDecks.Add({ MoveTemp(Outline), Rules.FloorHeight + Planned.Height });
		}

		void PlanTruss(const FStageSketchStroke& Stroke, int32 StrokeIndex, const FStageSketchBuildRules& Rules, FStageSketchBuildPlan& Plan, TArray<FPlannedTruss>& OutTrusses)
		{
			const UStageSpanData& Span = *Rules.TrussItem;
			const StageSpanMath::FRules SpanRules = StageSpanMath::MakeRules(Span);

			// Clicked truss lines snap to whole sections, so an axis-aligned rectangle has whole sides and closes; freehand curves
			// keep their shape on the grid and are normalised leg by leg.
			const bool bSectionGrid = Stroke.Kind == EStageSketchStrokeKind::Truss && Span.Fit == EStageSpanFit::WholeSegments;
			const double Step = bSectionGrid ? FMath::Max(Rules.GridStep, double(Span.SegmentLength)) : Rules.GridStep;

			// Snapped vertices, with points that land on the same grid point merged (and a closing point on the start dropped).
			TArray<FVector2D> Snapped;
			Snapped.Reserve(Stroke.Points.Num());
			for (const FVector2D& Point : Stroke.Points)
			{
				const FVector2D OnGrid = Rules.bSnapToGrid ? StageSketchMath::SnapToGrid(Point, Step) : Point;
				if (Snapped.IsEmpty() || FVector2D::Distance(Snapped.Last(), OnGrid) >= StageSpanMath::MinLegLength)
				{
					Snapped.Add(OnGrid);
				}
			}
			while (Stroke.bClosed && Snapped.Num() >= 2 && FVector2D::Distance(Snapped.Last(), Snapped[0]) < StageSpanMath::MinLegLength)
			{
				Snapped.Pop();
			}

			const FText Name = StrokeName(EStageSketchElement::Truss, StrokeIndex);
			const FVector Origin(Snapped[0].X, Snapped[0].Y, Rules.TrussHeight);
			TArray<FVector> Local;
			Local.Reserve(Snapped.Num());
			for (const FVector2D& Point : Snapped)
			{
				Local.Add(FVector(Point.X, Point.Y, Rules.TrussHeight) - Origin);
			}

			const FStageSpanPathLayout Layout = StageSpanMath::ComputePathLayout(SpanRules, Local, Stroke.bClosed && Local.Num() >= 3, 0.0);
			if (Local.Num() < 2 || !Layout.IsValid())
			{
				Plan.Warnings.Add(FText::Format(LOCTEXT("ShortTruss", "{0} is too short for {1} after snapping; skipped."), Name, Span.DisplayName));
				return;
			}
			if (Stroke.bClosed && !Layout.bClosed)
			{
				Plan.Warnings.Add(FText::Format(LOCTEXT("OpenLoop", "{0}: the loop's closing side is not a whole number of {1} sections; built open."), Name, Span.DisplayName));
			}

			// Every vertex should survive the layout; fewer means the section cap cut the path short.
			if (Layout.Points.Num() < FMath::Min(Local.Num(), StageSpanMath::MaxPathPoints))
			{
				Plan.Warnings.Add(FText::Format(LOCTEXT("CutTruss", "{0} was cut to {1} sections, the most {2} allows in one run."),
					Name, FText::AsNumber(Layout.GetSegmentCount()), Span.DisplayName));
			}

			FStageSketchTrussPlan& Planned = Plan.Trusses.AddDefaulted_GetRef();
			Planned.StrokeIndex = StrokeIndex;
			Planned.Transform = FTransform(Origin);
			Planned.LocalPoints = Layout.Points;
			Planned.bClosed = Layout.bClosed;
			Planned.SegmentCount = Layout.GetSegmentCount();

			FPlannedTruss& Placed = OutTrusses.AddDefaulted_GetRef();
			Placed.bClosed = Layout.bClosed;
			for (const FVector& Point : Layout.Points)
			{
				Placed.Points.Add(FVector2D(Origin.X + Point.X, Origin.Y + Point.Y));
			}
		}

		/** The point on the nearest truss within the attach distance, its distance along the leg snapped to the grid. */
		bool FindTrussMount(const FVector2D& Marker, TConstArrayView<FPlannedTruss> Trusses, const FStageSketchBuildRules& Rules, FVector2D& OutPoint)
		{
			double Nearest = Rules.TrussAttachDistance;
			bool bFound = false;
			for (const FPlannedTruss& Truss : Trusses)
			{
				const int32 LegCount = Truss.bClosed ? Truss.Points.Num() : Truss.Points.Num() - 1;
				for (int32 Leg = 0; Leg < LegCount; ++Leg)
				{
					const FVector2D& A = Truss.Points[Leg];
					const FVector2D& B = Truss.Points[(Leg + 1) % Truss.Points.Num()];
					double Alpha = 0.0;
					const FVector2D Closest = StageSketchMath::ClosestPointOnSegment(A, B, Marker, Alpha);
					const double Distance = FVector2D::Distance(Closest, Marker);
					if (Distance > Nearest)
					{
						continue;
					}
					const double LegLength = FVector2D::Distance(A, B);
					double Along = Alpha * LegLength;
					if (Rules.bSnapToGrid && Rules.GridStep > 0.0)
					{
						Along = FMath::Clamp(FMath::RoundToDouble(Along / Rules.GridStep) * Rules.GridStep, 0.0, LegLength);
					}
					Nearest = Distance;
					OutPoint = LegLength > UE_KINDA_SMALL_NUMBER ? A + (B - A) * (Along / LegLength) : A;
					bFound = true;
				}
			}
			return bFound;
		}

		void PlanFixture(const FStageSketchStroke& Stroke, int32 StrokeIndex, const FStageSketchBuildRules& Rules, TConstArrayView<FPlannedTruss> Trusses,
			TConstArrayView<FPlannedDeck> Decks, FStageSketchBuildPlan& Plan)
		{
			const FVector2D Marker = Stroke.Points[0];
			FStageSketchFixturePlan Planned;
			Planned.StrokeIndex = StrokeIndex;

			FVector2D Mount;
			if (FindTrussMount(Marker, Trusses, Rules, Mount))
			{
				Planned.Mount = EStageSketchFixtureMount::Truss;
				// Hung under the truss: upside down, clamped to its bottom chord.
				Planned.Transform = FTransform(FRotator(0.0, 0.0, 180.0), FVector(Mount.X, Mount.Y, Rules.TrussHeight));
			}
			else
			{
				const FVector2D Spot = Rules.bSnapToGrid ? StageSketchMath::SnapToGrid(Marker, Rules.GridStep) : Marker;
				double Top = Rules.FloorHeight;
				for (const FPlannedDeck& Deck : Decks)
				{
					if (Deck.Top > Top && StageDeckMath::IsPointInPolygon(Deck.Outline, Spot))
					{
						Top = Deck.Top;
						Planned.Mount = EStageSketchFixtureMount::Deck;
					}
				}
				Planned.Transform = FTransform(FVector(Spot.X, Spot.Y, Top));
			}

			for (const FStageSketchFixturePlan& Existing : Plan.Fixtures)
			{
				if (FVector::Dist(Existing.Transform.GetLocation(), Planned.Transform.GetLocation()) < DuplicateFixtureDistance)
				{
					Plan.Warnings.Add(FText::Format(LOCTEXT("DuplicateFixture", "{0} lands on the same spot as another fixture after snapping; skipped."),
						StrokeName(EStageSketchElement::Fixture, StrokeIndex)));
					return;
				}
			}
			Plan.Fixtures.Add(Planned);
		}

		void WarnMissingItem(EStageSketchElement Element, int32 Count, FStageSketchBuildPlan& Plan)
		{
			if (Count == 0)
			{
				return;
			}
			const FText Number = FText::AsNumber(Count);
			switch (Element)
			{
			case EStageSketchElement::Deck:
				Plan.Warnings.Add(FText::Format(LOCTEXT("NoDeck", "There is no deck item in the catalog: {0} {0}|plural(one=stage shape,other=stage shapes) skipped."), Number));
				break;
			case EStageSketchElement::Truss:
				Plan.Warnings.Add(FText::Format(LOCTEXT("NoTruss", "There is no truss item in the catalog: {0} {0}|plural(one=truss line,other=truss lines) skipped."), Number));
				break;
			default:
				Plan.Warnings.Add(FText::Format(LOCTEXT("NoFixture", "There is no fixture item in the catalog: {0} {0}|plural(one=fixture,other=fixtures) skipped."), Number));
				break;
			}
		}
	}

	FStageSketchBuildPlan BuildPlan(TConstArrayView<FStageSketchStroke> Strokes, const FStageSketchBuildRules& InRules)
	{
		FStageSketchBuildRules Rules = InRules;
		Rules.GridStep = FMath::IsFinite(Rules.GridStep) && Rules.GridStep > 0.0 ? Rules.GridStep : 0.0;
		Rules.bSnapToGrid = Rules.bSnapToGrid && Rules.GridStep > 0.0;
		Rules.FloorHeight = FMath::IsFinite(Rules.FloorHeight) ? Rules.FloorHeight : 0.0;
		Rules.TrussHeight = FMath::IsFinite(Rules.TrussHeight) ? Rules.TrussHeight : 600.0;
		Rules.TrussAttachDistance = FMath::IsFinite(Rules.TrussAttachDistance) ? FMath::Max(0.0, Rules.TrussAttachDistance) : 0.0;

		FStageSketchBuildPlan Plan;
		Plan.DeckItem = Rules.DeckItem;
		Plan.TrussItem = Rules.TrussItem;
		Plan.FixtureItem = Rules.FixtureItem;

		// Decks and truss first: fixtures mount on what was planned before them, whatever order they were drawn in.
		TArray<FPlannedDeck> Decks;
		TArray<FPlannedTruss> Trusses;
		int32 Skipped[int32(EStageSketchElement::Count)] = {};
		for (int32 Index = 0; Index < Strokes.Num(); ++Index)
		{
			const FStageSketchStroke& Stroke = Strokes[Index];
			const EStageSketchElement Element = Stroke.GetElement();
			if (Element == EStageSketchElement::Deck)
			{
				if (Rules.DeckItem && Stroke.Points.Num() >= 3)
				{
					PlanDeck(Stroke, Index, Rules, Plan, Decks);
				}
				else
				{
					Skipped[int32(Element)] += Rules.DeckItem ? 0 : 1;
				}
			}
			else if (Element == EStageSketchElement::Truss)
			{
				if (Rules.TrussItem && Stroke.Points.Num() >= 2)
				{
					PlanTruss(Stroke, Index, Rules, Plan, Trusses);
				}
				else
				{
					Skipped[int32(Element)] += Rules.TrussItem ? 0 : 1;
				}
			}
		}
		for (int32 Index = 0; Index < Strokes.Num(); ++Index)
		{
			const FStageSketchStroke& Stroke = Strokes[Index];
			if (Stroke.GetElement() != EStageSketchElement::Fixture)
			{
				continue;
			}
			if (Rules.FixtureItem && !Stroke.Points.IsEmpty())
			{
				PlanFixture(Stroke, Index, Rules, Trusses, Decks, Plan);
			}
			else
			{
				Skipped[int32(EStageSketchElement::Fixture)] += Rules.FixtureItem ? 0 : 1;
			}
		}
		for (int32 Element = 0; Element < int32(EStageSketchElement::Count); ++Element)
		{
			WarnMissingItem(EStageSketchElement(Element), Skipped[Element], Plan);
		}
		return Plan;
	}

	FText DescribePlan(const FStageSketchBuildPlan& Plan)
	{
		if (Plan.IsEmpty())
		{
			return LOCTEXT("NothingToBuild", "Nothing to build");
		}
		TArray<FText> Parts;
		if (!Plan.Decks.IsEmpty())
		{
			Parts.Add(FText::Format(LOCTEXT("DecksPart", "{0} {0}|plural(one=deck,other=decks) ({1} {1}|plural(one=panel,other=panels))"),
				FText::AsNumber(Plan.Decks.Num()), FText::AsNumber(Plan.GetPanelCount())));
		}
		if (!Plan.Trusses.IsEmpty())
		{
			Parts.Add(FText::Format(LOCTEXT("TrussPart", "{0} truss {0}|plural(one=run,other=runs) ({1} {1}|plural(one=section,other=sections))"),
				FText::AsNumber(Plan.Trusses.Num()), FText::AsNumber(Plan.GetSegmentCount())));
		}
		if (!Plan.Fixtures.IsEmpty())
		{
			Parts.Add(FText::Format(LOCTEXT("FixturePart", "{0} {0}|plural(one=fixture,other=fixtures)"), FText::AsNumber(Plan.Fixtures.Num())));
		}
		return FText::Join(LOCTEXT("PartSeparator", ", "), Parts);
	}
}

#undef LOCTEXT_NAMESPACE
