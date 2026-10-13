// Copyright DamenTools internal. Spline Route Tool plugin.

#include "SplineFollowerVessel.h"
#include "DolphinCompanion.h"
#include "SplineFollowerComponent.h"
#include "SplineRouteActor.h"
#include "SplineRouteTool.h"

#include "Camera/CameraComponent.h"
#include "Components/ArrowComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
	/**
	 * If the mesh has a socket named zPosition / ZPosition / WaterLine, return the HeightOffset that places
	 * that socket exactly at the spline plane (water surface). Convention: the socket marks the point on
	 * the model that should sit on the water, so HeightOffset = -socket.RelativeLocation.Z.
	 * Otherwise return Fallback (the catalog's per-vessel HeightOffset).
	 */
	float ResolveHeightOffsetFromSocket(UStaticMesh* Mesh, float Fallback, FString& OutSource)
	{
		if (!Mesh)
		{
			OutSource = TEXT("no-mesh-fallback");
			return Fallback;
		}
		static const FName Candidates[] = { TEXT("zPosition"), TEXT("ZPosition"), TEXT("WaterLine") };
		for (const FName& Name : Candidates)
		{
			if (UStaticMeshSocket* Socket = Mesh->FindSocket(Name))
			{
				const float SocketZ = Socket->RelativeLocation.Z;
				OutSource = FString::Printf(TEXT("socket '%s' (localZ=%.1f)"), *Name.ToString(), SocketZ);
				return -SocketZ;
			}
		}
		OutSource = TEXT("catalog-fallback");
		return Fallback;
	}
}

ASplineFollowerVessel::ASplineFollowerVessel()
{
	PrimaryActorTick.bCanEverTick = false;

	// Required by Waterline's BP_Waterline_Ocean_Gen_3 Buoyancy Box overlap gate
	// (OtherActor.HasTag("Boat")) — confirmed via direct Blueprint graph read, STATE.md 2026-08-17.
	// Purely a registration tag; does not affect this actor's own collision/physics behavior.
	Tags.Add(FName("Boat"));

	USceneComponent* SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	MeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	MeshComponent->SetupAttachment(SceneRoot);
	// OverlapAllDynamic (not NoCollision) so this mesh actually overlaps BP_Waterline_Ocean_Gen_3's
	// Buoyancy Box and registers for the wake/foam grid — confirmed compatible with the kinematic
	// spline-follower motion in the 2026-08-15 Pass 2 PIE test (no jitter/fighting observed).
	MeshComponent->SetCollisionProfileName(TEXT("OverlapAllDynamic"));
	// The Buoyancy Volume's own hand-authored collision profile Blocks every standard channel
	// (WorldStatic/WorldDynamic/Pawn/etc.) and only Overlaps its custom GameTraceChannel1 — the same
	// channel Waterline's own Floor/Wall geometry uses. OverlapAllDynamic alone leaves this mesh as
	// WorldDynamic, which the volume Blocks regardless of this mesh's own per-channel responses.
	// Confirmed via direct live collision-response read on both sides, STATE.md 2026-08-17.
	MeshComponent->SetCollisionObjectType(ECollisionChannel::ECC_GameTraceChannel1);
	MeshComponent->SetMobility(EComponentMobility::Movable);
	// Kept from the earlier (superseded) collision-channel investigation — harmless, and still the
	// setting that lets this mesh register with the separate Buoyancy Volume/tag force-solver path.
	MeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);

	Follower = CreateDefaultSubobject<USplineFollowerComponent>(TEXT("Follower"));

	// Plain AActor (not APawn) defaults this false — without it, SetViewTargetWithBlend(this, ...) would
	// blend the view to this actor but CalcCamera would never look at any of the 5 UCameraComponents
	// below, silently producing a broken/default view. Confirmed via AActor::CalcCamera's default impl.
	bFindCameraComponentWhenViewTarget = true;

	static const TCHAR* SlotLabels[] = { TEXT("Slot 1"), TEXT("Slot 2"), TEXT("Slot 3"), TEXT("Slot 4"), TEXT("Slot 5") };
	constexpr int32 NumCameraSlots = UE_ARRAY_COUNT(SlotLabels);
	CameraArrows.Reserve(NumCameraSlots);
	Cameras.Reserve(NumCameraSlots);
	CameraSlotNames.Reserve(NumCameraSlots);
	for (int32 Index = 0; Index < NumCameraSlots; ++Index)
	{
		const FString ArrowName = FString::Printf(TEXT("CameraArrow_Slot%d"), Index + 1);
		const FString CameraName = FString::Printf(TEXT("VesselCamera_Slot%d"), Index + 1);

		UArrowComponent* Arrow = CreateDefaultSubobject<UArrowComponent>(FName(*ArrowName));
		// Attached to MeshComponent, NOT SceneRoot — so arrow positions scale proportionally with
		// SetSizeMultiplier()'s MeshComponent->SetRelativeScale3D() calls (e.g. bow stays at the bow
		// whether the ship is scaled up or down), instead of staying fixed in actor-root space while
		// the mesh grows/shrinks around them.
		Arrow->SetupAttachment(MeshComponent);
		Arrow->SetHiddenInGame(true);
		CameraArrows.Add(Arrow);

		UCameraComponent* Camera = CreateDefaultSubobject<UCameraComponent>(FName(*CameraName));
		Camera->SetupAttachment(Arrow);
		Camera->SetActive(false);
		Cameras.Add(Camera);

		CameraSlotNames.Add(FText::FromString(SlotLabels[Index]));
	}
}

