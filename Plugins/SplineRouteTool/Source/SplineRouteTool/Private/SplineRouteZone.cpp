// Copyright DamenTools internal. Spline Route Tool plugin.

#include "SplineRouteZone.h"
#include "Components/BoxComponent.h"

ASplineRouteZone::ASplineRouteZone()
{
	PrimaryActorTick.bCanEverTick = false;

	Box = CreateDefaultSubobject<UBoxComponent>(TEXT("Box"));
	SetRootComponent(Box);
	Box->SetBoxExtent(FVector(500.f, 500.f, 100.f));
	Box->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Box->SetGenerateOverlapEvents(false);
	Box->SetHiddenInGame(true);
	ApplyShapeColor();
}

bool ASplineRouteZone::ContainsLocation(const FVector& WorldLocation, bool bUseZ) const
{
	if (!Box)
	{
		return false;
	}
	const FTransform Xform = Box->GetComponentTransform();
	const FVector Local = Xform.InverseTransformPosition(WorldLocation);
	const FVector Extent = Box->GetUnscaledBoxExtent();
	const bool InXY = FMath::Abs(Local.X) <= Extent.X && FMath::Abs(Local.Y) <= Extent.Y;
	if (!bUseZ)
	{
		return InXY;
	}
	return InXY && FMath::Abs(Local.Z) <= Extent.Z;
}

void ASplineRouteZone::ApplyShapeColor()
{
	if (!Box)
	{
		return;
	}
	Box->ShapeColor = (ZoneType == ESplineRouteZoneType::Water)
		? FColor(40, 140, 255)
		: FColor(180, 120, 60);
	Box->MarkRenderStateDirty();
}

#if WITH_EDITOR
void ASplineRouteZone::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	if (PropertyChangedEvent.Property &&
		PropertyChangedEvent.Property->GetFName() == GET_MEMBER_NAME_CHECKED(ASplineRouteZone, ZoneType))
	{
		ApplyShapeColor();
	}
}
#endif
