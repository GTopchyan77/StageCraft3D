// Copyright DamenTools internal. Spline Route Tool plugin.

#include "SplineRouteActor.h"
#include "SplineRouteTool.h"

#include "Components/SplineComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"
#include "TimerManager.h"

namespace SplineRouteActorColors
{
	/** WorldGridMaterial exposes "Color"; most authored materials use "BaseColor". Set both — a miss is a no-op. */
	static const FName ColorParam(TEXT("Color"));
	static const FName BaseColorParam(TEXT("BaseColor"));

	/** Hard ceiling on arrow components so a tiny ArrowSpacing on a long route cannot flood the level. */
	static constexpr int32 MaxDirectionArrows = 512;

	/** Engine BasicShapes primitives are 100 units on their driving axis; component scale is world size / 100. */
	static constexpr float BasicShapeBaseline = 100.0f;

	/** Uniform scale multiplier bumped onto the currently-selected handle so it reads larger than the rest. */
	static constexpr float SelectedHandleScaleBump = 1.4f;

	/** One step of the A -> Selected chase-light reveal: which array + index switches to its end colour next. */
	struct FChaseRevealStep
	{
		bool bIsArrow = false;
		int32 Index = INDEX_NONE;

		/**
		 * Control-point index this step belongs to: a segment step's own index, or the segment its
		 * arrow sits on. Segment mesh + all its arrows share a GroupIndex, letting ApplySegmentGradient
		 * detect "this whole group is now fully applied" purely by GroupIndex transitions, regardless of
		 * whether the group's mesh or one of its arrows happens to be walked first in a given direction.
		 */
		int32 GroupIndex = INDEX_NONE;
	};
}

ASplineRouteActor::ASplineRouteActor()
{
	PrimaryActorTick.bCanEverTick = false;

	Spline = CreateDefaultSubobject<USplineComponent>(TEXT("Spline"));
	SetRootComponent(Spline);
	Spline->ClearSplinePoints(false);
	Spline->SetMobility(EComponentMobility::Movable);
	Spline->SetUnselectedSplineSegmentColor(FLinearColor(0.0f, 0.6f, 1.0f));
	Spline->SetSelectedSplineSegmentColor(FLinearColor(1.0f, 0.6f, 0.0f));
	// Hide the native spline curve/handle debug draw — SegmentMeshes/HandleMeshes/DirectionArrows are
	// the actual visuals and stay unaffected, this only silences USplineComponent's own debug proxy.
	Spline->SetDrawDebug(false);

	// Default to engine cube for handles so the actor is demoable without extra assets.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> DefaultMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (DefaultMesh.Succeeded())
	{
		HandleMesh = DefaultMesh.Object;
	}

	// Default segment mesh to the engine cylinder (100u tall, Z up). We drive it with SetForwardAxis(Z).
	static ConstructorHelpers::FObjectFinder<UStaticMesh> DefaultSegmentMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (DefaultSegmentMesh.Succeeded())
	{
		SegmentMesh = DefaultSegmentMesh.Object;
	}

	// Engine cone is Z-up with its apex at +Z, so aligning local Z to the spline tangent points it downstream.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> DefaultArrowMesh(TEXT("/Engine/BasicShapes/Cone.Cone"));
	if (DefaultArrowMesh.Succeeded())
	{
		ArrowMesh = DefaultArrowMesh.Object;
	}
}

void ASplineRouteActor::BeginPlay()
{
	Super::BeginPlay();

	EnsureFallbackMaterials();

	UE_LOG(LogSplineRoute, Log,
		TEXT("[SplineRoute] %s BeginPlay. HandleMat=%s SelectedMat=%s SegmentMat=%s (fallbacks created when null)."),
		*GetName(),
		*GetNameSafe(HandleMaterial),
		*GetNameSafe(SelectedHandleMaterial),
		*GetNameSafe(SegmentMaterial));

	// Author-time placed routes can pre-populate handles in BeginPlay; runtime routes start empty.
	RebuildHandleMeshes();
	RebuildSegmentMeshes();
}

void ASplineRouteActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ChaseRevealTimerHandle);
	}
	PendingChaseGroupIndex = INDEX_NONE;
	Super::EndPlay(EndPlayReason);
}

int32 ASplineRouteActor::AddControlPoint(FVector WorldLocation)
{
	if (!Spline)
	{
		return INDEX_NONE;
	}

	const int32 NewIndex = Spline->GetNumberOfSplinePoints();
	Spline->AddSplinePoint(WorldLocation, ESplineCoordinateSpace::World, /*bUpdateSpline=*/true);
	Spline->SetSplinePointType(NewIndex, ESplinePointType::Curve, /*bUpdateSpline=*/true);

	UStaticMeshComponent* Handle = CreateHandleAt(NewIndex, WorldLocation);
	if (Handle)
	{
		HandleMeshes.Add(Handle);
	}
	ApplyHandleMaterial(NewIndex, /*bSelected=*/(NewIndex == SelectedIndex));

	RebuildSegmentMeshes();
	ResizePerPointArrays();

	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] %s AddControlPoint idx=%d loc=%s. Total=%d Segments=%d"),
		*GetName(), NewIndex, *WorldLocation.ToString(),
		Spline->GetNumberOfSplinePoints(), SegmentMeshes.Num());
	OnSplineGeometryChanged.Broadcast();
	return NewIndex;
}

void ASplineRouteActor::MoveControlPoint(int32 PointIndex, FVector NewWorldLocation)
{
	if (!Spline || PointIndex < 0 || PointIndex >= Spline->GetNumberOfSplinePoints())
	{
		return;
	}

	Spline->SetLocationAtSplinePoint(PointIndex, NewWorldLocation, ESplineCoordinateSpace::World, /*bUpdateSpline=*/true);

	if (HandleMeshes.IsValidIndex(PointIndex) && HandleMeshes[PointIndex])
	{
		const float SelectionScale = (PointIndex == SelectedIndex) ? SplineRouteActorColors::SelectedHandleScaleBump : 1.0f;
		const float AppliedScale = HandleSize * SelectionScale / SplineRouteActorColors::BasicShapeBaseline;
		HandleMeshes[PointIndex]->SetWorldLocation(GetBiasedHandleLocation(NewWorldLocation, AppliedScale));
	}

	RebuildSegmentMeshes();
	OnSplineGeometryChanged.Broadcast();
}

