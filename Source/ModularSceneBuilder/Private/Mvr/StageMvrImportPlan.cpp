// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mvr/StageMvrImportPlan.h"

#include "Data/StageSpanData.h"
#include "Mvr/StageMvrParser.h"
#include "Placement/StageSpanMath.h"

namespace StageMvrImport
{
	namespace
	{
		/** The largest fixture ID kept from a file; larger ones take the next free ID. */
		constexpr int32 MaxFixtureId = 99999;

		/** A run more than this far from horizontal (a tower) is not a truss StageCraft builds as a run. */
		constexpr double MaxRunSlopeDegrees = 45.0;

		struct FNotes
		{
			TArray<FString>& Lines;
			int32 Suppressed = 0;

			void Add(FString Line)
			{
				if (Lines.Num() < MaxNotes)
				{
					Lines.Add(MoveTemp(Line));
				}
				else
				{
					++Suppressed;
				}
			}

			void Finish()
			{
				if (Suppressed > 0)
				{
					Lines.Add(FString::Printf(TEXT("... and %d more."), Suppressed));
				}
			}
		};

		FString NameOf(const FStageMvrObject& Object, const TCHAR* Kind)
		{
			if (!Object.Name.IsEmpty())
			{
				return Object.Name;
			}
			return Object.FixtureId.IsEmpty() ? FString::Printf(TEXT("%s (line %d)"), Kind, Object.Line) : FString::Printf(TEXT("%s %s"), Kind, *Object.FixtureId);
		}

		/** Hands out fixture IDs: the file's when free, otherwise the next free one. */
		struct FFixtureIds
		{
			TSet<int32> Used;
			int32 NextFree = 1;

			int32 Take(int32 Wanted, bool& bRenumbered)
			{
				bRenumbered = false;
				if (Wanted > 0 && Wanted <= MaxFixtureId && !Used.Contains(Wanted))
				{
					Used.Add(Wanted);
					return Wanted;
				}
				bRenumbered = Wanted > 0;
				while (Used.Contains(NextFree))
				{
					++NextFree;
				}
				Used.Add(NextFree);
				return NextFree;
			}
		};

		int32 WantedFixtureId(const FStageMvrObject& Fixture)
		{
			if (Fixture.FixtureIdNumeric > 0)
			{
				return Fixture.FixtureIdNumeric;
			}
			const FString Trimmed = Fixture.FixtureId.TrimStartAndEnd();
			if (!Trimmed.IsEmpty() && Trimmed.Len() <= 9 && Trimmed.IsNumeric() && !Trimmed.Contains(TEXT(".")) && !Trimmed.Contains(TEXT("-")))
			{
				return FCString::Atoi(*Trimmed);
			}
			return 0;
		}

		FVector MvrPointToUnreal(const FVector& MvrMm)
		{
			return FVector(MvrMm.X, -MvrMm.Y, MvrMm.Z) / 10.0;
		}

		/** The union of the bounds of every geometry file the object uses (directly or through Symbols). Invalid when none is known. */
		FBox GeometryOf(const FStageMvrScene& Scene, const FStageMvrObject& Object, const FInputs& Inputs)
		{
			FBox Bounds(ForceInit);
			const auto AddFile = [&Bounds, &Inputs](const FString& File)
			{
				if (const FBox* Known = Inputs.GeometryBounds.Find(File.ToLower()))
				{
					Bounds += *Known;
				}
			};
			for (const FString& File : Object.GeometryFiles)
			{
				AddFile(File);
			}
			for (const FGuid& Symbol : Object.Symbols)
			{
				if (const FStageMvrSymdef* Symdef = Scene.Symdefs.FindByPredicate([&Symbol](const FStageMvrSymdef& Entry) { return Entry.Id == Symbol; }))
				{
					for (const FString& File : Symdef->GeometryFiles)
					{
						AddFile(File);
					}
				}
			}
			return Bounds;
		}

