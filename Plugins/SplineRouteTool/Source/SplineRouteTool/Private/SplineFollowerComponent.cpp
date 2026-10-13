// Copyright DamenTools internal. Spline Route Tool plugin.

#include "SplineFollowerComponent.h"
#include "SplineRouteActor.h"
#include "SplineRouteTimeSubsystem.h"
#include "SplineRouteTool.h"

#include "Components/SplineComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "WaterBodyComponent.h"
#include "WaterBodyOceanActor.h"

USplineFollowerComponent::USplineFollowerComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void USplineFollowerComponent::BeginPlay()
{
	Super::BeginPlay();
	DistanceAlongSpline = InitialDistance;
	ElapsedDelay = 0.0f;
	Direction = 1;
	bHasReachedStart = (InitialDistance >= 0.0f);
	ResolveOceanReference();
	UE_LOG(LogSplineRoute, Log,
		TEXT("[SplineRoute] Follower %s BeginPlay. Route=%s Speed=%.1f StartDelay=%.2f InitialDistance=%.1f AlignTangent=%s YawOnly=%s HeightOffset=%.1f EndBehavior=%d"),
		*GetOwner()->GetName(), *GetNameSafe(Route), Speed, StartDelay, InitialDistance,
		bAlignToTangent ? TEXT("true") : TEXT("false"),
		bYawOnly ? TEXT("true") : TEXT("false"),
		HeightOffset, static_cast<int32>(EndBehavior));

	if (!Route)
	{
		UE_LOG(LogSplineRoute, Warning, TEXT("[SplineRoute] Follower %s has no Route assigned — it will not move."),
			*GetOwner()->GetName());
	}
	else
	{
		BindRouteDelegate(Route);
		SnapToRouteStart();
	}
}

void USplineFollowerComponent::SnapToRouteStart()
{
	if (!Route)
	{
		return;
	}

	// Anchor visually to the spline immediately so we don't render at the spawn-time position for a frame.
	USplineComponent* Spline = Route->GetSpline();
	if (!Spline || Spline->GetNumberOfSplinePoints() < 1)
	{
		return;
	}

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	const float TotalLength = Spline->GetSplineLength();
	const float ClampedDist = FMath::Clamp(DistanceAlongSpline, 0.0f, TotalLength);
	FVector StartLoc = Spline->GetLocationAtDistanceAlongSpline(ClampedDist, ESplineCoordinateSpace::World);
	float OceanSurfaceZ = 0.0f;
	StartLoc.Z = TryGetOceanSurfaceZ(StartLoc, OceanSurfaceZ) ? (OceanSurfaceZ + HeightOffset) : (StartLoc.Z + HeightOffset);
	Owner->SetActorLocation(StartLoc);

	// Face the start tangent immediately so turn-rate damping doesn't visibly spin the vessel on spawn.
	if (bAlignToTangent)
	{
		const FVector StartTangent = Spline->GetTangentAtDistanceAlongSpline(ClampedDist, ESplineCoordinateSpace::World);
		if (!StartTangent.IsNearlyZero())
		{
			FRotator StartRot = StartTangent.Rotation();
			if (bYawOnly)
			{
				StartRot.Pitch = 0.0f;
				StartRot.Roll = 0.0f;
			}
			Owner->SetActorRotation(StartRot);
		}
	}
}

void USplineFollowerComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnbindRouteDelegate(Route);
	Super::EndPlay(EndPlayReason);
}

void USplineFollowerComponent::SetRoute(ASplineRouteActor* NewRoute)
{
	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] Follower %s SetRoute: %s -> %s"),
		*GetOwner()->GetName(), *GetNameSafe(Route), *GetNameSafe(NewRoute));
	UnbindRouteDelegate(Route);
	Route = NewRoute;
	DistanceAlongSpline = 0.0f;
	ElapsedDelay = 0.0f;
	Direction = 1;
	BindRouteDelegate(Route);
}

