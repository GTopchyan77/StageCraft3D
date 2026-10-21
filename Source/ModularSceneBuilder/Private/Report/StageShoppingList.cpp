// Copyright Epic Games, Inc. All Rights Reserved.

#include "Report/StageShoppingList.h"

#include "Report/StageCableSchedule.h"

namespace StageShoppingList
{
	namespace
	{
		/** Positions further out than this are treated as broken data; it also bounds how many cables one run can need. */
		constexpr double MaxCoordinateMeters = 10000.0;
		/** Rounding in a sum must not push a run that fits a stock length exactly onto the next one. */
		constexpr double LengthTolerance = 1e-6;
		/** cos(30°): a fixture tilted further than this from upright hangs from something. */
		constexpr double UprightMinUpZ = 0.8660254037844386;

		double ClampOrDefault(double Value, double Min, double Max, double Default)
		{
			return FMath::IsFinite(Value) ? FMath::Clamp(Value, Min, Max) : Default;
		}

		FVector ClampLocation(const FVector& Location)
		{
			auto Component = [](double Value) { return ClampOrDefault(Value, -MaxCoordinateMeters, MaxCoordinateMeters, 0.0); };
			return FVector(Component(Location.X), Component(Location.Y), Component(Location.Z));
		}

		/** Finite values within Min..Max, ascending and unique; Defaults when none is left. */
		TArray<double> SanitizeList(const TArray<double>& Values, double Min, double Max, const TArray<double>& Defaults)
		{
			TArray<double> Out;
			Out.Reserve(Values.Num());
			for (const double Value : Values)
			{
				if (FMath::IsFinite(Value) && Value >= Min && Value <= Max)
				{
					Out.AddUnique(Value);
				}
			}
			Out.Sort();
			return Out.IsEmpty() ? Defaults : Out;
		}

		FString SanitizeName(const FString& Name, const FString& Default)
		{
			const FString Trimmed = Name.TrimStartAndEnd();
			return Trimmed.IsEmpty() ? Default : Trimmed;
		}

		FString FormatMeters(double Meters)
		{
			// Stock lengths are usually whole metres; 0.5 m steps print with their decimal.
			return FMath::IsNearlyEqual(Meters, FMath::RoundToDouble(Meters)) ? FString::Printf(TEXT("%.0f m"), Meters) : FString::Printf(TEXT("%.1f m"), Meters);
		}

		FString FormatKilograms(double Kilograms)
		{
			return FMath::IsNearlyEqual(Kilograms, FMath::RoundToDouble(Kilograms)) ? FString::Printf(TEXT("%.0f kg"), Kilograms) : FString::Printf(TEXT("%.1f kg"), Kilograms);
		}

		// The *Safe helpers take rules that are already sanitized, so a list of hundreds of fixtures sanitizes them once.
		bool IsRiggedSafe(const FStageRiggedFixture& Fixture, const FStageCablingRules& Safe)
		{
			const double UpZ = FMath::IsFinite(Fixture.UpZ) ? Fixture.UpZ : 1.0;
			return UpZ < UprightMinUpZ || ClampLocation(Fixture.LocationMeters).Z >= Safe.RiggedAboveMeters;
		}

		int32 GetClampCountSafe(double WeightKg, const FStageCablingRules& Safe)
		{
			if (!FMath::IsFinite(WeightKg) || WeightKg <= 0.0)
			{
				return 1;
			}
			// Only absurd data could need more clamps than int32 holds; cap the weight like the locations.
			return FMath::Clamp(FMath::CeilToInt32(FMath::Min(WeightKg, 1.0e6) / Safe.WeightPerClampKg - LengthTolerance), 1, 1000000);
		}

		double PickSafetyBondSafe(double WeightKg, const FStageCablingRules& Safe)
		{
			const double Weight = FMath::IsFinite(WeightKg) && WeightKg > 0.0 ? WeightKg : 0.0;
			for (const double Rating : Safe.SafetyBondRatingsKg)
			{
				if (Weight <= Rating)
				{
					return Rating;
				}
			}
			return -1.0;
		}

		/** Adds Count cables of Meters to the per-length tally. */
		void Tally(TMap<double, int32>& ByLength, double Meters, int32 Count)
		{
			if (Count > 0 && Meters > 0.0)
			{
				ByLength.FindOrAdd(Meters) += Count;
			}
		}

