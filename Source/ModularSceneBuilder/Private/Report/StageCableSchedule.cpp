// Copyright Epic Games, Inc. All Rights Reserved.

#include "Report/StageCableSchedule.h"

#include "Misc/ComparisonUtility.h"
#include "Report/StageBillOfMaterials.h"
#include "Report/StageReportPdfComposer.h"

namespace StageCableSchedule
{
	namespace
	{
		const TCHAR* const LineEnd = TEXT("\r\n");
		constexpr int32 DmxUniverseSize = 512;
		constexpr double StockDmxLengths[] = { 1.0, 2.0, 3.0, 5.0, 10.0, 15.0, 20.0, 25.0, 30.0, 50.0 };

		double ClampOrDefault(double Value, double Min, double Max, double Default)
		{
			return FMath::IsFinite(Value) ? FMath::Clamp(Value, Min, Max) : Default;
		}

		/** Volts x power factor: watts per amp. Positive for every sanitized supply. */
		double GetWattsPerAmp(const FStagePowerDistribution& Supply)
		{
			return Supply.PhaseVoltage * Supply.PowerFactor;
		}

		FString GetLoadName(const FStagePowerLoad& Load)
		{
			const FString Label = Load.Label.TrimStartAndEnd();
			if (!Label.IsEmpty())
			{
				return Label;
			}
			const FString Type = Load.ItemType.TrimStartAndEnd();
			return Type.IsEmpty() ? FString(TEXT("Item")) : Type;
		}

		/** "#4 Spot 101": the fixture ID first, because unlabelled fixtures of one type all show the same type name. */
		FString GetNodeName(const FStageDmxNode& Node)
		{
			const FString Label = Node.Label.TrimStartAndEnd();
			return Label.IsEmpty() ? FString::Printf(TEXT("#%d"), Node.FixtureId) : FString::Printf(TEXT("#%d %s"), Node.FixtureId, *Label);
		}

		bool IsPatched(const FStageDmxNode& Node)
		{
			return Node.Universe >= 1 && Node.Address >= 1 && Node.Address <= DmxUniverseSize;
		}

		FVector SanitizeLocation(const FVector& Location)
		{
			return FVector(FMath::IsFinite(Location.X) ? Location.X : 0.0, FMath::IsFinite(Location.Y) ? Location.Y : 0.0, FMath::IsFinite(Location.Z) ? Location.Z : 0.0);
		}

		/** Least-loaded phase, the lower one on a tie, so the same stage always balances the same way. */
		int32 PickPhaseIndex(const TArray<FStagePowerPhase>& Phases)
		{
			int32 Best = 0;
			for (int32 Index = 1; Index < Phases.Num(); ++Index)
			{
				if (Phases[Index].Watts < Phases[Best].Watts)
				{
					Best = Index;
				}
			}
			return Best;
		}