void ASplineFollowerVessel::Configure(UStaticMesh* InMesh, FVector InRelativeScale, ASplineRouteActor* InRoute, float InSpeed, float InHeightOffset, float InTurnRate, FRotator InRotationOffset, float InReverseApproachSeconds, float InReverseMinSpeedFactor)
{
	BaseScale = InRelativeScale;
	VesselRotationOffset = InRotationOffset;
	// InMesh is optional now — catalog vessels bake their mesh into the Blueprint's MeshComponent
	// default, so only override it here if a caller explicitly passes a non-null mesh. Socket
	// resolution below still needs to see whichever mesh is actually in play.
	UStaticMesh* EffectiveMesh = InMesh;
	if (MeshComponent)
	{
		if (InMesh)
		{
			MeshComponent->SetStaticMesh(InMesh);
		}
		else
		{
			EffectiveMesh = MeshComponent->GetStaticMesh();
		}
		MeshComponent->SetRelativeScale3D(BaseScale);
		MeshComponent->SetRelativeRotation(VesselRotationOffset);
	}
	FString OffsetSource;
	const float ResolvedHeight = ResolveHeightOffsetFromSocket(EffectiveMesh, InHeightOffset, OffsetSource);
	BaseHeightOffset = ResolvedHeight;

	if (Follower)
	{
		Follower->Speed = InSpeed;
		Follower->TurnRateDegPerSec = InTurnRate;
		Follower->HeightOffset = BaseHeightOffset;
		Follower->ReverseApproachSeconds = InReverseApproachSeconds;
		Follower->ReverseMinSpeedFactor = InReverseMinSpeedFactor;
		// Assign Route directly (not SetRoute) — when called pre-BeginPlay, the follower will pick this up during its own BeginPlay snap logic.
		// Post-BeginPlay callers should use SetRoute() instead, which also rebinds delegates.
		Follower->Route = InRoute;
		Follower->SpawnOrder = 0; // will be set by subsystem after spawn
	}
	UE_LOG(LogSplineRoute, Log,
		TEXT("[SplineRoute] Vessel %s configured: mesh=%s route=%s speed=%.1f heightOffset=%.1f (source=%s)"),
		*GetName(), *GetNameSafe(EffectiveMesh), *GetNameSafe(InRoute), InSpeed, ResolvedHeight, *OffsetSource);
}

void ASplineFollowerVessel::SetDolphinCompanionSettings(const FDolphinCompanionSettings& InSettings)
{
	DolphinCompanion = InSettings;
}

void ASplineFollowerVessel::ActivateCameraSlot(int32 SlotIndex)
{
	for (int32 Index = 0; Index < Cameras.Num(); ++Index)
	{
		if (UCameraComponent* Camera = Cameras[Index])
		{
			Camera->SetActive(Index == SlotIndex);
		}
	}
}

