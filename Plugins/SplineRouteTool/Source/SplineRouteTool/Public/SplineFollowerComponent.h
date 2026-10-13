// Copyright DamenTools internal. Spline Route Tool plugin.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SplineFollowerComponent.generated.h"

class ASplineRouteActor;
class UWaterBodyComponent;

/** What the follower does after reaching the end of the spline. */
UENUM(BlueprintType)
enum class ESplineFollowerEndBehavior : uint8
{
	/** Snap back to the start and continue. */
	Loop UMETA(DisplayName = "Loop"),
	/** Stop at the end of the spline. */
	Clamp UMETA(DisplayName = "Clamp"),
	/** Reverse direction at each end. */
	PingPong UMETA(DisplayName = "PingPong")
};

/**
 * Drives the owning actor along a USplineRouteActor at a configurable speed,
 * scaled by USplineRouteTimeSubsystem::GetTimeScale().
 *
 * Generic — works on any AActor.
 */
UCLASS(ClassGroup = (SplineRoute), meta = (BlueprintSpawnableComponent))
class SPLINEROUTETOOL_API USplineFollowerComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USplineFollowerComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Route to follow. Can be set in the editor or at runtime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Follower")
	TObjectPtr<ASplineRouteActor> Route;

	/** Cruise speed in units per second along the spline. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Follower", meta = (ClampMin = "0.0"))
	float Speed = 200.0f;

	/** Seconds to wait before motion starts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Follower", meta = (ClampMin = "0.0"))
	float StartDelay = 0.0f;

	/**
	 * Starting distance along the spline (units).
	 * Negative values stagger the spawn: the follower is anchored to the spline START while distance counts up to 0,
	 * which produces a smooth time-staggered launch without the StartDelay-induced teleport.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Follower")
	float InitialDistance = 0.0f;

	/** Behaviour when the follower reaches the end of the spline. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Follower")
	ESplineFollowerEndBehavior EndBehavior = ESplineFollowerEndBehavior::Loop;

	/** If true, the owner is rotated to align with the spline tangent. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Follower")
	bool bAlignToTangent = true;

	/** If true (and bAlignToTangent), only yaw is taken from the tangent — pitch/roll stay at zero. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Follower")
	bool bYawOnly = true;

	/**
	 * Max turn rate in degrees/second used to ease the vessel toward the spline tangent instead of
	 * snapping. 0 = instant snap (old behaviour). Lower values make large vessels swing wide on sharp
	 * corners. Scaled by the global time slider so fast-forward keeps the motion consistent.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Follower", meta = (ClampMin = "0.0"))
	float TurnRateDegPerSec = 90.0f;

	/** Apply Z offset relative to the spline (e.g. lift a cube so it sits on top of the spline plane). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Follower")
	float HeightOffset = 0.0f;

	/**
	 * If true (default false — existing kinematic vessels are unaffected), drive the owning
	 * ACharacter's CharacterMovementComponent::Velocity from the spline tangent and computed speed
	 * each tick instead of teleporting via SetActorLocationAndRotation. Per Gevor's explicit
	 * instruction: real physics-integrated movement is required for the wake effect, not a teleport.
	 * Only meaningful on an ACharacter owner — no-ops (falls back to the kinematic path) otherwise.
	 * Forces the CharacterMovementComponent into MOVE_Flying while active so gravity doesn't fight
	 * the tangent-driven Velocity (there's no floor under an ocean route). DistanceAlongSpline is
	 * re-derived every tick from the actor's actual physics-simulated position (rather than assumed
	 * from the last commanded delta), since physics — not this component — now has final authority
	 * over where the actor actually ends up.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Follower")
	bool bDriveViaCharacterMovement = false;

	/** Seconds before/after a reverse waypoint over which speed ramps down/up. Time-based (not distance-based)
	 *  so the slowdown takes the same wall-clock duration regardless of vessel speed or TimeScale. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Follower", meta = (ClampMin = "0.0"))
	float ReverseApproachSeconds = 3.0f;

	/** Speed multiplier applied exactly at a reverse waypoint (1.0 = no slowdown, 0.1 = 10% speed). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Follower", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ReverseMinSpeedFactor = 0.1f;

	/** The follower directly ahead on the same route, if any. This one slows down to avoid closing the gap below MinFollowDistanceMeters. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Follower")
	TWeakObjectPtr<USplineFollowerComponent> LeaderFollower;

	/** Minimum spline-distance gap to LeaderFollower before this follower starts slowing down proportionally. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Follower", meta = (ClampMin = "0.0"))
	float MinFollowDistanceMeters = 3000.0f;

	/** Queue position among vessels on the same route, set by the subsystem after spawn. Used on Loop wrap to space this vessel behind start proportionally to its place in the convoy. */
	int32 SpawnOrder = 0;

	/** Assign / replace the route at runtime. Resets DistanceAlongSpline. */
	UFUNCTION(BlueprintCallable, Category = "Spline Follower")
	void SetRoute(ASplineRouteActor* NewRoute);

	/**
	 * Configure this follower for immediate use on ANY actor — sets the motion parameters and
	 * assigns the route via SetRoute(), which is safe to call whether the owner's BeginPlay has
	 * already run or not (unlike a raw Route= assignment, it also (re)binds the route's geometry-
	 * changed delegate). Deliberately excludes mesh/scale/rotation/dolphin-companion concerns —
	 * those are specific to ASplineFollowerVessel's own spawn flow (see its Configure()) and don't
	 * apply to a pre-built Blueprint that already bakes its own mesh (e.g. BP_Ship_Cargo_2/3/4/6).
	 * HeightOffset is intentionally not a parameter here either — for those Blueprints it's a
	 * per-instance value tuned once in the Blueprint's own Details panel, not something the catalog
	 * pushes at spawn time.
	 */
	UFUNCTION(BlueprintCallable, Category = "Spline Follower")
	void ConfigureFollower(ASplineRouteActor* InRoute, float InSpeed, float InTurnRate, float InReverseApproachSeconds, float InReverseMinSpeedFactor);

	/** Current distance travelled along the spline (read-only). */
	UFUNCTION(BlueprintCallable, Category = "Spline Follower")
	float GetDistanceAlongSpline() const { return DistanceAlongSpline; }

	/** Current speed the vessel is actually advancing at, in units/second — after segment-speed overrides,
	 *  reverse-waypoint slowdown ramping, and TimeScale. Zero during StartDelay, when paused (TimeScale
	 *  0), or when no route/spline is set. For driving vessel-speed-reactive VFX (e.g. wake intensity). */
	UFUNCTION(BlueprintCallable, Category = "Spline Follower")
	float GetCurrentEffectiveSpeed() const { return CurrentEffectiveSpeed; }

	/** Instantly offset DistanceAlongSpline by Delta and snap the owner to the resulting position. For
	 *  external systems (e.g. a size-change overlap correction) that need to resolve overlap immediately
	 *  instead of waiting for TickComponent's gradual push-apart. */
	UFUNCTION(BlueprintCallable, Category = "Spline Follower")
	void NudgeDistanceAlongSpline(float Delta);

	/** Snap DistanceAlongSpline to whatever point on the spline is closest to the owner's current world location. */
	UFUNCTION(BlueprintCallable, Category = "Spline Follower")
	void ReanchorToOwnerLocation();

	/** Rewind to the spline start (InitialDistance) and re-anchor. Used by the "Stop" orchestrator button. */
	UFUNCTION(BlueprintCallable, Category = "Spline Follower")
	void ResetToStart();

	/** Whether this follower has begun normal motion (DistanceAlongSpline has crossed 0). Gates the
	 *  negative-end-of-spline branch so a negative InitialDistance can stage a spawn without teleporting. */
	bool bHasReachedStart = false;

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	float DistanceAlongSpline = 0.0f;
	float ElapsedDelay = 0.0f;
	int32 Direction = 1;

	/** Backing value for GetCurrentEffectiveSpeed(), updated once per tick. */
	float CurrentEffectiveSpeed = 0.0f;

	/** First AWaterBodyOcean's water component found in the world at BeginPlay, if any. Null when no
	 *  ocean is present in the level — Z math then falls back to the static HeightOffset plane unchanged. */
	TWeakObjectPtr<UWaterBodyComponent> CachedOceanComponent;

	/** Resolves CachedOceanComponent once at BeginPlay by searching the world for the first AWaterBodyOcean —
	 *  generic across levels rather than hardcoding an actor name (e.g. "Ocean_Main"). */
	void ResolveOceanReference();

	/** Queries wave-perturbed water surface height at InQueryLocation's XY and writes it to OutSurfaceZ.
	 *  Returns false (OutSurfaceZ left untouched) when no ocean was found or the query fails, so every
	 *  call site can fall back to today's flat HeightOffset-above-the-spline-plane behaviour unchanged. */
	bool TryGetOceanSurfaceZ(const FVector& InQueryLocation, float& OutSurfaceZ) const;

	float GetEffectiveTimeScale() const;

	/** Places the owner at DistanceAlongSpline on Route right now (world-space location + tangent
	 *  rotation), instead of waiting for TickComponent's next pass. Called from BeginPlay() (Route
	 *  already assigned) and from the end of ConfigureFollower() (covers the case where Route is
	 *  assigned AFTER BeginPlay already ran — e.g. a Blueprint-added component that only exists once
	 *  FinishSpawning() has run its construction script, so Configure has to happen post-spawn). Without
	 *  this second call site, that owner is left sitting wherever it was spawned (near the route actor's
	 *  own root, which is always world origin) until the next tick nudges it — and with
	 *  bDriveViaCharacterMovement, TickComponent re-derives distance from the owner's CURRENT position
	 *  instead of teleporting it, so it never self-corrects at all. No-ops if Route isn't set. */
	void SnapToRouteStart();

	/** Speed multiplier in [ReverseMinSpeedFactor, 1.0], dipping to the minimum at the nearest reverse
	 *  waypoint within (ReverseApproachSeconds * CurrentSpeed) and ramping back to 1.0 on either side of it. */
	float ComputeReverseSpeedFactor(float CurrentDist, float CurrentSpeed) const;

	UFUNCTION()
	void HandleSplineGeometryChanged();

	void BindRouteDelegate(ASplineRouteActor* RouteToBind);
	void UnbindRouteDelegate(ASplineRouteActor* RouteToUnbind);
};