		void PlanCircuits(FStageCableSchedule& Schedule, const TArray<FStagePowerLoad>& SortedLoads)
		{
			const FStagePowerDistribution& Supply = Schedule.Supply;
			const double Usable = GetUsableWattsPerCircuit(Supply);
			// A load exactly at the usable share fits; rounding in the sum must not push it out.
			const double Tolerance = 1e-6;

			for (int32 Phase = 1; Phase <= Supply.PhaseCount; ++Phase)
			{
				Schedule.Phases.AddDefaulted_GetRef().Phase = Phase;
			}

			// Loads of each circuit, in the order assigned (largest first); flattened into Schedule.Loads below.
			TArray<TArray<int32>> CircuitLoads;
			for (int32 LoadIndex = 0; LoadIndex < SortedLoads.Num(); ++LoadIndex)
			{
				const double Watts = SortedLoads[LoadIndex].Watts;
				const int32 PhaseIndex = PickPhaseIndex(Schedule.Phases);
				const int32 Phase = Schedule.Phases[PhaseIndex].Phase;
				const bool bTooLarge = Watts > Usable + Tolerance;

				int32 CircuitIndex = INDEX_NONE;
				if (!bTooLarge)
				{
					CircuitIndex = Schedule.Circuits.IndexOfByPredicate([Phase, Watts, Usable, Tolerance](const FStagePowerCircuit& Circuit)
					{
						return Circuit.Phase == Phase && !Circuit.bOverloaded && Circuit.Watts + Watts <= Usable + Tolerance;
					});
				}
				if (CircuitIndex == INDEX_NONE)
				{
					CircuitIndex = Schedule.Circuits.AddDefaulted();
					FStagePowerCircuit& NewCircuit = Schedule.Circuits[CircuitIndex];
					NewCircuit.Number = CircuitIndex + 1;
					NewCircuit.Phase = Phase;
					NewCircuit.bOverloaded = bTooLarge;
					CircuitLoads.AddDefaulted();
					++Schedule.Phases[PhaseIndex].CircuitCount;
				}

				Schedule.Circuits[CircuitIndex].Watts += Watts;
				CircuitLoads[CircuitIndex].Add(LoadIndex);
				Schedule.Phases[PhaseIndex].Watts += Watts;
			}

			const double WattsPerAmp = GetWattsPerAmp(Supply);
			for (int32 CircuitIndex = 0; CircuitIndex < Schedule.Circuits.Num(); ++CircuitIndex)
			{
				FStagePowerCircuit& Circuit = Schedule.Circuits[CircuitIndex];
				Circuit.Amps = Circuit.Watts / WattsPerAmp;
				Circuit.RatingShare = Circuit.Amps / Supply.CircuitRatingAmps;
				Schedule.OverloadedCircuits += Circuit.bOverloaded ? 1 : 0;

				for (const int32 SortedIndex : CircuitLoads[CircuitIndex])
				{
					Circuit.Loads.Add(Schedule.Loads.Num());
					FStagePlannedLoad& Planned = Schedule.Loads.AddDefaulted_GetRef();
					Planned.Load = SortedLoads[SortedIndex];
					Planned.CircuitIndex = CircuitIndex;
					Planned.Amps = Planned.Load.Watts / WattsPerAmp;
				}
			}

			double MaxWatts = 0.0;
			double MinWatts = TNumericLimits<double>::Max();
			for (FStagePowerPhase& Phase : Schedule.Phases)
			{
				Phase.Amps = Phase.Watts / WattsPerAmp;
				Schedule.TotalWatts += Phase.Watts;
				MaxWatts = FMath::Max(MaxWatts, Phase.Watts);
				MinWatts = FMath::Min(MinWatts, Phase.Watts);
			}
			Schedule.ImbalancePercent = Schedule.Phases.Num() > 1 && MaxWatts > 0.0 ? (MaxWatts - MinWatts) / MaxWatts * 100.0 : 0.0;
		}

		void ListPowerCables(FStageCableSchedule& Schedule)
		{
			Schedule.PowerCables.Reserve(Schedule.Loads.Num());
			for (int32 LoadIndex = 0; LoadIndex < Schedule.Loads.Num(); ++LoadIndex)
			{
				const FStagePlannedLoad& Planned = Schedule.Loads[LoadIndex];
				const FStagePowerCircuit& Circuit = Schedule.Circuits[Planned.CircuitIndex];

				FStageCableRun& Run = Schedule.PowerCables.AddDefaulted_GetRef();
				Run.CableId = FString::Printf(TEXT("P-%03d"), LoadIndex + 1);
				Run.Kind = EStageCableKind::Power;
				Run.From = FString::Printf(TEXT("Distro %s / %s"), *FormatCircuit(Circuit.Number), *FormatPhase(Circuit.Phase));
				Run.To = GetLoadName(Planned.Load);
				Run.LoadIndex = LoadIndex;
			}
		}

