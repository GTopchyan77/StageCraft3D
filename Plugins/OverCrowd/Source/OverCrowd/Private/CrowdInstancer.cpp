// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved

#include "CrowdInstancer.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "OverCrowdRuntimeLibrary.h"

// Sets default values
ACrowdInstancer::ACrowdInstancer()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;
}

void ACrowdInstancer::ClearAllComponents()
{
	for (auto Components = GetComponents(); UActorComponent* Component : Components)
	{
		UHierarchicalInstancedStaticMeshComponent* HISM = Cast<UHierarchicalInstancedStaticMeshComponent>(Component);
		if (HISM)
		{
			HISM->DestroyComponent();
		}
	}
	HISMLookupWithWardrobeID.Empty();
}

void ACrowdInstancer::AddHISMComponent(UVatMeshPairing* VatAsset, const FWardrobe &Wardrobe)
{
	if (!VatAsset)
	{
		return;
	}

	UStaticMesh* TmpStaticMesh = VatAsset->VatStaticMesh.LoadSynchronous();
	
	if (!TmpStaticMesh)
	{
		return;
	}

	TArray<UMaterialInterface*> OverrideMaterials;
	if(Wardrobe.WardrobeAnimation && Wardrobe.WardrobeAnimation->MaterialReplacements.Contains(VatAsset))
	{
		auto MaterialReplacements = Wardrobe.WardrobeAnimation->MaterialReplacements.Find(VatAsset);
		if (MaterialReplacements)
		{
			for(auto MaterialReplacement : MaterialReplacements->Materials)
			{
				OverrideMaterials.Add(MaterialReplacement);
			}
		}
	}

	TmpStaticMesh->SetNegativeBoundsExtension( {ExtraBoundsSize,ExtraBoundsSize,ExtraBoundsSize});
	TmpStaticMesh->SetPositiveBoundsExtension({ExtraBoundsSize,ExtraBoundsSize,ExtraBoundsSize});
	TmpStaticMesh->CalculateExtendedBounds();
	
	UHierarchicalInstancedStaticMeshComponent* HISM = Cast<UHierarchicalInstancedStaticMeshComponent>
		(AddComponentByClass(UHierarchicalInstancedStaticMeshComponent::StaticClass(), false, FTransform::Identity, false));

	HISM->SetupAttachment(RootComponent);
	HISM->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	HISM->SetLODDistanceScale(LODDistanceScale);
	HISM->SetNumCustomDataFloats(NumPerInstanceCustomData);
	HISM->SetStaticMesh(TmpStaticMesh);
	HISM->OverrideMaterials = OverrideMaterials;
	
	FHISMData HISMData;
	HISMData.HISM = HISM;
	HISMData.DrivingMesh = VatAsset;

	HISMLookupWithWardrobeID.FindOrAdd(Wardrobe.WardrobeID).HISMLookup.Add(VatAsset,HISMData);
}

void ACrowdInstancer::SetPerInstanceAnimationFrames(UInstancedStaticMeshComponent* HISM, int32 Index, int32 AnimIndexFrom, int32 AnimIndexTo, int32 AdditiveAnimIndexFrom, int32 AdditiveAnimIndexTo)
{
	HISM->SetCustomDataValue(Index, 2, AnimIndexFrom, true);
	HISM->SetCustomDataValue(Index, 3, AnimIndexTo, true);
	HISM->SetCustomDataValue(Index, 4, AdditiveAnimIndexFrom, true);
	HISM->SetCustomDataValue(Index, 5, AdditiveAnimIndexTo, true);
}

void ACrowdInstancer::ConfigureBodyPartInstanceOverrides(UInstancedStaticMeshComponent* HISM, int32 Index, const TArray<FMaterialParamData> &Overrides)
{
	for (auto Override : Overrides)
	{
		HISM->SetCustomDataValue(Index, Override.ParamIndex, Override.Value);
	}
}

