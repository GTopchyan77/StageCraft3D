// Copyright DamenTools internal. Spline Route Tool plugin.

#include "SplineRouteAuthoringSubsystem.h"
#include "SplineRouteActor.h"
#include "SplineFollowerComponent.h"
#include "SplineFollowerVessel.h"
#include "SplineRouteObstruction.h"
#include "SplineRouteSaveGame.h"
#include "SplineRouteZone.h"
#include "SplineRouteTimeSubsystem.h"
#include "VesselCatalog.h"
#include "SplineRouteTool.h"

#include "CollisionQueryParams.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SplineComponent.h"
#include "Components/Widget.h"
#include "Engine/HitResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "EngineUtils.h"
#include "UObject/UObjectIterator.h"
#include "Materials/MaterialInterface.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	// Logs only — on-screen messages disabled.
	static void ScreenMsg(const FColor& /*Color*/, float /*Duration*/, const FString& Msg)
	{
		UE_LOG(LogSplineRoute, Log, TEXT("%s"), *Msg);
	}

	// New routes previously inherited whatever SegmentMaterial/ArrowMaterial the spawned class's CDO
	// happened to have — fine for BP_SplineRouteActor after its CDO was fixed, but the plain C++
	// ASplineRouteActor fallback (used when no BP-authored route exists yet) still points at the old
	// M_SplineSegment material. Force the current color material on every freshly spawned route so
	// runtime creation can't silently regress to the old look.
	static void ApplySplineColorMaterial(ASplineRouteActor* Route)
	{
		if (!Route)
		{
			return;
		}
		UMaterialInterface* ColorMat = Cast<UMaterialInterface>(StaticLoadObject(
			UMaterialInterface::StaticClass(), nullptr,
			TEXT("/Game/SplineRouteTool/Materials/M_SplineColor.M_SplineColor")));
		if (ColorMat)
		{
			Route->SegmentMaterial = ColorMat;
			Route->ArrowMaterial = ColorMat;
			Route->RebuildGeometry();
		}
	}
}

void USplineRouteAuthoringSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	DraggedPointIndex = INDEX_NONE;
	ActiveRoute = nullptr;
	if (!DefaultRouteClass)
	{
		// Try to load the Blueprint subclass that carries the correct waypoint mesh + materials.
		// Fall back to native only if the BP asset doesn't exist (e.g. in a stripped build).
		UClass* BPClass = LoadObject<UClass>(nullptr,
			TEXT("/Game/SplineRouteTool/Blueprints/BP_SplineRouteActor.BP_SplineRouteActor_C"));
		DefaultRouteClass = BPClass ? BPClass : ASplineRouteActor::StaticClass();
	}
	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] AuthoringSubsystem initialised. PlaneZ=%.1f DefaultRouteClass=%s"),
		PlaneZ, *GetNameSafe(DefaultRouteClass.Get()));
}

void USplineRouteAuthoringSubsystem::Deinitialize()
{
	ActiveRoute = nullptr;
	DraggedPointIndex = INDEX_NONE;
	Super::Deinitialize();
}

void USplineRouteAuthoringSubsystem::SetActiveRoute(ASplineRouteActor* NewRoute)
{
	AssignActiveRoute(NewRoute);
	DraggedPointIndex = INDEX_NONE;
}

ASplineRouteActor* USplineRouteAuthoringSubsystem::EnsureActiveRoute()
{
	return GetOrSpawnActiveRoute();
}

void USplineRouteAuthoringSubsystem::AssignActiveRoute(ASplineRouteActor* NewRoute)
{
	if (ActiveRoute == NewRoute)
	{
		return;
	}
	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] AssignActiveRoute %s -> %s"),
		*GetNameSafe(ActiveRoute), *GetNameSafe(NewRoute));
	ActiveRoute = NewRoute;
	OnActiveRouteChanged.Broadcast(ActiveRoute);
}

UVesselCatalog* USplineRouteAuthoringSubsystem::GetOrLoadCatalog()
{
	if (Catalog)
	{
		return Catalog;
	}
	// Lazy fallback so the demo works without anyone having to wire the field manually.
	Catalog = LoadObject<UVesselCatalog>(nullptr, TEXT("/Game/SplineRouteTool/Data/DA_VesselCatalog.DA_VesselCatalog"));
	if (Catalog)
	{
		UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] Loaded vessel catalog with %d entries."), Catalog->Entries.Num());
	}
	else
	{
		UE_LOG(LogSplineRoute, Warning, TEXT("[SplineRoute] Vessel catalog not set and /Game/SplineRouteTool/Data/DA_VesselCatalog not found."));
	}
	return Catalog;
}