void ASplineRouteActor::RemoveControlPoint(int32 PointIndex)
{
	if (!Spline || PointIndex < 0 || PointIndex >= Spline->GetNumberOfSplinePoints())
	{
		UE_LOG(LogSplineRoute, Warning, TEXT("[SplineRoute] RemoveControlPoint: invalid idx=%d (have %d points)"),
			PointIndex, Spline ? Spline->GetNumberOfSplinePoints() : 0);
		return;
	}

	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] %s RemoveControlPoint idx=%d"), *GetName(), PointIndex);
	Spline->RemoveSplinePoint(PointIndex, /*bUpdateSpline=*/true);

	if (HandleMeshes.IsValidIndex(PointIndex))
	{
		if (UStaticMeshComponent* Doomed = HandleMeshes[PointIndex])
		{
			Doomed->DestroyComponent();
		}
		HandleMeshes.RemoveAt(PointIndex);
	}

	const int32 OldSelection = SelectedIndex;
	if (SelectedIndex == PointIndex)
	{
		SelectedIndex = INDEX_NONE;
	}
	else if (SelectedIndex > PointIndex)
	{
		SelectedIndex--;
	}

	// Drop the per-point speed override + reverse flag for the point that just disappeared.
	if (PointSpeedOverrides.IsValidIndex(PointIndex))
	{
		PointSpeedOverrides.RemoveAt(PointIndex);
	}
	if (PointReverseFlags.IsValidIndex(PointIndex))
	{
		PointReverseFlags.RemoveAt(PointIndex);
	}
	RebuildSegmentMeshes();
	ResizePerPointArrays();
	OnSplineGeometryChanged.Broadcast();
	if (SelectedIndex != OldSelection)
	{
		OnSelectedControlPointChanged.Broadcast(SelectedIndex);
	}
}

void ASplineRouteActor::ClearControlPoints()
{
	if (Spline)
	{
		Spline->ClearSplinePoints(true);
	}
	for (UStaticMeshComponent* Handle : HandleMeshes)
	{
		if (Handle)
		{
			Handle->DestroyComponent();
		}
	}
	HandleMeshes.Reset();
	const int32 OldSelection = SelectedIndex;
	SelectedIndex = INDEX_NONE;
	PointSpeedOverrides.Reset();
	PointReverseFlags.Reset();

	RebuildSegmentMeshes();
	OnSplineGeometryChanged.Broadcast();
	if (OldSelection != INDEX_NONE)
	{
		OnSelectedControlPointChanged.Broadcast(SelectedIndex);
	}
}

int32 ASplineRouteActor::GetNumControlPoints() const
{
	return Spline ? Spline->GetNumberOfSplinePoints() : 0;
}

int32 ASplineRouteActor::FindControlPointForComponent(const UPrimitiveComponent* HitComponent) const
{
	for (int32 i = 0; i < HandleMeshes.Num(); ++i)
	{
		if (HandleMeshes[i] == HitComponent)
		{
			return i;
		}
	}
	return INDEX_NONE;
}

void ASplineRouteActor::SetSelectedControlPoint(int32 PointIndex)
{
	if (PointIndex == SelectedIndex)
	{
		return;
	}

	const int32 OldSelection = SelectedIndex;
	SelectedIndex = (PointIndex >= 0 && PointIndex < HandleMeshes.Num()) ? PointIndex : INDEX_NONE;
	UE_LOG(LogSplineRoute, Verbose, TEXT("[SplineRoute] %s SetSelected %d -> %d"),
		*GetName(), OldSelection, SelectedIndex);

	if (HandleMeshes.IsValidIndex(OldSelection))
	{
		ApplyHandleMaterial(OldSelection, false);
	}
	if (HandleMeshes.IsValidIndex(SelectedIndex))
	{
		ApplyHandleMaterial(SelectedIndex, true);
	}

	// Same update path as the handle tint above, so the segment gradient can never fall out of sync
	// with the selection — every selection route (click, Prev/Next cycle, delete) lands here.
	ApplySegmentGradient();

	OnSelectedControlPointChanged.Broadcast(SelectedIndex);
}

int32 ASplineRouteActor::GetNumSegments() const
{
	if (!Spline)
	{
		return 0;
	}
	const int32 NumPoints = Spline->GetNumberOfSplinePoints();
	if (NumPoints < 2)
	{
		return 0;
	}
	return Spline->IsClosedLoop() ? NumPoints : (NumPoints - 1);
}

void ASplineRouteActor::ResizePerPointArrays()
{
	const int32 Desired = GetNumControlPoints();

	if (PointSpeedOverrides.Num() > Desired)
	{
		PointSpeedOverrides.SetNum(Desired);
	}
	while (PointSpeedOverrides.Num() < Desired)
	{
		PointSpeedOverrides.Add(-1.0f);
	}

	if (PointReverseFlags.Num() > Desired)
	{
		PointReverseFlags.SetNum(Desired);
	}
	while (PointReverseFlags.Num() < Desired)
	{
		PointReverseFlags.Add(false);
	}
}

void ASplineRouteActor::SetPointSpeedOverride(int32 PointIndex, float NewSpeed)
{
	ResizePerPointArrays();
	if (!PointSpeedOverrides.IsValidIndex(PointIndex))
	{
		return;
	}
	PointSpeedOverrides[PointIndex] = (NewSpeed < 0.0f) ? -1.0f : NewSpeed;
	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] %s point %d speed override = %.1f"),
		*GetName(), PointIndex, PointSpeedOverrides[PointIndex]);
}

float ASplineRouteActor::GetPointSpeedOverride(int32 PointIndex) const
{
	if (!PointSpeedOverrides.IsValidIndex(PointIndex))
	{
		return -1.0f;
	}
	return PointSpeedOverrides[PointIndex];
}