		void AddCableLines(FStageShoppingList& List, EStageShoppingSection Section, const FString& Name, TArrayView<const FStageCableRun> Runs,
			int32& OutCableCount, double& OutMeters)
		{
			TMap<double, int32> ByLength;
			for (const FStageCableRun& Run : Runs)
			{
				Tally(ByLength, Run.Cable.StockMeters, Run.Cable.Count);
				Tally(ByLength, Run.Cable.TailMeters, Run.Cable.TailMeters > 0.0 ? 1 : 0);
				OutCableCount += Run.Cable.GetCableCount();
				OutMeters += Run.Cable.GetTotalMeters();
				List.JoinedRuns += Run.Cable.IsJoined() ? 1 : 0;
			}

			// TMap order is not defined: sort so the same stage always prints the same list.
			ByLength.KeySort([](double A, double B) { return A < B; });
			for (const TPair<double, int32>& Entry : ByLength)
			{
				FStageShoppingLine& Line = List.Lines.AddDefaulted_GetRef();
				Line.Section = Section;
				Line.Item = FString::Printf(TEXT("%s %s"), *Name, *FormatMeters(Entry.Key));
				Line.Size = FormatMeters(Entry.Key);
				Line.Quantity = Entry.Value;
			}
		}

		void AddPowerLines(FStageShoppingList& List, const FStageCableSchedule& Schedule, const FStageCablingRules& Rules)
		{
			if (Schedule.Distros.IsEmpty())
			{
				return;
			}

			FStageShoppingLine& Line = List.Lines.AddDefaulted_GetRef();
			Line.Section = EStageShoppingSection::Power;
			Line.Item = TEXT("Power distro");
			Line.Size = FString::Printf(TEXT("%d x %.0f A out, %.0f A %s feed"), Rules.DistroOutputs, Schedule.Supply.CircuitRatingAmps, Rules.DistroFeedAmps,
				Schedule.Supply.PhaseCount == 1 ? TEXT("single-phase") : TEXT("3-phase"));
			Line.Quantity = Schedule.Distros.Num();
			Line.Note = TEXT("Mains feed cable to each distro measured on site");
		}

		void AddDmxLines(FStageShoppingList& List, const FStageCableSchedule& Schedule)
		{
			const int32 Universes = Schedule.Universes.Num();
			if (Universes == 0)
			{
				return;
			}

			FStageShoppingLine& Outputs = List.Lines.AddDefaulted_GetRef();
			Outputs.Section = EStageShoppingSection::Dmx;
			Outputs.Item = TEXT("DMX node / splitter output");
			Outputs.Size = TEXT("1 universe");
			Outputs.Quantity = Universes;
			Outputs.Note = TEXT("One output per universe at the control node");

			FStageShoppingLine& Terminators = List.Lines.AddDefaulted_GetRef();
			Terminators.Section = EStageShoppingSection::Dmx;
			Terminators.Item = TEXT("DMX terminator");
			Terminators.Size = TEXT("XLR 5-pin, 120 ohm");
			Terminators.Quantity = Universes;
			Terminators.Note = TEXT("On the DMX out of the last fixture of each universe");
		}

