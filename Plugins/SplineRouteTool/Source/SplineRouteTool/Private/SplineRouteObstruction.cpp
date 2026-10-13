// Copyright DamenTools internal. Spline Route Tool plugin.

#include "SplineRouteObstruction.h"
#include "SplineRouteTool.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

ASplineRouteObstruction::ASplineRouteObstruction()
{
	PrimaryActorTick.bCanEverTick = false;

	MeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	SetRootComponent(MeshComponent);
	MeshComponent->SetMobility(EComponentMobility::Movable);
	// Block visibility so the obstruction can be clicked/raycast later if needed, but nothing else.
	MeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	MeshComponent->SetCollisionResponseToAllChannels(ECR_Ignore);
	MeshComponent->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);

	// Default to the engine cube so an obstruction is demoable without any catalog mesh assigned.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		MeshComponent->SetStaticMesh(CubeMesh.Object);
	}
}

void ASplineRouteObstruction::Configure(UStaticMesh* InMesh, FVector InScale, FName InTypeId, int32 InCatalogIndex)
{
	ObstructionTypeId = InTypeId;
	CatalogIndex = InCatalogIndex;
	if (MeshComponent)
	{
		if (InMesh)
		{
			MeshComponent->SetStaticMesh(InMesh);
		}
		MeshComponent->SetWorldScale3D(InScale.IsNearlyZero() ? FVector::OneVector : InScale);
	}
	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] Obstruction %s configured: type=%s mesh=%s scale=%s"),
		*GetName(), *InTypeId.ToString(), *GetNameSafe(InMesh), *InScale.ToString());
}
