// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved

#include "OverCrowdDataAsset.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Animation/Skeleton.h"

#if WITH_EDITOR
#include "MaterialEditingLibrary.h"
#include "AssetToolsModule.h"
#include "Factories/DataAssetFactory.h"
#endif

#include "OverCrowdRuntimeLibrary.h"
#include "VisualizeTexture.h"


bool UWardrobeCollection::AddOrUpdateWardrobe(FGameplayTag Key, const FWardrobe& Wardrobe)
{
	FWardrobe& Needle = BespokeWardrobes.FindOrAdd(Key);
	Needle = Wardrobe;
	Needle.WardrobeID = Key;

	return true;
}

bool UWardrobeCollection::Sanitize()
{
	bool bModified = false;
	
	for(auto &Wardrobe : BespokeWardrobes)
	{
		bModified |= Wardrobe.Value.Sanitize();
	}
	if(bModified)
		MarkPackageDirty();
	
	return bModified;
}

bool UWardrobeCollection::RemoveWardrobesNotInList(TArray<FGameplayTag> ToKeep)
{
	TArray<FGameplayTag> ToRemove;
	for(auto Wardrobe : BespokeWardrobes)
	{
		if(!ToKeep.Contains(Wardrobe.Key))
		{
			ToRemove.Add(Wardrobe.Key);
		}
	}
	for(auto Remove : ToRemove)
	{
		BespokeWardrobes.Remove(Remove);
	}
	
	return !ToRemove.IsEmpty();
}

bool UWardrobeAnimation::FindOrCreateMaterialReplacements(UVatMeshPairing* ForVatMesh, FMaterialReplacements& Out)
{
	if(!ForVatMesh)
		return false;
	
	FMaterialReplacements& TmpReplacements = MaterialReplacements.FindOrAdd(ForVatMesh);

#if WITH_EDITOR
	USkeletalMesh* TmpSkeletalMesh=  ForVatMesh->SkelMesh.LoadSynchronous();

	

	if(!TmpSkeletalMesh)
	{
		auto TmpStaticMesh=  ForVatMesh->VatStaticMesh.LoadSynchronous();
		if(!TmpStaticMesh)
			return false;
		
		auto Materials = TmpStaticMesh->GetStaticMaterials();

		if(TmpReplacements.Materials.Num() != Materials.Num())
			TmpReplacements.Materials.SetNum(Materials.Num());

		int32 Index = 0;
		for (auto Material : Materials)
		{
			UMaterialInterface* MaterialInterface = Material.MaterialInterface;

			if(MaterialInterface)
			{
				MaterialInterface = UOverCrowdRuntimeLibrary::GetOrCreateChildMaterial(MaterialInterface, UOverCrowdRuntimeLibrary::GetSanitizedWardrobeName(WardrobeID));
			}
			if(TmpReplacements.Materials[Index] != MaterialInterface)
			{
				TmpReplacements.Materials[Index] = MaterialInterface;
				MarkPackageDirty();
			}
			Index++;
		}
	}
	else
	{
		auto Materials = TmpSkeletalMesh->GetMaterials();

		//TmpReplacements.Materials.Empty();
		if(TmpReplacements.Materials.Num() != Materials.Num())
			TmpReplacements.Materials.SetNum(Materials.Num());

		int32 Index = 0;
		for (auto Material : Materials)
		{
			UMaterialInterface* MaterialInterface = Material.MaterialInterface;

			if(MaterialInterface)
			{
				MaterialInterface = UOverCrowdRuntimeLibrary::GetOrCreateChildMaterial(MaterialInterface, UOverCrowdRuntimeLibrary::GetSanitizedWardrobeName(WardrobeID));
			}
			if(TmpReplacements.Materials[Index] != MaterialInterface)
			{
				TmpReplacements.Materials[Index] = MaterialInterface;
				MarkPackageDirty();
			}
			Index++;
		}
	}
		
	
	
#endif

	Out = TmpReplacements;
	
	return true;
}

