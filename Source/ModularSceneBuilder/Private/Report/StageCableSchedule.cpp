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

		/**
		 * Makes circuits outputs of distros in circuit order. A distro is full when its outputs are used or one more circuit would
		 * take a phase of its feed past the rating; a circuit that alone exceeds the feed still gets a distro (it is already
		 * reported as overloaded or needs a larger feed).
		 */
		void AssignDistros(FStageCableSchedule& Schedule)
		{
			const FStageCablingRules& Rules = Schedule.Cabling;
			for (FStagePowerCircuit& Circuit : Schedule.Circuits)
			{
				const int32 PhaseSlot = FMath::Clamp(Circuit.Phase - 1, 0, 2);
				FStagePowerDistro* Distro = Schedule.Distros.IsEmpty() ? nullptr : &Schedule.Distros.Last();
				const bool bFits = Distro && Distro->CircuitCount < Rules.DistroOutputs
					&& Distro->PhaseAmps[PhaseSlot] + Circuit.Amps <= Rules.DistroFeedAmps + 1e-6;
				if (!bFits)
				{
					Distro = &Schedule.Distros.AddDefaulted_GetRef();
					Distro->Number = Schedule.Distros.Num();
				}
				++Distro->CircuitCount;
				Distro->PhaseAmps[PhaseSlot] += Circuit.Amps;
				Circuit.DistroNumber = Distro->Number;
			}
		}

		/** The centre of Locations on the floor (Z = 0); Fallback when there are none. */
		FVector GetFloorCentre(TConstArrayView<FVector> Locations, const FVector& Fallback)
		{
			if (Locations.IsEmpty())
			{
				return Fallback;
			}
			FVector Sum = FVector::ZeroVector;
			for (const FVector& Location : Locations)
			{
				Sum += SanitizeLocation(Location);
			}
			const FVector Centre = Sum / Locations.Num();
			return FVector(Centre.X, Centre.Y, 0.0);
		}

		void ResolveAnchors(FStageCableSchedule& Schedule, TConstArrayView<FStageDmxNode> Nodes)
		{
			const FStageCablingRules& Rules = Schedule.Cabling;
			TArray<FVector> LoadLocations;
			LoadLocations.Reserve(Schedule.Loads.Num());
			for (const FStagePlannedLoad& Planned : Schedule.Loads)
			{
				LoadLocations.Add(Planned.Load.LocationMeters);
			}
			Schedule.DistroLocationMeters = Rules.DistroAnchor == EStageCableAnchor::Fixed
				? Rules.DistroLocationMeters : GetFloorCentre(LoadLocations, FVector::ZeroVector);

			TArray<FVector> NodeLocations;
			NodeLocations.Reserve(Nodes.Num());
			for (const FStageDmxNode& Node : Nodes)
			{
				if (IsPatched(Node))
				{
					NodeLocations.Add(Node.LocationMeters);
				}
			}
			// With no patched fixture the node position is printed only; the distros are the natural place for it.
			Schedule.ControlLocationMeters = Rules.ControlAnchor == EStageCableAnchor::Fixed
				? Rules.ControlLocationMeters : GetFloorCentre(NodeLocations, Schedule.DistroLocationMeters);
		}

		void MeasureRun(FStageCableRun& Run, const FVector& From, const FVector& To, const FStageCablingRules& Rules)
		{
			Run.DistanceMeters = StageShoppingList::GetRouteMeters(From, To);
			Run.Cable = StageShoppingList::AssembleRun(Run.DistanceMeters, Rules);
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
				Run.From = FString::Printf(TEXT("PD%d %s / %s"), Circuit.DistroNumber, *FormatCircuit(Circuit.Number), *FormatPhase(Circuit.Phase));
				Run.To = GetLoadName(Planned.Load);
				Run.LoadIndex = LoadIndex;
				MeasureRun(Run, Schedule.DistroLocationMeters, Planned.Load.LocationMeters, Schedule.Cabling);
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
					Run.From = FString::Printf(TEXT("DMX node out U%d"), Node.Universe);
					MeasureRun(Run, Schedule.ControlLocationMeters, Node.LocationMeters, Schedule.Cabling);
					continue;
				}

				const FStageDmxNode& Previous = Patched[Index - 1];
				Run.From = GetNodeName(Previous);
				MeasureRun(Run, Previous.LocationMeters, Node.LocationMeters, Schedule.Cabling);
			}
		}

		FString FormatWatts(double Value) { return FString::Printf(TEXT("%.0f"), Value); }
		FString FormatAmps(double Value) { return FString::Printf(TEXT("%.1f"), Value); }
		FString FormatPercent(double Share) { return FString::Printf(TEXT("%.0f"), Share * 100.0); }
		FString FormatMeters(double Value) { return FString::Printf(TEXT("%.1f"), Value); }

		FString FormatLocation(const FVector& Meters)
		{
			return FString::Printf(TEXT("X %.1f, Y %.1f, Z %.1f m"), Meters.X, Meters.Y, Meters.Z);
		}

		FString FormatAddress(int32 Universe, int32 Address)
		{
			return FString::Printf(TEXT("%d.%03d"), Universe, Address);
		}

		FString GetCableNote(const FStageCableRun& Run)
		{
			FString Note = Run.Cable.IsJoined() ? FString(TEXT("Joined cables")) : FString();
			if (Run.bTerminate)
			{
				Note += Note.IsEmpty() ? TEXT("Terminate DMX out") : TEXT("; terminate DMX out");
			}
			return Note;
		}

		FString DescribeAnchor(EStageCableAnchor Anchor, const TCHAR* AutomaticRule)
		{
			return Anchor == EStageCableAnchor::Fixed ? FString(TEXT("fixed in Project Settings")) : FString::Printf(TEXT("automatic: %s"), AutomaticRule);
		}

		FString DescribeStock(const FStageCablingRules& Rules)
		{
			TArray<FString> Lengths;
			for (const double Length : Rules.StockLengthsMeters)
			{
				Lengths.Add(FString::Printf(TEXT("%g"), Length));
			}
			return FString::Join(Lengths, TEXT("/")) + TEXT(" m");
		}

		/** How the lengths were found, printed under the wiring list and in the CSV title block. */
		FString DescribeMeasuring(const FStageCableSchedule& Schedule)
		{
			return FString::Printf(TEXT("Lengths: route along floor and truss (horizontal distance + height difference) + %.0f %% safety margin, rounded up to stock %s; longer runs join several cables."),
				Schedule.Cabling.SafetyMargin * 100.0, *DescribeStock(Schedule.Cabling));
		}

		FString DescribeDistro(const FStageCableSchedule& Schedule)
		{
			return FString::Printf(TEXT("Distros at %s (%s)"), *FormatLocation(Schedule.DistroLocationMeters),
				*DescribeAnchor(Schedule.Cabling.DistroAnchor, TEXT("centre of the loads, on the floor")));
		}

		FString DescribeControl(const FStageCableSchedule& Schedule)
		{
			return FString::Printf(TEXT("DMX control node at %s (%s)"), *FormatLocation(Schedule.ControlLocationMeters),
				*DescribeAnchor(Schedule.Cabling.ControlAnchor, TEXT("centre of the patched fixtures, on the floor")));
		}

		double GetMaxPhaseAmps(const FStagePowerDistro& Distro)
		{
			return FMath::Max3(Distro.PhaseAmps[0], Distro.PhaseAmps[1], Distro.PhaseAmps[2]);
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

	FStageCableSchedule Build(TArray<FStagePowerLoad> Loads, TArray<FStageDmxNode> Nodes, TConstArrayView<FStageRiggedFixture> Fixtures,
		const FStagePowerDistribution& Supply, const FStageCablingRules& Cabling, const FString& SceneName, const FDateTime& GeneratedAt)
	{
		FStageCableSchedule Schedule;
		Schedule.SceneName = SceneName;
		Schedule.GeneratedAt = GeneratedAt;
		Schedule.Supply = Sanitize(Supply);
		Schedule.Cabling = StageShoppingList::Sanitize(Cabling);

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
		AssignDistros(Schedule);
		ResolveAnchors(Schedule, Nodes);
		ListPowerCables(Schedule);
		ChainDmx(Schedule, MoveTemp(Nodes));
		Schedule.Shopping = StageShoppingList::Build(Schedule, Fixtures);
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
		const FStageShoppingList& Shopping = Schedule.Shopping;
		if (Shopping.OverweightFixtures > 0)
		{
			Warnings.Add(FString::Printf(TEXT("%s heavier than the strongest stocked safety bond (WLL %.0f kg): a rigger must specify the secondary suspension."),
				*Plural(Shopping.OverweightFixtures, TEXT("rigged fixture is"), TEXT("rigged fixtures are")), Schedule.Cabling.SafetyBondRatingsKg.Last()));
		}
		if (Shopping.FixturesWithoutWeight > 0)
		{
			Warnings.Add(FString::Printf(TEXT("%s without weight data in the catalog: one clamp and the lightest bond are listed; check their rigging."),
				*Plural(Shopping.FixturesWithoutWeight, TEXT("rigged fixture"), TEXT("rigged fixtures"))));
		}
		if (Schedule.UnpatchedFixtures > 0)
		{
			Warnings.Add(FString::Printf(TEXT("%s unpatched and in no DMX chain."), *Plural(Schedule.UnpatchedFixtures, TEXT("lighting fixture is"), TEXT("lighting fixtures are"))));
		}
		return Warnings;
	}

	FString ToCsv(const FStageCableSchedule& Schedule)
	{
		const FStageShoppingList& Shopping = Schedule.Shopping;
		FString Out;
		Out.Reserve(1024 + (Schedule.Loads.Num() + Schedule.DmxCables.Num() + Schedule.Circuits.Num() + Shopping.Lines.Num()) * 96);

		// U+FEFF: written as the UTF-8 byte order mark, which spreadsheet programs need to read non-ASCII names correctly.
		Out += TEXT("﻿");
		AppendRow(Out, { TEXT("StageCraft 3D Cable Schedule, Power Plan and Shopping List (generated proposal)") });
		AppendRow(Out, { TEXT("Scene"), GetDisplaySceneName(Schedule.SceneName) });
		AppendRow(Out, { TEXT("Generated"), Schedule.GeneratedAt.ToString(TEXT("%Y-%m-%d %H:%M")) });
		AppendRow(Out, { TEXT("Supply"), DescribeSupply(Schedule.Supply) });
		AppendRow(Out, { TEXT("Total load (W)"), FormatWatts(Schedule.TotalWatts) });
		AppendRow(Out, { TEXT("Phase imbalance (%)"), FString::Printf(TEXT("%.0f"), Schedule.ImbalancePercent) });
		AppendRow(Out, { TEXT("Distros"), DescribeDistro(Schedule) });
		AppendRow(Out, { TEXT("DMX control"), DescribeControl(Schedule) });
		AppendRow(Out, { TEXT("Cable lengths"), DescribeMeasuring(Schedule) });
		for (const FString& Warning : GetWarnings(Schedule))
		{
			AppendRow(Out, { TEXT("Warning"), Warning });
		}
		Out += LineEnd;

		AppendRow(Out, { TEXT("Section"), TEXT("Item"), TEXT("Size"), TEXT("Quantity"), TEXT("Note") });
		for (const FStageShoppingLine& Line : Shopping.Lines)
		{
			AppendRow(Out, { StageShoppingList::GetSectionName(Line.Section), Line.Item, Line.Size, FString::FromInt(Line.Quantity), Line.Note });
		}
		AppendRow(Out, { TEXT("Total"), TEXT("Power cable"), FormatMeters(Shopping.PowerCableMeters) + TEXT(" m"), FString::FromInt(Shopping.PowerCableCount), FString() });
		AppendRow(Out, { TEXT("Total"), TEXT("DMX cable"), FormatMeters(Shopping.DmxCableMeters) + TEXT(" m"), FString::FromInt(Shopping.DmxCableCount), FString() });
		AppendRow(Out, { TEXT("Total"), TEXT("Rigged fixture weight"), FString::Printf(TEXT("%.1f kg"), Shopping.RiggedWeightKg), FString::FromInt(Shopping.RiggedFixtures),
			TEXT("Fixtures only: add truss, clamps and cable") });
		Out += LineEnd;

		AppendRow(Out, { TEXT("Phase"), TEXT("Circuits"), TEXT("Load (W)"), TEXT("Current (A)"), TEXT("Share of load (%)") });
		for (const FStagePowerPhase& Phase : Schedule.Phases)
		{
			AppendRow(Out, { FormatPhase(Phase.Phase), FString::FromInt(Phase.CircuitCount), FormatWatts(Phase.Watts), FormatAmps(Phase.Amps),
				FormatPercent(Schedule.TotalWatts > 0.0 ? Phase.Watts / Schedule.TotalWatts : 0.0) });
		}
		Out += LineEnd;

		AppendRow(Out, { TEXT("Distro"), TEXT("Circuits"), TEXT("Highest phase current (A)"), TEXT("Use of feed (%)") });
		for (const FStagePowerDistro& Distro : Schedule.Distros)
		{
			AppendRow(Out, { FString::Printf(TEXT("PD%d"), Distro.Number), FString::FromInt(Distro.CircuitCount), FormatAmps(GetMaxPhaseAmps(Distro)),
				FormatPercent(GetMaxPhaseAmps(Distro) / Schedule.Cabling.DistroFeedAmps) });
		}
		Out += LineEnd;

		AppendRow(Out, { TEXT("Circuit"), TEXT("Distro"), TEXT("Phase"), TEXT("Loads"), TEXT("Load (W)"), TEXT("Current (A)"), TEXT("Use of rating (%)"), TEXT("Note") });
		for (const FStagePowerCircuit& Circuit : Schedule.Circuits)
		{
			AppendRow(Out, { FormatCircuit(Circuit.Number), FString::Printf(TEXT("PD%d"), Circuit.DistroNumber), FormatPhase(Circuit.Phase), FString::FromInt(Circuit.Loads.Num()),
				FormatWatts(Circuit.Watts), FormatAmps(Circuit.Amps), FormatPercent(Circuit.RatingShare), Circuit.bOverloaded ? FString(TEXT("OVERLOAD: needs a larger feed")) : FString() });
		}
		Out += LineEnd;

		AppendRow(Out, { TEXT("Cable"), TEXT("From"), TEXT("To"), TEXT("Type"), TEXT("Fixture ID"), TEXT("Load (W)"), TEXT("Current (A)"), TEXT("Route (m)"), TEXT("Cables"), TEXT("Note") });
		for (const FStageCableRun& Run : Schedule.PowerCables)
		{
			const FStagePlannedLoad& Planned = Schedule.Loads[Run.LoadIndex];
			AppendRow(Out, { Run.CableId, Run.From, Run.To, Planned.Load.ItemType, Planned.Load.FixtureId > 0 ? FString::FromInt(Planned.Load.FixtureId) : FString(),
				FormatWatts(Planned.Load.Watts), FormatAmps(Planned.Amps), FormatMeters(Run.DistanceMeters), StageShoppingList::FormatAssembly(Run.Cable), GetCableNote(Run) });
		}
		Out += LineEnd;

		AppendRow(Out, { TEXT("Cable"), TEXT("Universe"), TEXT("From"), TEXT("To"), TEXT("Start address"), TEXT("Route (m)"), TEXT("Cables"), TEXT("Note") });
		for (const FStageCableRun& Run : Schedule.DmxCables)
		{
			AppendRow(Out, { Run.CableId, FString::FromInt(Run.Universe), Run.From, Run.To, FormatAddress(Run.Universe, Run.ToAddress),
				FormatMeters(Run.DistanceMeters), StageShoppingList::FormatAssembly(Run.Cable), GetCableNote(Run) });
		}
		return Out;
	}

	TArray<uint8> ToPdf(const FStageCableSchedule& Schedule)
	{
		const FStageShoppingList& Shopping = Schedule.Shopping;
		FStageReportPdfComposer Composer(TEXT("CABLE SCHEDULE & POWER"), TEXT("Cable schedule, power plan & shopping list (generated proposal)"), Schedule.SceneName, Schedule.GeneratedAt);
		Composer.SetSummary(FString::Printf(TEXT("%s · %.2f kW · %s on %s · %s · imbalance %.0f %% · %s in %s · %s"),
			*Plural(Schedule.Loads.Num(), TEXT("load"), TEXT("loads")), Schedule.TotalWatts / 1000.0,
			*Plural(Schedule.Circuits.Num(), TEXT("circuit"), TEXT("circuits")), *Plural(Schedule.Phases.Num(), TEXT("phase"), TEXT("phases")),
			*Plural(Schedule.Distros.Num(), TEXT("distro"), TEXT("distros")), Schedule.ImbalancePercent,
			*Plural(Schedule.DmxCables.Num(), TEXT("DMX link"), TEXT("DMX links")), *Plural(Schedule.Universes.Num(), TEXT("universe"), TEXT("universes")),
			*Plural(Shopping.PowerCableCount + Shopping.DmxCableCount, TEXT("cable"), TEXT("cables"))));

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
		Composer.AddNote(TEXT("Generated proposal: loads are balanced automatically from each item's maximum power draw. Confirm circuits, cable runs and rigging on site."),
			EStageReportTone::Muted);

		Composer.AddHeading(TEXT("Shopping & assembly list"));
		if (Shopping.IsEmpty())
		{
			Composer.AddNote(TEXT("Nothing on the stage needs cables or rigging hardware."), EStageReportTone::Muted);
		}
		else
		{
			Composer.BeginTable({ { TEXT("Section"), 1.4 }, { TEXT("Item"), 2.6 }, { TEXT("Size"), 2.2 }, { TEXT("Qty"), 0.7, EStageReportAlign::Right }, { TEXT("Note"), 3.4 } });
			for (const FStageShoppingLine& Line : Shopping.Lines)
			{
				Composer.AddRow({ FStageReportCell(StageShoppingList::GetSectionName(Line.Section), EStageReportTone::Muted), Line.Item,
					FStageReportCell(Line.Size, EStageReportTone::Muted), FString::Printf(TEXT("%dx"), Line.Quantity),
					FStageReportCell(Line.Note, Line.bNeedsAttention ? EStageReportTone::Strong : EStageReportTone::Muted) });
			}
			Composer.AddRow({ TEXT("Cable to bring"), FString::Printf(TEXT("Power %.0f m, DMX %.0f m"), Shopping.PowerCableMeters, Shopping.DmxCableMeters), FString(),
				FString::Printf(TEXT("%dx"), Shopping.PowerCableCount + Shopping.DmxCableCount),
				FString::Printf(TEXT("Rigged: %s, %.1f kg (fixtures only)"), *Plural(Shopping.RiggedFixtures, TEXT("fixture"), TEXT("fixtures")), Shopping.RiggedWeightKg) },
				EStageReportRowStyle::Total);
			Composer.EndTable();
		}
		Composer.AddNote(DescribeMeasuring(Schedule), EStageReportTone::Muted);
		Composer.AddNote(DescribeDistro(Schedule) + TEXT(" · ") + DescribeControl(Schedule), EStageReportTone::Muted);
		Composer.AddNote(FString::Printf(TEXT("Rigged: at least %.1f m high or tilted over 30 degrees. One clamp per %.0f kg of fixture; one safety bond per fixture, the lightest rated for its weight."),
			Schedule.Cabling.RiggedAboveMeters, Schedule.Cabling.WeightPerClampKg), EStageReportTone::Muted);

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

			Composer.AddHeading(TEXT("Distros"));
			Composer.BeginTable({ { TEXT("Distro"), 1.0 }, { TEXT("Circuits"), 1.0, EStageReportAlign::Right }, { TEXT("Highest phase (A)"), 1.4, EStageReportAlign::Right },
				{ TEXT("Use of feed"), 1.2, EStageReportAlign::Right }, { FString(), 4.0 } });
			for (const FStagePowerDistro& Distro : Schedule.Distros)
			{
				Composer.AddRow({ FString::Printf(TEXT("PD%d"), Distro.Number), FString::Printf(TEXT("%d of %d"), Distro.CircuitCount, Schedule.Cabling.DistroOutputs),
					FormatAmps(GetMaxPhaseAmps(Distro)), FormatPercent(GetMaxPhaseAmps(Distro) / Schedule.Cabling.DistroFeedAmps) + TEXT(" %") });
			}
			Composer.EndTable();

			Composer.AddHeading(TEXT("Circuits"));
			Composer.BeginTable({ { TEXT("Circuit"), 1.0 }, { TEXT("Distro"), 0.8 }, { TEXT("Phase"), 0.8 }, { TEXT("Loads"), 1.0, EStageReportAlign::Right },
				{ TEXT("Load (W)"), 1.2, EStageReportAlign::Right }, { TEXT("Current (A)"), 1.2, EStageReportAlign::Right }, { TEXT("Use of rating"), 1.2, EStageReportAlign::Right },
				{ TEXT("Note"), 3.0 } });
			for (const FStagePowerCircuit& Circuit : Schedule.Circuits)
			{
				Composer.AddRow({ FormatCircuit(Circuit.Number), FString::Printf(TEXT("PD%d"), Circuit.DistroNumber), FormatPhase(Circuit.Phase), FString::FromInt(Circuit.Loads.Num()),
					FormatWatts(Circuit.Watts), FormatAmps(Circuit.Amps), FormatPercent(Circuit.RatingShare) + TEXT(" %"),
					Circuit.bOverloaded ? FStageReportCell(TEXT("OVERLOAD: needs a larger feed"), EStageReportTone::Strong) : FStageReportCell() });
			}
			Composer.EndTable();

			Composer.AddHeading(TEXT("Power cables (one home-run per load)"));
			Composer.BeginTable({ { TEXT("Cable"), 0.7 }, { TEXT("From"), 1.5 }, { TEXT("To"), 2.3 }, { TEXT("Type"), 1.9 }, { TEXT("FID"), 0.5, EStageReportAlign::Right },
				{ TEXT("Load (W)"), 0.8, EStageReportAlign::Right }, { TEXT("Current (A)"), 0.9, EStageReportAlign::Right }, { TEXT("Route (m)"), 0.8, EStageReportAlign::Right },
				{ TEXT("Cables"), 1.2, EStageReportAlign::Right } });
			for (const FStageCableRun& Run : Schedule.PowerCables)
			{
				const FStagePlannedLoad& Planned = Schedule.Loads[Run.LoadIndex];
				Composer.AddRow({ Run.CableId, Run.From, Run.To, FStageReportCell(Planned.Load.ItemType, EStageReportTone::Muted),
					Planned.Load.FixtureId > 0 ? FString::FromInt(Planned.Load.FixtureId) : FString(), FormatWatts(Planned.Load.Watts), FormatAmps(Planned.Amps),
					FStageReportCell(FormatMeters(Run.DistanceMeters), EStageReportTone::Muted),
					FStageReportCell(StageShoppingList::FormatAssembly(Run.Cable), Run.Cable.IsJoined() ? EStageReportTone::Strong : EStageReportTone::Normal) });
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
			Composer.BeginTable({ { TEXT("Cable"), 0.8 }, { TEXT("Univ."), 0.6, EStageReportAlign::Right }, { TEXT("From"), 2.2 }, { TEXT("To"), 2.2 },
				{ TEXT("Address"), 0.9, EStageReportAlign::Right }, { TEXT("Route (m)"), 0.9, EStageReportAlign::Right }, { TEXT("Cables"), 1.2, EStageReportAlign::Right },
				{ TEXT("Note"), 2.0 } });
			for (const FStageCableRun& Run : Schedule.DmxCables)
			{
				Composer.AddRow({ Run.CableId, FString::FromInt(Run.Universe), Run.From, Run.To, FormatAddress(Run.Universe, Run.ToAddress),
					FStageReportCell(FormatMeters(Run.DistanceMeters), EStageReportTone::Muted),
					FStageReportCell(StageShoppingList::FormatAssembly(Run.Cable), Run.Cable.IsJoined() ? EStageReportTone::Strong : EStageReportTone::Normal),
					FStageReportCell(GetCableNote(Run), EStageReportTone::Muted) });
			}
			Composer.EndTable();
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