void ACrowdInstancer::SetPlayrateOfInstance(UInstancedStaticMeshComponent* HISM, int32 Index, double Playrate)
{
	HISM->SetCustomDataValue(Index, 1, Playrate);
}

void ACrowdInstancer::ConfigureAnimationOffset(UInstancedStaticMeshComponent* HISM, int32 Index, double TimeOffset)
{
	HISM->SetCustomDataValue(Index, 6, TimeOffset);
	//HISM->SetCustomDataValue(Index, 7, AdditiveOffset);
}

void ACrowdInstancer::UpdateInstanceOverrides(FGuid Guid, const FCrowdMemberData& CrowdMemberData)
{
	for (auto BodyPart : CrowdMemberData.BodyPartInstances)
	{
		if (const auto HISMDtata = HISMLookupWithWardrobeID.FindOrAdd(CrowdMemberData.WardrobeID).HISMLookup.Find(BodyPart.Key))
		{
			if(!HISMDtata)
				continue;
			
			if (const auto InstanceIndex = HISMDtata->CrowdMemberInstanceIndices.Find(Guid))
			{
				ConfigureBodyPartInstanceOverrides(HISMDtata->HISM, *InstanceIndex, BodyPart.Value.Overrides);
				TArray<FGuid> Guids = { Guid };
				SwitchAnimation(Guids, CrowdMemberData.AnimationState, CrowdMemberData.AdditiveAnimationState);
			}
		}
		else
		{
			//TODO:: implement adding crowd member instance
			FWardrobe Wardrobe;
			UOverCrowdRuntimeLibrary::GetWardrobeFromTag(Wardrobe, CrowdMemberData.WardrobeID);
			AddHISMComponent(BodyPart.Key, Wardrobe);
		}
	}
}

void ACrowdInstancer::UpdateInstanceTransform(FGuid Guid, const FTransform& Transform)
{
	if (const auto CrowdMemberDataPtr = CrowdMembersData.Find(Guid))
	{
		(*CrowdMemberDataPtr).Transform = Transform;

		for (auto BodyPart : (*CrowdMemberDataPtr).BodyPartInstances)
		{
			if (const auto HismDtata = HISMLookupWithWardrobeID.FindOrAdd(CrowdMemberDataPtr->WardrobeID).HISMLookup.Find(BodyPart.Key))
			{
				if (const auto InstanceIndex = HismDtata->CrowdMemberInstanceIndices.Find(Guid))
				{
					ConfigureBodyPartInstanceOverrides(HismDtata->HISM, *InstanceIndex, BodyPart.Value.Overrides);
					HismDtata->HISM->UpdateInstanceTransform(*InstanceIndex, Transform, true);
				}
			}
		}
	}
}

void ACrowdInstancer::ShowHideCrowdMemeber(const FGuid& NPCGuid, bool Hide)
{
	if (const auto CrowdMemberDataPtr = CrowdMembersData.Find(NPCGuid))
	{
		FTransform Trans = (*CrowdMemberDataPtr).Transform;

		if (Hide)
			Trans.SetScale3D(FVector(0,0,0));

		for (auto BodyPart : (*CrowdMemberDataPtr).BodyPartInstances)
		{
			if (const auto HISMDtata = HISMLookupWithWardrobeID.FindOrAdd(CrowdMemberDataPtr->WardrobeID).HISMLookup.Find(BodyPart.Key))
			{
				if (auto InstanceIndex = HISMDtata->CrowdMemberInstanceIndices.Find(NPCGuid))
				{
					HISMDtata->HISM->UpdateInstanceTransform(*InstanceIndex, Trans, true);
					if (!Hide)
					{
						ConfigureBodyPartInstanceOverrides(HISMDtata->HISM, *InstanceIndex, BodyPart.Value.Overrides);
					}
				}
			}
		}
	}
}