void USplineFollowerComponent::ConfigureFollower(ASplineRouteActor* InRoute, float InSpeed, float InTurnRate, float InReverseApproachSeconds, float InReverseMinSpeedFactor)
{
	Speed = InSpeed;
	TurnRateDegPerSec = InTurnRate;
	ReverseApproachSeconds = InReverseApproachSeconds;
	ReverseMinSpeedFactor = InReverseMinSpeedFactor;
	SetRoute(InRoute);
	// Place the owner on the route right now — safe whether the owner's BeginPlay has already run (the
	// generic AActor kind, configured after FinishSpawning) or hasn't yet (ASplineFollowerVessel,
	// configured before FinishSpawning — BeginPlay's own SnapToRouteStart() call then just repeats this
	// harmlessly once Route is confirmed set).
	SnapToRouteStart();
	UE_LOG(LogSplineRoute, Log,
		TEXT("[SplineRoute] Follower %s ConfigureFollower: route=%s speed=%.1f turnRate=%.1f"),
		*GetOwner()->GetName(), *GetNameSafe(InRoute), InSpeed, InTurnRate);
}

void USplineFollowerComponent::BindRouteDelegate(ASplineRouteActor* RouteToBind)
{
	if (RouteToBind)
	{
		RouteToBind->OnSplineGeometryChanged.AddDynamic(this, &USplineFollowerComponent::HandleSplineGeometryChanged);
	}
}

void USplineFollowerComponent::UnbindRouteDelegate(ASplineRouteActor* RouteToUnbind)
{
	if (RouteToUnbind)
	{
		RouteToUnbind->OnSplineGeometryChanged.RemoveDynamic(this, &USplineFollowerComponent::HandleSplineGeometryChanged);
	}
}

void USplineFollowerComponent::HandleSplineGeometryChanged()
{
	ReanchorToOwnerLocation();
}

void USplineFollowerComponent::ReanchorToOwnerLocation()
{
	AActor* Owner = GetOwner();
	if (!Owner || !Route)
	{
		return;
	}
	USplineComponent* Spline = Route->GetSpline();
	if (!Spline || Spline->GetNumberOfSplinePoints() < 2)
	{
		return;
	}

	// Snap to whichever spline point is closest to the owner's current location (minus the height offset that lifts the cube off the spline plane).
	FVector Query = Owner->GetActorLocation();
	Query.Z -= HeightOffset;

	const float InputKey = Spline->FindInputKeyClosestToWorldLocation(Query);
	const float NewDist  = Spline->GetDistanceAlongSplineAtSplineInputKey(InputKey);

	UE_LOG(LogSplineRoute, Verbose, TEXT("[SplineRoute] Follower %s reanchor %.1f -> %.1f (TotalLength=%.1f)"),
		*Owner->GetName(), DistanceAlongSpline, NewDist, Spline->GetSplineLength());

	DistanceAlongSpline = NewDist;
}

void USplineFollowerComponent::NudgeDistanceAlongSpline(float Delta)
{
	DistanceAlongSpline += Delta;

	AActor* Owner = GetOwner();
	if (!Owner || !Route)
	{
		return;
	}
	USplineComponent* Spline = Route->GetSpline();
	if (!Spline || Spline->GetNumberOfSplinePoints() < 1)
	{
		return;
	}
	const float ClampedDist = FMath::Clamp(DistanceAlongSpline, 0.0f, Spline->GetSplineLength());
	FVector NewLoc = Spline->GetLocationAtDistanceAlongSpline(ClampedDist, ESplineCoordinateSpace::World);
	float OceanSurfaceZ = 0.0f;
	NewLoc.Z = TryGetOceanSurfaceZ(NewLoc, OceanSurfaceZ) ? (OceanSurfaceZ + HeightOffset) : (NewLoc.Z + HeightOffset);
	Owner->SetActorLocation(NewLoc, false, nullptr, ETeleportType::TeleportPhysics);
}

void USplineFollowerComponent::ResetToStart()
{
	DistanceAlongSpline = InitialDistance;
	ElapsedDelay = 0.0f;
	Direction = 1;
	bHasReachedStart = (InitialDistance >= 0.0f);

	AActor* Owner = GetOwner();
	if (Owner && Route)
	{
		if (USplineComponent* Spline = Route->GetSpline())
		{
			if (Spline->GetNumberOfSplinePoints() >= 1)
			{
				const float ClampedDist = FMath::Clamp(DistanceAlongSpline, 0.0f, Spline->GetSplineLength());
				FVector StartLoc = Spline->GetLocationAtDistanceAlongSpline(ClampedDist, ESplineCoordinateSpace::World);
				float OceanSurfaceZ = 0.0f;
				StartLoc.Z = TryGetOceanSurfaceZ(StartLoc, OceanSurfaceZ) ? (OceanSurfaceZ + HeightOffset) : (StartLoc.Z + HeightOffset);
				FRotator StartRot = Owner->GetActorRotation();
				if (bAlignToTangent)
				{
					const FVector Tangent = Spline->GetTangentAtDistanceAlongSpline(ClampedDist, ESplineCoordinateSpace::World);
					if (!Tangent.IsNearlyZero())
					{
						StartRot = Tangent.Rotation();
						if (bYawOnly)
						{
							StartRot.Pitch = 0.0f;
							StartRot.Roll = 0.0f;
						}
					}
				}
				Owner->SetActorLocationAndRotation(StartLoc, StartRot, false, nullptr, ETeleportType::TeleportPhysics);
			}
		}
	}
	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] Follower %s reset to start (dist=%.1f)"),
		*GetNameSafe(GetOwner()), DistanceAlongSpline);
}

void USplineFollowerComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	AActor* Owner = GetOwner();

	// Resolve the CharacterMovementComponent once per tick (null for anything that isn't an
	// ACharacter, or when the switch is off) — every early-return below zeroes its Velocity so a
	// route/route-validity change mid-flight can't leave the actor coasting under stale physics.
	// Deliberately NOT forcing MOVE_Flying here yet — a vessel placed directly in the level with
	// no Route assigned would otherwise have gravity disabled on its very first tick and float
	// forever instead of falling to the water/ground like it did before bDriveViaCharacterMovement
	// existed. The mode switch only happens below, once we know there's an actual route to fly along.
	UCharacterMovementComponent* CharMove = nullptr;
	if (bDriveViaCharacterMovement && Owner)
	{
		if (ACharacter* Character = Cast<ACharacter>(Owner))
		{
			CharMove = Character->GetCharacterMovement();
		}
	}

	if (!Owner || !Route || !Route->GetSpline())
	{
		CurrentEffectiveSpeed = 0.0f;
		if (CharMove) { CharMove->RequestDirectMove(FVector::ZeroVector, false); }
		return;
	}

	USplineComponent* Spline = Route->GetSpline();
	if (Spline->GetNumberOfSplinePoints() < 2)
	{
		CurrentEffectiveSpeed = 0.0f;
		if (CharMove) { CharMove->RequestDirectMove(FVector::ZeroVector, false); }
		return;
	}

	// Only now do we know this follower actually has a route to drive along — safe to force Flying
	// (and the acceleration-bypass flag) so a tangent-derived Velocity isn't fought by gravity or
	// ramped via MaxAcceleration. A vessel with no Route never reaches this point, so it keeps its
	// default MovementMode (gravity applies, it falls normally).
	if (CharMove && CharMove->MovementMode != MOVE_Flying)
	{
		// No floor under an ocean route — Flying disables gravity so a tangent-derived
		// Velocity (which usually has ~zero Z) doesn't fight gravity's constant pull down.
		CharMove->SetMovementMode(MOVE_Flying);
		// "If false, path following velocity is set directly, disregarding acceleration"
		// (engine doc comment on this property) — without this, RequestDirectMove() below
		// ramps toward the target velocity via MaxAcceleration instead of matching it
		// immediately, which would drift the actor's real speed away from the spline math's
		// CurrentEffectiveSpeed. Set once here rather than every tick.
		CharMove->bRequestedMoveUseAcceleration = false;
	}

	// Physics — not this component — has final authority over where the actor actually ends up once
	// CharacterMovementComponent is integrating real Velocity, so re-derive DistanceAlongSpline from
	// the actor's actual current position every tick rather than trusting last tick's commanded value
	// (which is what ReanchorToOwnerLocation() already does for the "geometry changed" case).
	// Gated on bHasReachedStart: while a negative InitialDistance is still staging the spawn, the actor
	// is deliberately parked at the spline START (world position == distance 0) while DistanceAlongSpline
	// itself counts up from a negative virtual value in the background — reprojecting from the actual
	// (parked) position during that phase would read back "0" every tick and permanently stomp the
	// countdown before it ever finishes, defeating the staggered-launch mechanism entirely.
	if (CharMove && bHasReachedStart)
	{
		ReanchorToOwnerLocation();
	}

	const float TimeScale = GetEffectiveTimeScale();
	const float ScaledDelta = DeltaTime * TimeScale;

	if (ElapsedDelay < StartDelay)
	{
		ElapsedDelay += ScaledDelta;
		CurrentEffectiveSpeed = 0.0f;
		if (CharMove) { CharMove->RequestDirectMove(FVector::ZeroVector, false); }
		return;
	}

	const float TotalLength = Spline->GetSplineLength();
	if (TotalLength <= KINDA_SMALL_NUMBER)
	{
		CurrentEffectiveSpeed = 0.0f;
		if (CharMove) { CharMove->RequestDirectMove(FVector::ZeroVector, false); }
		return;
	}

	// Per-segment speed overrides on the route (if any) take precedence over the follower's base Speed.
	const float QueryDistForSpeed = FMath::Clamp(DistanceAlongSpline, 0.0f, TotalLength);
	float EffectiveSpeed = Route->GetSpeedAtDistance(QueryDistForSpeed, Speed);

	// Verbose, override-only log — silent during normal motion, fires every frame a segment override is active.
	if (!FMath::IsNearlyEqual(EffectiveSpeed, Speed))
	{
		UE_LOG(LogSplineRoute, Verbose,
			TEXT("[SplineRoute] %s seg-override active: dist=%.1f baseSpeed=%.1f effective=%.1f"),
			*GetOwner()->GetName(), QueryDistForSpeed, Speed, EffectiveSpeed);
	}

	// Slow down approaching a reverse waypoint and speed back up leaving it, so the heading flip in the
	// rotation block below isn't paired with the vessel still cruising at full speed through the turn.
	// Passing the pre-slowdown EffectiveSpeed converts ReverseApproachSeconds into a distance window, so
	// the deceleration takes the same wall-clock time regardless of vessel speed or TimeScale.
	EffectiveSpeed *= ComputeReverseSpeedFactor(QueryDistForSpeed, EffectiveSpeed);

	// TimeScale-scaled so paused/slow-mo playback reads as zero/reduced speed for anything sampling this
	// (e.g. wake-intensity VFX), not just the raw per-segment cruise speed.
	CurrentEffectiveSpeed = EffectiveSpeed * TimeScale;

	DistanceAlongSpline += EffectiveSpeed * ScaledDelta * Direction;

	if (!bHasReachedStart && DistanceAlongSpline >= 0.0f)
	{
		bHasReachedStart = true;
	}

	// Set when EndBehavior just snapped DistanceAlongSpline to a spatially-disconnected point (Loop's
	// wrap back to the start) or pinned it at a boundary (Clamp) — both need special handling in the
	// CharacterMovementComponent path below; a plain PingPong reflection doesn't (it stays continuous
	// in world space, just relabels which way "forward" counts).
	bool bWrappedThisTick = false;
	bool bClampedThisTick = false;

	// Handle reaching either end of the spline. The negative-end branch only kicks in after the follower has begun normal motion,
	// so a negative InitialDistance can be used as a "spawn-stagger without teleport" mechanism.
	if (DistanceAlongSpline >= TotalLength)
	{
		switch (EndBehavior)
		{
		case ESplineFollowerEndBehavior::Loop:
		{
			const float Overshoot = DistanceAlongSpline - TotalLength;
			DistanceAlongSpline = -Overshoot;
			bHasReachedStart = false;
			bWrappedThisTick = true;
			break;
		}
		case ESplineFollowerEndBehavior::Clamp:
			DistanceAlongSpline = TotalLength;
			bClampedThisTick = true;
			break;
		case ESplineFollowerEndBehavior::PingPong:
			DistanceAlongSpline = TotalLength - (DistanceAlongSpline - TotalLength);
			Direction = -1;
			break;
		}
	}
	else if (DistanceAlongSpline < 0.0f && bHasReachedStart)
	{
		switch (EndBehavior)
		{
		case ESplineFollowerEndBehavior::Loop:
			DistanceAlongSpline = TotalLength + FMath::Fmod(DistanceAlongSpline, TotalLength);
			bWrappedThisTick = true;
			break;
		case ESplineFollowerEndBehavior::Clamp:
			DistanceAlongSpline = 0.0f;
			bClampedThisTick = true;
			break;
		case ESplineFollowerEndBehavior::PingPong:
			DistanceAlongSpline = -DistanceAlongSpline;
			Direction = 1;
			break;
		}
	}

	// Position queries use a clamped distance so a negative InitialDistance keeps the cube parked at the spline start until ready.
	const float QueryDist = FMath::Clamp(DistanceAlongSpline, 0.0f, TotalLength);

	FVector NewLocation = Spline->GetLocationAtDistanceAlongSpline(QueryDist, ESplineCoordinateSpace::World);
	float OceanSurfaceZ = 0.0f;
	NewLocation.Z = TryGetOceanSurfaceZ(NewLocation, OceanSurfaceZ) ? (OceanSurfaceZ + HeightOffset) : (NewLocation.Z + HeightOffset);

	const FRotator CurrentRotation = Owner->GetActorRotation();
	FRotator TargetRotation = CurrentRotation;
	// Direction of travel at QueryDist, reused below as the CharacterMovementComponent Velocity
	// direction so it stays exactly consistent with the facing computed here (same reverse-heading
	// flip included) rather than re-deriving it separately.
	FVector TravelTangent = Owner->GetActorForwardVector();
	if (bAlignToTangent)
	{
		// Reverse nodes flip the heading: the vessel keeps progressing along the route but faces
		// backwards (stern-first), simulating a vessel backing into a dock or executing a K-turn.
		const bool bReverseHere = Route->IsReverseAtDistance(QueryDist);
		const float HeadingSign = static_cast<float>(Direction) * (bReverseHere ? -1.0f : 1.0f);
		const FVector Tangent = Spline->GetTangentAtDistanceAlongSpline(QueryDist, ESplineCoordinateSpace::World) * HeadingSign;
		if (!Tangent.IsNearlyZero())
		{
			TravelTangent = Tangent;
			TargetRotation = Tangent.Rotation();
			if (bYawOnly)
			{
				TargetRotation.Pitch = 0.0f;
				TargetRotation.Roll = 0.0f;
			}
		}
	}

	// Ease toward the target heading at TurnRateDegPerSec (0 = snap). Stops large vessels snapping
	// instantly at sharp corners. ScaledDelta keeps the turn consistent under the global time slider —
	// EffectiveTurnRate below counteracts that scaling specifically during a reverse maneuver, so the
	// heading swing still takes a fixed wall-clock duration there (matching ReverseApproachSeconds).
	float EffectiveTurnRate = TurnRateDegPerSec;
	if (Route && Route->IsReverseAtDistance(QueryDist))
	{
		EffectiveTurnRate = TurnRateDegPerSec / FMath::Max(1.0f, GetEffectiveTimeScale());
	}

	FRotator NewRotation = TargetRotation;
	if (bAlignToTangent && TurnRateDegPerSec > 0.0f)
	{
		NewRotation = FMath::RInterpConstantTo(CurrentRotation, TargetRotation, ScaledDelta, EffectiveTurnRate);
		if (bYawOnly)
		{
			NewRotation.Pitch = 0.0f;
			NewRotation.Roll = 0.0f;
		}
	}

	if (CharMove)
	{
		if (bWrappedThisTick)
		{
			// Loop's wrap-to-start is a genuine spatial discontinuity — an open spline's end and start
			// are typically nowhere near each other in world space, so there's no physically continuous
			// path between them for Velocity to integrate across. This one frame still needs an explicit
			// relocation regardless of bDriveViaCharacterMovement; every other frame moves the actor
			// purely via Velocity below. (PingPong's reflection is intentionally NOT included here — it
			// stays spatially continuous, so it never needs this.)
			Owner->SetActorLocationAndRotation(NewLocation, NewRotation, false, nullptr, ETeleportType::TeleportPhysics);
		}
		else
		{
			// Position is now physics-owned; only rotation is still set explicitly, per the task's own
			// recommendation, to keep the existing precise tangent-facing/turn-rate-easing behavior
			// intact rather than relying on CharacterMovementComponent's own orientation interpolation.
			Owner->SetActorRotation(NewRotation);
		}
		// Zero at a Clamp boundary (so the vessel actually stops there) and while a negative
		// InitialDistance is still staging the spawn (!bHasReachedStart — the kinematic path keeps the
		// actor visually parked at QueryDist==0 during this phase via the position clamp below, but
		// physics doesn't know about that clamp; it would just drift the actor forward on real Velocity
		// with nothing holding it at the anchor). Otherwise CurrentEffectiveSpeed already carries
		// StartDelay/TimeScale/reverse-ramp scaling correctly.
		const float VelocityMagnitude = (bClampedThisTick || !bHasReachedStart) ? 0.0f : CurrentEffectiveSpeed;
		// RequestDirectMove(), not a raw Velocity= assignment: CharacterMovementComponent's own
		// per-tick CalcVelocity() treats a directly-written Velocity with zero Acceleration as
		// residual momentum and brakes it toward zero every tick (confirmed empirically — the actor
		// barely crept forward when Velocity was set directly). RequestDirectMove() is the engine's
		// own API for exactly this case (an external system commanding a velocity every tick without
		// AddMovementInput's input-accumulation model) — it's a plain public function on the
		// component, not AI pathfinding; no AIController/NavMesh/PathFollowingComponent is involved.
		CharMove->RequestDirectMove(TravelTangent.GetSafeNormal() * VelocityMagnitude, false);
	}
	else
	{
		Owner->SetActorLocationAndRotation(NewLocation, NewRotation, false, nullptr, ETeleportType::TeleportPhysics);
	}
}