		void ChainDmx(FStageCableSchedule& Schedule, TArray<FStageDmxNode> Nodes)
		{
			TArray<FStageDmxNode> Patched;
			Patched.Reserve(Nodes.Num());
			for (FStageDmxNode& Node : Nodes)
			{
				if (!IsPatched(Node))
				{
					++Schedule.UnpatchedFixtures;
					continue;
				}
				Node.LocationMeters = SanitizeLocation(Node.LocationMeters);
				Patched.Add(MoveTemp(Node));
			}

			Patched.StableSort([](const FStageDmxNode& A, const FStageDmxNode& B)
			{
				if (A.Universe != B.Universe)
				{
					return A.Universe < B.Universe;
				}
				if (A.Address != B.Address)
				{
					return A.Address < B.Address;
				}
				if (A.FixtureId != B.FixtureId)
				{
					return A.FixtureId < B.FixtureId;
				}
				return UE::ComparisonUtility::CompareNaturalOrder(A.Label, B.Label) < 0;
			});

			int32 LinkInUniverse = 0;
			for (int32 Index = 0; Index < Patched.Num(); ++Index)
			{
				const FStageDmxNode& Node = Patched[Index];
				const bool bFirstInUniverse = Index == 0 || Patched[Index - 1].Universe != Node.Universe;
				LinkInUniverse = bFirstInUniverse ? 1 : LinkInUniverse + 1;
				if (bFirstInUniverse)
				{
					Schedule.Universes.Add(Node.Universe);
				}

				FStageCableRun& Run = Schedule.DmxCables.AddDefaulted_GetRef();
				Run.CableId = FString::Printf(TEXT("D%d-%02d"), Node.Universe, LinkInUniverse);
				Run.Kind = EStageCableKind::Dmx;
				Run.Universe = Node.Universe;
				Run.ToAddress = Node.Address;
				Run.To = GetNodeName(Node);
				Run.bTerminate = Index + 1 == Patched.Num() || Patched[Index + 1].Universe != Node.Universe;
				if (bFirstInUniverse)
				{
					// The console or node is not part of the stage: its run is measured on site.
					Run.From = FString::Printf(TEXT("DMX source U%d"), Node.Universe);
					continue;
				}

				const FStageDmxNode& Previous = Patched[Index - 1];
				Run.From = GetNodeName(Previous);
				Run.DistanceMeters = FVector::Dist(Previous.LocationMeters, Node.LocationMeters);
				Run.StockLengthMeters = GetStockLength(Run.DistanceMeters * DmxRoutingSlack);
				Schedule.OverlongDmxLinks += Run.StockLengthMeters < 0.0 ? 1 : 0;
			}
		}

		FString FormatWatts(double Value) { return FString::Printf(TEXT("%.0f"), Value); }
		FString FormatAmps(double Value) { return FString::Printf(TEXT("%.1f"), Value); }
		FString FormatPercent(double Share) { return FString::Printf(TEXT("%.0f"), Share * 100.0); }
		FString FormatMeters(double Value) { return Value < 0.0 ? FString(TEXT("site")) : FString::Printf(TEXT("%.1f"), Value); }

		FString FormatStockLength(double Meters)
		{
			if (Meters < 0.0)
			{
				return TEXT("split");
			}
			return Meters == 0.0 ? FString(TEXT("site")) : FString::Printf(TEXT("%.0f m"), Meters);
		}

		FString FormatAddress(int32 Universe, int32 Address)
		{
			return FString::Printf(TEXT("%d.%03d"), Universe, Address);
		}

		FString GetDmxNote(const FStageCableRun& Run)
		{
			if (Run.StockLengthMeters < 0.0)
			{
				return TEXT("Too long: add a node or splitter");
			}
			return Run.bTerminate ? FString(TEXT("Terminate DMX out")) : FString();
		}

		FString DescribeSupply(const FStagePowerDistribution& Supply)
		{
			return FString::Printf(TEXT("%s %.0f V · %.0f A circuits, %.0f %% usable (%.0f W each) · power factor %.2f"),
				Supply.PhaseCount == 1 ? TEXT("Single phase") : TEXT("3-phase"), Supply.PhaseVoltage, Supply.CircuitRatingAmps,
				Supply.UsableShare * 100.0, GetUsableWattsPerCircuit(Supply), Supply.PowerFactor);
		}

		FString Plural(int32 Count, const TCHAR* Singular, const TCHAR* PluralForm)
		{
			return FString::Printf(TEXT("%d %s"), Count, Count == 1 ? Singular : PluralForm);
		}

		void AppendRow(FString& Out, std::initializer_list<FString> Fields)
		{
			bool bFirst = true;
			for (const FString& Field : Fields)
			{
				if (!bFirst)
				{
					Out += TEXT(",");
				}
				Out += StageBillOfMaterials::EscapeCsvField(Field);
				bFirst = false;
			}
			Out += LineEnd;
		}

		FString GetDisplaySceneName(const FString& SceneName)
		{
			return SceneName.TrimStartAndEnd().IsEmpty() ? FString(TEXT("Untitled")) : SceneName;
		}
	}