AActor* USplineRouteAuthoringSubsystem::SpawnVesselOnActiveRoute(int32 VesselIndex)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	UVesselCatalog* Cat = GetOrLoadCatalog();
	if (!Cat || !Cat->Entries.IsValidIndex(VesselIndex))
	{
		UE_LOG(LogSplineRoute, Warning, TEXT("[SplineRoute] SpawnVesselOnActiveRoute: invalid index %d (have %d entries)"),
			VesselIndex, Cat ? Cat->Entries.Num() : 0);
		return nullptr;
	}

	ASplineRouteActor* Route = GetOrSpawnActiveRoute();
	if (!Route)
	{
		UE_LOG(LogSplineRoute, Warning, TEXT("[SplineRoute] SpawnVesselOnActiveRoute: no active route."));
		return nullptr;
	}

	// Spawn gate: refuse a new vessel until the last one has cleared MinSpawnGapMeters of spline distance,
	// so back-to-back spawns don't stack vessels on top of each other at the route start. Tracked via the
	// follower component directly (LastSpawnedFollower) so the gate works identically for either vessel kind.
	if (LastSpawnedVessel.IsValid() && LastSpawnedFollower.IsValid())
	{
		if (LastSpawnedFollower->GetDistanceAlongSpline() < MinSpawnGapMeters)
		{
			UE_LOG(LogSplineRoute, Log,
				TEXT("[SplineRoute] SpawnVesselOnActiveRoute: gated — last vessel only %.1f/%.1f units along spline."),
				LastSpawnedFollower->GetDistanceAlongSpline(), MinSpawnGapMeters);
			UE_LOG(LogSplineRoute, Warning,
				TEXT("[SplineRoute] SPAWN BLOCKED - TotalDistanceTraveled=%.1f MinSpawnGap=%.1f"),
				LastSpawnedFollower->GetDistanceAlongSpline(), MinSpawnGapMeters);
			return nullptr;
		}
	}

	const FVesselCatalogEntry& Entry = Cat->Entries[VesselIndex];
	// Classify the route by the spawned entry's medium so its handles tint correctly and placement
	// constraints (water vs land) apply consistently for any further authoring.
	Route->SetVehicleType(Entry.VehicleType);

	// Catalog entries point at EITHER a Blueprint child of ASplineFollowerVessel (mesh baked into
	// MeshComponent's default) OR any other AActor Blueprint that already carries its own
	// USplineFollowerComponent (e.g. BP_Ship_Cargo_2/3/4/6) — see VesselCatalog.h.
	TSubclassOf<AActor> VesselClass = Entry.VesselClass.LoadSynchronous();
	if (!VesselClass)
	{
		UE_LOG(LogSplineRoute, Warning,
			TEXT("[SplineRoute] SpawnVesselOnActiveRoute: catalog entry %d ('%s') has no VesselClass set."),
			VesselIndex, *Entry.DisplayName.ToString());
		return nullptr;
	}

	// Spawn at the first spline point. The follower's BeginPlay snap-to-spline (clamped against [0, length]) does the real placement —
	// the world spawn location is just somewhere safe near the route.
	const FVector SpawnLoc = Route->GetActorLocation() + FVector(0, 0, Entry.HeightOffset);

	// Deferred spawn so the ASplineFollowerVessel kind can call Configure() to set Route/Speed/HeightOffset
	// BEFORE its follower's BeginPlay runs (otherwise BeginPlay logs a "no Route assigned" warning and the
	// initial anchor uses HeightOffset=0). The generic kind's USplineFollowerComponent is Blueprint-added
	// (SCS), so it doesn't exist until FinishSpawning() runs the construction script — see the branch below.
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Vessel = World->SpawnActorDeferred<AActor>(
		VesselClass,
		FTransform(FRotator::ZeroRotator, SpawnLoc),
		nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Vessel)
	{
		USplineFollowerComponent* Follower = nullptr;
		if (ASplineFollowerVessel* SFV = Cast<ASplineFollowerVessel>(Vessel))
		{
			// InMesh=nullptr — the mesh comes from the spawned Blueprint's own MeshComponent default now.
			SFV->Configure(nullptr, Entry.RelativeScale, Route, Entry.DefaultSpeed, Entry.HeightOffset, VesselTurnRate, Entry.RotationOffset, Entry.ReverseApproachSeconds, Entry.ReverseMinSpeedFactor);
			// Must land before FinishSpawning — the vessel's BeginPlay is what spawns the companion.
			SFV->SetDolphinCompanionSettings(Entry.DolphinCompanion);
			// Apply the current size multiplier before measuring GetMeshLength() below, so the queue gap
			// reflects this vessel's actual runtime size instead of its unscaled catalog BaseScale.
			SFV->SetSizeMultiplier(VesselSizeMultiplier);

			// LeaderFollower spacing (set up below) handles convoy gaps at runtime, so every vessel
			// starts at the route's first point.
			if (SFV->Follower)
			{
				SFV->Follower->InitialDistance = 0.0f;
			}
			Vessel->FinishSpawning(FTransform(FRotator::ZeroRotator, SpawnLoc));
			SFV->CatalogIndex = VesselIndex; // recorded for scenario save/load
			Follower = SFV->Follower;
		}
		else
		{
			// Generic kind (e.g. BP_Ship_Cargo_2/3/4/6): mesh/scale/rotation/dolphin-companion/size-
			// multiplier are ASplineFollowerVessel-specific spawn-time concerns that don't apply here —
			// the Blueprint already bakes its own mesh, camera rig, and (per-instance) HeightOffset on
			// its own USplineFollowerComponent. FinishSpawning() must run first so that Blueprint-added
			// component actually exists to find.
			Vessel->FinishSpawning(FTransform(FRotator::ZeroRotator, SpawnLoc));
			Follower = Vessel->FindComponentByClass<USplineFollowerComponent>();
			if (Follower)
			{
				Follower->InitialDistance = 0.0f;
				Follower->ConfigureFollower(Route, Entry.DefaultSpeed, VesselTurnRate, Entry.ReverseApproachSeconds, Entry.ReverseMinSpeedFactor);
				UE_LOG(LogSplineRoute, Log,
					TEXT("[SplineRoute] Vessel %s (generic AActor kind) configured: route=%s speed=%.1f. Mesh/scale/rotation/dolphin-companion/size-multiplier catalog fields are not supported on this vessel kind and were skipped — those stay baked into the Blueprint itself."),
					*Vessel->GetName(), *GetNameSafe(Route), Entry.DefaultSpeed);
			}
			else
			{
				UE_LOG(LogSplineRoute, Warning,
					TEXT("[SplineRoute] SpawnVesselOnActiveRoute: '%s' has no USplineFollowerComponent — it was spawned but will not move. Add one via the Blueprint's Add Component button."),
					*Vessel->GetName());
			}
		}
		SelectedVesselIndex = VesselIndex;

		// Count vessels already on this route (excluding the one just spawned above) to determine queue
		// position. Generic across both vessel kinds — iterates every live USplineFollowerComponent in the
		// world rather than TActorIterator<ASplineFollowerVessel>, which would silently miss the new kind.
		int32 VesselCount = 0;
		for (TObjectIterator<USplineFollowerComponent> It; It; ++It)
		{
			USplineFollowerComponent* Other = *It;
			if (Other && Other != Follower && !Other->HasAnyFlags(RF_ClassDefaultObject) && Other->GetWorld() == World && Other->Route == Route)
				VesselCount++;
		}
		if (Follower) Follower->SpawnOrder = VesselCount; // 0 for first, 1 for second, etc.
		if (ASplineFollowerVessel* SFV = Cast<ASplineFollowerVessel>(Vessel))
		{
			SFV->SpawnOrder = VesselCount; // actor-level mirror, kept for this kind only (existing behavior)
		}

		// Chain this vessel to follow the one spawned just before it, so it slows down rather than
		// closing the gap below MinSpawnGapMeters. Must read LastSpawnedFollower BEFORE overwriting it below.
		if (Follower)
		{
			if (LastSpawnedFollower.IsValid())
			{
				Follower->LeaderFollower = LastSpawnedFollower;
			}
			Follower->MinFollowDistanceMeters = MinSpawnGapMeters;
		}
		LastSpawnedVessel = Vessel;
		LastSpawnedFollower = Follower;

		// Update gap based on newly spawned vessel's size. ASplineFollowerVessel measures its own mesh
		// bounds precisely (GetMeshLength()); the generic kind has no equivalent API, so fall back to the
		// actor's overall world bounding box — a looser but always-available proxy for "how big is this".
		float MeshLen = 1000.0f;
		if (ASplineFollowerVessel* SFV = Cast<ASplineFollowerVessel>(Vessel))
		{
			MeshLen = SFV->GetMeshLength();
		}
		else
		{
			FVector Origin, BoxExtent;
			Vessel->GetActorBounds(false, Origin, BoxExtent);
			MeshLen = BoxExtent.GetMax() * 2.0f;
		}
		MinFollowDistanceMeters = MeshLen * 1.5f;
		MinSpawnGapMeters = MeshLen * 1.5f;
		if (Follower)
			Follower->MinFollowDistanceMeters = MinFollowDistanceMeters;

		UE_LOG(LogSplineRoute, Log,
			TEXT("[SplineRoute] Vessel spawned at first point (InitialDistance=0, sizeMult=%.2f)"),
			VesselSizeMultiplier);
	}
	return Vessel;
}

void USplineRouteAuthoringSubsystem::SetVesselSizeMultiplier(float NewMultiplier)
{
	const float Clamped = FMath::Max(0.05f, NewMultiplier);
	VesselSizeMultiplier = Clamped;

	// Base the follow distance / spawn gap on the largest vessel actually in the world (measured after
	// rescaling it) rather than a fixed constant, so bigger ships get proportionally more room.
	TArray<ASplineFollowerVessel*> Vessels;
	float LargestMeshLength = 1000.0f;
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ASplineFollowerVessel> It(World); It; ++It)
		{
			if (ASplineFollowerVessel* Vessel = *It)
			{
				Vessel->SetSizeMultiplier(Clamped);
				LargestMeshLength = FMath::Max(LargestMeshLength, Vessel->GetMeshLength());
				Vessels.Add(Vessel);
			}
		}
	}

	MinFollowDistanceMeters = LargestMeshLength * 1.5f;
	MinSpawnGapMeters = LargestMeshLength * 1.5f;

	for (ASplineFollowerVessel* Vessel : Vessels)
	{
		if (Vessel->Follower)
		{
			Vessel->Follower->MinFollowDistanceMeters = MinFollowDistanceMeters;
		}
	}

	// A size increase can put a follower's own leader-relative gap into overlap immediately — resolve
	// that now instead of waiting for TickComponent's gradual push-apart to catch up over several frames.
	for (ASplineFollowerVessel* Vessel : Vessels)
	{
		if (!Vessel->Follower || !Vessel->Follower->LeaderFollower.IsValid())
		{
			continue;
		}
		const float LeaderDist = Vessel->Follower->LeaderFollower->GetDistanceAlongSpline();
		const float MyDist = Vessel->Follower->GetDistanceAlongSpline();
		const float Gap = LeaderDist - MyDist;
		const float RequiredGap = Vessel->Follower->MinFollowDistanceMeters;

		if (Gap < RequiredGap)
		{
			// Push follower back so gap equals RequiredGap
			float Correction = Gap - RequiredGap; // negative = push back
			const float NewDist = MyDist + Correction;

			// Don't push before spline start - clamp to 0
			if (NewDist < 0.0f)
			{
				Correction = -MyDist; // only push back to start, not beyond
			}

			if (Correction < 0.0f) // only nudge if actually pushing back
			{
				Vessel->Follower->NudgeDistanceAlongSpline(Correction);
			}
		}
	}

	UE_LOG(LogSplineRoute, Log,
		TEXT("[SplineRoute] SetVesselSizeMultiplier=%.3f, largestMeshLength=%.1f, followDist/spawnGap=%.1f, rescaled %d live vessel(s)"),
		Clamped, LargestMeshLength, MinFollowDistanceMeters, Vessels.Num());
}