float ASplineRouteActor::GetSpeedAtDistance(float Distance, float BaseSpeed) const
{
	if (!Spline)
	{
		return BaseSpeed;
	}
	const int32 NumPoints = Spline->GetNumberOfSplinePoints();
	if (NumPoints < 2 || PointSpeedOverrides.Num() == 0)
	{
		return BaseSpeed;
	}

	const float Length = Spline->GetSplineLength();
	const float Clamped = FMath::Clamp(Distance, 0.0f, Length);

	// Effective speed at a control point: its override, or the follower's base speed when unset.
	auto EffectiveAtPoint = [this, BaseSpeed](int32 Idx) -> float
	{
		if (PointSpeedOverrides.IsValidIndex(Idx) && PointSpeedOverrides[Idx] >= 0.0f)
		{
			return PointSpeedOverrides[Idx];
		}
		return BaseSpeed;
	};

	const int32 NumSegments = GetNumSegments();
	for (int32 i = 0; i < NumSegments; ++i)
	{
		const int32 NextIdx = (i + 1) % NumPoints; // closed loop wraps
		const float SegStart = Spline->GetDistanceAlongSplineAtSplinePoint(i);
		const float SegEnd = (i + 1 < NumPoints)
			? Spline->GetDistanceAlongSplineAtSplinePoint(NextIdx)
			: Length;

		if (Clamped >= SegStart && Clamped <= SegEnd)
		{
			const float Denom = FMath::Max(SegEnd - SegStart, KINDA_SMALL_NUMBER);
			const float Frac = FMath::Clamp((Clamped - SegStart) / Denom, 0.0f, 1.0f);
			// Linear blend between the two bounding points => smooth speed ramp, no step changes.
			return FMath::Lerp(EffectiveAtPoint(i), EffectiveAtPoint(NextIdx), Frac);
		}
	}
	return BaseSpeed;
}

void ASplineRouteActor::SetPointReverse(int32 PointIndex, bool bReverse)
{
	ResizePerPointArrays();
	if (!PointReverseFlags.IsValidIndex(PointIndex))
	{
		return;
	}
	PointReverseFlags[PointIndex] = bReverse;
	// Re-tint the handle so reverse nodes read distinctly (selected nodes keep the selection colour).
	ApplyHandleMaterial(PointIndex, PointIndex == SelectedIndex);
	// Direction arrows on the segment leaving this point must flip, so respawn them.
	RebuildDirectionArrows();
	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] %s point %d reverse = %s"),
		*GetName(), PointIndex, bReverse ? TEXT("true") : TEXT("false"));
}

bool ASplineRouteActor::GetPointReverse(int32 PointIndex) const
{
	return PointReverseFlags.IsValidIndex(PointIndex) && PointReverseFlags[PointIndex];
}

bool ASplineRouteActor::IsReverseAtDistance(float Distance) const
{
	if (!Spline)
	{
		return false;
	}
	const int32 NumPoints = Spline->GetNumberOfSplinePoints();
	if (NumPoints < 2 || PointReverseFlags.Num() == 0)
	{
		return false;
	}
	const float Length = Spline->GetSplineLength();
	const float Clamped = FMath::Clamp(Distance, 0.0f, Length);

	const int32 NumSegments = GetNumSegments();
	for (int32 i = 0; i < NumSegments; ++i)
	{
		const int32 NextIdx = (i + 1) % NumPoints;
		const float SegStart = Spline->GetDistanceAlongSplineAtSplinePoint(i);
		const float SegEnd = (i + 1 < NumPoints)
			? Spline->GetDistanceAlongSplineAtSplinePoint(NextIdx)
			: Length;
		if (Clamped >= SegStart && Clamped <= SegEnd)
		{
			// The segment leaving point i is reverse iff point i is flagged.
			return PointReverseFlags.IsValidIndex(i) && PointReverseFlags[i];
		}
	}
	return false;
}

void ASplineRouteActor::SetVehicleType(ERouteVehicleType NewType)
{
	if (VehicleType == NewType)
	{
		return;
	}
	VehicleType = NewType;
	// Re-tint every handle so the medium change is visible immediately (selected/reverse keep their colours).
	for (int32 i = 0; i < HandleMeshes.Num(); ++i)
	{
		ApplyHandleMaterial(i, i == SelectedIndex);
	}
	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] %s VehicleType -> %d"), *GetName(), static_cast<int32>(NewType));
}

FTransform ASplineRouteActor::GetTransformAtDistance(float Distance) const
{
	if (!Spline || Spline->GetNumberOfSplinePoints() == 0)
	{
		return FTransform::Identity;
	}
	return Spline->GetTransformAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World, /*bUseScale=*/false);
}

float ASplineRouteActor::GetTotalLength() const
{
	return Spline ? Spline->GetSplineLength() : 0.0f;
}

void ASplineRouteActor::SetHandleSize(float NewSize)
{
	const float OldSize = HandleSize;
	const float Clamped = FMath::Max(1.0f, NewSize);
	if (FMath::IsNearlyEqual(HandleSize, Clamped))
	{
		UE_LOG(LogSplineRoute, Log,
			TEXT("[SplineRoute] SetHandleSize %s: requested=%.2f clamped=%.2f == current %.2f, no-op (handles=%d)"),
			*GetName(), NewSize, Clamped, HandleSize, HandleMeshes.Num());
		return;
	}
	HandleSize = Clamped;

	int32 Rescaled = 0;
	for (int32 i = 0; i < HandleMeshes.Num(); ++i)
	{
		UStaticMeshComponent* Handle = HandleMeshes[i];
		if (!Handle)
		{
			continue;
		}
		const float SelectionScale = (i == SelectedIndex) ? SplineRouteActorColors::SelectedHandleScaleBump : 1.0f;
		const float AppliedScale = HandleSize * SelectionScale / SplineRouteActorColors::BasicShapeBaseline;
		Handle->SetWorldScale3D(FVector(AppliedScale));
		if (Spline)
		{
			const FVector SplinePointLocation = Spline->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::World);
			Handle->SetWorldLocation(GetBiasedHandleLocation(SplinePointLocation, AppliedScale));
		}
		++Rescaled;
	}

	// Arrows are sized off HandleSize, so they have to follow it.
	RebuildDirectionArrows();

	UE_LOG(LogSplineRoute, Log,
		TEXT("[SplineRoute] SetHandleSize %s: %.2f -> %.2f, rescaled %d/%d handles (selected=%d)"),
		*GetName(), OldSize, HandleSize, Rescaled, HandleMeshes.Num(), SelectedIndex);
}