void ACrowdInstancer::AddCrowdMemberInstance(FCrowdMemberData CrowdMemberData)
{
	for (auto BodyPart : CrowdMemberData.BodyPartInstances)
	{
		if (!BodyPart.Key)
			continue;

		auto HISMData = HISMLookupWithWardrobeID.FindOrAdd(CrowdMemberData.WardrobeID).HISMLookup.Find(BodyPart.Key);
		if (!HISMData)
		{
			FWardrobe Wardrobe;
			UOverCrowdRuntimeLibrary::GetWardrobeFromTag(Wardrobe, CrowdMemberData.WardrobeID);
			
			AddHISMComponent(BodyPart.Key, Wardrobe);
			
			HISMData = HISMLookupWithWardrobeID.FindOrAdd(CrowdMemberData.WardrobeID).HISMLookup.Find(BodyPart.Key);
		}
		if(!HISMData)
			continue;
		
		int32 InstanceIndex = HISMData->HISM->AddInstance(CrowdMemberData.Transform, true);

		if (CrowdMemberData.ID.IsValid())
		{
			HISMData->CrowdMemberInstanceIndices.Add(CrowdMemberData.ID, InstanceIndex);
		}
		
		int32 AnimIndexA = 0, AdditiveAnimIndexA = 0;
		float IncurredOffset = 0;
		float FirstAnimationDuration = 0;

		FGameplayTagContainer AnimState;
		AnimState.AddTag(CrowdMemberData.AnimationState);
		AnimState.AddTag(CrowdMemberData.AdditiveAnimationState);
		
		GetAnimationIndices(CrowdMemberData.WardrobeID, AnimState, AnimIndexA, AdditiveAnimIndexA, IncurredOffset, FirstAnimationDuration);
		ConfigureAnimationOffset(HISMData->HISM, InstanceIndex, CrowdMemberData.RandomAnimationOffset.X);
		SetPlayrateOfInstance(HISMData->HISM, InstanceIndex, 1.0f);
		SetPerInstanceAnimationFrames(HISMData->HISM, InstanceIndex, AnimIndexA, AnimIndexA, AdditiveAnimIndexA, AdditiveAnimIndexA);
		ConfigureBodyPartInstanceOverrides(HISMData->HISM, InstanceIndex, BodyPart.Value.Overrides);
	}
}

FGameplayTag ACrowdInstancer::GetAnimationPaths(FGameplayTag WardrobeID, FGameplayTag AnimationFrom,	FGameplayTag AnimationTo)
{
	FGameplayTag Res;
	if(!WardrobeID.IsValid() || !AnimationFrom.IsValid() || !AnimationTo.IsValid())
		return Res;
	
	auto CachedIndices = CachedAnimationIndices.Find(WardrobeID);
	if(CachedIndices != nullptr)
	{
		FAnimationTagPair BookEnd;

		if(CachedIndices->AnimationChainBookEnds.Contains(AnimationFrom))
		{
			BookEnd.From = CachedIndices->AnimationChainBookEnds.Find(AnimationFrom)->To;
		}
		else
		{
			BookEnd.From  = AnimationFrom;
		}
		
		if(CachedIndices->AnimationChainBookEnds.Contains(AnimationTo))
		{
			BookEnd.To = CachedIndices->AnimationChainBookEnds.Find(AnimationTo)->To;
		}
		else
		{
			BookEnd.To  = AnimationTo;
		}

		if(CachedIndices->AnimationChainTags.Contains(BookEnd))
		{
			Res = *CachedIndices->AnimationChainTags.Find(BookEnd);
		}
	}
	return Res;
}

