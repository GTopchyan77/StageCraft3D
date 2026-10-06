// Copyright Epic Games, Inc. All Rights Reserved.

#include "Actors/ModularTransformGizmo.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/HitResult.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/PlayerController.h"
#include "Interaction/StageCraftCollision.h"
#include "Interaction/StageTransformRules.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "UObject/ConstructorHelpers.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ModularTransformGizmo)

namespace ModularTransformGizmo
{
	// Handle geometry in gizmo space (cm at scale 1). BasicShapes are 100 cm tall, pivot at the centre.
	constexpr double ArrowShaftLength = 80.0;
	constexpr double ArrowShaftRadius = 3.0;
	constexpr double ArrowHeadLength = 20.0;
	constexpr double ArrowHeadRadius = 8.0;
	constexpr double RingRadius = 70.0;
	constexpr double RingTubeRadius = 3.0;
	constexpr int32 RingSegments = 48;
	constexpr int32 RingTubeSegments = 8;
	constexpr double ScaleHeadSize = 14.0;
	constexpr double UniformHandleSize = 18.0;

	const FName GizmoColorParameter(TEXT("GizmoColor"));

	// Far enough to reach the gizmo from any camera in a stage-sized level (10 km).
	constexpr double HandleTraceDistance = 1.0e6;

	// Draw after any other translucency (beams, haze cards), which would otherwise paint over the handles.
	constexpr int32 HandleTranslucencySortPriority = 1000;

	void ConfigureHandle(UPrimitiveComponent* Handle)
	{
		// Visible only to gizmo traces; placement/selection/deletion traces pass straight through.
		Handle->SetMobility(EComponentMobility::Movable);
		Handle->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Handle->SetCollisionResponseToAllChannels(ECR_Ignore);
		Handle->SetCollisionResponseToChannel(StageCraftCollision::GizmoChannel, ECR_Block);
		Handle->SetGenerateOverlapEvents(false);

		// The handles are an overlay, not scene content: keep them out of every lighting and capture path.
		Handle->SetCastShadow(false);
		Handle->SetReceivesDecals(false);
		Handle->SetAffectDynamicIndirectLighting(false);
		Handle->SetAffectDistanceFieldLighting(false);
		Handle->SetVisibleInRayTracing(false);
		Handle->bVisibleInReflectionCaptures = false;
		Handle->bVisibleInRealTimeSkyCaptures = false;
		Handle->SetTranslucentSortPriority(HandleTranslucencySortPriority);
	}