FText ASplineFollowerVessel::GetCameraSlotDisplayName(EVesselCameraSlot Slot) const
{
	const int32 Index = static_cast<int32>(Slot);
	return CameraSlotNames.IsValidIndex(Index) ? CameraSlotNames[Index] : FText::GetEmpty();
}

void ASplineFollowerVessel::BeginPlay()
{
	Super::BeginPlay();
	SpawnDolphinCompanion();
	SpawnWakeCompanion();
}

void ASplineFollowerVessel::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Both companions are attached, not owned by the level, so tear them down explicitly — otherwise
	// they would outlive a vessel removed by ClearScenario and linger as orphans.
	if (CompanionActor)
	{
		CompanionActor->Destroy();
		CompanionActor = nullptr;
	}
	if (WakeCompanionActor)
	{
		WakeCompanionActor->Destroy();
		WakeCompanionActor = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}

void ASplineFollowerVessel::SpawnDolphinCompanion()
{
	if (!DolphinCompanion.bSpawnCompanion || CompanionActor)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	UClass* CompanionClass = DolphinCompanion.CompanionClass.LoadSynchronous();
	if (!CompanionClass)
	{
		UE_LOG(LogSplineRoute, Warning,
			TEXT("[SplineRoute] Vessel %s requests a dolphin companion but CompanionClass is unset or failed to load (%s)."),
			*GetName(), *DolphinCompanion.CompanionClass.ToString());
		return;
	}

	// Spawn already at the final world pose so attaching with KeepWorldTransform below yields exactly
	// RelativeOffset — no post-attach correction, and no one-frame pop at the route start.
	const float Scale = FMath::Max(0.01f, DolphinCompanion.Scale);
	const FTransform SpawnTM(
		GetActorRotation(),
		GetActorTransform().TransformPosition(DolphinCompanion.RelativeOffset),
		FVector(Scale));

	ADolphinCompanion* Companion = World->SpawnActorDeferred<ADolphinCompanion>(
		CompanionClass, SpawnTM, this, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Companion)
	{
		return;
	}

	Companion->Configure(DolphinCompanion.RelativeOffset, Scale, DolphinCompanion.BobAmplitude, DolphinCompanion.BobSpeed, DolphinCompanion.MeshRotationOffset);
	Companion->FinishSpawning(SpawnTM);

	// Attach to the scene root, NOT MeshComponent — the mesh carries the vessel's size multiplier and any
	// DCC corrective rotation (see constructor), neither of which should distort or tilt the dolphin.
	Companion->AttachToComponent(GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
	CompanionActor = Companion;

	UE_LOG(LogSplineRoute, Log,
		TEXT("[SplineRoute] Vessel %s spawned dolphin companion %s (class=%s offset=%s scale=%.2f bob=%.1fcm@%.2fHz meshRot=%s)"),
		*GetName(), *Companion->GetName(), *CompanionClass->GetName(),
		*DolphinCompanion.RelativeOffset.ToString(), Scale,
		DolphinCompanion.BobAmplitude, DolphinCompanion.BobSpeed, *DolphinCompanion.MeshRotationOffset.ToString());
}

void ASplineFollowerVessel::SpawnWakeCompanion()
{
	if (WakeCompanionActor)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Soft reference to the confirmed-working BP_Ship_Cargo asset — spawning the exact proven
	// Blueprint as the wake companion, not a hand-rebuilt native approximation (that was tried as
	// AWakeCompanion and never showed wake, even with every property matched by hand). Per Gevor's
	// explicit direction, STATE.md 2026-08-17. Path confirmed via ue5_get_asset — it lives under
	// /Game/Ship, not /Game/SplineRouteTool/Blueprints.
	static const TSoftClassPtr<ACharacter> WakeCompanionClass(
		FSoftObjectPath(TEXT("/Game/Ship/BP_Ship_Cargo.BP_Ship_Cargo_C")));
	UClass* CompanionClass = WakeCompanionClass.LoadSynchronous();
	if (!CompanionClass)
	{
		UE_LOG(LogSplineRoute, Warning,
			TEXT("[SplineRoute] Vessel %s: wake companion class BP_Ship_Cargo failed to load (%s)."),
			*GetName(), *WakeCompanionClass.ToString());
		return;
	}

	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ACharacter* Companion = World->SpawnActor<ACharacter>(CompanionClass, GetActorTransform(), Params);
	if (!Companion)
	{
		return;
	}

	// NOT SetActorHiddenInGame() — that sets bHiddenInGame on every primitive, and
	// FPrimitiveSceneProxy::IsShown() checks bHiddenInGame (via IsForceHidden()) unconditionally,
	// before it even looks at which view it's rendering for. Waterline's own SceneCaptureComponent2D
	// runs in PrimitiveRenderMode=PRM_UseShowOnlyList (confirmed by reading its live component
	// defaults) — being on that show-only list does NOT override a force-hidden primitive, so hiding
	// the companion this way made it invisible everywhere INCLUDING the capture that drives wake/foam,
	// which is exactly why wake disappeared entirely once Fix 1 shipped. Confirmed against UE 5.7
	// engine source (PrimitiveSceneProxy.cpp): bVisibleInSceneCaptureOnly is the flag purpose-built for
	// this split — hidden from normal player views, still rendered into scene captures. Iterate every
	// primitive component (not just the known mesh types) since BP_Ship_Cargo carries multiple mesh
	// components (mannequin + ship mesh) and a single FindComponentByClass<T>() approach previously
	// missed one. Purely a rendering flag — does not touch collision/overlap, same as the old approach.
	TArray<UPrimitiveComponent*> WakeCompanionPrimitives;
	Companion->GetComponents<UPrimitiveComponent>(WakeCompanionPrimitives);
	for (UPrimitiveComponent* Prim : WakeCompanionPrimitives)
	{
		if (Prim)
		{
			Prim->SetVisibleInSceneCaptureOnly(true);
		}
	}

	// CharacterMovementComponent's tick is deliberately left running (confirmed necessary for wake,
	// nineteenth/twentieth entries) — but that means it's free to independently orient the capsule
	// based on its own movement logic, fighting whatever rotation the attachment below would
	// otherwise inherit from the vessel. Disable only the rotation-from-movement behavior, not the
	// tick itself.
	if (UCharacterMovementComponent* CompMove = Companion->GetCharacterMovement())
	{
		CompMove->bOrientRotationToMovement = false;
		CompMove->bUseControllerDesiredRotation = false;
	}

	// SnapToTarget resets the companion's relative transform to identity under the parent, so it
	// rides along exactly co-located with the vessel root regardless of the spawn transform above.
	Companion->AttachToComponent(GetRootComponent(), FAttachmentTransformRules::SnapToTargetNotIncludingScale);
	WakeCompanionActor = Companion;

	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] Vessel %s spawned wake companion %s (class=%s)"),
		*GetName(), *Companion->GetName(), *CompanionClass->GetName());
}