void USplineRouteAuthoringSubsystem::SetTurnRateForAllVessels(float NewTurnRate)
{
	VesselTurnRate = FMath::Max(0.0f, NewTurnRate);

	int32 Updated = 0;
	if (UWorld* World = GetWorld())
	{
		// Update EVERY follower in this world — spawned vessels AND level-placed demo followers (the cubes),
		// so the slider visibly affects whatever is currently moving, not just picker-spawned vessels.
		for (TObjectIterator<USplineFollowerComponent> It; It; ++It)
		{
			USplineFollowerComponent* Follower = *It;
			if (Follower && !Follower->HasAnyFlags(RF_ClassDefaultObject) && Follower->GetWorld() == World)
			{
				Follower->TurnRateDegPerSec = VesselTurnRate;
				++Updated;
			}
		}
	}
	UE_LOG(LogSplineRoute, Log,
		TEXT("[SplineRoute] SetTurnRateForAllVessels=%.1f deg/s, updated %d follower(s)"),
		VesselTurnRate, Updated);
}

void USplineRouteAuthoringSubsystem::SetSpeedForSelectedSegment(float NewSpeed)
{
	if (!ActiveRoute)
	{
		return;
	}
	const int32 Selected = ActiveRoute->GetSelectedControlPoint();
	if (Selected == INDEX_NONE)
	{
		return;
	}
	// Speeds are now per control point (interpolated between points), so every point is settable.
	ActiveRoute->SetPointSpeedOverride(Selected, NewSpeed);
}

float USplineRouteAuthoringSubsystem::GetSpeedForSelectedSegment() const
{
	if (!ActiveRoute)
	{
		return -1.0f;
	}
	const int32 Selected = ActiveRoute->GetSelectedControlPoint();
	if (Selected == INDEX_NONE)
	{
		return -1.0f;
	}
	return ActiveRoute->GetPointSpeedOverride(Selected);
}

void USplineRouteAuthoringSubsystem::SetReverseForSelectedPoint(bool bReverse)
{
	if (!ActiveRoute)
	{
		return;
	}
	const int32 Selected = ActiveRoute->GetSelectedControlPoint();
	if (Selected == INDEX_NONE)
	{
		return;
	}
	ActiveRoute->SetPointReverse(Selected, bReverse);
}

void USplineRouteAuthoringSubsystem::ToggleReverseForSelectedPoint()
{
	if (!ActiveRoute)
	{
		return;
	}
	const int32 Selected = ActiveRoute->GetSelectedControlPoint();
	if (Selected == INDEX_NONE)
	{
		return;
	}
	ActiveRoute->SetPointReverse(Selected, !ActiveRoute->GetPointReverse(Selected));
}

bool USplineRouteAuthoringSubsystem::GetReverseForSelectedPoint() const
{
	if (!ActiveRoute)
	{
		return false;
	}
	const int32 Selected = ActiveRoute->GetSelectedControlPoint();
	if (Selected == INDEX_NONE)
	{
		return false;
	}
	return ActiveRoute->GetPointReverse(Selected);
}

namespace
{
	USplineRouteTimeSubsystem* GetTimeSubsystem(const UWorld* World)
	{
		const UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
		return GI ? GI->GetSubsystem<USplineRouteTimeSubsystem>() : nullptr;
	}
}

void USplineRouteAuthoringSubsystem::SetActiveRouteVehicleType(ERouteVehicleType NewType)
{
	if (ASplineRouteActor* Route = EnsureActiveRoute())
	{
		Route->SetVehicleType(NewType);
	}
}

void USplineRouteAuthoringSubsystem::ToggleActiveRouteVehicleType()
{
	const ERouteVehicleType Next = (GetActiveRouteVehicleType() == ERouteVehicleType::Vessel)
		? ERouteVehicleType::Ground
		: ERouteVehicleType::Vessel;
	SetActiveRouteVehicleType(Next);
}

ERouteVehicleType USplineRouteAuthoringSubsystem::GetActiveRouteVehicleType() const
{
	return ActiveRoute ? ActiveRoute->VehicleType : ERouteVehicleType::Vessel;
}

void USplineRouteAuthoringSubsystem::SetObstructionPlaceMode(bool bEnabled)
{
	bObstructionPlaceMode = bEnabled;
	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] Obstruction place mode = %s"), bEnabled ? TEXT("ON") : TEXT("OFF"));
}

void USplineRouteAuthoringSubsystem::SetSelectedObstructionIndex(int32 Index)
{
	SelectedObstructionIndex = FMath::Max(0, Index);
}

int32 USplineRouteAuthoringSubsystem::GetNumObstructionTypes() const
{
	UVesselCatalog* Cat = const_cast<USplineRouteAuthoringSubsystem*>(this)->GetOrLoadCatalog();
	const int32 N = Cat ? Cat->Obstructions.Num() : 0;
	return FMath::Max(1, N); // always offer at least the default Container
}

FText USplineRouteAuthoringSubsystem::GetObstructionLabel(int32 Index) const
{
	UVesselCatalog* Cat = const_cast<USplineRouteAuthoringSubsystem*>(this)->GetOrLoadCatalog();
	if (Cat && Cat->Obstructions.IsValidIndex(Index) && !Cat->Obstructions[Index].DisplayName.IsEmpty())
	{
		return Cat->Obstructions[Index].DisplayName;
	}
	return FText::FromString(TEXT("Container"));
}

ASplineRouteObstruction* USplineRouteAuthoringSubsystem::SpawnObstructionAtCursor(APlayerController* PC)
{
	UWorld* World = GetWorld();
	if (!World || !PC)
	{
		return nullptr;
	}

	FVector PlanePoint;
	if (!ProjectMouseToPlane(PC, PlanePoint))
	{
		return nullptr;
	}

	UVesselCatalog* Cat = GetOrLoadCatalog();
	UStaticMesh* Mesh = nullptr;
	FVector Scale(6.0f, 2.4f, 2.6f);
	FName TypeId = TEXT("Container");
	int32 Index = SelectedObstructionIndex;
	if (Cat && Cat->Obstructions.IsValidIndex(Index))
	{
		const FObstructionCatalogEntry& E = Cat->Obstructions[Index];
		Mesh = E.Mesh.LoadSynchronous();
		Scale = E.Scale;
		TypeId = E.DisplayName.IsEmpty()
			? FName(*FString::Printf(TEXT("Obstruction%d"), Index))
			: FName(*E.DisplayName.ToString());
	}
	else
	{
		Index = INDEX_NONE; // using the default cube fallback
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ASplineRouteObstruction* Obstruction = World->SpawnActor<ASplineRouteObstruction>(
		ASplineRouteObstruction::StaticClass(), FTransform(FRotator::ZeroRotator, PlanePoint), Params);
	if (Obstruction)
	{
		Obstruction->Configure(Mesh, Scale, TypeId, Index);
	}
	return Obstruction;
}

void USplineRouteAuthoringSubsystem::PlayAll()
{
	if (USplineRouteTimeSubsystem* TimeSys = GetTimeSubsystem(GetWorld()))
	{
		const float Rate = (LastPlayRate > 0.0f) ? LastPlayRate : 1.0f;
		TimeSys->SetTimeScale(Rate);
		UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] PlayAll -> timeScale=%.2f"), Rate);
	}
}

void USplineRouteAuthoringSubsystem::PauseAll()
{
	if (USplineRouteTimeSubsystem* TimeSys = GetTimeSubsystem(GetWorld()))
	{
		const float Cur = TimeSys->GetTimeScale();
		if (Cur > 0.0f)
		{
			LastPlayRate = Cur;
		}
		TimeSys->SetTimeScale(0.0f);
		UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] PauseAll (remembered rate=%.2f)"), LastPlayRate);
	}
}

void USplineRouteAuthoringSubsystem::StopAll()
{
	PauseAll();

	int32 Reset = 0;
	if (UWorld* World = GetWorld())
	{
		for (TObjectIterator<USplineFollowerComponent> It; It; ++It)
		{
			USplineFollowerComponent* Follower = *It;
			if (Follower && !Follower->HasAnyFlags(RF_ClassDefaultObject) && Follower->GetWorld() == World)
			{
				Follower->ResetToStart();
				++Reset;
			}
		}
	}
	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] StopAll -> reset %d follower(s)"), Reset);
}