void ACrowdInstancer::GetAnimationIndices(FGameplayTag WardrobeID, FGameplayTagContainer AnimationTags, int32& AnimIndex, int32& AdditiveAnimIndex, float &IncurredOffset, float &FisrtAnimDuration)
{
	if(!WardrobeID.IsValid() || !AnimationTags.IsValid())
		return;
	
	for(auto Tag : AnimationTags)
	{
		if(!Tag.IsValid())
			return;
	}
	auto TagArray = AnimationTags.GetGameplayTagArray();
	auto CachedIndices = CachedAnimationIndices.Find(WardrobeID);
	if(CachedIndices != nullptr)
	{
		for(auto Tag : TagArray)
		{
			if(CachedIndices->PrimaryAnimationIndices.Contains(Tag))
			{
				AnimIndex = *CachedIndices->PrimaryAnimationIndices.Find(Tag);
			}
			if(CachedIndices->IncurredAnimOffsets.Contains(Tag))
			{
				IncurredOffset = *CachedIndices->IncurredAnimOffsets.Find(Tag);
			}
			if(CachedIndices->FirstAnimDurations.Contains(Tag))
			{
				FisrtAnimDuration = *CachedIndices->FirstAnimDurations.Find(Tag);
			}
			if(CachedIndices->AdditiveAnimationIndices.Contains(Tag))
			{
				AdditiveAnimIndex = *CachedIndices->AdditiveAnimationIndices.Find(Tag);
			}
		}
	}
}

void ACrowdInstancer::BuildAnimationIndexLookup()
{
	TArray<FGameplayTag> WardrobeIDs;
	HISMLookupWithWardrobeID.GetKeys(WardrobeIDs);
	CachedAnimationIndices.Empty();
	
	for(auto WardrobeID : WardrobeIDs)
	{
		UWardrobeAnimation* WardrobeAnimation = UOverCrowdRuntimeLibrary::CreateOrLoadWardrobeAnimation(WardrobeID);
		if(WardrobeAnimation && WardrobeAnimation->AnimationsToBake)
		{
			FCachedAnimationIndices AnimIndices;
			WardrobeAnimation->AnimationsToBake->GetCachedAnimationIndices(AnimIndices.PrimaryAnimationIndices);
			WardrobeAnimation->AnimationsToBake->GetCachedIncurredAnimOffsets(AnimIndices.IncurredAnimOffsets, AnimIndices.FirstAnimDurations);
			WardrobeAnimation->AnimationsToBake->GetCachedAnimationChains(AnimIndices.AnimationChainTags, AnimIndices.AnimationChainBookEnds);

			for(auto VatAnim : WardrobeAnimation->AnimAssets)
			{
				if(VatAnim.Value && VatAnim.Value->AdditiveVatAnimationDataAsset && VatAnim.Value->AdditiveVatAnimationDataAsset->AnimationsToBake)
				{
					VatAnim.Value->AdditiveVatAnimationDataAsset->AnimationsToBake->GetCachedAnimationIndices(AnimIndices.AdditiveAnimationIndices);
					break;
				}
			}
			CachedAnimationIndices.Add(WardrobeID, AnimIndices);
		}
	}
}

void ACrowdInstancer::SetLODDistanceScaleFactor(float LODScale)
{
	LODDistanceScale = LODScale;
	
	for(auto HISMLookup: HISMLookupWithWardrobeID)
	{
	 	for(auto HISM: HISMLookup.Value.HISMLookup)
	 	{
	 		if(HISM.Value.HISM)
	 		{
	 			HISM.Value.HISM->SetLODDistanceScale(LODScale);
	 		}
	 	}
	}
}