	void SetHandleActive(UPrimitiveComponent* Handle, bool bActive)
	{
		// Hidden components still block traces, so collision is toggled together with visibility.
		Handle->SetVisibility(bActive);
		Handle->SetCollisionEnabled(bActive ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
	}
}

AModularTransformGizmo::AModularTransformGizmo()
{
	using namespace ModularTransformGizmo;

	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> ConeMesh(TEXT("/Engine/BasicShapes/Cone.Cone"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> OnTopMaterial(TEXT("/Game/StageCraft/Gizmo/M_GizmoHandle.M_GizmoHandle"));
	ArrowShaftMesh = CylinderMesh.Object;
	ArrowHeadMesh = ConeMesh.Object;
	ScaleHandleMesh = CubeMesh.Object;
	HandleMaterial = OnTopMaterial.Object;
	if (!HandleMaterial)
	{
		// Keeps the gizmo usable if the project asset is missing, at the cost of being hidden by geometry.
		static ConstructorHelpers::FObjectFinder<UMaterialInterface> EngineGizmoMaterial(TEXT("/Engine/EngineMaterials/GizmoMaterial.GizmoMaterial"));
		HandleMaterial = EngineGizmoMaterial.Object;
	}

	GizmoRoot = CreateDefaultSubobject<USceneComponent>(TEXT("GizmoRoot"));
	GizmoRoot->SetMobility(EComponentMobility::Movable);
	// Follow the target's location only; handles stay world-aligned and size is driven by camera distance.
	GizmoRoot->SetUsingAbsoluteRotation(true);
	GizmoRoot->SetUsingAbsoluteScale(true);
	SetRootComponent(GizmoRoot);

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const FVector Direction = AxisVector(Axis);
		const FRotator AxisRotation = FRotationMatrix::MakeFromZ(Direction).Rotator();

		UStaticMeshComponent* Shaft = CreateDefaultSubobject<UStaticMeshComponent>(*FString::Printf(TEXT("Shaft%d"), Axis));
		Shaft->SetupAttachment(GizmoRoot);
		Shaft->SetRelativeLocationAndRotation(Direction * (ArrowShaftLength * 0.5), AxisRotation);
		Shaft->SetRelativeScale3D(FVector(ArrowShaftRadius / 50.0, ArrowShaftRadius / 50.0, ArrowShaftLength / 100.0));
		ConfigureHandle(Shaft);
		ShaftComponents[Axis] = Shaft;

		UStaticMeshComponent* Head = CreateDefaultSubobject<UStaticMeshComponent>(*FString::Printf(TEXT("Head%d"), Axis));
		Head->SetupAttachment(GizmoRoot);
		Head->SetRelativeLocationAndRotation(Direction * (ArrowShaftLength + ArrowHeadLength * 0.5), AxisRotation);
		Head->SetRelativeScale3D(FVector(ArrowHeadRadius / 50.0, ArrowHeadRadius / 50.0, ArrowHeadLength / 100.0));
		ConfigureHandle(Head);
		HeadComponents[Axis] = Head;

		UProceduralMeshComponent* Ring = CreateDefaultSubobject<UProceduralMeshComponent>(*FString::Printf(TEXT("Ring%d"), Axis));
		Ring->SetupAttachment(GizmoRoot);
		Ring->SetRelativeRotation(AxisRotation);
		Ring->bUseComplexAsSimpleCollision = true;
		ConfigureHandle(Ring);
		RingComponents[Axis] = Ring;

		UStaticMeshComponent* ScaleHead = CreateDefaultSubobject<UStaticMeshComponent>(*FString::Printf(TEXT("ScaleHead%d"), Axis));
		ScaleHead->SetupAttachment(GizmoRoot);
		ScaleHead->SetRelativeLocationAndRotation(Direction * (ArrowShaftLength + ScaleHeadSize * 0.5), AxisRotation);
		ScaleHead->SetRelativeScale3D(FVector(ScaleHeadSize / 100.0));
		ConfigureHandle(ScaleHead);
		ScaleHeadComponents[Axis] = ScaleHead;
	}

	UniformScaleComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("UniformScale"));
	UniformScaleComponent->SetupAttachment(GizmoRoot);
	UniformScaleComponent->SetRelativeScale3D(FVector(UniformHandleSize / 100.0));
	ConfigureHandle(UniformScaleComponent);

	SetActorHiddenInGame(true);
	SetActorEnableCollision(false);
}

void AModularTransformGizmo::BeginPlay()
{
	Super::BeginPlay();

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		ShaftComponents[Axis]->SetStaticMesh(ArrowShaftMesh);
		HeadComponents[Axis]->SetStaticMesh(ArrowHeadMesh);

		AxisMaterials[Axis] = UMaterialInstanceDynamic::Create(HandleMaterial, this);
		SetAxisHighlighted(Axis, false);

		ShaftComponents[Axis]->SetMaterial(0, AxisMaterials[Axis]);
		HeadComponents[Axis]->SetMaterial(0, AxisMaterials[Axis]);

		ScaleHeadComponents[Axis]->SetStaticMesh(ScaleHandleMesh);
		ScaleHeadComponents[Axis]->SetMaterial(0, AxisMaterials[Axis]);
	}

	AxisMaterials[UniformHandleIndex] = UMaterialInstanceDynamic::Create(HandleMaterial, this);
	SetAxisHighlighted(UniformHandleIndex, false);
	UniformScaleComponent->SetStaticMesh(ScaleHandleMesh);
	UniformScaleComponent->SetMaterial(0, AxisMaterials[UniformHandleIndex]);

	// Ring materials are assigned per section inside BuildRingMeshes.
	BuildRingMeshes();
	ApplyModeVisibility();
}

void AModularTransformGizmo::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Defensive: selection normally detaches us before the target dies.
	if (!IsValid(Target))
	{
		DetachFromTarget();
		return;
	}

	UpdateScreenScale();
}

void AModularTransformGizmo::AttachToTarget(AActor* NewTarget)
{
	if (NewTarget == Target)
	{
		return;
	}

	if (!IsValid(NewTarget) || !NewTarget->GetRootComponent())
	{
		DetachFromTarget();
		return;
	}

	EndDrag();
	Target = NewTarget;

	AttachToActor(Target, FAttachmentTransformRules(EAttachmentRule::SnapToTarget, EAttachmentRule::KeepWorld, EAttachmentRule::KeepWorld, false));
	SetActorRotation(FRotator::ZeroRotator);

	SetActorHiddenInGame(false);
	SetActorEnableCollision(true);
	SetActorTickEnabled(true);
	UpdateScreenScale();
}