bool USplineRouteAuthoringSubsystem::SaveScenario(const FString& SlotName)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	USplineRouteSaveGame* Save = Cast<USplineRouteSaveGame>(
		UGameplayStatics::CreateSaveGameObject(USplineRouteSaveGame::StaticClass()));
	if (!Save)
	{
		return false;
	}

	// Stable route ordering so vehicles can reference routes by index.
	TArray<ASplineRouteActor*> RouteActors;
	for (TActorIterator<ASplineRouteActor> It(World); It; ++It)
	{
		if (*It) { RouteActors.Add(*It); }
	}

	for (ASplineRouteActor* Route : RouteActors)
	{
		FSavedRoute SR;
		SR.VehicleType = Route->VehicleType;
		USplineComponent* Sp = Route->GetSpline();
		const int32 N = Route->GetNumControlPoints();
		for (int32 i = 0; i < N; ++i)
		{
			FSavedRoutePoint P;
			P.Location = Sp ? Sp->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::World) : FVector::ZeroVector;
			P.SpeedOverride = Route->GetPointSpeedOverride(i);
			P.bReverse = Route->GetPointReverse(i);
			SR.Points.Add(P);
		}
		Save->Routes.Add(SR);
	}

	for (TActorIterator<ASplineFollowerVessel> It(World); It; ++It)
	{
		ASplineFollowerVessel* Vessel = *It;
		if (!Vessel) { continue; }
		FSavedVehicle V;
		V.CatalogIndex = Vessel->CatalogIndex;
		V.SizeMultiplier = Vessel->CurrentSizeMultiplier;
		V.DistanceAlongSpline = Vessel->Follower ? Vessel->Follower->GetDistanceAlongSpline() : 0.0f;
		V.RouteIndex = (Vessel->Follower && Vessel->Follower->Route)
			? RouteActors.IndexOfByKey(Vessel->Follower->Route.Get())
			: INDEX_NONE;
		Save->Vehicles.Add(V);
	}

	// Scenario save/load is not supported yet for the generic vessel kind (e.g. BP_Ship_Cargo_2/3/4/6) —
	// FSavedVehicle is keyed on ASplineFollowerVessel-specific fields (CatalogIndex, CurrentSizeMultiplier)
	// that don't exist on a plain AActor. Rather than silently dropping them, count and log a clear,
	// non-crashing note so Gevor knows why a generic vessel didn't come back after a Load. Extending the
	// save format to cover both kinds is a separate, larger change than this task's scope.
	{
		int32 UnsavedGenericVessels = 0;
		for (TObjectIterator<USplineFollowerComponent> It; It; ++It)
		{
			USplineFollowerComponent* Follower = *It;
			if (Follower && !Follower->HasAnyFlags(RF_ClassDefaultObject) && Follower->GetWorld() == World
				&& !Cast<ASplineFollowerVessel>(Follower->GetOwner()))
			{
				++UnsavedGenericVessels;
			}
		}
		if (UnsavedGenericVessels > 0)
		{
			UE_LOG(LogSplineRoute, Warning,
				TEXT("[SplineRoute] SaveScenario: %d generic-kind vessel(s) (e.g. BP_Ship_Cargo_2/3/4/6) are NOT supported by scenario save/load yet and were skipped — they will not be restored by LoadScenario."),
				UnsavedGenericVessels);
		}
	}

	for (TActorIterator<ASplineRouteObstruction> It(World); It; ++It)
	{
		ASplineRouteObstruction* Obs = *It;
		if (!Obs) { continue; }
		FSavedObstruction O;
		O.CatalogIndex = Obs->CatalogIndex;
		O.TypeId = Obs->ObstructionTypeId;
		O.Transform = Obs->GetActorTransform();
		Save->Obstructions.Add(O);
	}

	Save->VesselSizeMultiplier = VesselSizeMultiplier;
	Save->VesselTurnRate = VesselTurnRate;
	// The subsystem members are canonical for convoy spacing — each vessel's follower is only ever a
	// mirror of MinFollowDistanceMeters (see SpawnVesselOnActiveRoute / SetVesselSizeMultiplier).
	Save->MinFollowDistanceMeters = MinFollowDistanceMeters;
	Save->MinSpawnGapMeters = MinSpawnGapMeters;
	if (USplineRouteTimeSubsystem* TimeSys = GetTimeSubsystem(World))
	{
		Save->TimeScale = TimeSys->GetTimeScale();
	}

	const bool bOk = UGameplayStatics::SaveGameToSlot(Save, SlotName, 0);
	UE_LOG(LogSplineRoute, Log,
		TEXT("[SplineRoute] SaveScenario '%s' -> %s (routes=%d vehicles=%d obstructions=%d)"),
		*SlotName, bOk ? TEXT("OK") : TEXT("FAILED"),
		Save->Routes.Num(), Save->Vehicles.Num(), Save->Obstructions.Num());
	return bOk;
}

void USplineRouteAuthoringSubsystem::ClearScenario()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	AssignActiveRoute(nullptr);
	DraggedPointIndex = INDEX_NONE;

	ClearAllVessels();

	TArray<AActor*> Doomed;
	for (TActorIterator<ASplineRouteObstruction> It(World); It; ++It) { if (*It) { Doomed.Add(*It); } }
	for (TActorIterator<ASplineRouteActor> It(World); It; ++It)       { if (*It) { Doomed.Add(*It); } }
	for (AActor* A : Doomed)
	{
		if (A) { A->Destroy(); }
	}
	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] ClearScenario destroyed %d route/obstruction actor(s)"), Doomed.Num());
}

void USplineRouteAuthoringSubsystem::ClearAllVessels()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Collect every follower's owner generically (rather than TActorIterator<ASplineFollowerVessel>,
	// which would silently leave any generic-kind vessel — e.g. BP_Ship_Cargo_2/3/4/6 — behind after
	// Clear) so both vessel kinds get torn down. A TSet dedupes in case an owner somehow carries more
	// than one follower.
	TSet<AActor*> FollowerOwners;
	for (TObjectIterator<USplineFollowerComponent> It; It; ++It)
	{
		USplineFollowerComponent* Follower = *It;
		if (Follower && !Follower->HasAnyFlags(RF_ClassDefaultObject) && Follower->GetWorld() == World)
		{
			if (AActor* Owner = Follower->GetOwner())
			{
				FollowerOwners.Add(Owner);
			}
		}
	}

	SetSelectedVessel(nullptr);
	LastSpawnedVessel.Reset();
	LastSpawnedFollower.Reset();

	for (AActor* A : FollowerOwners)
	{
		if (A) { A->Destroy(); }
	}
	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] ClearAllVessels destroyed %d actor(s)"), FollowerOwners.Num());
}

void USplineRouteAuthoringSubsystem::ClearAllWaypoints()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Clear control points per-route (rather than destroying the ASplineRouteActor, which is what
	// ClearScenario does) so vessels already following a route are left in place — only their path
	// disappears, matching the "waypoints button doesn't touch vessels" requirement.
	int32 NumRoutes = 0;
	for (TActorIterator<ASplineRouteActor> It(World); It; ++It)
	{
		if (ASplineRouteActor* Route = *It)
		{
			Route->ClearControlPoints();
			++NumRoutes;
		}
	}
	DraggedPointIndex = INDEX_NONE;
	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] ClearAllWaypoints cleared control points on %d route(s)"), NumRoutes);
}