	FStagePowerDistribution Sanitize(const FStagePowerDistribution& Supply)
	{
		const FStagePowerDistribution Defaults;
		FStagePowerDistribution Out;
		Out.PhaseVoltage = ClampOrDefault(Supply.PhaseVoltage, 100.0, 400.0, Defaults.PhaseVoltage);
		Out.PhaseCount = Supply.PhaseCount == 1 ? 1 : 3;
		Out.CircuitRatingAmps = ClampOrDefault(Supply.CircuitRatingAmps, 1.0, 125.0, Defaults.CircuitRatingAmps);
		Out.UsableShare = ClampOrDefault(Supply.UsableShare, 0.1, 1.0, Defaults.UsableShare);
		Out.PowerFactor = ClampOrDefault(Supply.PowerFactor, 0.5, 1.0, Defaults.PowerFactor);
		return Out;
	}

	double GetUsableWattsPerCircuit(const FStagePowerDistribution& Supply)
	{
		const FStagePowerDistribution Safe = Sanitize(Supply);
		return Safe.PhaseVoltage * Safe.CircuitRatingAmps * Safe.UsableShare * Safe.PowerFactor;
	}

	double GetStockLength(double Meters)
	{
		if (!FMath::IsFinite(Meters))
		{
			return -1.0;
		}
		for (const double Stock : StockDmxLengths)
		{
			if (Meters <= Stock)
			{
				return Stock;
			}
		}
		return -1.0;
	}

	FStageCableSchedule Build(TArray<FStagePowerLoad> Loads, TArray<FStageDmxNode> Nodes, const FStagePowerDistribution& Supply,
		const FString& SceneName, const FDateTime& GeneratedAt)
	{
		FStageCableSchedule Schedule;
		Schedule.SceneName = SceneName;
		Schedule.GeneratedAt = GeneratedAt;
		Schedule.Supply = Sanitize(Supply);

		TArray<FStagePowerLoad> Powered;
		Powered.Reserve(Loads.Num());
		for (FStagePowerLoad& Load : Loads)
		{
			if (FMath::IsFinite(Load.Watts) && Load.Watts > 0.0)
			{
				Powered.Add(MoveTemp(Load));
			}
			else if (Load.FixtureId > 0)
			{
				++Schedule.FixturesWithoutPowerData;
			}
		}

		Powered.StableSort([](const FStagePowerLoad& A, const FStagePowerLoad& B)
		{
			if (A.Watts != B.Watts)
			{
				return A.Watts > B.Watts;
			}
			const int32 ByName = UE::ComparisonUtility::CompareNaturalOrder(GetLoadName(A), GetLoadName(B));
			if (ByName != 0)
			{
				return ByName < 0;
			}
			if (A.FixtureId != B.FixtureId)
			{
				return A.FixtureId < B.FixtureId;
			}
			return A.ItemType < B.ItemType;
		});

		PlanCircuits(Schedule, Powered);
		ListPowerCables(Schedule);
		ChainDmx(Schedule, MoveTemp(Nodes));
		return Schedule;
	}

	FString FormatPhase(int32 Phase)
	{
		return FString::Printf(TEXT("L%d"), Phase);
	}

	FString FormatCircuit(int32 Number)
	{
		return FString::Printf(TEXT("C%02d"), Number);
	}

	TArray<FString> GetWarnings(const FStageCableSchedule& Schedule)
	{
		TArray<FString> Warnings;
		if (Schedule.OverloadedCircuits > 0)
		{
			Warnings.Add(FString::Printf(TEXT("%s with a single load above the usable %.0f W of a %.0f A circuit: it needs a larger feed."),
				*Plural(Schedule.OverloadedCircuits, TEXT("circuit"), TEXT("circuits")), GetUsableWattsPerCircuit(Schedule.Supply), Schedule.Supply.CircuitRatingAmps));
		}
		if (Schedule.ImbalancePercent > ImbalanceWarningPercent)
		{
			Warnings.Add(FString::Printf(TEXT("Phase imbalance %.0f %% (above %.0f %%): re-balance the loads on site."), Schedule.ImbalancePercent, ImbalanceWarningPercent));
		}
		if (Schedule.FixturesWithoutPowerData > 0)
		{
			Warnings.Add(FString::Printf(TEXT("%s without power data in the catalog: their load is missing from this plan."),
				*Plural(Schedule.FixturesWithoutPowerData, TEXT("lighting fixture"), TEXT("lighting fixtures"))));
		}
		if (Schedule.OverlongDmxLinks > 0)
		{
			Warnings.Add(FString::Printf(TEXT("%s longer than the longest stock cable (50 m): add a DMX node or an opto-splitter."),
				*Plural(Schedule.OverlongDmxLinks, TEXT("DMX link"), TEXT("DMX links"))));
		}
		if (Schedule.UnpatchedFixtures > 0)
		{
			Warnings.Add(FString::Printf(TEXT("%s unpatched and in no DMX chain."), *Plural(Schedule.UnpatchedFixtures, TEXT("lighting fixture is"), TEXT("lighting fixtures are"))));
		}
		return Warnings;
	}