void ASplineRouteActor::SetShowDirectionArrows(bool bShow)
{
	if (bShowDirectionArrows == bShow)
	{
		return;
	}
	bShowDirectionArrows = bShow;
	RebuildDirectionArrows();
	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] %s direction arrows = %s (%d arrows)"),
		*GetName(), bShow ? TEXT("on") : TEXT("off"), DirectionArrows.Num());
}

void ASplineRouteActor::SetArrowSpacing(float NewSpacing)
{
	const float Clamped = FMath::Max(50.0f, NewSpacing);
	if (FMath::IsNearlyEqual(ArrowSpacing, Clamped))
	{
		return;
	}
	const float OldSpacing = ArrowSpacing;
	ArrowSpacing = Clamped;
	RebuildDirectionArrows();
	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] %s ArrowSpacing %.1f -> %.1f (%d arrows)"),
		*GetName(), OldSpacing, ArrowSpacing, DirectionArrows.Num());
}

void ASplineRouteActor::RebuildHandleMeshes()
{
	for (UStaticMeshComponent* Existing : HandleMeshes)
	{
		if (Existing)
		{
			Existing->DestroyComponent();
		}
	}
	HandleMeshes.Reset();

	if (!Spline)
	{
		return;
	}

	const int32 NumPoints = Spline->GetNumberOfSplinePoints();
	for (int32 i = 0; i < NumPoints; ++i)
	{
		const FVector Loc = Spline->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::World);
		UStaticMeshComponent* Handle = CreateHandleAt(i, Loc);
		if (Handle)
		{
			HandleMeshes.Add(Handle);
		}
	}
}

void ASplineRouteActor::ApplyHandleMaterial(int32 PointIndex, bool bSelected)
{
	if (!HandleMeshes.IsValidIndex(PointIndex) || !HandleMeshes[PointIndex])
	{
		return;
	}

	UMaterialInterface* Material = GetEffectiveHandleMaterialForPoint(PointIndex, bSelected);

	UStaticMeshComponent* Handle = HandleMeshes[PointIndex];
	if (Material)
	{
		const int32 NumSlots = Handle->GetNumMaterials();
		for (int32 Slot = 0; Slot < NumSlots; ++Slot)
		{
			Handle->SetMaterial(Slot, Material);
		}
	}

	// Optional scale bump for the selected point.
	const float Scale = bSelected ? SplineRouteActorColors::SelectedHandleScaleBump : 1.0f;
	const float AppliedScale = HandleSize * Scale / SplineRouteActorColors::BasicShapeBaseline;
	Handle->SetWorldScale3D(FVector(AppliedScale));
	if (Spline)
	{
		const FVector SplinePointLocation = Spline->GetLocationAtSplinePoint(PointIndex, ESplineCoordinateSpace::World);
		Handle->SetWorldLocation(GetBiasedHandleLocation(SplinePointLocation, AppliedScale));
	}
}

FVector ASplineRouteActor::GetBiasedHandleLocation(const FVector& SplinePointWorldLocation, float AppliedWorldScale) const
{
	if (!HandleMesh)
	{
		return SplinePointWorldLocation;
	}

	// Measure HandleMesh's real local bounds instead of assuming a centered 100-unit cube — custom
	// waypoint meshes (e.g. SM_SplineRoute_Waypoint) are neither centered nor 100 units tall, and that
	// mismatch scales linearly with HandleSize, so it's invisible small but very visible at large sizes.
	const FBox LocalBounds = HandleMesh->GetBoundingBox();
	const float BottomOffset = -LocalBounds.Min.Z;
	return SplinePointWorldLocation + FVector(0.0f, 0.0f, BottomOffset * AppliedWorldScale);
}

UStaticMeshComponent* ASplineRouteActor::CreateHandleAt(int32 PointIndex, const FVector& WorldLocation)
{
	if (!HandleMesh)
	{
		return nullptr;
	}

	UStaticMeshComponent* Handle = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
	Handle->SetupAttachment(Spline);
	Handle->SetMobility(EComponentMobility::Movable);
	Handle->SetStaticMesh(HandleMesh);
	Handle->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Handle->SetCollisionObjectType(ECC_WorldDynamic);
	Handle->SetCollisionResponseToAllChannels(ECR_Ignore);
	Handle->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	const float SelectionScale = (PointIndex == SelectedIndex) ? SplineRouteActorColors::SelectedHandleScaleBump : 1.0f;
	const float AppliedScale = HandleSize * SelectionScale / SplineRouteActorColors::BasicShapeBaseline;
	Handle->RegisterComponent();
	Handle->SetWorldLocation(GetBiasedHandleLocation(WorldLocation, AppliedScale));
	Handle->SetWorldScale3D(FVector(AppliedScale));

	if (UMaterialInterface* Mat = GetEffectiveHandleMaterial(/*bSelected=*/false))
	{
		const int32 NumSlots = Handle->GetNumMaterials();
		for (int32 Slot = 0; Slot < NumSlots; ++Slot)
		{
			Handle->SetMaterial(Slot, Mat);
		}
	}

	return Handle;
}

void ASplineRouteActor::RebuildGeometry()
{
	// Also refreshes direction arrows internally.
	RebuildSegmentMeshes();
}

