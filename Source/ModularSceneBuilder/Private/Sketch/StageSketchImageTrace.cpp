// Copyright Epic Games, Inc. All Rights Reserved.

#include "Sketch/StageSketchImageTrace.h"

#include "Placement/StageDeckMath.h"
#include "Sketch/StageSketchMath.h"

#define LOCTEXT_NAMESPACE "StageSketchImageTrace"

namespace StageSketchImageTrace
{
	namespace
	{
		/** The 8 neighbours clockwise from west, in image coordinates (y down). Moore tracing relies on this order. */
		const FIntPoint Ring[8] = { { -1, 0 }, { -1, -1 }, { 0, -1 }, { 1, -1 }, { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 } };
		const FIntPoint Four[4] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
		const FIntPoint Diagonals[4] = { { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 } };

		// Thresholds, relative to the traced image so a phone photo and a small scan read alike.
		constexpr double ThresholdWindowFraction = 1.0 / 12.0;	// neighbourhood of the adaptive threshold
		constexpr double InkDarkness = 0.15;						// ink is this much darker than its neighbourhood...
		constexpr int32 MinInkContrast = 12;						// ...and at least this many grey levels darker
		constexpr int32 DarkImageMean = 110;						// a darker image is read as light lines on dark
		constexpr double SpeckFraction = 1.0 / 50000.0;			// smaller ink marks are dust
		constexpr double MinDeckFraction = 0.002;				// enclosed areas below this share of the image are not decks
		constexpr double FixtureSizeFraction = 0.03;			// longest side of a fixture mark
		constexpr double MaxFixtureAspect = 1.8;
		constexpr double FilledDotRatio = 0.45;					// ink share of a filled dot's box
		constexpr double RingRatio = 0.2;						// ink share of a drawn circle's box
		constexpr double MinLineFraction = 0.04;				// shorter runs are spurs or ticks
		constexpr double LineToleranceFraction = 0.008;			// line simplification, of the longest side
		constexpr double DeckToleranceFraction = 0.012;			// outline simplification, of the deck's longest side

		struct FComponent
		{
			int32 Area = 0;
			FIntPoint Min = FIntPoint(MAX_int32, MAX_int32);
			FIntPoint Max = FIntPoint(MIN_int32, MIN_int32);
			FVector2D Sum = FVector2D::ZeroVector;
			bool bTouchesBorder = false;
			/** First pixel in raster order. */
			int32 First = INDEX_NONE;

			int32 GetWidth() const { return Max.X - Min.X + 1; }
			int32 GetHeight() const { return Max.Y - Min.Y + 1; }
			bool Contains(const FComponent& Other) const
			{
				return Min.X <= Other.Min.X && Min.Y <= Other.Min.Y && Max.X >= Other.Max.X && Max.Y >= Other.Max.Y;
			}
		};

		/** Labels the connected regions of Mask == Value (1..Count; 0 elsewhere) and measures them (index 0 unused). */
		TArray<FComponent> Label(const TArray<uint8>& Mask, int32 Width, int32 Height, uint8 Value, bool bEightConnected, TArray<int32>& OutLabels)
		{
			OutLabels.Init(0, Width * Height);
			TArray<FComponent> Components;
			Components.AddDefaulted();
			TArray<int32> Stack;
			for (int32 Seed = 0; Seed < Mask.Num(); ++Seed)
			{
				if (Mask[Seed] != Value || OutLabels[Seed] != 0)
				{
					continue;
				}
				const int32 Id = Components.Num();
				FComponent& Component = Components.AddDefaulted_GetRef();
				Component.First = Seed;
				OutLabels[Seed] = Id;
				Stack.Reset();
				Stack.Push(Seed);
				while (!Stack.IsEmpty())
				{
					const int32 Pixel = Stack.Pop(EAllowShrinking::No);
					const int32 X = Pixel % Width;
					const int32 Y = Pixel / Width;
					++Component.Area;
					Component.Min = FIntPoint(FMath::Min(Component.Min.X, X), FMath::Min(Component.Min.Y, Y));
					Component.Max = FIntPoint(FMath::Max(Component.Max.X, X), FMath::Max(Component.Max.Y, Y));
					Component.Sum += FVector2D(X + 0.5, Y + 0.5);
					Component.bTouchesBorder |= X == 0 || Y == 0 || X == Width - 1 || Y == Height - 1;
					const int32 Neighbours = bEightConnected ? 8 : 4;
					for (int32 Index = 0; Index < Neighbours; ++Index)
					{
						const FIntPoint Offset = bEightConnected ? Ring[Index] : Four[Index];
						const int32 NX = X + Offset.X;
						const int32 NY = Y + Offset.Y;
						if (NX < 0 || NY < 0 || NX >= Width || NY >= Height)
						{
							continue;
						}
						const int32 Next = NY * Width + NX;
						if (Mask[Next] == Value && OutLabels[Next] == 0)
						{
							OutLabels[Next] = Id;
							Stack.Push(Next);
						}
					}
				}
			}
			return Components;
		}