UVatAnimationDataAsset* UWardrobeAnimation::FindOrCreateAnimationAsset(UVatMeshPairing* ForVatMesh)
{
	if(!ForVatMesh)
		return nullptr;
	
	UVatAnimationDataAsset* Out = nullptr;
	
	auto Skel = ForVatMesh->GetSkelMeshSkeleton();
	if (!Skel)
		return nullptr;

	Out = AnimAssets.FindOrAdd(Skel);

	if(Out)
		return Out;

	FString SkeletonName = Skel->GetName();
	
	FString VatAnimName = "VA_" + SkeletonName + "_" + UOverCrowdRuntimeLibrary::GetSanitizedWardrobeName(WardrobeID);
	FString PackagePath = UOverCrowdRuntimeLibrary::GetPluginSaveDirectory() + "/" + VatAnimName;

	//FString PackagePath = FString::Printf(TEXT("%s/DataAssets/A2B_%s"), *UOverCrowdRuntimeLibrary::GetPluginSaveDirectory(), *WardrobeName);

	Out = LoadObject<UVatAnimationDataAsset>(nullptr, *PackagePath);

#if WITH_EDITOR
	if(!Out)
	{
		IAssetTools& AssetTools = FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	
		UFactory* DataAssetFactory = NewObject<UFactory>(GetTransientPackage(), UDataAssetFactory::StaticClass());
		//UObject* NewAsset = AssetTools.CreateAsset(VatAnimName, PackagePath, UVatAnimationDataAsset::StaticClass(), DataAssetFactory);
		UObject* NewAsset = AssetTools.CreateAsset(FString(FPathViews::GetCleanFilename(PackagePath)), FString(FPathViews::GetPath(PackagePath)), UVatAnimationDataAsset::StaticClass(), DataAssetFactory);
	
		Out = Cast<UVatAnimationDataAsset>(NewAsset);	
	}
#endif

	AnimAssets[Skel] = Out;
	return Out;
}

UAnimSequence* UVatAnimationDataAsset::GetAnimationSequenceWithTag(FGameplayTagContainer AnimationTags, int32& AnimSequenceIndex, int32& StartFrame, int32& EndFrame, float &SampleRate)
{
	AnimSequenceIndex = -1;
	if (!AnimationsToBake || Animations.Num() != AnimationsToBake->AnimSequences.Num())
	{
		return nullptr;
	}
	
	for (int i = 0; auto [AnimSequence, AnimationTag, Rate] : AnimationsToBake->AnimSequences)
	{
		if (AnimationTags.HasTag(AnimationTag))
		{
			StartFrame = Animations[i].StartFrame;
			EndFrame = Animations[i].EndFrame;
			SampleRate = Animations[i].SampleRate;
			AnimSequenceIndex = i;
			
			return AnimSequence;
		}
		i++;
	}
	return nullptr;
}


void FAnimationChain::GetBookEnds(FAnimationTagPair& BookEnd)
{
	TArray<FGameplayTag> AnimChainTags {AnimA,AnimB,AnimC,AnimD};
	
	BookEnd.From = AnimA;
	for(int i = AnimChainTags.Num() - 1; i >= 0; i--)
	{
		auto AnimChainTag = AnimChainTags[i];
		if (AnimChainTag.IsValid())
		{
			BookEnd.To = AnimChainTag;
			return;
		}
	}
}
#if WITH_EDITOR
void UAnimationsToBakeDataAsset::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	TMap<FGameplayTag,float> AnimationDurations;
	for (auto [AnimSequence, AnimationTag, SampleRate] : AnimSequences)
	{
		if (AnimationTag.IsValid() && AnimSequence)
		{
			float Duration = AnimSequence->GetPlayLength();
			
		 	AnimationDurations.FindOrAdd(AnimationTag);
			AnimationDurations[AnimationTag] = Duration;
		}
	}

	//incurring offsets for animations all but last
	for(auto& AnimChain : AnimationChains)
	{
		AnimChain.Value.IncurredAnimOffset = 0;
		TArray<FGameplayTag> AnimChainTags {AnimChain.Value.AnimA, AnimChain.Value.AnimB, AnimChain.Value.AnimC, AnimChain.Value.AnimD};
		bool IsFirstValid = false;
		for(int i = AnimChainTags.Num() - 1; i >= 0; i--)
		{
			auto AnimChainTag = AnimChainTags[i];
			if (AnimChainTag.IsValid())
			{
				if((IsFirstValid || i==0) && AnimationDurations.Contains(AnimChainTag))
				{
					AnimChain.Value.FirstAnimDuration = AnimationDurations[AnimChainTag];
					AnimChain.Value.IncurredAnimOffset += AnimChain.Value.FirstAnimDuration;
					//UE_LOG(LogOverCrowdEditor,Warning,TEXT("AnimChain.Value.FirstAnimDuration %f"), AnimChain.Value.FirstAnimDuration);
					//UE_LOG(LogOverCrowdEditor,Warning,TEXT("AnimChain.Value.IncurredAnimOffset %f"), AnimChain.Value.IncurredAnimOffset);
				}
				IsFirstValid = true;
			}
		}
	}
}
#endif