bool USplineRouteAuthoringSubsystem::LoadScenario(const FString& SlotName)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	if (!UGameplayStatics::DoesSaveGameExist(SlotName, 0))
	{
		UE_LOG(LogSplineRoute, Warning, TEXT("[SplineRoute] LoadScenario: slot '%s' not found."), *SlotName);
		return false;
	}
	USplineRouteSaveGame* Save = Cast<USplineRouteSaveGame>(UGameplayStatics::LoadGameFromSlot(SlotName, 0));
	if (!Save)
	{
		return false;
	}

	// Decide which class to rebuild routes with BEFORE clearing. Prefer the class of an existing authored
	// route (the level's BP_SplineRouteActor, which carries the proper waypoint mesh + materials) so loaded
	// routes look identical to hand-authored ones instead of plain C++ engine-cube handles.
	TSubclassOf<ASplineRouteActor> Cls = DefaultRouteClass;
	for (TActorIterator<ASplineRouteActor> It(World); It; ++It)
	{
		if (*It)
		{
			Cls = (*It)->GetClass();
			break;
		}
	}
	if (!Cls)
	{
		Cls = ASplineRouteActor::StaticClass();
	}
	// Remember it so any further auto-spawned routes also use the nice appearance.
	DefaultRouteClass = Cls;

	ClearScenario();

	// Rebuild routes (keep a parallel array so vehicles can resolve their route by index).
	TArray<ASplineRouteActor*> NewRoutes;
	for (const FSavedRoute& SR : Save->Routes)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ASplineRouteActor* Route = World->SpawnActor<ASplineRouteActor>(Cls, FVector::ZeroVector, FRotator::ZeroRotator, Params);
		if (!Route)
		{
			NewRoutes.Add(nullptr);
			continue;
		}
		ApplySplineColorMaterial(Route);
		// A freshly-spawned BP route may carry its own baked-in spline points — wipe them so the
		// rebuilt route contains ONLY the saved waypoints (otherwise the vessel follows the BP's
		// default "first spline" instead of the loaded one).
		Route->ClearControlPoints();
		Route->SetVehicleType(SR.VehicleType);
		for (const FSavedRoutePoint& P : SR.Points)
		{
			const int32 Idx = Route->AddControlPoint(P.Location);
			Route->SetPointSpeedOverride(Idx, P.SpeedOverride);
			Route->SetPointReverse(Idx, P.bReverse);
		}
		NewRoutes.Add(Route);
	}

	VesselSizeMultiplier = Save->VesselSizeMultiplier;
	VesselTurnRate = Save->VesselTurnRate;

	// Rebuild vehicles onto their saved route.
	TArray<TPair<ASplineFollowerVessel*, const FSavedVehicle*>> SpawnedVehicles;
	for (const FSavedVehicle& V : Save->Vehicles)
	{
		ASplineRouteActor* Route = NewRoutes.IsValidIndex(V.RouteIndex) ? NewRoutes[V.RouteIndex] : nullptr;
		if (!Route)
		{
			// Fall back to the first valid rebuilt route so the vessel still attaches to loaded geometry,
			// never to a leftover/stale active route.
			for (ASplineRouteActor* R : NewRoutes)
			{
				if (R) { Route = R; break; }
			}
		}
		if (!Route)
		{
			continue;
		}
		SetActiveRoute(Route);
		// Loaded vessels are spawned back-to-back with no elapsed tick time between them, so the spawn
		// gate (meant to stop back-to-back MANUAL spawns from stacking) would otherwise block every
		// vessel after the first — it'd see the previous one still sitting at DistanceAlongSpline == 0.
		// Loading a saved scenario should always succeed regardless of gap spacing, so bypass it here.
		LastSpawnedVessel.Reset();
		LastSpawnedFollower.Reset();
		// Scenario save/load only ever captures the ASplineFollowerVessel kind (see SaveScenario's
		// generic-vessel note), so this Cast is expected to always succeed for a real saved entry.
		if (ASplineFollowerVessel* Vessel = Cast<ASplineFollowerVessel>(SpawnVesselOnActiveRoute(V.CatalogIndex)))
		{
			SpawnedVehicles.Add(TPair<ASplineFollowerVessel*, const FSavedVehicle*>(Vessel, &V));
		}
	}
	SetVesselSizeMultiplier(VesselSizeMultiplier);
	SetTurnRateForAllVessels(VesselTurnRate);

	// SetVesselSizeMultiplier() above just rescaled every vessel to the global slider value, so per-vessel
	// size/position have to be restored AFTER it, or they'd be immediately overwritten.
	for (const TPair<ASplineFollowerVessel*, const FSavedVehicle*>& Pair : SpawnedVehicles)
	{
		ASplineFollowerVessel* Vessel = Pair.Key;
		const FSavedVehicle* SavedData = Pair.Value;
		if (!Vessel || !SavedData)
		{
			continue;
		}
		Vessel->SetSizeMultiplier(SavedData->SizeMultiplier);
		if (Vessel->Follower)
		{
			// Snap to the saved resume position immediately rather than waiting for TickComponent to travel there.
			const float Delta = SavedData->DistanceAlongSpline - Vessel->Follower->GetDistanceAlongSpline();
			Vessel->Follower->NudgeDistanceAlongSpline(Delta);
			Vessel->Follower->bHasReachedStart = true;
		}
	}

	// Rebuild the convoy chain. SpawnVesselOnActiveRoute() derives LeaderFollower from LastSpawnedVessel,
	// which the spawn loop above deliberately resets on EVERY iteration to bypass the spawn gate — so every
	// loaded vessel came back with a null leader. A null leader silently disables convoy spacing altogether
	// (the overlap correction in SetVesselSizeMultiplier skips any follower without a valid one), which is
	// why loaded vessels used to overlap the moment the size slider grew them. Chain per route in order of
	// distance travelled: the vessel furthest along the spline leads.
	TMap<ASplineRouteActor*, TArray<ASplineFollowerVessel*>> VesselsByRoute;
	for (const TPair<ASplineFollowerVessel*, const FSavedVehicle*>& Pair : SpawnedVehicles)
	{
		ASplineFollowerVessel* Vessel = Pair.Key;
		if (Vessel && Vessel->Follower && Vessel->Follower->Route)
		{
			VesselsByRoute.FindOrAdd(Vessel->Follower->Route.Get()).Add(Vessel);
		}
	}
	for (TPair<ASplineRouteActor*, TArray<ASplineFollowerVessel*>>& RoutePair : VesselsByRoute)
	{
		TArray<ASplineFollowerVessel*>& Convoy = RoutePair.Value;
		Convoy.Sort([](const ASplineFollowerVessel& A, const ASplineFollowerVessel& B)
		{
			return A.Follower->GetDistanceAlongSpline() > B.Follower->GetDistanceAlongSpline();
		});
		for (int32 i = 0; i < Convoy.Num(); ++i)
		{
			Convoy[i]->SpawnOrder = i;
			Convoy[i]->Follower->SpawnOrder = i;
			Convoy[i]->Follower->LeaderFollower = (i > 0) ? Convoy[i - 1]->Follower : nullptr;
		}
		UE_LOG(LogSplineRoute, Log,
			TEXT("[SplineRoute] LoadScenario: rebuilt convoy chain on route %s -> %d vessel(s)"),
			*GetNameSafe(RoutePair.Key), Convoy.Num());
	}

	// Point the spawn gate at the rear of the active route's convoy, so the next MANUAL spawn chains behind
	// it instead of starting a second unchained head (which would reintroduce the same overlap for itself).
	if (NewRoutes.Num() > 0 && NewRoutes[0])
	{
		if (const TArray<ASplineFollowerVessel*>* ActiveConvoy = VesselsByRoute.Find(NewRoutes[0]))
		{
			if (ActiveConvoy->Num() > 0)
			{
				LastSpawnedVessel = ActiveConvoy->Last();
				LastSpawnedFollower = ActiveConvoy->Last()->Follower;
			}
		}
	}

	// Restore convoy spacing LAST of the vessel steps: both SpawnVesselOnActiveRoute() and
	// SetVesselSizeMultiplier() above unconditionally recompute these two from the largest live mesh,
	// so restoring any earlier would just be overwritten by the rebuild itself. Note this only seeds the
	// value — it does NOT gate the recompute in SetVesselSizeMultiplier, which is unconditional, so the
	// size slider always wins once the operator touches it.
	if (Save->MinSpawnGapMeters >= 0.0f)
	{
		MinSpawnGapMeters = Save->MinSpawnGapMeters;
	}
	if (Save->MinFollowDistanceMeters >= 0.0f)
	{
		MinFollowDistanceMeters = Save->MinFollowDistanceMeters;

		// Push it out to every live follower — spawned vessels AND level-placed demo followers — so the
		// convoy honours the saved gap immediately rather than the mesh-derived value it spawned with.
		for (TObjectIterator<USplineFollowerComponent> It; It; ++It)
		{
			USplineFollowerComponent* Follower = *It;
			if (Follower && !Follower->HasAnyFlags(RF_ClassDefaultObject) && Follower->GetWorld() == World)
			{
				Follower->MinFollowDistanceMeters = MinFollowDistanceMeters;
			}
		}
	}

	// Rebuild obstructions.
	UVesselCatalog* Cat = GetOrLoadCatalog();
	for (const FSavedObstruction& O : Save->Obstructions)
	{
		UStaticMesh* Mesh = (Cat && Cat->Obstructions.IsValidIndex(O.CatalogIndex))
			? Cat->Obstructions[O.CatalogIndex].Mesh.LoadSynchronous()
			: nullptr;
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ASplineRouteObstruction* Obs = World->SpawnActor<ASplineRouteObstruction>(
			ASplineRouteObstruction::StaticClass(), O.Transform, Params);
		if (Obs)
		{
			Obs->Configure(Mesh, O.Transform.GetScale3D(), O.TypeId, O.CatalogIndex);
		}
	}

	// Re-point any ORPHANED level-placed followers (e.g. the demo FollowerCubes) onto a loaded route.
	// Their original route was just destroyed by ClearScenario, so without this they'd keep following a
	// dead "first spline" instead of the freshly-loaded waypoints. Freshly-spawned vessels already point
	// at a NewRoutes entry, so they're left alone.
	if (NewRoutes.Num() > 0 && NewRoutes[0])
	{
		for (TObjectIterator<USplineFollowerComponent> It; It; ++It)
		{
			USplineFollowerComponent* Follower = *It;
			if (!Follower || Follower->HasAnyFlags(RF_ClassDefaultObject) || Follower->GetWorld() != World)
			{
				continue;
			}
			if (Follower->Route == nullptr || !NewRoutes.Contains(Follower->Route.Get()))
			{
				Follower->SetRoute(NewRoutes[0]);
			}
		}
	}

	// Restore the saved playback rate immediately instead of pausing on load.
	LastPlayRate = (Save->TimeScale > 0.0f) ? Save->TimeScale : 1.0f;
	if (USplineRouteTimeSubsystem* TimeSys = GetTimeSubsystem(World))
	{
		TimeSys->SetTimeScale(Save->TimeScale);
	}

	if (NewRoutes.Num() > 0 && NewRoutes[0])
	{
		SetActiveRoute(NewRoutes[0]);
	}

	UE_LOG(LogSplineRoute, Log,
		TEXT("[SplineRoute] LoadScenario '%s' OK (routes=%d vehicles=%d obstructions=%d followDist=%.1f spawnGap=%.1f)"),
		*SlotName, Save->Routes.Num(), Save->Vehicles.Num(), Save->Obstructions.Num(),
		MinFollowDistanceMeters, MinSpawnGapMeters);
	return true;
}

