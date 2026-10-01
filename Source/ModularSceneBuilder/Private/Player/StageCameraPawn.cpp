// Copyright Epic Games, Inc. All Rights Reserved.

#include "Player/StageCameraPawn.h"

#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageCameraPawn)

AStageCameraPawn::AStageCameraPawn(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bAddDefaultMovementBindings = false;

	// The movement component still sweeps its updated component, so collision must be off entirely.
	GetCollisionComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GetCollisionComponent()->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);

	if (UStaticMeshComponent* Mesh = GetMeshComponent())
	{
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetHiddenInGame(true);
	}
}