void ASplineRouteActor::RebuildSegmentMeshes()
{
	// Arrows depend only on the spline geometry, and every geometry mutation already funnels through
	// this function — refreshing them here (before the early-outs below) keeps the two visuals on a
	// single update path instead of requiring every caller to remember both.
	RebuildDirectionArrows();

	// Destroy and clear any existing segments.
	for (USplineMeshComponent* Existing : SegmentMeshes)
	{
		if (Existing)
		{
			Existing->DestroyComponent();
		}
	}
	SegmentMeshes.Reset();
	SegmentMIDs.Reset();

	if (!Spline || !SegmentMesh)
	{
		return;
	}

	const int32 NumPoints = Spline->GetNumberOfSplinePoints();
	if (NumPoints < 2)
	{
		return;
	}

	const bool bClosed = Spline->IsClosedLoop();
	const int32 NumSegments = bClosed ? NumPoints : (NumPoints - 1);

	// Late-spawned routes can rebuild before BeginPlay; make sure the fallback MIDs exist so the
	// gradient always has a Color-parameterised parent to instance from.
	EnsureFallbackMaterials();
	UMaterialInterface* SegMat = GetEffectiveSegmentMaterial();

	for (int32 i = 0; i < NumSegments; ++i)
	{
		const int32 StartIdx = i;
		const int32 EndIdx = (i + 1) % NumPoints;

		const FVector StartPos = Spline->GetLocationAtSplinePoint(StartIdx, ESplineCoordinateSpace::World);
		const FVector EndPos   = Spline->GetLocationAtSplinePoint(EndIdx,   ESplineCoordinateSpace::World);
		const FVector StartTan = Spline->GetTangentAtSplinePoint(StartIdx, ESplineCoordinateSpace::World);
		const FVector EndTan   = Spline->GetTangentAtSplinePoint(EndIdx,   ESplineCoordinateSpace::World);

		USplineMeshComponent* SegComp = NewObject<USplineMeshComponent>(this, NAME_None, RF_Transient);
		SegComp->SetupAttachment(Spline);
		SegComp->SetMobility(EComponentMobility::Movable);
		SegComp->SetStaticMesh(SegmentMesh);
		// Engine cylinder is Z-up, 100u tall — drive it along its Z axis.
		SegComp->SetForwardAxis(ESplineMeshAxis::Z, /*bUpdateMesh=*/false);
		SegComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);

		// Engine cylinder is ~50u radius (100u diameter). Divide thickness by 100 so SegmentThickness reads as world units of diameter.
		const float ThicknessScale = SegmentThickness / 100.0f;
		SegComp->SetStartScale(FVector2D(ThicknessScale, ThicknessScale), /*bUpdateMesh=*/false);
		SegComp->SetEndScale(FVector2D(ThicknessScale, ThicknessScale), /*bUpdateMesh=*/false);

		SegComp->SetStartAndEnd(StartPos, StartTan, EndPos, EndTan, /*bUpdateMesh=*/true);

		SegComp->RegisterComponent();
		// Component visibility, not colour — see bShowSegmentMeshes' comment for why alpha=0 doesn't work.
		SegComp->SetVisibility(bShowSegmentMeshes, /*bPropagateToChildren=*/false);

		// One MID per segment: the gradient needs to tint segments independently, and sharing a
		// single material instance across all of them would make that impossible.
		UMaterialInstanceDynamic* SegMID = SegMat ? UMaterialInstanceDynamic::Create(SegMat, this) : nullptr;
		if (SegMID)
		{
			const int32 NumSlots = SegComp->GetNumMaterials();
			for (int32 Slot = 0; Slot < NumSlots; ++Slot)
			{
				SegComp->SetMaterial(Slot, SegMID);
			}
		}

		SegmentMeshes.Add(SegComp);
		SegmentMIDs.Add(SegMID);
	}

	// Freshly created MIDs still carry the parent colour — resync them to the current selection
	// instantly. Not ApplySegmentGradient: these MIDs are brand new, so LastRevealedIndex no longer
	// refers to anything real and an animated chase here would replay on every geometry edit.
	ResyncSegmentGradientInstant();
}