void USplineRouteAuthoringSubsystem::RegisterHUDPanel(UWidget* InHUDPanel)
{
	HUDPanel = InHUDPanel;
	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] HUD panel registered: %s"), *GetNameSafe(InHUDPanel));
}

bool USplineRouteAuthoringSubsystem::IsCursorOverHUD(APlayerController* PC) const
{
	if (!HUDPanel || !PC)
	{
		return false;
	}
	float MX = 0.f, MY = 0.f;
	if (!PC->GetMousePosition(MX, MY))
	{
		return false;
	}
	const FGeometry& Geom = HUDPanel->GetCachedGeometry();
	const FVector2D LocalSize = Geom.GetLocalSize();
	if (LocalSize.IsNearlyZero())
	{
		return false;
	}
	// GetMousePosition returns viewport-local pixels. The widget's cached geometry is in Slate absolute coords.
	// Multiply by the viewport's DPI to get Slate-absolute, OR use AbsoluteToLocal with the cursor in absolute coords.
	// PC->GetMousePosition is already in Slate absolute coords (viewport top-left = 0,0 absolute), so AbsoluteToLocal works.
	const FVector2D LocalPos = Geom.AbsoluteToLocal(FVector2D(MX, MY));
	return LocalPos.X >= 0.0f && LocalPos.X <= LocalSize.X
		&& LocalPos.Y >= 0.0f && LocalPos.Y <= LocalSize.Y;
}

bool USplineRouteAuthoringSubsystem::ProjectMouseToPlane(APlayerController* PC, FVector& OutWorldLocation) const
{
	if (!PC)
	{
		return false;
	}

	FVector WorldOrigin;
	FVector WorldDirection;
	if (!PC->DeprojectMousePositionToWorld(WorldOrigin, WorldDirection))
	{
		return false;
	}

	// Intersect ray (Origin, Dir) with plane Z = PlaneZ.
	const float DirZ = WorldDirection.Z;
	if (FMath::IsNearlyZero(DirZ))
	{
		return false;
	}

	const float T = (PlaneZ - WorldOrigin.Z) / DirZ;
	if (T <= 0.0f)
	{
		return false; // plane is behind the camera
	}

	OutWorldLocation = WorldOrigin + WorldDirection * T;
	OutWorldLocation.Z = PlaneZ;
	return true;
}

bool USplineRouteAuthoringSubsystem::CanPlaceAt(const FVector& WorldLocation) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return true;
	}

	// The "allowed" zone type depends on the active route's medium: vessels travel on Water zones,
	// ground vehicles on Ground (land) zones. Being inside an opposite-medium zone always blocks —
	// this is the shipyard boundary constraint that keeps trucks off the water and ships off the land.
	const ERouteVehicleType Medium = ActiveRoute ? ActiveRoute->VehicleType : ERouteVehicleType::Vessel;
	const ESplineRouteZoneType AllowedZone = (Medium == ERouteVehicleType::Ground)
		? ESplineRouteZoneType::Ground
		: ESplineRouteZoneType::Water;

	bool bHasAllowedZone = false;
	bool bInsideAllowedZone = false;
	for (TActorIterator<ASplineRouteZone> It(World); It; ++It)
	{
		const ASplineRouteZone* Zone = *It;
		if (!Zone)
		{
			continue;
		}
		const bool bContains = Zone->ContainsLocation(WorldLocation);
		if (Zone->ZoneType == AllowedZone)
		{
			bHasAllowedZone = true;
			if (bContains)
			{
				bInsideAllowedZone = true;
			}
		}
		else if (bContains)
		{
			return false;
		}
	}

	// If allowed zones exist, the point must be inside one; with none present, placement is unconstrained.
	return !bHasAllowedZone || bInsideAllowedZone;
}

ASplineRouteActor* USplineRouteAuthoringSubsystem::GetOrSpawnActiveRoute()
{
	if (ActiveRoute)
	{
		return ActiveRoute;
	}

	// If a route already exists in the world (level-placed), pick the first one.
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ASplineRouteActor> It(World); It; ++It)
		{
			AssignActiveRoute(*It);
			break;
		}
	}

	if (ActiveRoute)
	{
		return ActiveRoute;
	}

	if (UWorld* World = GetWorld())
	{
		TSubclassOf<ASplineRouteActor> Cls = DefaultRouteClass;
		if (!Cls)
		{
			Cls = ASplineRouteActor::StaticClass();
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AssignActiveRoute(World->SpawnActor<ASplineRouteActor>(Cls, FVector::ZeroVector, FRotator::ZeroRotator, Params));
		ApplySplineColorMaterial(ActiveRoute);
	}
	return ActiveRoute;
}

void USplineRouteAuthoringSubsystem::HandlePlaceOrSelect(APlayerController* PC)
{
	if (!PC)
	{
		ScreenMsg(FColor::Red, 3.0f, TEXT("[SplineRoute] HandlePlaceOrSelect: PC null"));
		return;
	}

	// Bail if the click landed inside the HUD widget. Enhanced Input bypasses UMG hit testing, so we have to filter here.
	if (IsCursorOverHUD(PC))
	{
		UE_LOG(LogSplineRoute, Verbose, TEXT("[SplineRoute] Place ignored — cursor over HUD."));
		return;
	}

	// First: check if the cursor is over an existing control-point handle.
	FHitResult Hit;
	if (PC->GetHitResultUnderCursorByChannel(UEngineTypes::ConvertToTraceType(ECC_Visibility), true, Hit))
	{
		if (UPrimitiveComponent* HitComp = Hit.GetComponent())
		{
			if (AActor* HitActor = Hit.GetActor())
			{
				if (ASplineRouteActor* Route = Cast<ASplineRouteActor>(HitActor))
				{
					const int32 PointIndex = Route->FindControlPointForComponent(HitComp);
					if (PointIndex != INDEX_NONE)
					{
						SetActiveRoute(Route);
						Route->SetSelectedControlPoint(PointIndex);
						DraggedPointIndex = PointIndex;
						ScreenMsg(FColor::Green, 2.0f, FString::Printf(
							TEXT("[SplineRoute] SELECT handle idx=%d on %s"),
							PointIndex, *Route->GetName()));
						return;
					}
				}
			}
		}
	}

	// Otherwise: project to the plane and append a new control point on the active route.
	FVector PlanePoint;
	if (!ProjectMouseToPlane(PC, PlanePoint))
	{
		ScreenMsg(FColor::Red, 3.0f, TEXT("[SplineRoute] PLACE: failed to project mouse to plane."));
		return;
	}

	ASplineRouteActor* Route = GetOrSpawnActiveRoute();
	if (!Route)
	{
		ScreenMsg(FColor::Red, 5.0f, TEXT("[SplineRoute] PLACE: no active route and failed to spawn one."));
		return;
	}

	const int32 NewIndex = Route->AddControlPoint(PlanePoint);
	Route->SetSelectedControlPoint(NewIndex);
	DraggedPointIndex = NewIndex;
	ScreenMsg(FColor::Green, 2.0f, FString::Printf(
		TEXT("[SplineRoute] PLACE idx=%d at (%.0f, %.0f, %.0f). Total points=%d"),
		NewIndex, PlanePoint.X, PlanePoint.Y, PlanePoint.Z, Route->GetNumControlPoints()));
}