		/** One 3 x 3 dilation (or erosion) of a 0/1 mask; outside the image counts as background. */
		void Morph(TArray<uint8>& Mask, int32 Width, int32 Height, bool bDilate)
		{
			const TArray<uint8> Source = Mask;
			for (int32 Y = 0; Y < Height; ++Y)
			{
				for (int32 X = 0; X < Width; ++X)
				{
					bool bAny = false;
					bool bAll = true;
					for (int32 DY = -1; DY <= 1; ++DY)
					{
						for (int32 DX = -1; DX <= 1; ++DX)
						{
							const int32 NX = X + DX;
							const int32 NY = Y + DY;
							const bool bInk = NX >= 0 && NY >= 0 && NX < Width && NY < Height && Source[NY * Width + NX] != 0;
							bAny |= bInk;
							bAll &= bInk;
						}
					}
					Mask[Y * Width + X] = (bDilate ? bAny : bAll) ? 1 : 0;
				}
			}
		}

		/** Zhang-Suen thinning of a 0/1 mask to one-pixel lines. The border row and column are cleared first. */
		void Thin(TArray<uint8>& Mask, int32 Width, int32 Height)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				Mask[X] = 0;
				Mask[(Height - 1) * Width + X] = 0;
			}
			for (int32 Y = 0; Y < Height; ++Y)
			{
				Mask[Y * Width] = 0;
				Mask[Y * Width + Width - 1] = 0;
			}
			TArray<int32> Remove;
			bool bChanged = true;
			while (bChanged)
			{
				bChanged = false;
				for (int32 Pass = 0; Pass < 2; ++Pass)
				{
					Remove.Reset();
					for (int32 Y = 1; Y < Height - 1; ++Y)
					{
						for (int32 X = 1; X < Width - 1; ++X)
						{
							const int32 I = Y * Width + X;
							if (!Mask[I])
							{
								continue;
							}
							// P2..P9 clockwise from north.
							const uint8 P[8] = { Mask[I - Width], Mask[I - Width + 1], Mask[I + 1], Mask[I + Width + 1], Mask[I + Width], Mask[I + Width - 1], Mask[I - 1], Mask[I - Width - 1] };
							int32 Neighbours = 0;
							int32 Transitions = 0;
							for (int32 K = 0; K < 8; ++K)
							{
								Neighbours += P[K];
								Transitions += (!P[K] && P[(K + 1) % 8]) ? 1 : 0;
							}
							if (Neighbours < 2 || Neighbours > 6 || Transitions != 1)
							{
								continue;
							}
							const bool bKeep = Pass == 0 ? (P[0] && P[2] && P[4]) || (P[2] && P[4] && P[6]) : (P[0] && P[2] && P[6]) || (P[0] && P[4] && P[6]);
							if (!bKeep)
							{
								Remove.Add(I);
							}
						}
					}
					for (const int32 I : Remove)
					{
						Mask[I] = 0;
					}
					bChanged |= !Remove.IsEmpty();
				}
			}
		}

		/**
		 * The outer edge of one labelled region, as pixel centres in order (Moore neighbour tracing, stopping when the start is about to
		 * be left the way it was first left: Jacob's criterion). Start must be the region's first pixel in raster order, so its west neighbour is outside.
		 */
		TArray<FVector2D> TraceBoundary(const TArray<int32>& Labels, int32 Id, int32 Start, int32 Width, int32 Height)
		{
			const auto Inside = [&Labels, Id, Width, Height](const FIntPoint& P)
			{
				return P.X >= 0 && P.Y >= 0 && P.X < Width && P.Y < Height && Labels[P.Y * Width + P.X] == Id;
			};
			const FIntPoint First(Start % Width, Start / Width);
			FIntPoint Current = First;
			int32 Back = 0;
			int32 FirstMove = INDEX_NONE;
			TArray<FVector2D> Points;
			const int32 MaxSteps = 4 * Width * Height + 8;
			for (int32 Step = 0; Step < MaxSteps; ++Step)
			{
				int32 Found = INDEX_NONE;
				for (int32 K = 1; K <= 8; ++K)
				{
					const int32 Direction = (Back + K) % 8;
					if (Inside(Current + Ring[Direction]))
					{
						Found = Direction;
						break;
					}
				}
				// Jacob's criterion: back at the start about to leave it the same way, the edge is complete. The way the start is
				// entered is not enough: a start reachable only from the east is never entered from the west again.
				if (Step > 0 && Current == First && Found == FirstMove)
				{
					break;
				}
				Points.Add(FVector2D(Current.X + 0.5, Current.Y + 0.5));
				if (Found == INDEX_NONE)
				{
					break;
				}
				if (Step == 0)
				{
					FirstMove = Found;
				}
				// The last outside pixel looked at becomes the next backtrack; consecutive neighbours are always adjacent.
				const FIntPoint Outside = Current + Ring[(Found + 7) % 8];
				const FIntPoint Next = Current + Ring[Found];
				const FIntPoint Delta = Outside - Next;
				Back = 0;
				for (int32 K = 0; K < 8; ++K)
				{
					if (Ring[K] == Delta)
					{
						Back = K;
					}
				}
				Current = Next;
			}
			return Points;
		}

		/** A closed ring simplified with Ramer-Douglas-Peucker, without repeating its first point. */
		TArray<FVector2D> SimplifyRing(const TArray<FVector2D>& Loop, double Tolerance)
		{
			if (Loop.Num() < 4)
			{
				return Loop;
			}
			// Split at the point farthest from the first, so both halves keep the shape's extremes.
			int32 Far = 0;
			double FarDistance = -1.0;
			for (int32 Index = 1; Index < Loop.Num(); ++Index)
			{
				const double Distance = FVector2D::DistSquared(Loop[0], Loop[Index]);
				if (Distance > FarDistance)
				{
					FarDistance = Distance;
					Far = Index;
				}
			}
			TArray<FVector2D> FirstHalf(Loop.GetData(), Far + 1);
			TArray<FVector2D> SecondHalf(Loop.GetData() + Far, Loop.Num() - Far);
			SecondHalf.Add(Loop[0]);
			TArray<FVector2D> Result = StageSketchMath::Simplify(FirstHalf, Tolerance);
			const TArray<FVector2D> Rest = StageSketchMath::Simplify(SecondHalf, Tolerance);
			for (int32 Index = 1; Index + 1 < Rest.Num(); ++Index)
			{
				Result.Add(Rest[Index]);
			}
			return Result;
		}

		/** Crossings from background to line around a skeleton pixel: 1 at an end, 2 along a line, 3 or more at a junction. */
		int32 CountCrossings(const TArray<uint8>& Skeleton, int32 Width, int32 Height, const FIntPoint& P)
		{
			const auto On = [&Skeleton, Width, Height](int32 X, int32 Y) { return X >= 0 && Y >= 0 && X < Width && Y < Height && Skeleton[Y * Width + X] != 0; };
			int32 Crossings = 0;
			for (int32 K = 0; K < 8; ++K)
			{
				const FIntPoint A = P + Ring[K];
				const FIntPoint B = P + Ring[(K + 1) % 8];
				Crossings += (!On(A.X, A.Y) && On(B.X, B.Y)) ? 1 : 0;
			}
			return Crossings;
		}

		/** Skeleton runs from ends and junctions to the next end or junction, as pixel centres, in raster order of their start. */
		TArray<TArray<FVector2D>> FollowLines(const TArray<uint8>& Skeleton, int32 Width, int32 Height)
		{
			const auto On = [&Skeleton, Width, Height](const FIntPoint& P) { return P.X >= 0 && P.Y >= 0 && P.X < Width && P.Y < Height && Skeleton[P.Y * Width + P.X] != 0; };
			TArray<uint8> Junction;
			Junction.Init(0, Width * Height);
			for (int32 I = 0; I < Skeleton.Num(); ++I)
			{
				if (Skeleton[I])
				{
					Junction[I] = CountCrossings(Skeleton, Width, Height, FIntPoint(I % Width, I / Width)) >= 3 ? 1 : 0;
				}
			}
			TArray<uint8> Visited;
			Visited.Init(0, Width * Height);
			TArray<TArray<FVector2D>> Lines;

			const auto Walk = [&](const FIntPoint& From, const FIntPoint& Into)
			{
				TArray<FVector2D> Line = { FVector2D(From.X + 0.5, From.Y + 0.5) };
				if (!Junction[From.Y * Width + From.X])
				{
					Visited[From.Y * Width + From.X] = 1;
				}
				FIntPoint Previous = From;
				FIntPoint Current = Into;
				for (int32 Step = 0; Step < Width * Height; ++Step)
				{
					Line.Add(FVector2D(Current.X + 0.5, Current.Y + 0.5));
					const int32 I = Current.Y * Width + Current.X;
					if (Junction[I])
					{
						break;
					}
					Visited[I] = 1;
					TOptional<FIntPoint> Next;
					for (const TConstArrayView<FIntPoint> Offsets : { TConstArrayView<FIntPoint>(Four), TConstArrayView<FIntPoint>(Diagonals) })
					{
						for (const FIntPoint& Offset : Offsets)
						{
							const FIntPoint Candidate = Current + Offset;
							if (!Next.IsSet() && Candidate != Previous && On(Candidate) && !Visited[Candidate.Y * Width + Candidate.X])
							{
								Next = Candidate;
							}
						}
					}
					if (!Next.IsSet())
					{
						break;
					}
					Previous = Current;
					Current = *Next;
				}
				Lines.Add(MoveTemp(Line));
			};
			const auto StartFrom = [&](const FIntPoint& From)
			{
				for (const TConstArrayView<FIntPoint> Offsets : { TConstArrayView<FIntPoint>(Four), TConstArrayView<FIntPoint>(Diagonals) })
				{
					for (const FIntPoint& Offset : Offsets)
					{
						const FIntPoint Into = From + Offset;
						if (On(Into) && !Visited[Into.Y * Width + Into.X] && !(Junction[From.Y * Width + From.X] && Junction[Into.Y * Width + Into.X]))
						{
							Walk(From, Into);
							if (!Junction[From.Y * Width + From.X])
							{
								return;
							}
						}
					}
				}
			};

			// Ends first, then the branches leaving junctions, then whatever is left (loops).
			for (int32 Pass = 0; Pass < 3; ++Pass)
			{
				for (int32 I = 0; I < Skeleton.Num(); ++I)
				{
					if (!Skeleton[I] || Visited[I])
					{
						continue;
					}
					const FIntPoint P(I % Width, I / Width);
					const int32 Crossings = CountCrossings(Skeleton, Width, Height, P);
					if ((Pass == 0 && Crossings == 1) || (Pass == 1 && Junction[I]) || (Pass == 2 && !Junction[I]))
					{
						StartFrom(P);
					}
				}
			}
			return Lines;
		}

		double LineLength(const TArray<FVector2D>& Line)
		{
			double Length = 0.0;
			for (int32 Index = 1; Index < Line.Num(); ++Index)
			{
				Length += FVector2D::Distance(Line[Index - 1], Line[Index]);
			}
			return Length;
		}
	}

	FStageSketchGrayImage Downscale(const FStageSketchGrayImage& Image, int32 MaxSide)
	{
		if (!Image.IsValid() || MaxSide <= 0 || FMath::Max(Image.Width, Image.Height) <= MaxSide)
		{
			return Image;
		}
		const double Scale = double(MaxSide) / double(FMath::Max(Image.Width, Image.Height));
		FStageSketchGrayImage Out;
		Out.Width = FMath::Max(1, FMath::RoundToInt(Image.Width * Scale));
		Out.Height = FMath::Max(1, FMath::RoundToInt(Image.Height * Scale));
		Out.Pixels.SetNumUninitialized(Out.Width * Out.Height);
		for (int32 Y = 0; Y < Out.Height; ++Y)
		{
			const int32 Y0 = int32(double(Y) * Image.Height / Out.Height);
			const int32 Y1 = FMath::Max(Y0 + 1, int32(double(Y + 1) * Image.Height / Out.Height));
			for (int32 X = 0; X < Out.Width; ++X)
			{
				const int32 X0 = int32(double(X) * Image.Width / Out.Width);
				const int32 X1 = FMath::Max(X0 + 1, int32(double(X + 1) * Image.Width / Out.Width));
				uint32 Sum = 0;
				for (int32 SY = Y0; SY < Y1 && SY < Image.Height; ++SY)
				{
					for (int32 SX = X0; SX < X1 && SX < Image.Width; ++SX)
					{
						Sum += Image.At(SX, SY);
					}
				}
				const int32 Count = (FMath::Min(Y1, Image.Height) - Y0) * (FMath::Min(X1, Image.Width) - X0);
				Out.Pixels[Y * Out.Width + X] = uint8(Sum / uint32(FMath::Max(Count, 1)));
			}
		}
		return Out;
	}

	TArray<uint8> FindInk(const FStageSketchGrayImage& Image, bool& bOutInverted)
	{
		bOutInverted = false;
		TArray<uint8> Ink;
		if (!Image.IsValid())
		{
			return Ink;
		}
		const int32 W = Image.Width;
		const int32 H = Image.Height;
		uint64 Total = 0;
		for (const uint8 Pixel : Image.Pixels)
		{
			Total += Pixel;
		}
		bOutInverted = Total / uint64(Image.Pixels.Num()) < uint64(DarkImageMean);

		// Integral image of the (possibly inverted) grey levels.
		TArray<uint64> Integral;
		Integral.Init(0, (W + 1) * (H + 1));
		for (int32 Y = 0; Y < H; ++Y)
		{
			uint64 Row = 0;
			for (int32 X = 0; X < W; ++X)
			{
				const uint8 Pixel = bOutInverted ? uint8(255 - Image.At(X, Y)) : Image.At(X, Y);
				Row += Pixel;
				Integral[(Y + 1) * (W + 1) + X + 1] = Integral[Y * (W + 1) + X + 1] + Row;
			}
		}
		const int32 Half = FMath::Max(7, int32(FMath::Max(W, H) * ThresholdWindowFraction) / 2);
		Ink.Init(0, W * H);
		for (int32 Y = 0; Y < H; ++Y)
		{
			const int32 Y0 = FMath::Max(0, Y - Half);
			const int32 Y1 = FMath::Min(H, Y + Half + 1);
			for (int32 X = 0; X < W; ++X)
			{
				const int32 X0 = FMath::Max(0, X - Half);
				const int32 X1 = FMath::Min(W, X + Half + 1);
				const uint64 Sum = Integral[Y1 * (W + 1) + X1] - Integral[Y0 * (W + 1) + X1] - Integral[Y1 * (W + 1) + X0] + Integral[Y0 * (W + 1) + X0];
				const double Mean = double(Sum) / double((Y1 - Y0) * (X1 - X0));
				const double Pixel = bOutInverted ? 255.0 - Image.At(X, Y) : double(Image.At(X, Y));
				Ink[Y * W + X] = (Pixel < Mean * (1.0 - InkDarkness) && Mean - Pixel >= MinInkContrast) ? 1 : 0;
			}
		}
		// A closing bridges one-pixel gaps in pencil lines.
		Morph(Ink, W, H, /*bDilate*/ true);
		Morph(Ink, W, H, /*bDilate*/ false);
		return Ink;
	}

	FVector2D PixelToPlan(const FVector2D& Pixel, int32 Width, int32 Height, double WidthCm)
	{
		const double Scale = Width > 0 ? WidthCm / double(Width) : 0.0;
		return FVector2D((double(Height) * 0.5 - Pixel.Y) * Scale, (Pixel.X - double(Width) * 0.5) * Scale);
	}

	FStageSketchTraceResult Trace(const FStageSketchGrayImage& Source, const FStageSketchTraceOptions& Options)
	{
		FStageSketchTraceResult Result;
		const FStageSketchGrayImage Image = Downscale(Source, MaxTraceSide);
		if (!Image.IsValid() || Image.Width < 8 || Image.Height < 8)
		{
			Result.Warnings.Add(LOCTEXT("TooSmall", "The image is too small to read."));
			return Result;
		}
		const int32 W = Image.Width;
		const int32 H = Image.Height;
		const int32 Side = FMath::Max(W, H);
		const double WidthCm = FMath::IsFinite(Options.WidthCm) && Options.WidthCm > 0.0 ? Options.WidthCm : 2000.0;

		TArray<uint8> Ink = FindInk(Image, Result.bInverted);

		// Ink marks; dust is removed.
		TArray<int32> InkLabels;
		TArray<FComponent> Marks = Label(Ink, W, H, 1, /*bEightConnected*/ true, InkLabels);
		const int32 SpeckArea = FMath::Max(4, int32(double(W) * H * SpeckFraction));
		for (int32 I = 0; I < Ink.Num(); ++I)
		{
			if (Ink[I] && Marks[InkLabels[I]].Area < SpeckArea)
			{
				Ink[I] = 0;
				InkLabels[I] = 0;
			}
		}

		// Enclosed paper: background regions (4-connected, so a diagonal gap in a line does not leak) away from the border.
		TArray<int32> PaperLabels;
		const TArray<FComponent> Paper = Label(Ink, W, H, 0, /*bEightConnected*/ false, PaperLabels);
		const int32 MinDeckArea = FMath::Max(64, int32(double(W) * H * MinDeckFraction));
		TArray<uint8> IsDeck;
		IsDeck.Init(0, Paper.Num());
		TArray<uint8> IsSmallHole;
		IsSmallHole.Init(0, Paper.Num());
		for (int32 Id = 1; Id < Paper.Num(); ++Id)
		{
			if (!Paper[Id].bTouchesBorder)
			{
				(Paper[Id].Area >= MinDeckArea ? IsDeck : IsSmallHole)[Id] = 1;
			}
		}

		// Which marks surround a deck (its outline) or a small hole (a drawn circle).
		TArray<uint8> IsOutline;
		IsOutline.Init(0, Marks.Num());
		TArray<uint8> HasHole;
		HasHole.Init(0, Marks.Num());
		for (int32 I = 0; I < PaperLabels.Num(); ++I)
		{
			const int32 PaperId = PaperLabels[I];
			if (!PaperId || (!IsDeck[PaperId] && !IsSmallHole[PaperId]))
			{
				continue;
			}
			const int32 X = I % W;
			const int32 Y = I / W;
			for (const FIntPoint& Offset : Four)
			{
				const int32 NX = X + Offset.X;
				const int32 NY = Y + Offset.Y;
				if (NX < 0 || NY < 0 || NX >= W || NY >= H)
				{
					continue;
				}
				const int32 MarkId = InkLabels[NY * W + NX];
				// Only a mark around the hole, not an island inside it (a fixture dot on a deck).
				if (MarkId && Marks[MarkId].Contains(Paper[PaperId]))
				{
					(IsDeck[PaperId] ? IsOutline : HasHole)[MarkId] = 1;
				}
			}
		}

		// Line thickness, from the ink area per skeleton pixel: it sets how much ink around a deck belongs to its outline.
		TArray<uint8> Skeleton = Ink;
		Thin(Skeleton, W, H);
		int32 InkArea = 0;
		int32 SkeletonArea = 0;
		for (int32 I = 0; I < Ink.Num(); ++I)
		{
			InkArea += Ink[I];
			SkeletonArea += Skeleton[I];
		}
		const int32 Thickness = FMath::Clamp(FMath::CeilToInt(double(InkArea) / double(FMath::Max(SkeletonArea, 1))), 1, 10);

		// --- Decks ---
		struct FDeck
		{
			TArray<FVector2D> Outline;
			/** Area inside the outline, in square pixels: an outer deck's enclosed paper can be smaller than a riser drawn in it, its outline cannot. */
			double Area = 0.0;
			FVector2D Inside = FVector2D::ZeroVector;
		};
		TArray<FDeck> Decks;
		TArray<uint8> DeckMask;
		DeckMask.Init(0, W * H);
		for (int32 Id = 1; Id < Paper.Num(); ++Id)
		{
			if (!IsDeck[Id])
			{
				continue;
			}
			const FComponent& Region = Paper[Id];
			const TArray<FVector2D> Edge = TraceBoundary(PaperLabels, Id, Region.First, W, H);
			const double Tolerance = FMath::Max(2.0, DeckToleranceFraction * FMath::Max(Region.GetWidth(), Region.GetHeight()));
			FDeck& Deck = Decks.AddDefaulted_GetRef();
			Deck.Outline = SimplifyRing(Edge, Tolerance);
			Deck.Area = FMath::Abs(StageDeckMath::GetSignedArea(Deck.Outline));
			Deck.Inside = FVector2D(Region.First % W + 0.5, Region.First / W + 0.5);
			if (Deck.Outline.Num() < 3)
			{
				Decks.Pop();
			}
		}
		for (int32 I = 0; I < PaperLabels.Num(); ++I)
		{
			DeckMask[I] = PaperLabels[I] && IsDeck[PaperLabels[I]] ? 1 : 0;
		}
		// Everything within one line width of a deck is its outline.
		TArray<uint8> OutlineBand = DeckMask;
		for (int32 Step = 0; Step <= Thickness; ++Step)
		{
			Morph(OutlineBand, W, H, /*bDilate*/ true);
		}
		Decks.StableSort([](const FDeck& A, const FDeck& B) { return A.Area > B.Area; });
		if (Decks.Num() > MaxDecks)
		{
			Result.Warnings.Add(FText::Format(LOCTEXT("TooManyDecks", "Only the {0} largest of {1} closed shapes were kept."), FText::AsNumber(MaxDecks), FText::AsNumber(Decks.Num())));
			Decks.SetNum(MaxDecks);
		}

		// --- Fixtures, and marks too small to be lines ---
		const int32 FixtureSize = FMath::Max(6, int32(Side * FixtureSizeFraction));
		TArray<uint8> IsLine;
		IsLine.Init(0, Marks.Num());
		TArray<FVector2D> Fixtures;
		for (int32 Id = 1; Id < Marks.Num(); ++Id)
		{
			const FComponent& Mark = Marks[Id];
			if (Mark.Area < SpeckArea || IsOutline[Id])
			{
				IsLine[Id] = IsOutline[Id];
				continue;
			}
			const int32 Longest = FMath::Max(Mark.GetWidth(), Mark.GetHeight());
			const int32 Shortest = FMath::Min(Mark.GetWidth(), Mark.GetHeight());
			const double Fill = double(Mark.Area) / double(Mark.GetWidth() * Mark.GetHeight());
			const bool bCompact = Longest <= FixtureSize && double(Longest) <= MaxFixtureAspect * double(FMath::Max(Shortest, 1));
			if (bCompact && (Fill >= FilledDotRatio || (HasHole[Id] && Fill >= RingRatio)))
			{
				Fixtures.Add(Mark.Sum / double(Mark.Area));
			}
			else if (Longest <= FixtureSize * 3 / 2)
			{
				++Result.IgnoredMarks;
			}
			else
			{
				IsLine[Id] = 1;
			}
		}
		if (Fixtures.Num() > MaxFixtures)
		{
			Result.Warnings.Add(FText::Format(LOCTEXT("TooManyFixtures", "Only the first {0} of {1} fixture marks were kept."), FText::AsNumber(MaxFixtures), FText::AsNumber(Fixtures.Num())));
			Fixtures.SetNum(MaxFixtures);
		}

		// --- Truss: line marks and whatever leaves a deck outline, thinned and followed ---
		TArray<uint8> LineInk;
		LineInk.Init(0, W * H);
		for (int32 I = 0; I < Ink.Num(); ++I)
		{
			LineInk[I] = Ink[I] && InkLabels[I] && IsLine[InkLabels[I]] && !OutlineBand[I] ? 1 : 0;
		}
		Thin(LineInk, W, H);
		const double MinLine = FMath::Max(10.0, Side * MinLineFraction);
		const double LineTolerance = FMath::Max(2.0, Side * LineToleranceFraction);
		TArray<TArray<FVector2D>> Lines;
		for (TArray<FVector2D>& Run : FollowLines(LineInk, W, H))
		{
			if (LineLength(Run) >= MinLine)
			{
				Lines.Add(StageSketchMath::Simplify(Run, LineTolerance));
			}
		}
		Lines.StableSort([](const TArray<FVector2D>& A, const TArray<FVector2D>& B) { return LineLength(A) > LineLength(B); });
		if (Lines.Num() > MaxTrussLines)
		{
			Result.Warnings.Add(FText::Format(LOCTEXT("TooManyLines", "Only the {0} longest of {1} lines were kept."), FText::AsNumber(MaxTrussLines), FText::AsNumber(Lines.Num())));
			Lines.SetNum(MaxTrussLines);
		}

		// --- Strokes in plan cm ---
		const auto ToPlan = [W, H, WidthCm](const FVector2D& Pixel) { return PixelToPlan(Pixel, W, H, WidthCm); };
		for (const FDeck& Deck : Decks)
		{
			// A deck inside another stands one deck height higher per level (a riser on a stage).
			int32 Depth = 0;
			for (const FDeck& Other : Decks)
			{
				Depth += &Other != &Deck && Other.Area > Deck.Area && StageDeckMath::IsPointInPolygon(Other.Outline, Deck.Inside) ? 1 : 0;
			}
			FStageSketchStroke& Stroke = Result.Strokes.AddDefaulted_GetRef();
			Stroke.Kind = EStageSketchStrokeKind::Shape;
			Stroke.bClosed = true;
			Stroke.DeckHeight = FMath::Min(Options.DeckHeight * float(1 + Depth), StageSketchMath::MaxDeckHeight);
			for (const FVector2D& Point : Deck.Outline)
			{
				Stroke.Points.Add(ToPlan(Point));
			}
			++Result.DeckCount;
		}
		for (const TArray<FVector2D>& Line : Lines)
		{
			FStageSketchStroke& Stroke = Result.Strokes.AddDefaulted_GetRef();
			Stroke.Kind = EStageSketchStrokeKind::Truss;
			for (const FVector2D& Point : Line)
			{
				Stroke.Points.Add(ToPlan(Point));
			}
			++Result.TrussCount;
		}
		for (const FVector2D& Fixture : Fixtures)
		{
			FStageSketchStroke& Stroke = Result.Strokes.AddDefaulted_GetRef();
			Stroke.Kind = EStageSketchStrokeKind::Fixture;
			Stroke.Points.Add(ToPlan(Fixture));
			++Result.FixtureCount;
		}

		if (Result.bInverted)
		{
			Result.Warnings.Add(LOCTEXT("Inverted", "The image is mostly dark, so it was read as light lines on a dark background."));
		}
		if (Result.Strokes.IsEmpty())
		{
			Result.Warnings.Add(LOCTEXT("NothingFound", "No closed shapes, lines or fixture marks were found. Use a darker pen, more even light, or a closer photo."));
		}
		if (Result.IgnoredMarks > 0)
		{
			Result.Warnings.Add(FText::Format(LOCTEXT("Ignored", "{0} small {0}|plural(one=mark,other=marks) (text or smudges) {0}|plural(one=was,other=were) left out."),
				FText::AsNumber(Result.IgnoredMarks)));
		}
		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