void ASplineRouteActor::ApplySegmentGradient()
{
	UE_LOG(LogSplineRoute, Warning, TEXT("[Gradient] SelectedIdx=%d LastRevealed=%d PendingGroup=%d SegMIDs=%d ArrowMIDs=%d"),
		SelectedIndex, LastRevealedIndex, PendingChaseGroupIndex, SegmentMIDs.Num(), ArrowMIDs.Num());

	// Any reveal in flight belongs to the previous selection — stop it before touching colours so a
	// fast re-select can't let two reveals race on the same MIDs. LastRevealedIndex and
	// PendingChaseGroupIndex already reflect wherever that reveal actually got to (updated below), so
	// this call always resumes from the true current boundary — never jumping back over already-lit
	// ground, never skipping, and never stranding a half-painted group (see the fold-in below).
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ChaseRevealTimerHandle);
	}

	if (SelectedIndex == INDEX_NONE)
	{
		// Clearing selection isn't a "point" to chase to/from — snap everything instantly, and let the
		// full resync sort out any group a previous chase left half-painted.
		ResyncSegmentGradientInstant();
		return;
	}

	// Segment i spans control point i -> i+1, so "lit up to control point N" is exactly segments
	// [0, N-1]. A (point 0) is always the anchor for both spans.
	const int32 NumMIDs = SegmentMIDs.Num();
	int32 OldSpan = (LastRevealedIndex > 0) ? FMath::Min(LastRevealedIndex, NumMIDs) : 0;
	const int32 NewSpan = (SelectedIndex > 0) ? FMath::Min(SelectedIndex, NumMIDs) : 0;

	if (PendingChaseGroupIndex != INDEX_NONE)
	{
		// The previous reveal was interrupted with one segment-group (its mesh + its arrows) only
		// partially painted toward its old target colour. Fold that group into whichever range we're
		// about to process now, in EITHER direction, so it always gets fully finished rather than being
		// silently excluded by a boundary that no longer describes its true painted state.
		OldSpan = (NewSpan >= OldSpan)
			? FMath::Min(OldSpan, PendingChaseGroupIndex)
			: FMath::Max(OldSpan, PendingChaseGroupIndex + 1);
		PendingChaseGroupIndex = INDEX_NONE;
	}

	if (NewSpan == OldSpan)
	{
		// Re-selecting the same point (or a clamped no-op edge case) — nothing to light or unlight.
		LastRevealedIndex = SelectedIndex;
		return;
	}

	const bool bExtending = (NewSpan > OldSpan);
	const int32 SpanLo = FMath::Min(OldSpan, NewSpan);
	const int32 SpanHi = FMath::Max(OldSpan, NewSpan);

	// Build the interleaved segment+arrow step list across the span, ascending by spline distance —
	// same construction regardless of direction. GroupIndex is the control-point index the step
	// belongs to (a segment step's own index, or the segment its arrow sits on).
	struct FDistanceStep
	{
		float Distance = 0.0f;
		SplineRouteActorColors::FChaseRevealStep Step;
	};
	TArray<FDistanceStep> Ordered;
	Ordered.Reserve((SpanHi - SpanLo) + ArrowMIDs.Num());

	for (int32 i = SpanLo; i < SpanHi; ++i)
	{
		FDistanceStep Entry;
		Entry.Distance = Spline ? Spline->GetDistanceAlongSplineAtSplinePoint(i) : 0.0f;
		Entry.Step.bIsArrow = false;
		Entry.Step.Index = i;
		Entry.Step.GroupIndex = i;
		Ordered.Add(Entry);
	}
	for (int32 j = 0; j < ArrowMIDs.Num(); ++j)
	{
		const int32 SegIdx = GetSegmentIndexAtDistance(ArrowDistances[j]);
		if (SegIdx != INDEX_NONE && SegIdx >= SpanLo && SegIdx < SpanHi)
		{
			FDistanceStep Entry;
			Entry.Distance = ArrowDistances[j];
			Entry.Step.bIsArrow = true;
			Entry.Step.Index = j;
			Entry.Step.GroupIndex = SegIdx;
			Ordered.Add(Entry);
		}
	}
	Ordered.StableSort([](const FDistanceStep& A, const FDistanceStep& B) { return A.Distance < B.Distance; });

	TArray<SplineRouteActorColors::FChaseRevealStep> RevealSteps;
	RevealSteps.Reserve(Ordered.Num());
	if (bExtending)
	{
		for (const FDistanceStep& Entry : Ordered)
		{
			RevealSteps.Add(Entry.Step);
		}
	}
	else
	{
		// Reverse travel order: unlight from the far tip back toward the new (closer-to-A) boundary.
		for (int32 i = Ordered.Num() - 1; i >= 0; --i)
		{
			RevealSteps.Add(Ordered[i].Step);
		}
	}

	// One step function serves both directions. PendingChaseGroupIndex (a member, so it survives an
	// interruption) tracks whichever group the walk is currently inside. A step whose GroupIndex
	// differs from it means the previous group just had its LAST element applied: extending confirms
	// the boundary grows to include the group being entered; retracting confirms the boundary shrinks
	// to exclude the group being left. This needs no special-casing for "does this group have arrows" —
	// it fires purely on GroupIndex transitions, correct for both walk orders.
	auto ProcessStep = [bExtending](ASplineRouteActor& Self, const SplineRouteActorColors::FChaseRevealStep& Step)
	{
		if (Self.PendingChaseGroupIndex != INDEX_NONE && Self.PendingChaseGroupIndex != Step.GroupIndex)
		{
			Self.LastRevealedIndex = bExtending ? Step.GroupIndex : Self.PendingChaseGroupIndex;
		}
		Self.PendingChaseGroupIndex = Step.GroupIndex;

		const FLinearColor SegColor = bExtending ? Self.GradientEndColor : Self.SegmentDefaultColor;
		const FLinearColor ArrowColor = bExtending ? Self.ArrowGradientEndColor : Self.ArrowDefaultColor;
		if (Step.bIsArrow)
		{
			if (Self.ArrowMIDs.IsValidIndex(Step.Index))
			{
				if (UMaterialInstanceDynamic* ArrowMID = Self.ArrowMIDs[Step.Index])
				{
					ArrowMID->SetVectorParameterValue(SplineRouteActorColors::ColorParam, ArrowColor);
					ArrowMID->SetVectorParameterValue(SplineRouteActorColors::BaseColorParam, ArrowColor);
				}
			}
		}
		else if (Self.SegmentMIDs.IsValidIndex(Step.Index))
		{
			if (UMaterialInstanceDynamic* SegMID = Self.SegmentMIDs[Step.Index])
			{
				SegMID->SetVectorParameterValue(SplineRouteActorColors::ColorParam, SegColor);
				SegMID->SetVectorParameterValue(SplineRouteActorColors::BaseColorParam, SegColor);
			}
		}
	};

	// Step 0 (closest to the already-lit/unlit boundary) applies immediately; the timer below carries
	// steps 1..N at ChaseStepDelay intervals, so a longer span simply takes proportionally longer
	// wall-clock time to finish instead of speeding up or slowing down the per-step delay.
	ProcessStep(*this, RevealSteps[0]);

	UE_LOG(LogSplineRoute, Verbose,
		TEXT("[SplineRoute] %s gradient chase started (%s): span %d -> %d, %d steps queued"),
		*GetName(), bExtending ? TEXT("extend") : TEXT("retract"), OldSpan, NewSpan, RevealSteps.Num());

	if (RevealSteps.Num() <= 1)
	{
		// Nothing more to reveal — this span is already fully confirmed.
		LastRevealedIndex = SelectedIndex;
		PendingChaseGroupIndex = INDEX_NONE;
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		LastRevealedIndex = SelectedIndex;
		PendingChaseGroupIndex = INDEX_NONE;
		return;
	}

	const int32 CapturedSelection = SelectedIndex;
	TWeakObjectPtr<ASplineRouteActor> WeakThis(this);
	FTimerDelegate RevealDelegate = FTimerDelegate::CreateLambda(
		[WeakThis, RevealSteps, CapturedSelection, ProcessStep, Cursor = 1]() mutable
	{
		ASplineRouteActor* Self = WeakThis.Get();
		// Actor gone, or a newer selection already tore this reveal down elsewhere — stop quietly.
		// PendingChaseGroupIndex is deliberately left as-is here: it still accurately names whichever
		// group this reveal last touched, and the newer selection's own ApplySegmentGradient call is
		// what folds it back in.
		if (!Self || Self->SelectedIndex != CapturedSelection)
		{
			return;
		}

		ProcessStep(*Self, RevealSteps[Cursor]);
		++Cursor;

		if (!RevealSteps.IsValidIndex(Cursor))
		{
			// Reached the end of this pass — the final group is now fully confirmed too.
			Self->LastRevealedIndex = CapturedSelection;
			Self->PendingChaseGroupIndex = INDEX_NONE;
			if (UWorld* TickWorld = Self->GetWorld())
			{
				TickWorld->GetTimerManager().ClearTimer(Self->ChaseRevealTimerHandle);
			}
		}
	});

	World->GetTimerManager().SetTimer(ChaseRevealTimerHandle, RevealDelegate, FMath::Max(0.01f, ChaseStepDelay), /*bLoop=*/true);
}