void USplineRouteAuthoringSubsystem::HandlePlaceAtCursor(APlayerController* PC)
{
	if (!PC)
	{
		return;
	}
	if (IsCursorOverHUD(PC))
	{
		return;
	}

	// In obstruction-placement mode the place action drops a yard obstruction instead of a waypoint.
	if (bObstructionPlaceMode)
	{
		SpawnObstructionAtCursor(PC);
		return;
	}

	// If the cursor is over an existing handle, treat F as "select + drag" instead of placing.
	// Use screen-space proximity rather than a world line trace — the level's cadastral plane
	// (and other large background meshes) occludes the line trace even when the handle is
	// clearly visible. Iterate every route's control points, project to screen, and pick the
	// closest one inside HandleScreenSelectRadius pixels.
	{
		float MX = 0.f, MY = 0.f;
		if (PC->GetMousePosition(MX, MY))
		{
			const FVector2D Cursor(MX, MY);
			constexpr float SelectRadiusPx = 32.0f;
			float BestDistSq = SelectRadiusPx * SelectRadiusPx;
			ASplineRouteActor* BestRoute = nullptr;
			int32 BestIndex = INDEX_NONE;

			if (UWorld* World = GetWorld())
			{
				for (TActorIterator<ASplineRouteActor> It(World); It; ++It)
				{
					ASplineRouteActor* Route = *It;
					if (!Route)
					{
						continue;
					}
					const int32 NumPoints = Route->GetNumControlPoints();
					USplineComponent* Sp = Route->GetSpline();
					if (!Sp || NumPoints == 0)
					{
						continue;
					}
					for (int32 i = 0; i < NumPoints; ++i)
					{
						const FVector WorldLoc = Sp->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::World);
						FVector2D ScreenLoc;
						if (!PC->ProjectWorldLocationToScreen(WorldLoc, ScreenLoc))
						{
							continue;
						}
						const float DistSq = FVector2D::DistSquared(Cursor, ScreenLoc);
						if (DistSq < BestDistSq)
						{
							BestDistSq = DistSq;
							BestRoute = Route;
							BestIndex = i;
						}
					}
				}
			}

			UE_LOG(LogSplineRoute, Log,
				TEXT("[SplineRoute] PLACE (F) screen-proximity: cursor=(%.1f, %.1f) best=%s idx=%d distPx=%.1f (radius=%.1f)"),
				MX, MY,
				BestRoute ? *BestRoute->GetName() : TEXT("None"),
				BestIndex,
				BestIndex == INDEX_NONE ? -1.0f : FMath::Sqrt(BestDistSq),
				SelectRadiusPx);

			if (BestRoute && BestIndex != INDEX_NONE)
			{
				SetActiveRoute(BestRoute);
				BestRoute->SetSelectedControlPoint(BestIndex);
				// Select only — movement is Spacebar-gated (see HandleDrag), so don't arm a drag here.
				DraggedPointIndex = INDEX_NONE;
				UE_LOG(LogSplineRoute, Log,
					TEXT("[SplineRoute] PLACE (F) over handle idx=%d on %s — selecting instead of placing."),
					BestIndex, *BestRoute->GetName());
				return;
			}
		}
	}

	FVector PlanePoint;
	if (!ProjectMouseToPlane(PC, PlanePoint))
	{
		ScreenMsg(FColor::Red, 3.0f, TEXT("[SplineRoute] PLACE: failed to project mouse to plane."));
		return;
	}

	if (!CanPlaceAt(PlanePoint))
	{
		UE_LOG(LogSplineRoute, Log,
			TEXT("[SplineRoute] PLACE blocked by zone at (%.0f, %.0f, %.0f)"),
			PlanePoint.X, PlanePoint.Y, PlanePoint.Z);
		return;
	}

	ASplineRouteActor* Route = GetOrSpawnActiveRoute();
	if (!Route)
	{
		ScreenMsg(FColor::Red, 5.0f, TEXT("[SplineRoute] PLACE: no active route and failed to spawn one."));
		return;
	}

	const int32 NewIndex = Route->AddControlPoint(PlanePoint);
	Route->SetSelectedControlPoint(NewIndex);
	// Intentionally NOT setting DraggedPointIndex here. F is a discrete tap, not a held drag, so leaving
	// DraggedPointIndex pointing at the newly-placed point causes the next LMB-drag (when its own
	// hit-test fails to find a handle) to grab and move this stale point instead.
	DraggedPointIndex = INDEX_NONE;
	UE_LOG(LogSplineRoute, Log,
		TEXT("[SplineRoute] PLACE (F) idx=%d at (%.0f, %.0f, %.0f). Total=%d"),
		NewIndex, PlanePoint.X, PlanePoint.Y, PlanePoint.Z, Route->GetNumControlPoints());
}

bool USplineRouteAuthoringSubsystem::FindClosestControlPointOnScreen(APlayerController* PC, float RadiusPx, ASplineRouteActor*& OutRoute, int32& OutIndex) const
{
	OutRoute = nullptr;
	OutIndex = INDEX_NONE;
	if (!PC)
	{
		return false;
	}
	float MX = 0.f, MY = 0.f;
	if (!PC->GetMousePosition(MX, MY))
	{
		return false;
	}
	const FVector2D Cursor(MX, MY);
	float BestDistSq = RadiusPx * RadiusPx;

	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ASplineRouteActor> It(World); It; ++It)
		{
			ASplineRouteActor* Route = *It;
			if (!Route)
			{
				continue;
			}
			USplineComponent* Sp = Route->GetSpline();
			const int32 NumPoints = Route->GetNumControlPoints();
			if (!Sp || NumPoints == 0)
			{
				continue;
			}
			for (int32 i = 0; i < NumPoints; ++i)
			{
				const FVector WorldLoc = Sp->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::World);
				FVector2D ScreenLoc;
				if (!PC->ProjectWorldLocationToScreen(WorldLoc, ScreenLoc))
				{
					continue;
				}
				const float DistSq = FVector2D::DistSquared(Cursor, ScreenLoc);
				if (DistSq < BestDistSq)
				{
					BestDistSq = DistSq;
					OutRoute = Route;
					OutIndex = i;
				}
			}
		}
	}

	return OutRoute != nullptr && OutIndex != INDEX_NONE;
}

ASplineFollowerVessel* USplineRouteAuthoringSubsystem::GetSelectedVessel() const
{
	return Cast<ASplineFollowerVessel>(SelectedVessel.Get());
}

void USplineRouteAuthoringSubsystem::SetSelectedVessel(AActor* NewVessel)
{
	if (SelectedVessel.Get() == NewVessel)
	{
		return;
	}

	// Restore the exact pre-highlight scale on whatever was selected before — never assume a fixed
	// "base" scale, since SetSizeMultiplier() (the Vessel Size HUD slider) can change the actual scale
	// independently of this highlight while it's selected. If it's been destroyed since, Get() is
	// already null and there's nothing to restore.
	if (AActor* OldVessel = SelectedVessel.Get())
	{
		OldVessel->SetActorScale3D(PreHighlightVesselScale);
	}

	SelectedVessel = NewVessel;

	if (NewVessel)
	{
		PreHighlightVesselScale = NewVessel->GetActorScale3D();
		NewVessel->SetActorScale3D(PreHighlightVesselScale * VesselSelectionHighlightScale);
	}
}