		void AddRiggingLines(FStageShoppingList& List, TConstArrayView<FStageRiggedFixture> Fixtures, const FStageCablingRules& Rules)
		{
			int32 Clamps = 0;
			/** Bond rating -> count; -1 for fixtures heavier than every stocked bond. */
			TMap<double, int32> Bonds;
			for (const FStageRiggedFixture& Fixture : Fixtures)
			{
				if (!IsRiggedSafe(Fixture, Rules))
				{
					continue;
				}

				const bool bHasWeight = FMath::IsFinite(Fixture.WeightKg) && Fixture.WeightKg > 0.0;
				++List.RiggedFixtures;
				List.RiggedWeightKg += bHasWeight ? Fixture.WeightKg : 0.0;
				List.FixturesWithoutWeight += bHasWeight ? 0 : 1;
				Clamps += GetClampCountSafe(Fixture.WeightKg, Rules);

				const double Bond = PickSafetyBondSafe(Fixture.WeightKg, Rules);
				List.OverweightFixtures += Bond < 0.0 ? 1 : 0;
				++Bonds.FindOrAdd(Bond);
			}

			if (List.RiggedFixtures == 0)
			{
				return;
			}

			FStageShoppingLine& ClampLine = List.Lines.AddDefaulted_GetRef();
			ClampLine.Section = EStageShoppingSection::Rigging;
			ClampLine.Item = TEXT("Truss clamp (half-coupler)");
			ClampLine.Size = FString::Printf(TEXT("1 per %s of fixture"), *FormatKilograms(Rules.WeightPerClampKg));
			ClampLine.Quantity = Clamps;
			ClampLine.Note = FString::Printf(TEXT("%d rigged %s"), List.RiggedFixtures, List.RiggedFixtures == 1 ? TEXT("fixture") : TEXT("fixtures"));

			Bonds.KeySort([](double A, double B) { return A < B; });
			for (const TPair<double, int32>& Entry : Bonds)
			{
				// Overweight fixtures sort first (-1) but print last: they are the exception a rigger has to resolve.
				if (Entry.Key < 0.0)
				{
					continue;
				}
				FStageShoppingLine& Line = List.Lines.AddDefaulted_GetRef();
				Line.Section = EStageShoppingSection::Rigging;
				Line.Item = TEXT("Safety bond");
				Line.Size = FString::Printf(TEXT("WLL %s"), *FormatKilograms(Entry.Key));
				Line.Quantity = Entry.Value;
			}
			if (const int32* Overweight = Bonds.Find(-1.0))
			{
				FStageShoppingLine& Line = List.Lines.AddDefaulted_GetRef();
				Line.Section = EStageShoppingSection::Rigging;
				Line.Item = TEXT("Secondary suspension");
				Line.Size = FString::Printf(TEXT("above WLL %s"), *FormatKilograms(Rules.SafetyBondRatingsKg.Last()));
				Line.Quantity = *Overweight;
				Line.Note = TEXT("Heavier than every stocked bond: rigger to specify");
				Line.bNeedsAttention = true;
			}
		}
	}

	FStageCablingRules Sanitize(const FStageCablingRules& Rules)
	{
		const FStageCablingRules Defaults;
		FStageCablingRules Out;
		Out.SafetyMargin = ClampOrDefault(Rules.SafetyMargin, 0.0, 1.0, Defaults.SafetyMargin);
		Out.StockLengthsMeters = SanitizeList(Rules.StockLengthsMeters, 0.5, 500.0, Defaults.StockLengthsMeters);
		Out.PowerCableName = SanitizeName(Rules.PowerCableName, Defaults.PowerCableName);
		Out.DmxCableName = SanitizeName(Rules.DmxCableName, Defaults.DmxCableName);
		Out.DistroAnchor = Rules.DistroAnchor == EStageCableAnchor::Fixed ? EStageCableAnchor::Fixed : EStageCableAnchor::Automatic;
		Out.DistroLocationMeters = ClampLocation(Rules.DistroLocationMeters);
		Out.ControlAnchor = Rules.ControlAnchor == EStageCableAnchor::Fixed ? EStageCableAnchor::Fixed : EStageCableAnchor::Automatic;
		Out.ControlLocationMeters = ClampLocation(Rules.ControlLocationMeters);
		Out.DistroOutputs = FMath::Clamp(Rules.DistroOutputs, 1, 96);
		Out.DistroFeedAmps = ClampOrDefault(Rules.DistroFeedAmps, 1.0, 1000.0, Defaults.DistroFeedAmps);
		Out.RiggedAboveMeters = ClampOrDefault(Rules.RiggedAboveMeters, 0.0, 100.0, Defaults.RiggedAboveMeters);
		Out.WeightPerClampKg = ClampOrDefault(Rules.WeightPerClampKg, 1.0, 1000.0, Defaults.WeightPerClampKg);
		Out.SafetyBondRatingsKg = SanitizeList(Rules.SafetyBondRatingsKg, 1.0, 10000.0, Defaults.SafetyBondRatingsKg);
		return Out;
	}