void UAnimationsToBakeDataAsset::GetCachedAnimationIndices(TMap<FGameplayTag, int32> &Out_AnimationIndices)
{
	int32 AnimIndex = 0; 
	for (auto [AnimSequence, AnimationTag, Rate] : AnimSequences)
	{
		Out_AnimationIndices.Add(AnimationTag, AnimIndex++);
	}
	for (auto AnimChain : AnimationChains)
	{
		Out_AnimationIndices.Add(AnimChain.Key, AnimIndex++);
	}
}

int32 UAnimationsToBakeDataAsset::GetAnimationIndex(FGameplayTag FindAnimationTag)
{
	int32 Res = 0;
	int32 AnimIndex = 0;

	for (auto [AnimSequence, AnimationTag, Rate] : AnimSequences)
	{
		if(FindAnimationTag == AnimationTag)
			return AnimIndex;

		AnimIndex++;
	}
	return Res;
}

void UAnimationsToBakeDataAsset::GetCachedAnimationChains(TMap<FAnimationTagPair, FGameplayTag> &Out_AnimationChains, TMap<FGameplayTag, FAnimationTagPair> &Out_ChainToBookEnds)
{
	for (auto AnimChain : AnimationChains)
	{
		FAnimationTagPair AnimTagPair;
		AnimChain.Value.GetBookEnds(AnimTagPair);

		Out_AnimationChains.Add(AnimTagPair, AnimChain.Key);

		Out_ChainToBookEnds.Add(AnimChain.Key, AnimTagPair);
	}
}

void UAnimationsToBakeDataAsset::GetCachedIncurredAnimOffsets(TMap<FGameplayTag, float>& Out_AnimationOffsets, TMap<FGameplayTag, float>& Out_FirstAnimationDurations)
{
	for (auto [AnimSequence, AnimationTag, Rate] : AnimSequences)
	{
		if (!AnimSequence)
		{
			//UE_LOG(L)
		}
		float Duration = AnimSequence->GetPlayLength();
		Out_FirstAnimationDurations.Add(AnimationTag, Duration);
	}
	for (auto AnimChain : AnimationChains)
	{
		Out_AnimationOffsets.Add(AnimChain.Key, AnimChain.Value.IncurredAnimOffset);
		Out_FirstAnimationDurations.Add(AnimChain.Key, AnimChain.Value.FirstAnimDuration);
	}
}

void UAnimationsToBakeDataAsset::GetFirstAnimationDuration(TMap<FGameplayTag, float>& Out_Durations)
{
}
//
// FCachedAnimationIndices UVatAnimationDataAsset::GetCachedAnimationIndices()
// {
// 	FCachedAnimationIndices Out;
// 	
// 	if(AnimationsToBake)
// 	{
// 		AnimationsToBake->GetCachedAnimationIndices(Out.PrimaryAnimationIndices);
// 		// for (int32 AnimIndex = 0; auto [AnimSequence, AnimationTag, Rate] : AnimationsToBake->AnimSequences)
// 		// {
// 		// 	for(auto Tag : AnimationTag.GetGameplayTagArray())
// 		// 	{
// 		// 		Out.PrimaryAnimationIndices.Add(Tag, AnimIndex);	
// 		// 	}
// 		//
// 		// 	AnimIndex++;
// 		// }	
// 	}
//
// 	if(AdditiveVatAnimationDataAsset && AdditiveVatAnimationDataAsset->AnimationsToBake)
// 	{
// 		AdditiveVatAnimationDataAsset->AnimationsToBake->GetCachedAnimationIndices(Out.AdditiveAnimationIndices);
// 		// for (int32 AnimIndex = 0; auto [AnimSequence, AnimationTag, Rate] : AdditiveVatAnimationDataAsset->AnimationsToBake->AnimSequences)
// 		// {
// 		// 	for(auto Tag : AnimationTag.GetGameplayTagArray())
// 		// 	{
// 		// 		Out.AdditiveAnimationIndices.Add(Tag, AnimIndex);	
// 		// 	}
// 		//
// 		// 	AnimIndex++;
// 		// }
// 	}
// 	
// 	return Out;
// }