	FString ToCsv(const FStageCableSchedule& Schedule)
	{
		FString Out;
		Out.Reserve(512 + (Schedule.Loads.Num() + Schedule.DmxCables.Num() + Schedule.Circuits.Num()) * 96);

		// U+FEFF: written as the UTF-8 byte order mark, which spreadsheet programs need to read non-ASCII names correctly.
		Out += TEXT("﻿");
		AppendRow(Out, { TEXT("StageCraft 3D Cable Schedule and Power Plan (generated proposal)") });
		AppendRow(Out, { TEXT("Scene"), GetDisplaySceneName(Schedule.SceneName) });
		AppendRow(Out, { TEXT("Generated"), Schedule.GeneratedAt.ToString(TEXT("%Y-%m-%d %H:%M")) });
		AppendRow(Out, { TEXT("Supply"), DescribeSupply(Schedule.Supply) });
		AppendRow(Out, { TEXT("Total load (W)"), FormatWatts(Schedule.TotalWatts) });
		AppendRow(Out, { TEXT("Phase imbalance (%)"), FString::Printf(TEXT("%.0f"), Schedule.ImbalancePercent) });
		for (const FString& Warning : GetWarnings(Schedule))
		{
			AppendRow(Out, { TEXT("Warning"), Warning });
		}
		Out += LineEnd;

		AppendRow(Out, { TEXT("Phase"), TEXT("Circuits"), TEXT("Load (W)"), TEXT("Current (A)"), TEXT("Share of load (%)") });
		for (const FStagePowerPhase& Phase : Schedule.Phases)
		{
			AppendRow(Out, { FormatPhase(Phase.Phase), FString::FromInt(Phase.CircuitCount), FormatWatts(Phase.Watts), FormatAmps(Phase.Amps),
				FormatPercent(Schedule.TotalWatts > 0.0 ? Phase.Watts / Schedule.TotalWatts : 0.0) });
		}
		Out += LineEnd;

		AppendRow(Out, { TEXT("Circuit"), TEXT("Phase"), TEXT("Loads"), TEXT("Load (W)"), TEXT("Current (A)"), TEXT("Use of rating (%)"), TEXT("Note") });
		for (const FStagePowerCircuit& Circuit : Schedule.Circuits)
		{
			AppendRow(Out, { FormatCircuit(Circuit.Number), FormatPhase(Circuit.Phase), FString::FromInt(Circuit.Loads.Num()), FormatWatts(Circuit.Watts),
				FormatAmps(Circuit.Amps), FormatPercent(Circuit.RatingShare), Circuit.bOverloaded ? FString(TEXT("OVERLOAD: needs a larger feed")) : FString() });
		}
		Out += LineEnd;

		AppendRow(Out, { TEXT("Cable"), TEXT("From"), TEXT("To"), TEXT("Type"), TEXT("Fixture ID"), TEXT("Load (W)"), TEXT("Current (A)"), TEXT("Length") });
		for (const FStageCableRun& Run : Schedule.PowerCables)
		{
			const FStagePlannedLoad& Planned = Schedule.Loads[Run.LoadIndex];
			AppendRow(Out, { Run.CableId, Run.From, Run.To, Planned.Load.ItemType, Planned.Load.FixtureId > 0 ? FString::FromInt(Planned.Load.FixtureId) : FString(),
				FormatWatts(Planned.Load.Watts), FormatAmps(Planned.Amps), FormatStockLength(Run.StockLengthMeters) });
		}
		Out += LineEnd;

		AppendRow(Out, { TEXT("Cable"), TEXT("Universe"), TEXT("From"), TEXT("To"), TEXT("Start address"), TEXT("Distance (m)"), TEXT("Stock length"), TEXT("Note") });
		for (const FStageCableRun& Run : Schedule.DmxCables)
		{
			AppendRow(Out, { Run.CableId, FString::FromInt(Run.Universe), Run.From, Run.To, FormatAddress(Run.Universe, Run.ToAddress),
				FormatMeters(Run.DistanceMeters), FormatStockLength(Run.StockLengthMeters), GetDmxNote(Run) });
		}
		return Out;
	}

