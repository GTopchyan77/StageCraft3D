// Copyright Epic Games, Inc. All Rights Reserved.

#include "People/StageAudienceMath.h"

#include "Math/RandomStream.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageAudienceMath)

namespace StageAudienceMath
{
	namespace
	{
		constexpr double SquareCmPerSquareMetre = 10000.0;
		/** How far from the middle of its cell a person may stand, as a share of the cell. Below 0.5, so neighbours never meet. */
		constexpr double Jitter = 0.32;
		/** Turn either way from facing the stage, in degrees. */
		constexpr double MaxTurn = 22.0;
		constexpr double MinHeightScale = 0.92;
		constexpr double MaxHeightScale = 1.07;
		constexpr double MinSpeed = 0.85;
		constexpr double MaxSpeed = 1.15;

		EStageFigureClip PickClip(const TArray<TPair<EStageFigureClip, float>>& Mix, float Roll)
		{
			float Sum = 0.f;
			for (const TPair<EStageFigureClip, float>& Entry : Mix)
			{
				Sum += Entry.Value;
				if (Roll < Sum)
				{
					return Entry.Key;
				}
			}
			return Mix.IsEmpty() ? EStageFigureClip::CrowdIdle : Mix.Last().Key;
		}
	}

	int32 GetPeopleCount(double Width, double Depth, double Density, int32 MaxPeople)
	{
		if (!FMath::IsFinite(Width) || !FMath::IsFinite(Depth) || !FMath::IsFinite(Density) || Width <= 0.0 || Depth <= 0.0 || Density <= 0.0
			|| MaxPeople <= 0)
		{
			return 0;
		}
		const double People = Width * Depth / SquareCmPerSquareMetre * FMath::Min(Density, MaxDensity);
		return static_cast<int32>(FMath::Min(FMath::FloorToDouble(People), double(MaxPeople)));
	}

	TArray<TPair<EStageFigureClip, float>> GetMoodMix(EStageCrowdMood Mood)
	{
		switch (Mood)
		{
		case EStageCrowdMood::Dancing:
			return { { EStageFigureClip::CrowdBounce, 0.7f }, { EStageFigureClip::CrowdIdle, 0.15f }, { EStageFigureClip::CrowdCheer, 0.1f },
				{ EStageFigureClip::CrowdClap, 0.05f } };
		case EStageCrowdMood::Cheering:
			return { { EStageFigureClip::CrowdCheer, 0.6f }, { EStageFigureClip::CrowdBounce, 0.25f }, { EStageFigureClip::CrowdClap, 0.15f } };
		case EStageCrowdMood::Calm:
		default:
			return { { EStageFigureClip::CrowdIdle, 0.8f }, { EStageFigureClip::CrowdClap, 0.2f } };
		}
	}

	void PlacePeople(double Width, double Depth, int32 Count, EStageCrowdMood Mood, int32 Seed, TConstArrayView<FLinearColor> TopColors,
		TArray<FPerson>& OutPeople)
	{
		OutPeople.Reset();
		if (Count <= 0 || !FMath::IsFinite(Width) || !FMath::IsFinite(Depth) || Width <= 0.0 || Depth <= 0.0)
		{
			return;
		}

		// A grid of at least Count cells with roughly square cells, filled front row first; the last row may be partly empty.
		const double CellSize = FMath::Sqrt(Width * Depth / Count);
		const int32 Columns = FMath::Max(1, FMath::RoundToInt32(Width / CellSize));
		const int32 Rows = FMath::Max(1, FMath::DivideAndRoundUp(Count, Columns));
		const double CellWidth = Width / Columns;
		const double CellDepth = Depth / Rows;

		FRandomStream Random(Seed);
		// Leave the free cells of the last row spread across it rather than all at one side.
		TArray<int32> Cells;
		Cells.Reserve(Columns * Rows);
		for (int32 Cell = 0; Cell < Columns * Rows; ++Cell)
		{
			Cells.Add(Cell);
		}
		const int32 FullCells = Columns * (Rows - 1);
		for (int32 Index = Cells.Num() - 1; Index > FullCells; --Index)
		{
			Cells.Swap(Index, FullCells + Random.RandHelper(Index - FullCells + 1));
		}

		const TArray<TPair<EStageFigureClip, float>> Mix = GetMoodMix(Mood);
		OutPeople.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const int32 Cell = Cells[Index];
			const int32 Row = Cell / Columns;
			const int32 Column = Cell % Columns;
			const double X = (Row + 0.5 + Random.FRandRange(-Jitter, Jitter)) * CellDepth;
			const double Y = -Width * 0.5 + (Column + 0.5 + Random.FRandRange(-Jitter, Jitter)) * CellWidth;
			const double Yaw = 180.0 + Random.FRandRange(-MaxTurn, MaxTurn);
			const double Scale = Random.FRandRange(MinHeightScale, MaxHeightScale);

			FPerson& Person = OutPeople.AddDefaulted_GetRef();
			Person.Transform = FTransform(FRotator(0.0, Yaw, 0.0), FVector(X, Y, 0.0), FVector(Scale, Scale, Scale));
			Person.Clip = PickClip(Mix, Random.GetFraction());
			Person.Phase = Random.GetFraction();
			Person.CyclesPerSecond = StageFigure::GetClipCyclesPerSecond(Person.Clip) * Random.FRandRange(MinSpeed, MaxSpeed);
			Person.TopColor = TopColors.IsEmpty() ? FLinearColor(0.2f, 0.2f, 0.2f) : TopColors[Random.RandHelper(TopColors.Num())];
			Person.SkinTone = Random.GetFraction();
			Person.BottomShade = Random.GetFraction();
		}
	}
}