void ASplineRouteActor::ResyncSegmentGradientInstant()
{
	// Any in-flight chase is animating a span whose MIDs are being destroyed and recreated by this
	// rebuild — its captured indices no longer refer to anything meaningful, so stop it before it can
	// write into the wrong (or a stale) MID.
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ChaseRevealTimerHandle);
	}

	const int32 NumMIDs = SegmentMIDs.Num();
	const int32 GradientSpan = (SelectedIndex > 0) ? FMath::Min(SelectedIndex, NumMIDs) : 0;

	for (int32 i = 0; i < NumMIDs; ++i)
	{
		UMaterialInstanceDynamic* MID = SegmentMIDs[i];
		if (!MID)
		{
			continue;
		}
		const FLinearColor SegColor = (i < GradientSpan) ? GradientEndColor : SegmentDefaultColor;
		MID->SetVectorParameterValue(SplineRouteActorColors::ColorParam, SegColor);
		MID->SetVectorParameterValue(SplineRouteActorColors::BaseColorParam, SegColor);
	}

	for (int32 j = 0; j < ArrowMIDs.Num(); ++j)
	{
		UMaterialInstanceDynamic* ArrowMID = ArrowMIDs[j];
		if (!ArrowMID)
		{
			continue;
		}
		const int32 SegIdx = GetSegmentIndexAtDistance(ArrowDistances[j]);
		const FLinearColor ArrowColor = (SegIdx != INDEX_NONE && SegIdx < GradientSpan) ? ArrowGradientEndColor : ArrowDefaultColor;
		ArrowMID->SetVectorParameterValue(SplineRouteActorColors::ColorParam, ArrowColor);
		ArrowMID->SetVectorParameterValue(SplineRouteActorColors::BaseColorParam, ArrowColor);
	}

	LastRevealedIndex = SelectedIndex;
	PendingChaseGroupIndex = INDEX_NONE;

	UE_LOG(LogSplineRoute, Verbose,
		TEXT("[SplineRoute] %s gradient resynced instantly: selected=%d span=%d/%d segments"),
		*GetName(), SelectedIndex, GradientSpan, NumMIDs);
}

void ASplineRouteActor::RebuildDirectionArrows()
{
	for (UStaticMeshComponent* Existing : DirectionArrows)
	{
		if (Existing)
		{
			Existing->DestroyComponent();
		}
	}
	DirectionArrows.Reset();
	ArrowMIDs.Reset();
	ArrowDistances.Reset();

	if (!bShowDirectionArrows || !Spline || !ArrowMesh || Spline->GetNumberOfSplinePoints() < 2)
	{
		return;
	}

	const float Length = Spline->GetSplineLength();
	if (Length <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	EnsureFallbackMaterials();
	UMaterialInterface* ArrowMat = GetEffectiveArrowMaterial();

	const float Spacing = FMath::Max(50.0f, ArrowSpacing);
	// Arrows are sized off the waypoint handles so the whole route reads at one visual scale.
	const float ArrowWorldSize = FMath::Max(1.0f, HandleSize) * FMath::Max(0.01f, ArrowScale);
	const FVector ArrowScale3D(ArrowWorldSize / SplineRouteActorColors::BasicShapeBaseline);

	// Offset the first arrow by half a spacing so it doesn't sit on top of the start handle, and so
	// routes shorter than one spacing interval still get a single mid-route arrow.
	const float FirstDistance = FMath::Min(Spacing * 0.5f, Length * 0.5f);

	int32 NumReversed = 0;
	for (float Distance = FirstDistance;
		Distance <= Length - KINDA_SMALL_NUMBER && DirectionArrows.Num() < SplineRouteActorColors::MaxDirectionArrows;
		Distance += Spacing)
	{
		const FVector Location = Spline->GetLocationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);
		FVector Direction = Spline->GetDirectionAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);

		// On a reverse segment the vessel travels stern-first, so the arrow points back down the route.
		if (IsReverseAtDistance(Distance))
		{
			Direction = -Direction;
			++NumReversed;
		}

		if (Direction.IsNearlyZero())
		{
			continue;
		}

		UStaticMeshComponent* Arrow = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
		Arrow->SetupAttachment(Spline);
		Arrow->SetMobility(EComponentMobility::Movable);
		Arrow->SetStaticMesh(ArrowMesh);
		// Pure visualisation: never block the handle picking trace or generate overlaps.
		Arrow->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Arrow->SetCanEverAffectNavigation(false);
		Arrow->SetCastShadow(false);
		Arrow->RegisterComponent();
		// Cone apex is +Z, so mapping local Z onto the travel direction aims the arrow downstream.
		// ArrowRotationOffset is then applied in the arrow's own local frame, on top of that alignment.
		const FQuat AlignmentRot = FRotationMatrix::MakeFromZ(Direction.GetSafeNormal()).ToQuat();
		Arrow->SetWorldRotation(AlignmentRot * ArrowRotationOffset.Quaternion());
		Arrow->SetWorldLocation(Location + ArrowLocationOffset);
		Arrow->SetWorldScale3D(ArrowScale3D);

		// One MID per arrow: the gradient needs to tint each arrow independently, matching SegmentMIDs.
		UMaterialInstanceDynamic* ArrowMID = ArrowMat ? UMaterialInstanceDynamic::Create(ArrowMat, this) : nullptr;
		if (ArrowMID)
		{
			const int32 NumSlots = Arrow->GetNumMaterials();
			for (int32 Slot = 0; Slot < NumSlots; ++Slot)
			{
				Arrow->SetMaterial(Slot, ArrowMID);
			}
		}

		DirectionArrows.Add(Arrow);
		ArrowMIDs.Add(ArrowMID);
		ArrowDistances.Add(Distance);
	}

	UE_LOG(LogSplineRoute, Verbose,
		TEXT("[SplineRoute] %s rebuilt %d direction arrows (spacing=%.1f length=%.1f reversed=%d)"),
		*GetName(), DirectionArrows.Num(), Spacing, Length, NumReversed);

	// Freshly created arrow MIDs still carry the parent colour — resync them to the current selection
	// instantly, same as RebuildSegmentMeshes does for segments. Matters when this is called standalone
	// (e.g. from SetPointReverse/SetHandleSize) rather than as part of a full RebuildSegmentMeshes pass.
	ResyncSegmentGradientInstant();
}