void ACrowdInstancer::SwitchAnimation(UPARAM(ref)TArray<FGuid>& CrowdMemberGuids, FGameplayTag AnimationStateTo, FGameplayTag AdditiveAnimationStateTo)
{
	double GameTime = GetGameTimeSinceCreation();
	for (auto Guid : CrowdMemberGuids)
	{
		if (FCrowdMemberData* CrowdMemberDataPtr = CrowdMembersData.Find(Guid))
		{
			if(!CrowdMemberDataPtr)
				continue;

			FCrowdMemberData &CrowdMemberData = *CrowdMemberDataPtr;
			
			int32 AnimIndexFrom = 0, AdditiveAnimIndexFrom = 0, AnimIndexTo = 0, AdditiveAnimIndexTo = 0;
			float IncurredOffsetFrom = 0, IncurredOffsetTo = 0, FirstAnimationDurationFrom = 0, FirstAnimationDurationTo = 0 ;

			FGameplayTagContainer AnimationStateContainerFrom, AnimationStateContainerTo; 
			AnimationStateContainerFrom.AddTag(CrowdMemberData.AnimationState);
			if(CrowdMemberData.AdditiveAnimationState.IsValid())
				AnimationStateContainerFrom.AddTag(CrowdMemberData.AdditiveAnimationState);
			
			if(AdditiveAnimationStateTo.IsValid())
			{
				AnimationStateContainerTo.AddTag(AdditiveAnimationStateTo);	
			}
			
			FGameplayTag AnimChainTo = GetAnimationPaths(CrowdMemberData.WardrobeID, CrowdMemberData.AnimationState, AnimationStateTo);
			if(AnimChainTo.IsValid())
			{
				AnimationStateTo = AnimChainTo;
				CrowdMemberData.AnimationState = AnimChainTo;
				AnimationStateContainerTo.AddTag(AnimChainTo);
			}
			else
				AnimationStateContainerTo.AddTag(AnimationStateTo);
			
			GetAnimationIndices(CrowdMemberData.WardrobeID, AnimationStateContainerFrom,  AnimIndexFrom, AdditiveAnimIndexFrom,IncurredOffsetFrom, FirstAnimationDurationFrom);
			GetAnimationIndices(CrowdMemberData.WardrobeID, AnimationStateContainerTo,  AnimIndexTo, AdditiveAnimIndexTo,IncurredOffsetTo, FirstAnimationDurationTo);

			float PlayheadFromStart = GameTime - CrowdMemberData.AnimStartTimeStamp;
			float AnimOffset = FMath::Fmod(PlayheadFromStart - (IncurredOffsetFrom - CrowdMemberData.RandomAnimationOffset.X), FirstAnimationDurationTo );
			//if(AnimOffset < 0)
			//	continue;
			AnimOffset = FMath::Abs(AnimOffset);
			
			for (auto BodyPart : CrowdMemberData.BodyPartInstances)
			{
				if (const FHISMData* HISMData = HISMLookupWithWardrobeID.FindOrAdd(CrowdMemberData.WardrobeID).HISMLookup.Find(BodyPart.Key))
				{
					if(!HISMData)
						continue;
					
					if (const auto InstanceIndex = HISMData->CrowdMemberInstanceIndices.Find(Guid))
					{
						ConfigureAnimationOffset(HISMData->HISM, *InstanceIndex, AnimOffset);
						SetPerInstanceAnimationFrames(HISMData->HISM, *InstanceIndex, AnimIndexFrom, AnimIndexTo, AdditiveAnimIndexFrom, AdditiveAnimIndexTo);
						HISMData->HISM->SetCustomDataValue(*InstanceIndex, 0, GetWorld()->GetTimeSeconds(), true);
					}
				}
			}

			CrowdMemberData.AdditiveAnimationState = AdditiveAnimationStateTo;
			CrowdMemberData.AnimationState = AnimationStateTo;
			CrowdMemberData.AnimStartTimeStamp = GameTime;
			CrowdMemberData.RandomAnimationOffset.X = AnimOffset;
		}
	}
}