float USplineFollowerComponent::ComputeReverseSpeedFactor(float CurrentDist, float CurrentSpeed) const
{
	// Approach window is a TIME budget converted to a distance at the current real-world rate of travel
	// (CurrentSpeed * TimeScale, matching how TickComponent advances DistanceAlongSpline via ScaledDelta),
	// so the slowdown takes ReverseApproachSeconds of wall-clock time regardless of vessel speed OR TimeScale.
	const float ApproachDistance = ReverseApproachSeconds * CurrentSpeed * GetEffectiveTimeScale();
	if (ApproachDistance <= KINDA_SMALL_NUMBER || !Route)
	{
		return 1.0f;
	}
	USplineComponent* Spline = Route->GetSpline();
	if (!Spline)
	{
		return 1.0f;
	}
	const int32 NumPoints = Spline->GetNumberOfSplinePoints();

	float MinFactor = 1.0f;
	for (int32 i = 0; i < NumPoints; ++i)
	{
		if (!Route->GetPointReverse(i))
		{
			continue;
		}
		const float BoundaryDist = Spline->GetDistanceAlongSplineAtSplinePoint(i);
		const float AbsDelta = FMath::Abs(BoundaryDist - CurrentDist);
		if (AbsDelta > ApproachDistance)
		{
			continue;
		}
		// T=0 at the boundary (min speed) ramping to T=1 at ApproachDistance away on either side —
		// gives a symmetric slow-down-into / speed-up-out-of curve around the reverse waypoint.
		const float T = AbsDelta / ApproachDistance;
		const float Factor = FMath::Lerp(ReverseMinSpeedFactor, 1.0f, T);
		MinFactor = FMath::Min(MinFactor, Factor);
	}
	return MinFactor;
}

