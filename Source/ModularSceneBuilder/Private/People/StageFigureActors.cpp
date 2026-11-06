// Copyright Epic Games, Inc. All Rights Reserved.

#include "People/StageFigureActors.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Interaction/StageCraftCollision.h"
#include "ModularSceneBuilder.h"
#include "People/StageFigureAssetSubsystem.h"

namespace StageFigureActors
{
	void ConfigureFigureComponent(UInstancedStaticMeshComponent& Component, EStageFigureCollision Collision)
	{
		Component.SetMobility(EComponentMobility::Movable);
		Component.SetCanEverAffectNavigation(false);
		Component.SetGenerateOverlapEvents(false);
		Component.SetNumCustomDataFloats(UStageFigureAssetSubsystem::CustomDataCount);
		if (Collision == EStageFigureCollision::None)
		{
			Component.SetCollisionEnabled(ECollisionEnabled::NoCollision);
			return;
		}
		Component.SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Component.SetCollisionResponseToAllChannels(ECR_Ignore);
		Component.SetCollisionResponseToChannel(StageCraftCollision::StageItemChannel, ECR_Block);
	}

	void MakeCustomData(const StageAudienceMath::FPerson& Person, TArray<float>& OutData)
	{
		using ECustomData = UStageFigureAssetSubsystem::ECustomData;
		OutData.SetNumZeroed(UStageFigureAssetSubsystem::CustomDataCount);
		OutData[ECustomData::Clip] = float(static_cast<uint8>(Person.Clip));
		OutData[ECustomData::Phase] = FMath::Frac(Person.Phase);
		OutData[ECustomData::Rate] = FMath::IsFinite(Person.CyclesPerSecond) ? FMath::Max(Person.CyclesPerSecond, 0.f) : 0.f;
		OutData[ECustomData::TopR] = Person.TopColor.R;
		OutData[ECustomData::TopG] = Person.TopColor.G;
		OutData[ECustomData::TopB] = Person.TopColor.B;
		OutData[ECustomData::SkinTone] = FMath::Clamp(Person.SkinTone, 0.f, 1.f);
		OutData[ECustomData::BottomShade] = FMath::Clamp(Person.BottomShade, 0.f, 1.f);
	}

	int32 ShowPeople(UInstancedStaticMeshComponent& Component, TConstArrayView<StageAudienceMath::FPerson> People, UMaterialInterface* FigureMaterial)
	{
		Component.ClearInstances();
		UStageFigureAssetSubsystem* Assets = GEngine ? GEngine->GetEngineSubsystem<UStageFigureAssetSubsystem>() : nullptr;
		if (People.IsEmpty() || !Assets || !FigureMaterial)
		{
			if (!People.IsEmpty() && !FigureMaterial)
			{
				UE_LOG(LogStageCraft, Warning, TEXT("Figures: %s has no figure material; its %d people are not shown."),
					*GetNameSafe(Component.GetOwner()), People.Num());
			}
			return 0;
		}
		UStaticMesh* Mesh = Assets->GetFigureMesh();
		UMaterialInterface* Material = Assets->GetFigureMaterial(FigureMaterial);
		if (!Mesh || !Material)
		{
			return 0;
		}
		if (Component.GetStaticMesh() != Mesh)
		{
			Component.SetStaticMesh(Mesh);
		}
		Component.SetMaterial(0, Material);
		if (Component.NumCustomDataFloats != UStageFigureAssetSubsystem::CustomDataCount)
		{
			Component.SetNumCustomDataFloats(UStageFigureAssetSubsystem::CustomDataCount);
		}

		TArray<FTransform> Transforms;
		Transforms.Reserve(People.Num());
		for (const StageAudienceMath::FPerson& Person : People)
		{
			Transforms.Add(Person.Transform);
		}
		Component.AddInstances(Transforms, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ false, /*bUpdateNavigation*/ false);

		TArray<float> Data;
		for (int32 Index = 0; Index < People.Num(); ++Index)
		{
			MakeCustomData(People[Index], Data);
			Component.SetCustomData(Index, Data, /*bMarkRenderStateDirty*/ false);
		}
		Component.MarkRenderStateDirty();
		return People.Num();
	}
}