void ACrowdInstancer::BlendAnimation(UPARAM(ref)TArray<FGuid>& CrowdMemberGuids, FGameplayTag AnimationStateTo, FGameplayTag AdditiveAnimationStateTo)
{
	double GameTime = GetGameTimeSinceCreation();
	for (auto Guid : CrowdMemberGuids)
	{
		if (FCrowdMemberData* CrowdMemberDataPtr = CrowdMembersData.Find(Guid))
		{
			if (!CrowdMemberDataPtr)
				continue;

			FCrowdMemberData& CrowdMemberData = *CrowdMemberDataPtr;

			int32 AnimIndexFrom = 0, AdditiveAnimIndexFrom = 0, AnimIndexTo = 0, AdditiveAnimIndexTo = 0;
			FGameplayTagContainer AnimationStateContainerFrom, AnimationStateContainerTo;
			AnimationStateContainerFrom.AddTag(CrowdMemberData.AnimationState);
			if (CrowdMemberData.AdditiveAnimationState.IsValid())
				AnimationStateContainerFrom.AddTag(CrowdMemberData.AdditiveAnimationState);

			AnimationStateContainerTo.AddTag(AnimationStateTo);
			if (AdditiveAnimationStateTo.IsValid())
			{
				AnimationStateContainerTo.AddTag(AdditiveAnimationStateTo);
			}
			float Dummy;
			GetAnimationIndices(CrowdMemberData.WardrobeID, AnimationStateContainerFrom, AnimIndexFrom, AdditiveAnimIndexFrom, Dummy, Dummy);
			GetAnimationIndices(CrowdMemberData.WardrobeID, AnimationStateContainerTo, AnimIndexTo, AdditiveAnimIndexTo, Dummy, Dummy);
			
			for (auto BodyPart : CrowdMemberData.BodyPartInstances)
			{
				if (const FHISMData* HISMData = HISMLookupWithWardrobeID.FindOrAdd(CrowdMemberData.WardrobeID).HISMLookup.Find(BodyPart.Key))
				{
					if (!HISMData)
						continue;

					if (const auto InstanceIndex = HISMData->CrowdMemberInstanceIndices.Find(Guid))
					{
						SetPerInstanceAnimationFrames(HISMData->HISM, *InstanceIndex, AnimIndexFrom, AnimIndexTo, AdditiveAnimIndexFrom, AdditiveAnimIndexTo);
						HISMData->HISM->SetCustomDataValue(*InstanceIndex, 0, GetWorld()->GetTimeSeconds(), true);
					}
				}
			}
			CrowdMemberData.PreviousAnimationState = CrowdMemberData.AnimationState;
			CrowdMemberData.AdditiveAnimationState = AdditiveAnimationStateTo;
			CrowdMemberData.AnimationState = AnimationStateTo;
			CrowdMemberData.AnimStartTimeStamp = GameTime;
			
		}
	}
}

void ACrowdInstancer::ReturnToPreviousAnimation(UPARAM(ref)TArray<FGuid>& CrowdMemberGuids)
{
	for (auto Guid : CrowdMemberGuids)
	{
		if (FCrowdMemberData* CrowdMemberDataPtr = CrowdMembersData.Find(Guid))
		{
			if (!CrowdMemberDataPtr)
				continue;

			FCrowdMemberData& CrowdMemberData = *CrowdMemberDataPtr;

			auto Tmp = CrowdMemberData.PreviousAnimationState;
			CrowdMemberData.PreviousAnimationState = CrowdMemberData.AnimationState;
			CrowdMemberData.AnimationState = Tmp;
		}
	}
	BlendAnimationFromPreviousState(CrowdMemberGuids);
}