void USplineFollowerComponent::ResolveOceanReference()
{
	CachedOceanComponent.Reset();
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	for (TActorIterator<AWaterBodyOcean> It(World); It; ++It)
	{
		if (UWaterBodyComponent* Component = It->GetWaterBodyComponent())
		{
			CachedOceanComponent = Component;
			UE_LOG(LogSplineRoute, Log,
				TEXT("[SplineRoute] Follower %s found ocean %s — Z will follow wave height instead of the static HeightOffset plane."),
				*GetNameSafe(GetOwner()), *It->GetName());
			return;
		}
	}
	UE_LOG(LogSplineRoute, Verbose,
		TEXT("[SplineRoute] Follower %s found no AWaterBodyOcean in the level — Z stays on the static HeightOffset plane."),
		*GetNameSafe(GetOwner()));
}

bool USplineFollowerComponent::TryGetOceanSurfaceZ(const FVector& InQueryLocation, float& OutSurfaceZ) const
{
	UWaterBodyComponent* Ocean = CachedOceanComponent.Get();
	if (!Ocean)
	{
		return false;
	}
	const TValueOrError<FWaterBodyQueryResult, EWaterBodyQueryError> QueryResult = Ocean->TryQueryWaterInfoClosestToWorldLocation(
		InQueryLocation,
		EWaterBodyQueryFlags::ComputeLocation | EWaterBodyQueryFlags::ComputeNormal | EWaterBodyQueryFlags::IncludeWaves);
	if (!QueryResult.HasValue())
	{
		return false;
	}
	OutSurfaceZ = QueryResult.GetValue().GetWaterSurfaceLocation().Z;
	return true;
}

float USplineFollowerComponent::GetEffectiveTimeScale() const
{
	if (const UWorld* World = GetWorld())
	{
		if (const UGameInstance* GI = World->GetGameInstance())
		{
			if (const USplineRouteTimeSubsystem* TimeSys = GI->GetSubsystem<USplineRouteTimeSubsystem>())
			{
				return TimeSys->GetTimeScale();
			}
		}
	}
	return 1.0f;
}