void ASplineRouteActor::EnsureFallbackMaterials()
{
	// Now called from every geometry rebuild, so short-circuit instead of re-running LoadObject.
	if (bFallbackMaterialsReady)
	{
		return;
	}

	// Create runtime MIDs from WorldGridMaterial (which has a Color parameter) when explicit assets are not set.
	UMaterialInterface* Source = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
	if (!Source)
	{
		return;
	}
	bFallbackMaterialsReady = true;

	if (!HandleMaterial && !FallbackHandleMID)
	{
		FallbackHandleMID = UMaterialInstanceDynamic::Create(Source, this);
		if (FallbackHandleMID)
		{
			FallbackHandleMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.05f, 0.45f, 1.0f, 1.0f));
		}
	}
	if (!SelectedHandleMaterial && !FallbackSelectedHandleMID)
	{
		FallbackSelectedHandleMID = UMaterialInstanceDynamic::Create(Source, this);
		if (FallbackSelectedHandleMID)
		{
			FallbackSelectedHandleMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(1.0f, 0.45f, 0.05f, 1.0f));
		}
	}
	if (!ReverseHandleMaterial && !FallbackReverseHandleMID)
	{
		FallbackReverseHandleMID = UMaterialInstanceDynamic::Create(Source, this);
		if (FallbackReverseHandleMID)
		{
			// Deep red so reverse nodes are obvious against the blue/orange normal/selected handles.
			FallbackReverseHandleMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(1.0f, 0.08f, 0.08f, 1.0f));
		}
	}
	if (!FallbackGroundHandleMID)
	{
		FallbackGroundHandleMID = UMaterialInstanceDynamic::Create(Source, this);
		if (FallbackGroundHandleMID)
		{
			// Tan/amber so land-route handles read distinctly from the blue water-route handles.
			FallbackGroundHandleMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.85f, 0.55f, 0.1f, 1.0f));
		}
	}
	if (!SegmentMaterial && !FallbackSegmentMID)
	{
		FallbackSegmentMID = UMaterialInstanceDynamic::Create(Source, this);
		if (FallbackSegmentMID)
		{
			FallbackSegmentMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.0f, 0.85f, 1.0f, 1.0f));
		}
	}
	if (!ArrowMaterial && !FallbackArrowMID)
	{
		FallbackArrowMID = UMaterialInstanceDynamic::Create(Source, this);
		if (FallbackArrowMID)
		{
			// Bright yellow: reads against both the gradient span and the grey tail of the route.
			FallbackArrowMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(1.0f, 0.95f, 0.15f, 1.0f));
		}
	}
}

UMaterialInterface* ASplineRouteActor::GetEffectiveHandleMaterial(bool bSelected) const
{
	if (bSelected)
	{
		if (SelectedHandleMaterial)
		{
			return SelectedHandleMaterial;
		}
		return FallbackSelectedHandleMID;
	}
	if (HandleMaterial)
	{
		return HandleMaterial;
	}
	return FallbackHandleMID;
}

UMaterialInterface* ASplineRouteActor::GetEffectiveHandleMaterialForPoint(int32 PointIndex, bool bSelected) const
{
	// Selection always wins so the user can see what they're editing, even on a reverse node.
	if (bSelected)
	{
		return GetEffectiveHandleMaterial(true);
	}
	if (GetPointReverse(PointIndex))
	{
		if (ReverseHandleMaterial)
		{
			return ReverseHandleMaterial;
		}
		return FallbackReverseHandleMID;
	}
	// Normal handle: an explicit HandleMaterial wins; otherwise tint by the route's medium (water vs land).
	if (HandleMaterial)
	{
		return HandleMaterial;
	}
	if (VehicleType == ERouteVehicleType::Ground && FallbackGroundHandleMID)
	{
		return FallbackGroundHandleMID;
	}
	return GetEffectiveHandleMaterial(false);
}

UMaterialInterface* ASplineRouteActor::GetEffectiveSegmentMaterial() const
{
	if (SegmentMaterial)
	{
		return SegmentMaterial;
	}
	return FallbackSegmentMID;
}

UMaterialInterface* ASplineRouteActor::GetEffectiveArrowMaterial() const
{
	if (ArrowMaterial)
	{
		return ArrowMaterial;
	}
	return FallbackArrowMID;
}

int32 ASplineRouteActor::GetSegmentIndexAtDistance(float Distance) const
{
	if (!Spline)
	{
		return INDEX_NONE;
	}
	const int32 NumPoints = Spline->GetNumberOfSplinePoints();
	if (NumPoints < 2)
	{
		return INDEX_NONE;
	}
	const float Length = Spline->GetSplineLength();
	const float Clamped = FMath::Clamp(Distance, 0.0f, Length);

	// Same boundary walk as IsReverseAtDistance, kept separate since callers want the index itself here.
	const int32 NumSegments = GetNumSegments();
	for (int32 i = 0; i < NumSegments; ++i)
	{
		const int32 NextIdx = (i + 1) % NumPoints;
		const float SegStart = Spline->GetDistanceAlongSplineAtSplinePoint(i);
		const float SegEnd = (i + 1 < NumPoints)
			? Spline->GetDistanceAlongSplineAtSplinePoint(NextIdx)
			: Length;
		if (Clamped >= SegStart && Clamped <= SegEnd)
		{
			return i;
		}
	}
	return INDEX_NONE;
}