	TArray<uint8> ToPdf(const FStageCableSchedule& Schedule)
	{
		FStageReportPdfComposer Composer(TEXT("CABLE SCHEDULE & POWER"), TEXT("Cable schedule & power plan (generated proposal)"), Schedule.SceneName, Schedule.GeneratedAt);
		Composer.SetSummary(FString::Printf(TEXT("%s · %.2f kW · %s on %s · imbalance %.0f %% · %s in %s"),
			*Plural(Schedule.Loads.Num(), TEXT("load"), TEXT("loads")), Schedule.TotalWatts / 1000.0,
			*Plural(Schedule.Circuits.Num(), TEXT("circuit"), TEXT("circuits")), *Plural(Schedule.Phases.Num(), TEXT("phase"), TEXT("phases")),
			Schedule.ImbalancePercent, *Plural(Schedule.DmxCables.Num(), TEXT("DMX link"), TEXT("DMX links")),
			*Plural(Schedule.Universes.Num(), TEXT("universe"), TEXT("universes"))));

		Composer.AddNote(TEXT("Supply: ") + DescribeSupply(Schedule.Supply));
		const TArray<FString> Warnings = GetWarnings(Schedule);
		for (const FString& Warning : Warnings)
		{
			Composer.AddNote(TEXT("WARNING: ") + Warning, EStageReportTone::Strong);
		}
		if (Warnings.IsEmpty())
		{
			Composer.AddNote(TEXT("No warnings."), EStageReportTone::Muted);
		}
		Composer.AddNote(TEXT("Generated proposal: loads are balanced automatically from each item's maximum power draw. Confirm circuits and cable runs on site."),
			EStageReportTone::Muted);

		Composer.AddHeading(TEXT("Phase loads"));
		if (Schedule.Loads.IsEmpty())
		{
			Composer.AddNote(TEXT("No equipment on the stage draws power."), EStageReportTone::Muted);
		}
		else
		{
			Composer.BeginTable({ { TEXT("Phase"), 1.0 }, { TEXT("Circuits"), 1.0, EStageReportAlign::Right }, { TEXT("Load (W)"), 1.2, EStageReportAlign::Right },
				{ TEXT("Current (A)"), 1.2, EStageReportAlign::Right }, { TEXT("Share of load"), 1.2, EStageReportAlign::Right }, { FString(), 4.0 } });
			for (const FStagePowerPhase& Phase : Schedule.Phases)
			{
				Composer.AddRow({ FormatPhase(Phase.Phase), FString::FromInt(Phase.CircuitCount), FormatWatts(Phase.Watts), FormatAmps(Phase.Amps),
					FormatPercent(Schedule.TotalWatts > 0.0 ? Phase.Watts / Schedule.TotalWatts : 0.0) + TEXT(" %") });
			}
			Composer.AddRow({ TEXT("Total"), FString::FromInt(Schedule.Circuits.Num()), FormatWatts(Schedule.TotalWatts), FString(), TEXT("100 %") }, EStageReportRowStyle::Total);
			Composer.EndTable();

			Composer.AddHeading(TEXT("Circuits"));
			Composer.BeginTable({ { TEXT("Circuit"), 1.0 }, { TEXT("Phase"), 1.0 }, { TEXT("Loads"), 1.0, EStageReportAlign::Right }, { TEXT("Load (W)"), 1.2, EStageReportAlign::Right },
				{ TEXT("Current (A)"), 1.2, EStageReportAlign::Right }, { TEXT("Use of rating"), 1.2, EStageReportAlign::Right }, { TEXT("Note"), 3.4 } });
			for (const FStagePowerCircuit& Circuit : Schedule.Circuits)
			{
				Composer.AddRow({ FormatCircuit(Circuit.Number), FormatPhase(Circuit.Phase), FString::FromInt(Circuit.Loads.Num()), FormatWatts(Circuit.Watts),
					FormatAmps(Circuit.Amps), FormatPercent(Circuit.RatingShare) + TEXT(" %"),
					Circuit.bOverloaded ? FStageReportCell(TEXT("OVERLOAD: needs a larger feed"), EStageReportTone::Strong) : FStageReportCell() });
			}
			Composer.EndTable();

			Composer.AddHeading(TEXT("Power cables (one home-run per load)"));
			Composer.BeginTable({ { TEXT("Cable"), 0.8 }, { TEXT("From"), 1.6 }, { TEXT("To"), 2.6 }, { TEXT("Type"), 2.2 }, { TEXT("FID"), 0.6, EStageReportAlign::Right },
				{ TEXT("Load (W)"), 1.0, EStageReportAlign::Right }, { TEXT("Current (A)"), 1.0, EStageReportAlign::Right }, { TEXT("Length"), 0.8, EStageReportAlign::Right } });
			for (const FStageCableRun& Run : Schedule.PowerCables)
			{
				const FStagePlannedLoad& Planned = Schedule.Loads[Run.LoadIndex];
				Composer.AddRow({ Run.CableId, Run.From, Run.To, FStageReportCell(Planned.Load.ItemType, EStageReportTone::Muted),
					Planned.Load.FixtureId > 0 ? FString::FromInt(Planned.Load.FixtureId) : FString(), FormatWatts(Planned.Load.Watts), FormatAmps(Planned.Amps),
					FStageReportCell(FormatStockLength(Run.StockLengthMeters), EStageReportTone::Muted) });
			}
			Composer.EndTable();
		}

		Composer.AddHeading(TEXT("DMX cables (one daisy chain per universe, in address order)"));
		if (Schedule.DmxCables.IsEmpty())
		{
			Composer.AddNote(TEXT("No patched lighting fixtures: there are no DMX runs."), EStageReportTone::Muted);
		}
		else
		{
			Composer.BeginTable({ { TEXT("Cable"), 0.8 }, { TEXT("Univ."), 0.6, EStageReportAlign::Right }, { TEXT("From"), 2.4 }, { TEXT("To"), 2.4 },
				{ TEXT("Address"), 0.9, EStageReportAlign::Right }, { TEXT("Distance (m)"), 1.0, EStageReportAlign::Right }, { TEXT("Cable"), 0.8, EStageReportAlign::Right },
				{ TEXT("Note"), 2.2 } });
			for (const FStageCableRun& Run : Schedule.DmxCables)
			{
				Composer.AddRow({ Run.CableId, FString::FromInt(Run.Universe), Run.From, Run.To, FormatAddress(Run.Universe, Run.ToAddress),
					FormatMeters(Run.DistanceMeters), FormatStockLength(Run.StockLengthMeters),
					FStageReportCell(GetDmxNote(Run), Run.StockLengthMeters < 0.0 ? EStageReportTone::Strong : EStageReportTone::Muted) });
			}
			Composer.EndTable();
			Composer.AddNote(FString::Printf(TEXT("DMX link lengths: straight-line distance + %.0f %% for routing and drops, rounded up to stock lengths."),
				(DmxRoutingSlack - 1.0) * 100.0), EStageReportTone::Muted);
		}
		return Composer.Finish();
	}

	FString MakeFileStem(const FString& SceneName, const FDateTime& GeneratedAt)
	{
		// Same sanitizing as StageBillOfMaterials::MakeFileName: safe on every file system, recognisable to the user.
		FString Safe;
		Safe.Reserve(SceneName.Len());
		for (const TCHAR Character : SceneName.TrimStartAndEnd())
		{
			Safe.AppendChar(FChar::IsAlnum(Character) || Character == TEXT('-') || Character == TEXT('_') ? Character : TEXT('_'));
		}
		if (Safe.IsEmpty())
		{
			Safe = TEXT("Untitled");
		}
		return FString::Printf(TEXT("CableSchedule_%s_%s"), *Safe, *GeneratedAt.ToString(TEXT("%Y%m%d-%H%M%S")));
	}
}