		/** Plans one truss; false (and a note) when it cannot be built as a run. */
		bool PlanTruss(const FStageMvrScene& Scene, const FStageMvrObject& Object, const FInputs& Inputs, const StageSpanMath::FRules& Rules, FTrussPlacement& Out, FString& OutWhy)
		{
			const FBox Local = GeometryOf(Scene, Object, Inputs);
			if (!Local.IsValid)
			{
				OutWhy = TEXT("its geometry could not be read");
				return false;
			}
			// The run follows the longest side of the geometry, along its centre line, at its bottom.
			const FVector Size = Local.GetSize();
			const int32 Axis = Size.X >= Size.Y ? (Size.X >= Size.Z ? 0 : 2) : (Size.Y >= Size.Z ? 1 : 2);
			FVector Start = Local.GetCenter();
			FVector End = Local.GetCenter();
			Start[Axis] = Local.Min[Axis];
			End[Axis] = Local.Max[Axis];
			if (Axis != 2)
			{
				Start.Z = End.Z = Local.Min.Z;
			}
			const FVector WorldStart = MvrPointToUnreal(Object.World.TransformPosition(Start));
			const FVector WorldEnd = MvrPointToUnreal(Object.World.TransformPosition(End));
			const FVector Run = WorldEnd - WorldStart;
			const double Length = Run.Size();
			if (Length < StageSpanMath::MinLegLength)
			{
				OutWhy = TEXT("it is too short for a truss section");
				return false;
			}
			const double Slope = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(FMath::Abs(Run.Z) / Length, 0.0, 1.0)));
			if (Slope > MaxRunSlopeDegrees)
			{
				OutWhy = TEXT("it stands upright (a tower), which StageCraft does not build as a truss run");
				return false;
			}
			const FVector Direction = FVector(Run.X, Run.Y, 0.0).GetSafeNormal();
			const TArray<FVector> Path = { FVector::ZeroVector, Direction * Length };
			const FStageSpanPathLayout Layout = StageSpanMath::ComputePathLayout(Rules, Path, false, 0.0);
			if (!Layout.IsValid())
			{
				OutWhy = TEXT("no whole truss section fits its length");
				return false;
			}
			// Centred on the file's truss: whole sections may be a little shorter or longer than its geometry.
			const FVector Middle = (WorldStart + WorldEnd) * 0.5;
			const FVector Origin = Middle - Direction * (Layout.GetLength() * 0.5);
			Out.MvrId = Object.Id;
			Out.Transform = FTransform(FVector(Origin.X, Origin.Y, FMath::Min(WorldStart.Z, WorldEnd.Z)));
			Out.LocalPoints = Layout.Points;
			Out.Sections = FMath::Max(1, Layout.GetSegmentCount());
			Out.Label = Object.Name;
			Out.Layer = Object.Layer;
			return true;
		}
	}

	FVector BeamDirection(double Pan, double Tilt)
	{
		return FRotator(0.0, Pan, 0.0).RotateVector(FRotator(Tilt, 0.0, 0.0).RotateVector(FVector::UpVector));
	}

	FVector2D SolvePanTilt(const FVector& LocalDirection, const FVector2D& PanRange, const FVector2D& TiltRange, bool& bClamped)
	{
		bClamped = false;
		const FVector Direction = LocalDirection.GetSafeNormal();
		if (Direction.IsNearlyZero())
		{
			return FVector2D::ZeroVector;
		}
		// BeamDirection = (-sin T cos P, -sin T sin P, cos T).
		const double Tilt = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Direction.Z, -1.0, 1.0)));
		const double Pan = FVector2D(Direction.X, Direction.Y).IsNearlyZero(1e-9) ? 0.0 : FMath::RadiansToDegrees(FMath::Atan2(-Direction.Y, -Direction.X));

		const auto Outside = [](double Value, const FVector2D& Range)
		{
			return Value < Range.X ? Range.X - Value : (Value > Range.Y ? Value - Range.Y : 0.0);
		};
		FVector2D Best = FVector2D::ZeroVector;
		double BestOutside = TNumericLimits<double>::Max();
		double BestMovement = TNumericLimits<double>::Max();
		const double Solutions[2][2] = { { Pan, Tilt }, { Pan + 180.0, -Tilt } };
		for (const double (&Solution)[2] : Solutions)
		{
			for (int32 Turn = -3; Turn <= 3; ++Turn)
			{
				const FVector2D Candidate(Solution[0] + 360.0 * Turn, Solution[1]);
				const double Out = Outside(Candidate.X, PanRange) + Outside(Candidate.Y, TiltRange);
				const double Movement = FMath::Abs(Candidate.X) + FMath::Abs(Candidate.Y);
				if (Out < BestOutside - 1e-9 || (FMath::IsNearlyEqual(Out, BestOutside, 1e-9) && Movement < BestMovement))
				{
					Best = Candidate;
					BestOutside = Out;
					BestMovement = Movement;
				}
			}
		}
		if (BestOutside > 1e-6)
		{
			bClamped = true;
			Best.X = FMath::Clamp(Best.X, PanRange.X, PanRange.Y);
			Best.Y = FMath::Clamp(Best.Y, TiltRange.X, TiltRange.Y);
		}
		return Best;
	}

	FTransform FixtureTransform(const FMatrix& MvrWorld)
	{
		const FTransform Converted = StageMvrUnits::ToUnreal(MvrWorld);
		const FTransform Unscaled(Converted.GetRotation().GetNormalized(), Converted.GetLocation());
		// Hanging is MVR's rest pose; a StageCraft fixture at rest stands, beam up: turn it over first.
		return FTransform(FRotator(0.0, 0.0, 180.0)) * Unscaled;
	}

	FPlan Build(const FStageMvrScene& Scene, const FInputs& Inputs)
	{
		FPlan Plan;
		FNotes Notes{ Plan.Notes };
		Plan.TrussItem = Inputs.TrussItem;
		Plan.LayerNames.Reserve(Scene.Layers.Num());
		for (const FStageMvrLayer& Layer : Scene.Layers)
		{
			Plan.LayerNames.Add(Layer.Name);
		}

		TMap<FGuid, int32> ById;
		ById.Reserve(Scene.Objects.Num());
		for (int32 Index = 0; Index < Scene.Objects.Num(); ++Index)
		{
			ById.Add(Scene.Objects[Index].Id, Index);
		}

		FFixtureIds Ids;
		Ids.Used = Inputs.UsedFixtureIds;
		const UStageSpanData* TrussItem = Inputs.TrussItem.Get();
		const StageSpanMath::FRules TrussRules = TrussItem ? StageSpanMath::MakeRules(*TrussItem) : StageSpanMath::FRules();

		for (const FStageMvrObject& Object : Scene.Objects)
		{
			if (Object.Kind != EStageMvrObjectKind::Fixture && Object.Kind != EStageMvrObjectKind::Truss)
			{
				const bool bPlaceable = Object.Kind == EStageMvrObjectKind::SceneObject || Object.Kind == EStageMvrObjectKind::Support
					|| Object.Kind == EStageMvrObjectKind::VideoScreen || Object.Kind == EStageMvrObjectKind::Projector;
				Plan.NotPlacedObjects += bPlaceable ? 1 : 0;
				continue;
			}
			if (Inputs.ExistingMvrIds.Contains(Object.Id))
			{
				++Plan.SkippedExisting;
				continue;
			}

			if (Object.Kind == EStageMvrObjectKind::Truss)
			{
				FString Why;
				FTrussPlacement Truss;
				if (!TrussItem)
				{
					Why = TEXT("there is no truss in the Library to build it with");
				}
				else if (PlanTruss(Scene, Object, Inputs, TrussRules, Truss, Why))
				{
					Plan.Trusses.Add(MoveTemp(Truss));
					continue;
				}
				++Plan.SkippedTrusses;
				Notes.Add(FString::Printf(TEXT("Truss \"%s\" was not placed: %s."), *NameOf(Object, TEXT("Truss")), *Why));
				continue;
			}

			// Fixture.
			const FString Name = NameOf(Object, TEXT("Fixture"));
			if (Object.MultipatchOf.IsValid())
			{
				++Plan.SkippedFixtures;
				Notes.Add(FString::Printf(TEXT("%s is a second patch (multipatch) of another fixture; only the first is placed."), *Name));
				continue;
			}
			const FStageMvrModeTarget* Target = nullptr;
			if (const TMap<FString, FStageMvrModeTarget>* Modes = Object.GdtfSpec.IsEmpty() ? nullptr : Inputs.Modes.Find(Object.GdtfSpec.ToLower()))
			{
				Target = Modes->Find(Object.GdtfMode.ToLower());
			}
			const bool bFallback = !Target || !Target->Item.IsValid();
			if (bFallback)
			{
				Target = Inputs.Fallback.Item.IsValid() ? &Inputs.Fallback : nullptr;
			}
			if (!Target)
			{
				++Plan.SkippedFixtures;
				Notes.Add(FString::Printf(TEXT("%s was not placed: its fixture type is not available and there is no generic fixture to stand in."), *Name));
				continue;
			}

			FFixturePlacement& Fixture = Plan.Fixtures.AddDefaulted_GetRef();
			Fixture.MvrId = Object.Id;
			Fixture.Item = Target->Item;
			Fixture.Transform = FixtureTransform(Object.World);
			Fixture.Label = Object.Name;
			Fixture.Layer = Object.Layer;
			Fixture.bFallback = bFallback;
			if (bFallback)
			{
				++Plan.FallbackFixtures;
				Notes.Add(Object.GdtfSpec.IsEmpty()
					? FString::Printf(TEXT("%s has no GDTF type; it is placed as %s."), *Name, *Target->Label)
					: FString::Printf(TEXT("%s: %s mode \"%s\" is not available; it is placed as %s."), *Name, *Object.GdtfSpec, *Object.GdtfMode, *Target->Label));
			}

			const int32 Wanted = WantedFixtureId(Object);
			bool bRenumbered = false;
			Fixture.FixtureId = Ids.Take(Wanted, bRenumbered);
			if (bRenumbered)
			{
				++Plan.RenumberedFixtures;
				Notes.Add(FString::Printf(TEXT("%s: fixture ID %d is taken; it is fixture %d."), *Name, Wanted, Fixture.FixtureId));
			}

			if (const FStageMvrAddress* Address = Object.Addresses.FindByPredicate([](const FStageMvrAddress& Entry) { return Entry.Break == 0; }))
			{
				Fixture.Patch.Universe = Address->Universe;
				Fixture.Patch.Address = Address->Address;
			}

			const int32* FocusIndex = Object.Focus.IsValid() ? ById.Find(Object.Focus) : nullptr;
			if (FocusIndex && Target->bHasPanTilt)
			{
				const FVector Focus = StageMvrUnits::ToUnreal(Scene.Objects[*FocusIndex].World).GetLocation();
				const FVector Pivot = Fixture.Transform.TransformPosition(Target->PivotOffset);
				const FVector Local = Fixture.Transform.InverseTransformVectorNoScale(Focus - Pivot);
				bool bClamped = false;
				Fixture.PanTilt = SolvePanTilt(Local, Target->PanRange, Target->TiltRange, bClamped);
				++Plan.AimedFixtures;
				if (bClamped)
				{
					++Plan.ClampedAims;
					Notes.Add(FString::Printf(TEXT("%s cannot reach its focus point within its pan and tilt; it aims as close as it can."), *Name));
				}
			}
		}
		Notes.Finish();
		return Plan;
	}
}