void AModularTransformGizmo::DetachFromTarget()
{
	EndDrag();

	if (Target)
	{
		DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		Target = nullptr;
	}

	SetActorHiddenInGame(true);
	SetActorEnableCollision(false);
	SetActorTickEnabled(false);
}

void AModularTransformGizmo::SetMode(EGizmoMode NewMode)
{
	if (NewMode == Mode)
	{
		return;
	}

	EndDrag();
	Mode = NewMode;
	ApplyModeVisibility();
	OnModeChanged.Broadcast(Mode);
}

void AModularTransformGizmo::CycleMode()
{
	switch (Mode)
	{
	case EGizmoMode::Translate:	SetMode(EGizmoMode::Rotate);	break;
	case EGizmoMode::Rotate:	SetMode(EGizmoMode::Scale);		break;
	case EGizmoMode::Scale:		SetMode(EGizmoMode::Translate);	break;
	}
}

bool AModularTransformGizmo::TryBeginDrag(const FHitResult& HandleHit, const FVector& RayOrigin, const FVector& RayDirection)
{
	if (!IsValid(Target) || IsDragging())
	{
		return false;
	}

	const int32 Handle = FindHandleAxis(HandleHit.GetComponent());
	if (Handle == INDEX_NONE)
	{
		return false;
	}

	const FVector Pivot = Target->GetActorLocation();
	const FVector Direction = DragDirection(Handle, RayDirection);
	if (Direction.IsZero())
	{
		return false; // Looking straight down the axis: movement along it is not observable.
	}

	FVector PlaneNormal = Direction;
	if (Mode != EGizmoMode::Rotate)
	{
		// Plane containing the drag direction and facing the camera as much as possible, for a stable projection.
		PlaneNormal = RayDirection - (RayDirection | Direction) * Direction;
		if (!PlaneNormal.Normalize(UE_KINDA_SMALL_NUMBER))
		{
			return false;
		}
	}

	DragPlane = FPlane(Pivot, PlaneNormal);

	FVector StartPoint;
	if (!IntersectDragPlane(RayOrigin, RayDirection, StartPoint))
	{
		return false;
	}

	DragAxisIndex = Handle;
	DragAxisDirection = Direction;
	DragStartPoint = StartPoint;
	DragStartLocation = Pivot;
	DragStartRotation = Target->GetActorQuat();
	DragStartScale = Target->GetActorScale3D();
	DragHandleLength = ModularTransformGizmo::ArrowShaftLength * GizmoRoot->GetComponentScale().X;
	SetAxisHighlighted(Handle, true);
	return true;
}

void AModularTransformGizmo::UpdateDrag(const FVector& RayOrigin, const FVector& RayDirection)
{
	if (!IsDragging() || !IsValid(Target))
	{
		return;
	}

	FVector Point;
	if (!IntersectDragPlane(RayOrigin, RayDirection, Point))
	{
		return;
	}

	switch (Mode)
	{
	case EGizmoMode::Translate:	UpdateTranslateDrag(Point);	break;
	case EGizmoMode::Rotate:	UpdateRotateDrag(Point);	break;
	case EGizmoMode::Scale:		UpdateScaleDrag(Point);		break;
	}
}

void AModularTransformGizmo::UpdateTranslateDrag(const FVector& Point)
{
	double Distance = (Point - DragStartPoint) | DragAxisDirection;
	if (TranslationSnap > 0.f)
	{
		Distance = FMath::GridSnap(Distance, static_cast<double>(TranslationSnap));
	}

	Target->SetActorLocation(DragStartLocation + DragAxisDirection * Distance);
}

void AModularTransformGizmo::UpdateRotateDrag(const FVector& Point)
{
	const FVector From = DragStartPoint - DragStartLocation;
	const FVector To = Point - DragStartLocation;
	if (From.IsNearlyZero() || To.IsNearlyZero())
	{
		return;
	}

	// Signed angle from the grab point to the cursor, measured around the axis.
	double AngleDegrees = FMath::RadiansToDegrees(FMath::Atan2((From ^ To) | DragAxisDirection, From | To));
	if (RotationSnapDegrees > 0.f)
	{
		AngleDegrees = FMath::GridSnap(AngleDegrees, static_cast<double>(RotationSnapDegrees));
	}

	Target->SetActorRotation(FQuat(DragAxisDirection, FMath::DegreesToRadians(AngleDegrees)) * DragStartRotation);
}