void UVatClothingMask::GetBitMaskValue(UVatMeshPairing* DrivingMesh, UPARAM(ref)TArray<UVatMeshPairing*>& BodyPartMeshes, int32& BitMask) const
{
	BitMask = 0;
	FMaskingObjects MaskingObjects;
	if (!GetMaskingObjects(DrivingMesh, MaskingObjects))
	{
		return;
	}

	for (auto BodyPart : BodyPartMeshes)
	{
		if (BodyPart == DrivingMesh || !BodyPart)
			continue;

		for (FMaskSource& MaskSource : MaskingObjects.MaskSources)
		{
			if (MaskSource.SourcesForMask.Contains(BodyPart))
			{
				BitMask |= IndexToBitMaskValue(MaskSource.MaskIndex);
			}
		}
	}
}

int32 UVatClothingMask::IndexToBitMaskValue(int32 Index)
{
	//skipping the last bit of every byte
	if (Index >= 7)
		Index++;
	if (Index >= 15)
		Index++;
	if (Index >= 23)
		Index++;

	return static_cast<int32>(FMath::Pow(2.0, Index));
}

USkeleton* UVatMeshPairing::GetSkelMeshSkeleton()
{
	USkeleton *Res = Skeleton;
	if(!Res)
	{
		if(USkeletalMesh* Tmp = SkelMesh.LoadSynchronous())
		{
			Res = Tmp->GetSkeleton();
			Skeleton = Res;
			
			MarkPackageDirty();
		}
	}
	return Res;
}

bool FWardrobe::Sanitize()
{
	bool bModified = false;

	TArray<UVatMeshPairing*> ToMove;
	for(auto &Compartment : Compartments)
	{
		TArray<UVatMeshPairing*> ToRemove;
		bModified |= (Compartment.Value.TaggedVatAssets.Remove(nullptr) > 0);
		for(auto VatAsset :  Compartment.Value.TaggedVatAssets)
		{
			if(VatAsset && !VatAsset->BodyPart.MatchesTag(Compartment.Key))
			{
				ToRemove.Add(VatAsset);
			}
		}
		for(auto VatAsset : ToRemove)
		{
			ToMove.AddUnique(VatAsset);
			Compartment.Value.TaggedVatAssets.Remove(VatAsset);
			
			bModified = true;
		}
	}

	for(auto VatAsset : ToMove)
	{
		if(!VatAsset->BodyPart.MatchesTag(FGameplayTag::EmptyTag))
		{
			auto& [TaggedVatAssets] = Compartments.FindOrAdd(VatAsset->BodyPart);
			TaggedVatAssets.AddUnique(VatAsset);
		}
	}
	
	return bModified;
}

bool UVatClothingMask::Collate()
{
	bool bIsDirty = false;

	TArray<TSoftObjectPtr<UTexture2D> > TmpElements;

	for (auto [MaskTargets, MaskSources] : this->MaskingGroup)
	{
		for (auto [SourcesForMask, MaskTexture, MaskIndex] : MaskSources)
		{
			if (const int32 Index = TmpElements.AddUnique(MaskTexture); ElementsOfCompositedMask.Find(MaskTexture) != Index)
				bIsDirty = true;
		}
	}

	if(bIsDirty)
		ElementsOfCompositedMask = TmpElements;

	for (auto& [MaskTargets, MaskSources] : MaskingGroup)
	{
		for (auto& [SourcesForMask, MaskTexture, MaskIndex] : MaskSources)
		{
			if(const int32 Index = ElementsOfCompositedMask.Find(MaskTexture); MaskIndex != Index)
			{
				MaskIndex = Index;

				bIsDirty = true;
			}
		}
	}

	if(bIsDirty)
		MarkPackageDirty();

	return bIsDirty;
}