void USplineRouteAuthoringSubsystem::HandleSelectAtCursor(APlayerController* PC)
{
	if (!PC || IsCursorOverHUD(PC))
	{
		return;
	}

	ASplineRouteActor* BestRoute = nullptr;
	int32 BestIndex = INDEX_NONE;
	if (FindClosestControlPointOnScreen(PC, HandleSelectRadiusPx, BestRoute, BestIndex))
	{
		SetActiveRoute(BestRoute);
		BestRoute->SetSelectedControlPoint(BestIndex);
		// Mutually exclusive with vessel selection — a waypoint pick always wins the Delete-key target,
		// and restores/clears any vessel highlight so at most one of the two is ever visibly selected.
		SetSelectedVessel(nullptr);
		// Selection only — do NOT arm a drag. Moving a point now requires holding the Spacebar modifier
		// while dragging (see HandleDrag). This is the core fix for "clicking to select moved my point".
		DraggedPointIndex = INDEX_NONE;
		UE_LOG(LogSplineRoute, Log,
			TEXT("[SplineRoute] SELECT (LMB) idx=%d on %s"),
			BestIndex, *BestRoute->GetName());
		return;
	}

	// No waypoint handle under the cursor — try selecting a vessel instead. A miss here (empty
	// water/ground) deliberately leaves SelectedVessel untouched, matching the waypoint path above
	// (a miss doesn't clear the existing waypoint selection either).
	//
	// Not a standard visibility trace: ASplineFollowerVessel's MeshComponent uses the
	// OverlapAllDynamic profile (every standard channel set to Overlap, not Block — required so it
	// registers with Waterline's buoyancy volume, see SplineFollowerVessel.cpp's constructor comment),
	// so a normal blocking line trace against ECC_Visibility would silently never hit it. Its actual
	// Object Type is the custom ECC_GameTraceChannel1, so an object-type trace against that channel is
	// used instead — and Multi (not Single) because Waterline's own floor/wall geometry shares the same
	// channel, so the first hit along the ray isn't guaranteed to be the vessel.
	//
	// The generic kind (BP_Ship_Cargo_2/3/4/6) uses a completely different setup — confirmed via
	// ue5_get_component_defaults that its mesh sits on the "CharacterMesh" collision profile with
	// ObjectType=ECC_Pawn, not ECC_GameTraceChannel1 — so ECC_Pawn is queried too, purely as an
	// additional object type on this same trace (no change to either Blueprint's own collision setup,
	// which is out of this task's scope). Safe to broaden: a stray hit on some other pawn (e.g. the
	// player's own) is harmless, since the FindComponentByClass check below still has to pass before
	// anything is selected.
	FVector RayStart, RayDir;
	if (PC->DeprojectMousePositionToWorld(RayStart, RayDir))
	{
		const FVector RayEnd = RayStart + RayDir * 1000000.0f;
		TArray<FHitResult> Hits;
		FCollisionObjectQueryParams VesselQuery;
		VesselQuery.AddObjectTypesToQuery(ECollisionChannel::ECC_GameTraceChannel1);
		VesselQuery.AddObjectTypesToQuery(ECollisionChannel::ECC_Pawn);
		if (GetWorld() && GetWorld()->LineTraceMultiByObjectType(Hits, RayStart, RayEnd, VesselQuery))
		{
			for (const FHitResult& Hit : Hits)
			{
				AActor* HitActor = Hit.GetActor();
				// Either vessel kind carries a USplineFollowerComponent somewhere on it — checking for the
				// component directly (rather than Cast<ASplineFollowerVessel>) is what makes this generic
				// across both kinds, same convention SpawnVesselOnActiveRoute's generic branch already uses.
				if (HitActor && HitActor->FindComponentByClass<USplineFollowerComponent>())
				{
					// Mutually exclusive with waypoint selection — a vessel pick always wins the Delete-key
					// target, and clears any waypoint highlight on the active route so at most one of the
					// two is ever visibly selected.
					if (ActiveRoute && ActiveRoute->GetSelectedControlPoint() != INDEX_NONE)
					{
						ActiveRoute->SetSelectedControlPoint(INDEX_NONE);
					}
					SetSelectedVessel(HitActor);
					UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] SELECT (LMB) vessel %s"), *HitActor->GetName());
					break;
				}
			}
		}
	}
}

void USplineRouteAuthoringSubsystem::SelectNextPoint()
{
	CycleSelection(+1);
}

void USplineRouteAuthoringSubsystem::SelectPrevPoint()
{
	CycleSelection(-1);
}

void USplineRouteAuthoringSubsystem::CycleSelection(int32 Delta)
{
	ASplineRouteActor* Route = GetActiveRoute();
	if (!Route)
	{
		Route = EnsureActiveRoute();
	}
	if (!Route)
	{
		return;
	}
	const int32 NumPoints = Route->GetNumControlPoints();
	if (NumPoints <= 0)
	{
		return;
	}
	const int32 Current = Route->GetSelectedControlPoint();
	int32 NewIndex;
	if (Current == INDEX_NONE)
	{
		NewIndex = (Delta >= 0) ? 0 : NumPoints - 1;
	}
	else
	{
		// Positive modulo so Delta=-1 wraps from 0 back to the last point.
		NewIndex = ((Current + Delta) % NumPoints + NumPoints) % NumPoints;
	}
	Route->SetSelectedControlPoint(NewIndex);
	DraggedPointIndex = INDEX_NONE;
	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] CYCLE selection -> idx=%d / %d on %s"),
		NewIndex, NumPoints, *Route->GetName());
}

void USplineRouteAuthoringSubsystem::HandleDrag(APlayerController* PC)
{
	if (!PC || IsCursorOverHUD(PC))
	{
		return;
	}

	// First call after the Spacebar modifier engaged: pick a drag target.
	// Prefer the already-selected waypoint (so "select with LMB, then Spacebar-drag" moves THAT point),
	// otherwise grab the handle nearest the cursor as a convenience.
	if (!IsDragging())
	{
		int32 Grab = ActiveRoute ? ActiveRoute->GetSelectedControlPoint() : INDEX_NONE;
		if (Grab == INDEX_NONE)
		{
			ASplineRouteActor* HitRoute = nullptr;
			int32 HitIndex = INDEX_NONE;
			if (FindClosestControlPointOnScreen(PC, HandleSelectRadiusPx, HitRoute, HitIndex))
			{
				SetActiveRoute(HitRoute);            // note: resets DraggedPointIndex internally
				HitRoute->SetSelectedControlPoint(HitIndex);
				Grab = HitIndex;
			}
		}
		if (Grab == INDEX_NONE)
		{
			return;
		}
		DraggedPointIndex = Grab;                     // set AFTER any SetActiveRoute so it isn't cleared
	}

	if (!ActiveRoute)
	{
		return;
	}

	FVector PlanePoint;
	if (!ProjectMouseToPlane(PC, PlanePoint))
	{
		return;
	}

	if (!CanPlaceAt(PlanePoint))
	{
		// Don't move the point into a forbidden zone — just leave it at its last valid spot.
		return;
	}

	ActiveRoute->MoveControlPoint(DraggedPointIndex, PlanePoint);
	UE_LOG(LogSplineRoute, Verbose, TEXT("[SplineRoute] DRAG idx=%d -> %s"),
		DraggedPointIndex, *PlanePoint.ToString());
}

void USplineRouteAuthoringSubsystem::HandleReleaseDrag()
{
	if (DraggedPointIndex != INDEX_NONE)
	{
		ScreenMsg(FColor::Cyan, 2.0f, FString::Printf(TEXT("[SplineRoute] RELEASE drag idx=%d"), DraggedPointIndex));
	}
	DraggedPointIndex = INDEX_NONE;
}

void USplineRouteAuthoringSubsystem::HandleDeleteSelected()
{
	// Vessel deletion takes priority when one is selected — mutually exclusive with waypoint selection
	// per HandleSelectAtCursor, so SelectedVessel and ActiveRoute's selected control point are never
	// both set at once, and this is never ambiguous about which one Delete should act on.
	if (AActor* Vessel = SelectedVessel.Get())
	{
		// Clear the spawn gate if it was tracking this exact vessel/follower, so a future spawn doesn't
		// compare against a distance read off an about-to-be-destroyed component.
		if (LastSpawnedVessel.Get() == Vessel)
		{
			LastSpawnedVessel.Reset();
			LastSpawnedFollower.Reset();
		}
		// Any OTHER vessel's Follower->LeaderFollower pointing at this one's follower is a
		// TWeakObjectPtr and auto-invalidates once this actor (and its component) is destroyed below;
		// its only reader (SetVesselSizeMultiplier's resize-correction pass) already guards with
		// LeaderFollower.IsValid() before dereferencing, so a dangling reference doesn't crash — verified,
		// not assumed, since USplineFollowerComponent::TickComponent() doesn't touch LeaderFollower at all.
		const FString VesselName = Vessel->GetName();
		// Restores the pre-highlight scale (cosmetic only — moot once destroyed, but cheap) and clears
		// SelectedVessel so nothing still references this actor afterward.
		SetSelectedVessel(nullptr);
		Vessel->Destroy();
		ScreenMsg(FColor::Orange, 2.0f, FString::Printf(TEXT("[SplineRoute] DELETE vessel %s"), *VesselName));
		UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] DELETE vessel %s"), *VesselName);
		return;
	}

	if (!ActiveRoute)
	{
		ScreenMsg(FColor::Orange, 2.0f, TEXT("[SplineRoute] DELETE: no active route."));
		return;
	}
	const int32 Sel = ActiveRoute->GetSelectedControlPoint();
	if (Sel == INDEX_NONE)
	{
		ScreenMsg(FColor::Orange, 2.0f, TEXT("[SplineRoute] DELETE: no point selected."));
		return;
	}
	ActiveRoute->RemoveControlPoint(Sel);
	if (DraggedPointIndex == Sel)
	{
		DraggedPointIndex = INDEX_NONE;
	}
	ScreenMsg(FColor::Orange, 2.0f, FString::Printf(
		TEXT("[SplineRoute] DELETE idx=%d. Remaining=%d"), Sel, ActiveRoute->GetNumControlPoints()));
}
