// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mvr/StageMvrImportPlan.h"

#include "Data/StageSpanData.h"
#include "Misc/Crc.h"
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

		/**
		 * Re-import without fingerprints (items imported before MV5) compares the file's placement with the stage's: closer than this
		 * is "not moved". Half a centimetre and a tenth of a degree are below what a plot states and above float noise.
		 */
		constexpr double SameLocationCm = 0.5;
		constexpr double SameAngleDegrees = 0.1;

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

		/** The patch the file gives a fixture: its first break-0 address; unpatched without one. */
		FStageDMXPatch FilePatch(const FStageMvrObject& Fixture)
		{
			FStageDMXPatch Patch;
			if (const FStageMvrAddress* Address = Fixture.Addresses.FindByPredicate([](const FStageMvrAddress& Entry) { return Entry.Break == 0; }))
			{
				Patch.Universe = Address->Universe;
				Patch.Address = Address->Address;
			}
			return Patch;
		}

		FVector MvrPointToUnreal(const FVector& MvrMm)
		{
			return FVector(MvrMm.X, -MvrMm.Y, MvrMm.Z) / 10.0;
		}

		/** The union of the bounds of every geometry file the object uses (directly or through Symbols). Invalid when none is known. */
		FBox GeometryOf(const FStageMvrScene& Scene, const FStageMvrObject& Object, const TMap<FString, FBox>& GeometryBounds)
		{
			FBox Bounds(ForceInit);
			const auto AddFile = [&Bounds, &GeometryBounds](const FString& File)
			{
				if (const FBox* Known = GeometryBounds.Find(File.ToLower()))
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
			const FBox Local = GeometryOf(Scene, Object, Inputs.GeometryBounds);
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

		uint32 NonZero(uint32 Hash)
		{
			return Hash == 0 ? 1u : Hash;
		}

		uint32 HashText(const FString& Text)
		{
			return NonZero(FCrc::StrCrc32(*Text));
		}

		/** A matrix as text a hash can take: rotation and scale to 1e-5, the offset to a tenth of a millimetre, so float noise is equal. */
		FString MatrixKey(const FMatrix& Matrix)
		{
			FString Key;
			for (int32 Row = 0; Row < 4; ++Row)
			{
				const double Step = Row < 3 ? 1e-5 : 0.1;
				for (int32 Column = 0; Column < 3; ++Column)
				{
					Key += FString::Printf(TEXT("%lld,"), FMath::RoundToInt64(Matrix.M[Row][Column] / Step));
				}
			}
			return Key;
		}

		FString BoxKey(const FBox& Box)
		{
			if (!Box.IsValid)
			{
				return TEXT("-");
			}
			return FString::Printf(TEXT("%lld,%lld,%lld,%lld,%lld,%lld"), FMath::RoundToInt64(Box.Min.X * 10.0), FMath::RoundToInt64(Box.Min.Y * 10.0),
				FMath::RoundToInt64(Box.Min.Z * 10.0), FMath::RoundToInt64(Box.Max.X * 10.0), FMath::RoundToInt64(Box.Max.Y * 10.0), FMath::RoundToInt64(Box.Max.Z * 10.0));
		}

		bool SameTransform(const FTransform& A, const FTransform& B)
		{
			return A.GetLocation().Equals(B.GetLocation(), SameLocationCm)
				&& FMath::RadiansToDegrees(A.GetRotation().AngularDistance(B.GetRotation())) <= SameAngleDegrees;
		}

		bool SamePath(const TArray<FVector>& A, const TArray<FVector>& B)
		{
			if (A.Num() != B.Num())
			{
				return false;
			}
			for (int32 Index = 0; Index < A.Num(); ++Index)
			{
				if (!A[Index].Equals(B[Index], SameLocationCm))
				{
					return false;
				}
			}
			return true;
		}

		/**
		 * Whether Item came from the plot Scene is a version of, so that missing from Scene means "the designer took it out": one of its
		 * layers. A file name alone is not enough when the item knows its layer (two venues' plots are easily both "Plot.mvr"); it only
		 * counts for items recorded without a layer. Items on a layer the new file dropped entirely are therefore kept, never removed.
		 */
		bool IsSamePlot(const FStageMvrExistingItem& Item, const TSet<FGuid>& LayerIds, const FString& SourceFile)
		{
			if (Item.Origin.Layer.IsValid())
			{
				return LayerIds.Contains(Item.Origin.Layer);
			}
			return !Item.Origin.SourceFile.IsEmpty() && Item.Origin.SourceFile.Equals(SourceFile, ESearchCase::IgnoreCase);
		}
		/** The fixtures and trusses a plan places or updates: not multipatches (only the first patch of a fixture is placed). */
		bool IsPlanned(const FStageMvrObject& Object)
		{
			return Object.Kind == EStageMvrObjectKind::Truss || (Object.Kind == EStageMvrObjectKind::Fixture && !Object.MultipatchOf.IsValid());
		}

		/** What the item records about Object for the next import. */
		FStageMvrItemOrigin MakeOrigin(const FStageMvrScene& Scene, const FStageMvrObject& Object, const FFingerprint& Print, const FString& SourceFile)
		{
			FStageMvrItemOrigin Origin;
			Origin.Layer = Scene.Layers.IsValidIndex(Object.Layer) ? Scene.Layers[Object.Layer].Id : FGuid();
			Origin.SourceFile = SourceFile;
			Origin.Placement = Print.Placement;
			Origin.Name = Print.Name;
			Origin.Patch = Print.Patch;
			Origin.Type = Print.Type;
			Origin.Aim = Print.Aim;
			Origin.Preserved = Object.Preserved;
			return Origin;
		}

		/**
		 * What an updated item records: the file's fingerprints, except that a locked item keeps the last import's placement and aim,
		 * so once it is unlocked the next import moves it to where the file has it. (An item without fingerprints has no last import
		 * to keep: its locked place is taken as the accepted one.)
		 */
		FStageMvrItemOrigin UpdatedOrigin(const FStageMvrScene& Scene, const FStageMvrObject& Object, const FFingerprint& Print, const FString& SourceFile,
			const FStageMvrExistingItem& Existing, bool bKeptLocked)
		{
			FStageMvrItemOrigin Origin = MakeOrigin(Scene, Object, Print, SourceFile);
			if (bKeptLocked && Existing.Origin.HasFingerprints())
			{
				Origin.Placement = Existing.Origin.Placement;
				Origin.Aim = Existing.Origin.Aim;
			}
			return Origin;
		}

		/**
		 * Which fields of Existing the file changed. With fingerprints: the fields whose hash differs from the last import's. Without
		 * (imported before MV5): placement, name and patch compared with the stage (the planned truss run for a truss; null when it
		 * cannot be planned, so its placement counts as unchanged); type and aim are not known and left alone, and Origin is always
		 * set so the item is updated once to record its fingerprints. A locked item's placement and aim are never changed
		 * (bOutKeptLocked tells the caller to say so and to keep the last import's placement and aim fingerprints).
		 */
		EChange CompareToFile(const FStageMvrObject& Object, const FFingerprint& Print, const FStageMvrExistingItem& Existing, const FTrussPlacement* PlannedTruss);

		EChange CompareFields(const FStageMvrObject& Object, const FFingerprint& Print, const FStageMvrExistingItem& Existing, const FTrussPlacement* PlannedTruss, bool& bOutKeptLocked)
		{
			const EChange Changes = CompareToFile(Object, Print, Existing, PlannedTruss);
			// The user locked it: moving or resizing it is refused everywhere else, so the file does not do it either. Its aim follows
			// its placement (the aim fingerprint includes it), so the home look the user set from where it hangs stays too.
			bOutKeptLocked = Existing.bLocked && EnumHasAnyFlags(Changes, EChange::Placement | EChange::Aim);
			return bOutKeptLocked ? Changes & ~(EChange::Placement | EChange::Aim) : Changes;
		}

		EChange CompareToFile(const FStageMvrObject& Object, const FFingerprint& Print, const FStageMvrExistingItem& Existing, const FTrussPlacement* PlannedTruss)
		{
			EChange Changes = EChange::None;
			// The UUID held something else (a fixture is now a truss or the other way round): it is replaced as a new kind of item.
			if (Existing.bFixture != (Object.Kind == EStageMvrObjectKind::Fixture))
			{
				Changes |= EChange::Type | EChange::Placement;
			}
			const FStageMvrItemOrigin& Base = Existing.Origin;
			if (Base.HasFingerprints())
			{
				Changes |= Print.Placement != Base.Placement ? EChange::Placement : EChange::None;
				Changes |= Print.Name != Base.Name ? EChange::Name : EChange::None;
				Changes |= Print.Patch != Base.Patch ? EChange::Patch : EChange::None;
				Changes |= Print.Type != Base.Type ? EChange::Type : EChange::None;
				Changes |= Print.Aim != Base.Aim ? EChange::Aim : EChange::None;
				// A fixture's preserved XML follows the file too; it changes nothing the user sees, so it never makes an update alone.
				return Changes;
			}
			if (Object.Kind == EStageMvrObjectKind::Fixture)
			{
				Changes |= !SameTransform(FixtureTransform(Object.World), Existing.Transform) ? EChange::Placement : EChange::None;
				Changes |= !(FilePatch(Object) == Existing.Patch) ? EChange::Patch : EChange::None;
			}
			else if (PlannedTruss)
			{
				Changes |= !SameTransform(PlannedTruss->Transform, Existing.Transform) || !SamePath(PlannedTruss->LocalPoints, Existing.TrussPoints) ? EChange::Placement : EChange::None;
			}
			Changes |= !Object.Name.IsEmpty() && Object.Name != Existing.Label ? EChange::Name : EChange::None;
			// Once updated, the item carries fingerprints, and from then on only the file's own changes are applied.
			return Changes | EChange::Origin;
		}

		/** The file's objects by UUID, the existing items by MVR UUID, and the scene's layer ids. */
		struct FIndex
		{
			TMap<FGuid, int32> Objects;
			TMap<FGuid, const FStageMvrExistingItem*> Existing;
			TSet<FGuid> LayerIds;

			FIndex(const FStageMvrScene& Scene, const FInputs& Inputs)
			{
				Objects.Reserve(Scene.Objects.Num());
				for (int32 Index = 0; Index < Scene.Objects.Num(); ++Index)
				{
					Objects.Add(Scene.Objects[Index].Id, Index);
				}
				Existing.Reserve(Inputs.Existing.Num());
				for (const FStageMvrExistingItem& Item : Inputs.Existing)
				{
					if (Item.MvrId.IsValid())
					{
						Existing.FindOrAdd(Item.MvrId, &Item);
					}
				}
				for (const FStageMvrLayer& Layer : Scene.Layers)
				{
					LayerIds.Add(Layer.Id);
				}
			}

			/** Items of this plot whose UUID the file no longer has. */
			TArray<const FStageMvrExistingItem*> GetMissing(const FInputs& Inputs) const
			{
				TArray<const FStageMvrExistingItem*> Missing;
				for (const FStageMvrExistingItem& Item : Inputs.Existing)
				{
					if (Item.MvrId.IsValid() && !Objects.Contains(Item.MvrId) && IsSamePlot(Item, LayerIds, Inputs.SourceFile))
					{
						Missing.Add(&Item);
					}
				}
				return Missing;
			}
		};

		/** The truss run a base-less comparison needs; false when it cannot be planned (then its placement is "unchanged"). */
		bool PlanTrussForCompare(const FStageMvrScene& Scene, const FStageMvrObject& Object, const FInputs& Inputs, const FStageMvrExistingItem& Existing, FTrussPlacement& Out)
		{
			// With the run's own truss type, as Build plans it: another type's sections would make a run that did not move look moved.
			const UStageSpanData* Own = Cast<UStageSpanData>(Existing.Item.Get());
			const UStageSpanData* TrussItem = Own ? Own : Inputs.TrussItem.Get();
			FString Why;
			return !Existing.Origin.HasFingerprints() && TrussItem && PlanTruss(Scene, Object, Inputs, StageSpanMath::MakeRules(*TrussItem), Out, Why);
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

	FFingerprint Fingerprint(const FStageMvrScene& Scene, const FStageMvrObject& Object, const TMap<FString, FBox>& GeometryBounds)
	{
		FFingerprint Print;
		const FString Placement = MatrixKey(Object.World);
		// A truss run is built from its geometry's bounds, so a changed model is a changed placement.
		Print.Placement = HashText(Object.Kind == EStageMvrObjectKind::Truss ? Placement + TEXT("|") + BoxKey(GeometryOf(Scene, Object, GeometryBounds)) : Placement);
		Print.Name = HashText(Object.Name);
		const FStageDMXPatch Patch = FilePatch(Object);
		Print.Patch = HashText(Patch.IsPatched() ? FString::Printf(TEXT("%d.%d"), Patch.Universe, Patch.Address) : FString(TEXT("-")));
		Print.Type = HashText(Object.Kind == EStageMvrObjectKind::Fixture ? Object.GdtfSpec.ToLower() + TEXT("|") + Object.GdtfMode.ToLower() : FString(TEXT("truss")));
		// The aim depends on where the fixture hangs and where its focus point is: either moving re-aims it.
		const FStageMvrObject* Focus = Object.Focus.IsValid() ? Scene.FindObject(Object.Focus) : nullptr;
		Print.Aim = HashText(Focus ? Placement + TEXT("|") + MatrixKey(Focus->World) : FString(TEXT("-")));
		return Print;
	}

	FDiff Diff(const FStageMvrScene& Scene, const FInputs& Inputs)
	{
		FDiff Out;
		const FIndex Index(Scene, Inputs);
		for (const FStageMvrObject& Object : Scene.Objects)
		{
			if (!IsPlanned(Object))
			{
				continue;
			}
			const FStageMvrExistingItem* const* Existing = Index.Existing.Find(Object.Id);
			if (!Existing)
			{
				++Out.New;
				continue;
			}
			FTrussPlacement Planned;
			const bool bPlanned = Object.Kind == EStageMvrObjectKind::Truss && PlanTrussForCompare(Scene, Object, Inputs, **Existing, Planned);
			bool bKeptLocked = false;
			const EChange Changes = CompareFields(Object, Fingerprint(Scene, Object, Inputs.GeometryBounds), **Existing, bPlanned ? &Planned : nullptr, bKeptLocked);
			++(Changes == EChange::None ? Out.Unchanged : Out.Changed);
		}
		Out.Missing = Index.GetMissing(Inputs).Num();
		return Out;
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
		const FIndex Index(Scene, Inputs);

		FFixtureIds Ids;
		Ids.Used = Inputs.UsedFixtureIds;

		// Removals first: they go first when the plan is placed, so their fixture IDs are free for the file's new fixtures.
		for (const FStageMvrExistingItem* Missing : Index.GetMissing(Inputs))
		{
			if (Inputs.Missing == EStageMvrMissingItems::Keep)
			{
				++Plan.KeptMissing;
				continue;
			}
			Plan.Removals.Add({ Missing->InstanceId, Missing->Label });
			Notes.Add(FString::Printf(TEXT("%s is no longer in the file; it is removed."), *Missing->Label));
			if (Missing->bFixture)
			{
				Ids.Used.Remove(Missing->FixtureId);
			}
		}
		if (Plan.KeptMissing > 0)
		{
			Notes.Add(FString::Printf(TEXT("%d %s of this plot %s no longer in the file; %s kept."), Plan.KeptMissing, Plan.KeptMissing == 1 ? TEXT("item") : TEXT("items"),
				Plan.KeptMissing == 1 ? TEXT("is") : TEXT("are"), Plan.KeptMissing == 1 ? TEXT("it is") : TEXT("they are")));
		}

		UStageSpanData* TrussItem = Inputs.TrussItem.Get();

		for (const FStageMvrObject& Object : Scene.Objects)
		{
			if (Object.Kind != EStageMvrObjectKind::Fixture && Object.Kind != EStageMvrObjectKind::Truss)
			{
				const bool bPlaceable = Object.Kind == EStageMvrObjectKind::SceneObject || Object.Kind == EStageMvrObjectKind::Support
					|| Object.Kind == EStageMvrObjectKind::VideoScreen || Object.Kind == EStageMvrObjectKind::Projector;
				Plan.NotPlacedObjects += bPlaceable ? 1 : 0;
				continue;
			}
			const FStageMvrExistingItem* const* Found = Index.Existing.Find(Object.Id);
			const FStageMvrExistingItem* Existing = Found ? *Found : nullptr;
			const FFingerprint Print = Fingerprint(Scene, Object, Inputs.GeometryBounds);

			if (Object.Kind == EStageMvrObjectKind::Truss)
			{
				const FString Name = NameOf(Object, TEXT("Truss"));
				// An existing run keeps its own truss type (the Sketch window's may have changed since); anything else that held this
				// UUID (a fixture, a truss piece) is replaced by a new run of the Sketch window's truss.
				UStageSpanData* Own = Existing ? Cast<UStageSpanData>(Existing->Item.Get()) : nullptr;
				const bool bKindChange = Existing && (Existing->bFixture || !Own);
				UStageSpanData* BuildWith = Own && !bKindChange ? Own : TrussItem;
				FTrussPlacement Truss;
				FString Why;
				const bool bPlanned = BuildWith && PlanTruss(Scene, Object, Inputs, StageSpanMath::MakeRules(*BuildWith), Truss, Why);
				if (!BuildWith)
				{
					Why = TEXT("there is no truss in the Library to build it with");
				}
				if (Existing)
				{
					bool bKeptLocked = false;
					EChange Changes = CompareFields(Object, Print, *Existing, bPlanned ? &Truss : nullptr, bKeptLocked);
					if (bKindChange)
					{
						Changes |= EChange::Placement | EChange::Type;
						bKeptLocked = false;
					}
					if (bKeptLocked)
					{
						Notes.Add(FString::Printf(TEXT("Truss \"%s\" is locked; it stays where it is."), *Existing->Label));
					}
					if (Changes == EChange::None)
					{
						++Plan.Unchanged;
						continue;
					}
					if (EnumHasAnyFlags(Changes, EChange::Placement))
					{
						if (!bPlanned)
						{
							// The item stays as it is: better an old run than none.
							++Plan.SkippedTrusses;
							Notes.Add(FString::Printf(TEXT("Truss \"%s\" was not updated: %s."), *Name, *Why));
							continue;
						}
					}
					else
					{
						Truss = FTrussPlacement();
						Truss.MvrId = Object.Id;
						Truss.Transform = Existing->Transform;
						Truss.bSetPath = false;
						Truss.Sections = FMath::Max(1, Existing->Quantity);
						Truss.Layer = Object.Layer;
					}
					Truss.Item = BuildWith;
					Truss.Label = EnumHasAnyFlags(Changes, EChange::Name) ? Object.Name : Existing->Label;
					Truss.Replaces = Existing->InstanceId;
					Truss.Changes = Changes;
					Truss.Origin = UpdatedOrigin(Scene, Object, Print, Inputs.SourceFile, *Existing, bKeptLocked);
					Plan.Trusses.Add(MoveTemp(Truss));
					++Plan.Updated;
					continue;
				}
				if (bPlanned)
				{
					Truss.Item = BuildWith;
					Truss.Origin = MakeOrigin(Scene, Object, Print, Inputs.SourceFile);
					Plan.Trusses.Add(MoveTemp(Truss));
					++Plan.Added;
					continue;
				}
				++Plan.SkippedTrusses;
				Notes.Add(FString::Printf(TEXT("Truss \"%s\" was not placed: %s."), *Name, *Why));
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

			EChange Changes = EChange::None;
			bool bKeptLocked = false;
			if (Existing)
			{
				Changes = CompareFields(Object, Print, *Existing, nullptr, bKeptLocked);
				if (bKeptLocked)
				{
					Notes.Add(FString::Printf(TEXT("%s is locked; it stays where it is."), *Existing->Label));
				}
				if (Changes == EChange::None)
				{
					++Plan.Unchanged;
					continue;
				}
			}
			const bool bRetype = !Existing || EnumHasAnyFlags(Changes, EChange::Type) || !Existing->Item.IsValid();
			if (bRetype && !Target)
			{
				++Plan.SkippedFixtures;
				Notes.Add(Existing
					? FString::Printf(TEXT("%s was not updated: its new fixture type is not available and there is no generic fixture to stand in."), *Name)
					: FString::Printf(TEXT("%s was not placed: its fixture type is not available and there is no generic fixture to stand in."), *Name));
				continue;
			}

			FFixturePlacement& Fixture = Plan.Fixtures.AddDefaulted_GetRef();
			Fixture.MvrId = Object.Id;
			Fixture.Layer = Object.Layer;
			Fixture.Origin = Existing ? UpdatedOrigin(Scene, Object, Print, Inputs.SourceFile, *Existing, bKeptLocked) : MakeOrigin(Scene, Object, Print, Inputs.SourceFile);
			Fixture.Changes = Changes;
			if (bRetype)
			{
				Fixture.Item = Target->Item;
				Fixture.bFallback = bFallback;
				if (bFallback)
				{
					++Plan.FallbackFixtures;
					Notes.Add(Object.GdtfSpec.IsEmpty()
						? FString::Printf(TEXT("%s has no GDTF type; it is placed as %s."), *Name, *Target->Label)
						: FString::Printf(TEXT("%s: %s mode \"%s\" is not available; it is placed as %s."), *Name, *Object.GdtfSpec, *Object.GdtfMode, *Target->Label));
				}
			}
			else
			{
				Fixture.Item = Existing->Item;
			}

			const int32 Wanted = WantedFixtureId(Object);
			if (Existing)
			{
				// What the show knows the fixture by (cues, groups, palettes) is its fixture ID: an update keeps it.
				Fixture.Replaces = Existing->InstanceId;
				Fixture.FixtureId = Existing->FixtureId;
				Fixture.Transform = EnumHasAnyFlags(Changes, EChange::Placement) ? FixtureTransform(Object.World) : Existing->Transform;
				Fixture.Label = EnumHasAnyFlags(Changes, EChange::Name) ? Object.Name : Existing->Label;
				Fixture.Patch = EnumHasAnyFlags(Changes, EChange::Patch) ? FilePatch(Object) : Existing->Patch;
				if (Wanted > 0 && Wanted != Existing->FixtureId)
				{
					Notes.Add(FString::Printf(TEXT("%s keeps fixture ID %d (the file says %d), so its cues and groups still find it."), *Name, Existing->FixtureId, Wanted));
				}
				++Plan.Updated;
			}
			else
			{
				bool bRenumbered = false;
				Fixture.FixtureId = Ids.Take(Wanted, bRenumbered);
				if (bRenumbered)
				{
					++Plan.RenumberedFixtures;
					Notes.Add(FString::Printf(TEXT("%s: fixture ID %d is taken; it is fixture %d."), *Name, Wanted, Fixture.FixtureId));
				}
				Fixture.Transform = FixtureTransform(Object.World);
				Fixture.Label = Object.Name;
				Fixture.Patch = FilePatch(Object);
				++Plan.Added;
			}

			// A new fixture aims at its focus point; an update only when its aim (or its type, and with it its pivots) changed, so a
			// home look the user set stays.
			const bool bAim = !Existing || EnumHasAnyFlags(Changes, EChange::Aim | EChange::Type);
			const FStageMvrModeTarget* AimTarget = Target && Target->Item == Fixture.Item ? Target : nullptr;
			const int32* FocusIndex = Object.Focus.IsValid() ? Index.Objects.Find(Object.Focus) : nullptr;
			if (bAim && FocusIndex && AimTarget && AimTarget->bHasPanTilt)
			{
				const FVector Focus = StageMvrUnits::ToUnreal(Scene.Objects[*FocusIndex].World).GetLocation();
				const FVector Pivot = Fixture.Transform.TransformPosition(AimTarget->PivotOffset);
				const FVector Local = Fixture.Transform.InverseTransformVectorNoScale(Focus - Pivot);
				bool bClamped = false;
				Fixture.PanTilt = SolvePanTilt(Local, AimTarget->PanRange, AimTarget->TiltRange, bClamped);
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