void AModularTransformGizmo::UpdateScaleDrag(const FVector& Point)
{
	const double Distance = (Point - DragStartPoint) | DragAxisDirection;
	const double Factor = StageTransformRules::ComputeDragScaleFactor(Distance, DragHandleLength);

	FVector NewScale = DragStartScale;
	if (DragAxisIndex == UniformHandleIndex)
	{
		NewScale *= Factor;
	}
	else
	{
		NewScale[DragAxisIndex] *= Factor;
	}

	Target->SetActorScale3D(StageTransformRules::ClampScale(NewScale));
}

FVector AModularTransformGizmo::DragDirection(int32 HandleIndex, const FVector& RayDirection) const
{
	if (HandleIndex != UniformHandleIndex)
	{
		return AxisVector(HandleIndex);
	}

	// Uniform scale: dragging "up the screen" grows. World up projected onto the camera-facing plane.
	FVector ScreenUp = FVector::UpVector - (FVector::UpVector | RayDirection) * RayDirection;
	if (!ScreenUp.Normalize(UE_KINDA_SMALL_NUMBER))
	{
		// Looking straight down: use world X as "up the screen" instead.
		ScreenUp = FVector::XAxisVector - (FVector::XAxisVector | RayDirection) * RayDirection;
		ScreenUp.Normalize(UE_KINDA_SMALL_NUMBER);
	}
	return ScreenUp;
}

void AModularTransformGizmo::EndDrag()
{
	if (IsDragging())
	{
		SetAxisHighlighted(DragAxisIndex, false);
		DragAxisIndex = INDEX_NONE;
	}
}

void AModularTransformGizmo::BuildRingMeshes()
{
	using namespace ModularTransformGizmo;

	// Torus in the XY plane; each ring component is rotated onto its axis.
	TArray<FVector> Vertices;
	TArray<FVector> Normals;
	TArray<int32> Triangles;
	Vertices.Reserve(RingSegments * RingTubeSegments);
	Normals.Reserve(RingSegments * RingTubeSegments);
	Triangles.Reserve(RingSegments * RingTubeSegments * 12);

	for (int32 Segment = 0; Segment < RingSegments; ++Segment)
	{
		const double Theta = UE_TWO_PI * Segment / RingSegments;
		const FVector Radial(FMath::Cos(Theta), FMath::Sin(Theta), 0.0);

		for (int32 Tube = 0; Tube < RingTubeSegments; ++Tube)
		{
			const double Phi = UE_TWO_PI * Tube / RingTubeSegments;
			const FVector Normal = Radial * FMath::Cos(Phi) + FVector::UpVector * FMath::Sin(Phi);
			Vertices.Add(Radial * RingRadius + Normal * RingTubeRadius);
			Normals.Add(Normal);
		}
	}

	for (int32 Segment = 0; Segment < RingSegments; ++Segment)
	{
		const int32 NextSegment = (Segment + 1) % RingSegments;
		for (int32 Tube = 0; Tube < RingTubeSegments; ++Tube)
		{
			const int32 NextTube = (Tube + 1) % RingTubeSegments;
			const int32 A = Segment * RingTubeSegments + Tube;
			const int32 B = NextSegment * RingTubeSegments + Tube;
			const int32 C = NextSegment * RingTubeSegments + NextTube;
			const int32 D = Segment * RingTubeSegments + NextTube;

			// Both windings: the engine gizmo material may be one-sided, and a few hundred extra
			// triangles are cheaper than depending on winding conventions.
			Triangles.Append({ A, B, C, A, C, D });
			Triangles.Append({ A, C, B, A, D, C });
		}
	}

	const TArray<FVector2D> NoUVs;
	const TArray<FLinearColor> NoColors;
	const TArray<FProcMeshTangent> NoTangents;

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		RingComponents[Axis]->CreateMeshSection_LinearColor(0, Vertices, Triangles, Normals, NoUVs, NoColors, NoTangents, /*bCreateCollision*/ true);
		RingComponents[Axis]->SetMaterial(0, AxisMaterials[Axis]);
	}
}

void AModularTransformGizmo::ApplyModeVisibility()
{
	const bool bTranslate = Mode == EGizmoMode::Translate;
	const bool bRotate = Mode == EGizmoMode::Rotate;
	const bool bScale = Mode == EGizmoMode::Scale;

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		ModularTransformGizmo::SetHandleActive(ShaftComponents[Axis], bTranslate || bScale);
		ModularTransformGizmo::SetHandleActive(HeadComponents[Axis], bTranslate);
		ModularTransformGizmo::SetHandleActive(ScaleHeadComponents[Axis], bScale);
		ModularTransformGizmo::SetHandleActive(RingComponents[Axis], bRotate);
	}
	ModularTransformGizmo::SetHandleActive(UniformScaleComponent, bScale);
}