	double GetRouteMeters(const FVector& FromMeters, const FVector& ToMeters)
	{
		const FVector From = ClampLocation(FromMeters);
		const FVector To = ClampLocation(ToMeters);
		return FVector::Dist2D(From, To) + FMath::Abs(To.Z - From.Z);
	}

	FStageCableAssembly AssembleRun(double RouteMeters, const FStageCablingRules& Rules)
	{
		const FStageCablingRules Safe = Sanitize(Rules);
		const TArray<double>& Stock = Safe.StockLengthsMeters;
		const double Required = FMath::IsFinite(RouteMeters) && RouteMeters > 0.0 ? RouteMeters * (1.0 + Safe.SafetyMargin) : 0.0;

		auto ShortestCovering = [&Stock](double Meters)
		{
			for (const double Length : Stock)
			{
				if (Meters <= Length + LengthTolerance)
				{
					return Length;
				}
			}
			return Stock.Last();
		};

		FStageCableAssembly Assembly;
		const double Longest = Stock.Last();
		if (Required <= Longest + LengthTolerance)
		{
			Assembly.Count = 1;
			Assembly.StockMeters = ShortestCovering(Required);
			return Assembly;
		}

		// Locations are clamped to ±10 km, so the count stays far below int32 range even for 0.5 m stock.
		Assembly.Count = FMath::FloorToInt32(Required / Longest);
		Assembly.StockMeters = Longest;
		const double Rest = Required - Assembly.Count * Longest;
		Assembly.TailMeters = Rest > LengthTolerance ? ShortestCovering(Rest) : 0.0;
		// The rest can need the longest stock again: count it as one more full length.
		if (Assembly.TailMeters == Longest)
		{
			++Assembly.Count;
			Assembly.TailMeters = 0.0;
		}
		return Assembly;
	}

	FString FormatAssembly(const FStageCableAssembly& Assembly)
	{
		if (Assembly.Count <= 0)
		{
			return Assembly.TailMeters > 0.0 ? FormatMeters(Assembly.TailMeters) : FString();
		}
		FString Text = Assembly.Count == 1 ? FormatMeters(Assembly.StockMeters) : FString::Printf(TEXT("%d x %s"), Assembly.Count, *FormatMeters(Assembly.StockMeters));
		if (Assembly.TailMeters > 0.0)
		{
			Text += TEXT(" + ") + FormatMeters(Assembly.TailMeters);
		}
		return Text;
	}

	bool IsRigged(const FStageRiggedFixture& Fixture, const FStageCablingRules& Rules)
	{
		return IsRiggedSafe(Fixture, Sanitize(Rules));
	}

	int32 GetClampCount(double WeightKg, const FStageCablingRules& Rules)
	{
		return GetClampCountSafe(WeightKg, Sanitize(Rules));
	}

	double PickSafetyBond(double WeightKg, const FStageCablingRules& Rules)
	{
		return PickSafetyBondSafe(WeightKg, Sanitize(Rules));
	}

	FStageShoppingList Build(const FStageCableSchedule& Schedule, TConstArrayView<FStageRiggedFixture> Fixtures)
	{
		// Sanitized again: a caller may build a schedule by hand.
		const FStageCablingRules Safe = Sanitize(Schedule.Cabling);
		FStageShoppingList List;
		AddCableLines(List, EStageShoppingSection::PowerCables, Safe.PowerCableName, Schedule.PowerCables, List.PowerCableCount, List.PowerCableMeters);
		AddCableLines(List, EStageShoppingSection::DmxCables, Safe.DmxCableName, Schedule.DmxCables, List.DmxCableCount, List.DmxCableMeters);
		AddPowerLines(List, Schedule, Safe);
		AddDmxLines(List, Schedule);
		AddRiggingLines(List, Fixtures, Safe);
		return List;
	}

	FString GetSectionName(EStageShoppingSection Section)
	{
		switch (Section)
		{
		case EStageShoppingSection::PowerCables:	return TEXT("Power cables");
		case EStageShoppingSection::DmxCables:		return TEXT("DMX cables");
		case EStageShoppingSection::Power:			return TEXT("Power distribution");
		case EStageShoppingSection::Dmx:			return TEXT("DMX");
		case EStageShoppingSection::Rigging:		return TEXT("Rigging");
		}
		return FString();
	}
}