void ASplineFollowerVessel::SetSizeMultiplier(float Multiplier)
{
	if (!MeshComponent)
	{
		return;
	}
	const float Clamped = FMath::Max(0.01f, Multiplier);
	CurrentSizeMultiplier = Clamped;
	const FVector NewScale = BaseScale * Clamped;
	MeshComponent->SetRelativeScale3D(NewScale);
	// Scale the height offset by the same factor so the water-line socket stays at the spline plane
	// instead of sinking under (multiplier > 1) or floating above (multiplier < 1) the water.
	const float NewHeightOffset = BaseHeightOffset * Clamped;
	if (Follower)
	{
		Follower->HeightOffset = NewHeightOffset;
	}
	UE_LOG(LogSplineRoute, Log,
		TEXT("[SplineRoute] Vessel %s SetSizeMultiplier=%.3f -> world scale=%s heightOffset=%.1f (base=%.1f)"),
		*GetName(), Clamped, *NewScale.ToString(), NewHeightOffset, BaseHeightOffset);
}

float ASplineFollowerVessel::GetMeshLength() const
{
	if (MeshComponent && MeshComponent->GetStaticMesh())
	{
		// MeshComponent carries its own scale independent of the actor root (see constructor comment),
		// so GetComponentScale() — not GetActorScale3D() — reflects what SetSizeMultiplier actually changed.
		return MeshComponent->GetStaticMesh()->GetBounds().BoxExtent.GetMax() * 2.0f * MeshComponent->GetComponentScale().GetMax();
	}
	return 1000.0f;
}