void AModularTransformGizmo::SetAxisHighlighted(int32 HandleIndex, bool bHighlighted)
{
	if (!ensure(HandleIndex >= 0 && HandleIndex <= UniformHandleIndex) || !AxisMaterials[HandleIndex])
	{
		return;
	}

	const FLinearColor& IdleColor = HandleIndex == UniformHandleIndex ? UniformScaleColor : AxisColors[HandleIndex];
	AxisMaterials[HandleIndex]->SetVectorParameterValue(ModularTransformGizmo::GizmoColorParameter, bHighlighted ? ActiveAxisColor : IdleColor);
}

void AModularTransformGizmo::UpdateScreenScale()
{
	const APlayerController* Viewer = Cast<APlayerController>(GetOwner());
	if (!Viewer || !Viewer->PlayerCameraManager)
	{
		return;
	}

	const double Distance = FVector::Dist(Viewer->PlayerCameraManager->GetCameraLocation(), GetActorLocation());
	GizmoRoot->SetWorldScale3D(FVector(FMath::Max(Distance * ScreenSizeFactor, 0.01)));
}

int32 AModularTransformGizmo::FindHandleAxis(const UPrimitiveComponent* Component) const
{
	if (!Component || Component->GetOwner() != this)
	{
		return INDEX_NONE;
	}

	if (Mode == EGizmoMode::Scale && Component == UniformScaleComponent)
	{
		return UniformHandleIndex;
	}

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const bool bIsShaft = Component == ShaftComponents[Axis];
		switch (Mode)
		{
		case EGizmoMode::Translate:
			if (bIsShaft || Component == HeadComponents[Axis]) { return Axis; }
			break;
		case EGizmoMode::Rotate:
			if (Component == RingComponents[Axis]) { return Axis; }
			break;
		case EGizmoMode::Scale:
			if (bIsShaft || Component == ScaleHeadComponents[Axis]) { return Axis; }
			break;
		}
	}
	return INDEX_NONE;
}

bool AModularTransformGizmo::TraceHandles(const FVector& RayOrigin, const FVector& RayDirection, FHitResult& OutHit) const
{
	if (!IsValid(Target) || IsHidden())
	{
		return false;
	}

	const FVector RayEnd = RayOrigin + RayDirection * ModularTransformGizmo::HandleTraceDistance;
	// Simple collision is enough: shafts/heads have engine convex hulls and the rings use complex-as-simple.
	const FCollisionQueryParams Params(SCENE_QUERY_STAT(GizmoHandleTrace), /*bTraceComplex*/ false);

	bool bHit = false;
	double ClosestDistanceSquared = TNumericLimits<double>::Max();

	auto TestHandle = [&](UPrimitiveComponent* Handle)
	{
		// FindHandleAxis rejects handles of the inactive mode; the collision check rejects hidden ones.
		if (!Handle || !Handle->IsQueryCollisionEnabled() || FindHandleAxis(Handle) == INDEX_NONE)
		{
			return;
		}

		FHitResult Hit;
		if (Handle->LineTraceComponent(Hit, RayOrigin, RayEnd, Params))
		{
			const double DistanceSquared = FVector::DistSquared(RayOrigin, Hit.ImpactPoint);
			if (DistanceSquared < ClosestDistanceSquared)
			{
				ClosestDistanceSquared = DistanceSquared;
				OutHit = Hit;
				bHit = true;
			}
		}
	};

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		TestHandle(ShaftComponents[Axis]);
		TestHandle(HeadComponents[Axis]);
		TestHandle(RingComponents[Axis]);
		TestHandle(ScaleHeadComponents[Axis]);
	}
	TestHandle(UniformScaleComponent);
	return bHit;
}

bool AModularTransformGizmo::IntersectDragPlane(const FVector& RayOrigin, const FVector& RayDirection, FVector& OutPoint) const
{
	const double Denominator = DragPlane.GetNormal() | RayDirection;
	// Near-parallel rays project to huge distances and make the target jump.
	if (FMath::Abs(Denominator) < 1.e-3)
	{
		return false;
	}

	const double T = -DragPlane.PlaneDot(RayOrigin) / Denominator;
	if (T < 0.0)
	{
		return false;
	}

	OutPoint = RayOrigin + RayDirection * T;
	return true;
}

FVector AModularTransformGizmo::AxisVector(int32 AxisIndex)
{
	return AxisIndex == 0 ? FVector::XAxisVector : (AxisIndex == 1 ? FVector::YAxisVector : FVector::ZAxisVector);
}