void ACrowdInstancer::BlendAnimationFromPreviousState(UPARAM(ref)TArray<FGuid>& CrowdMemberGuids)
{
	double GameTime = GetGameTimeSinceCreation();
	for (auto Guid : CrowdMemberGuids)
	{
		if (FCrowdMemberData* CrowdMemberDataPtr = CrowdMembersData.Find(Guid))
		{
			if (!CrowdMemberDataPtr)
				continue;

			FCrowdMemberData& CrowdMemberData = *CrowdMemberDataPtr;

			int32 AnimIndexFrom = 0, AdditiveAnimIndexFrom = 0, AnimIndexTo = 0, AdditiveAnimIndexTo = 0;
			FGameplayTagContainer AnimationStateContainerFrom, AnimationStateContainerTo;
			AnimationStateContainerFrom.AddTag(CrowdMemberData.PreviousAnimationState);
			//if (CrowdMemberData.AdditiveAnimationState.IsValid())
			//	AnimationStateContainerFrom.AddTag(CrowdMemberData.AdditiveAnimationState);

			AnimationStateContainerTo.AddTag(CrowdMemberData.AnimationState);
			/*if (AdditiveAnimationStateTo.IsValid())
			{
				AnimationStateContainerTo.AddTag(AdditiveAnimationStateTo);
			}*/
			float Dummy;
			GetAnimationIndices(CrowdMemberData.WardrobeID, AnimationStateContainerFrom, AnimIndexFrom, AdditiveAnimIndexFrom, Dummy, Dummy);
			GetAnimationIndices(CrowdMemberData.WardrobeID, AnimationStateContainerTo, AnimIndexTo, AdditiveAnimIndexTo, Dummy, Dummy);

			for (auto BodyPart : CrowdMemberData.BodyPartInstances)
			{
				if (const FHISMData* HISMData = HISMLookupWithWardrobeID.FindOrAdd(CrowdMemberData.WardrobeID).HISMLookup.Find(BodyPart.Key))
				{
					if (!HISMData)
						continue;

					if (const auto InstanceIndex = HISMData->CrowdMemberInstanceIndices.Find(Guid))
					{
						SetPerInstanceAnimationFrames(HISMData->HISM, *InstanceIndex, AnimIndexFrom, AnimIndexTo, AdditiveAnimIndexFrom, AdditiveAnimIndexTo);
						HISMData->HISM->SetCustomDataValue(*InstanceIndex, 0, GetWorld()->GetTimeSeconds(), true);
					}
				}
			}
			/*CrowdMemberData.PreviousAnimationState = CrowdMemberData.AnimationState;
			CrowdMemberData.AdditiveAnimationState = AdditiveAnimationStateTo;
			CrowdMemberData.AnimationState = AnimationStateTo;*/
			CrowdMemberData.AnimStartTimeStamp = GameTime;

		}
	}
}

TArray<FGuid> ACrowdInstancer::GetGuidsInRange(FBox Extent)
{
	TArray<FGuid> Out;
	CrowdOctree.FindElementsWithBoundsTest(Extent, [&Out](const FCrowdMemberOctreeElement& CrowdElement)
	{
		Out.Add(CrowdElement.Guid);
	});

	return Out;
}

void ACrowdInstancer::GetGuidsInsideBoxes(TArray<FBox> Boxes, TSet<FGuid>& OutGuids)
{
	for(auto Box : Boxes)
	{
		CrowdOctree.FindElementsWithBoundsTest(Box, [&OutGuids](const FCrowdMemberOctreeElement& CrowdElement)
		{
			OutGuids.Add(CrowdElement.Guid);
		});	
	}
}

void ACrowdInstancer::InitOctree(const TMap<FGuid, FVector>& InLocations, FBox Extent)
{
	CrowdOctree = FCrowdMemberOctree(Extent.GetCenter(), Extent.GetExtent().GetMax());
	
	for (auto Location : InLocations)
	{
		const FVector &Pos = Location.Value;
		FBox Box(FVector(Pos.X - 1.0, Pos.Y - 1.0, Pos.Z - 1.0), FVector(Pos.X + 1.0, Pos.Y + 1.0, Pos.Z + 1.0));
		FCrowdMemberOctreeElement Element(Location.Key,Box);

		CrowdOctree.AddElement(Element);
	}
}

bool ACrowdInstancer::ShouldTickIfViewportsOnly() const
{
	return true;
}

void ACrowdInstancer::BeginPlay()
{
	Super::BeginPlay();
}

void ACrowdInstancer::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	
}

