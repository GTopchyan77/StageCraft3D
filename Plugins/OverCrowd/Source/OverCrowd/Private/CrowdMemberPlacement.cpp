// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved
#include "CrowdMemberPlacement.h"
#include "OverCrowdRuntimeLibrary.h"

// Sets default values
ACrowdMemberPlacement::ACrowdMemberPlacement()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = false;

}

void ACrowdMemberPlacement::OnPropertyEdited_Implementation(FName PropteryName)
{
}

// Called when the game starts or when spawned
void ACrowdMemberPlacement::BeginPlay()
{
	Super::BeginPlay();
	//OnPropertyEdited(NAME_None);
}

// Called every frame
void ACrowdMemberPlacement::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

}

void ACrowdMemberPlacement::OverrideMaterialParams(UPARAM(ref)TArray<FMaterialParamData>& Master, UPARAM(ref)TArray<FMaterialParamData>& Slave, TArray<FMaterialParamData>& OverriddenParams)
{
	for (auto Param : Slave)
	{
		FMaterialParamData FoundParam;
		if (UOverCrowdRuntimeLibrary::GetMaterialParamWithTag(Master, Param.ParamName, FoundParam))
		{
			OverriddenParams.Add(FoundParam);
		}
		else
		{
			OverriddenParams.Add(Param);
		}
	}
}

void ACrowdMemberPlacement::SetCrowdMemberData(FCrowdMemberData InCrowdMemberData)
{
	CrowdMemberData = InCrowdMemberData;
	CrowdMemberData.ID = Guid;
	//RandomAnimationOffset = InCrowdMemberData.RandomAnimationOffset;
	AnimationState = InCrowdMemberData.AnimationState;
	AdditiveAnimationState = InCrowdMemberData.AdditiveAnimationState;
	
	UOverCrowdRuntimeLibrary::CalculateAndSetBitMasks(CrowdMemberData,UOverCrowdRuntimeLibrary::GetClothingMaskDataAsset(),8);
}


void ACrowdMemberPlacement::GetCrowdMemberData(FCrowdMemberData& OutCrowdMemberData)
{
	
	CrowdMemberData.AnimationState = AnimationState;
	CrowdMemberData.ID = Guid;
	//CrowdMemberData.RandomAnimationOffset = RandomAnimationOffset;
	//auto CacheScale = CrowdMemberData.Transform.GetScale3D();
	//CrowdMemberData.Transform.SetLocation(GetActorTransform().GetLocation());
	//CrowdMemberData.Transform.SetRotation(GetActorTransform().GetRotation());
	//CrowdMemberData.Transform.SetScale3D(CacheScale);
	CrowdMemberData.Transform = GetActorTransform();

#if WITH_EDITOR
	if (CrowdMemberData.BodyPartInstances.IsEmpty() && !WardrobeOverride.IsEmpty())
	{
		FWardrobe Wardrobe;
		auto WardrobeID = FGameplayTag::RequestGameplayTag(FName(*WardrobeOverride));
		UOverCrowdRuntimeLibrary::GetWardrobeFromTag(Wardrobe,WardrobeID);
		UOverCrowdRuntimeLibrary::CreateRandomCrowdMemberData(Wardrobe, CrowdMemberData);
		if (!AnimationOverride.IsEmpty())
		{
			CrowdMemberData.AnimationState = FGameplayTag::RequestGameplayTag(FName(*AnimationOverride));
			AnimationState = CrowdMemberData.AnimationState;
		}
		SetActorTransform(CrowdMemberData.Transform);
			

		//float Scale = FMath::FRandRange(Wardrobe.ScaleVariation.X, Wardrobe.ScaleVariation.Y);
		//FVector Scale3D(Scale, Scale, Scale);
		//CrowdMemberData.RandomAnimationOffset
		
	}
#endif
	
	OutCrowdMemberData = CrowdMemberData;
}

void ACrowdMemberPlacement::RecalculateBitMasks(UVatClothingMask* ClothingMask)
{
	if(UOverCrowdRuntimeLibrary::CalculateAndSetBitMasks(CrowdMemberData, ClothingMask, 8))
	{
		MarkPackageDirty();
	}
}

