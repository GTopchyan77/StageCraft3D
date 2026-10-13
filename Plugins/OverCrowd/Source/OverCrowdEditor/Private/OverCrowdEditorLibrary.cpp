// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved

#include "OverCrowdEditorLibrary.h"
#include "OverCrowdEditor.h"
#include "OverCrowdUtils.h"
#include "OverCrowdSkeletalMesh.h"

#include "MeshUtilities.h"
#include "Modules/ModuleManager.h"
#include "Engine/SkeletalMesh.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimSequence.h"
#include "Math/Vector.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Math/NumericLimits.h"
#include "MeshDescription.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialEditingLibrary.h"
#include "Misc/ScopedSlowTask.h"
#include "StaticMeshAttributes.h"
#include "AssetToolsModule.h"
#include "GameplayTagsEditorModule.h"
#include "GameplayTagsManager.h"
#include "GameplayTagsSettings.h"
#include "IContentBrowserSingleton.h"
#include "ContentBrowserModule.h"
#include "Components/PoseableMeshComponent.h"
#include "Framework/Application/SlateApplication.h"
#include "GameplayTagContainer.h"
#include "ToolMenuSection.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "PackageTools.h"
#include "ObjectTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "LODUtilities.h"
#include "OverCrowdRuntimeLibrary.h"
#include "Engine/SkeletalMeshLODSettings.h"
#include "Windows/WindowsPlatformApplicationMisc.h"
#include "EditorUtilitySubsystem.h"
#include "EditorUtilityWidgetBlueprint.h"
#include "EditorUtilityBlueprint.h"

#define LOCTEXT_NAMESPACE "OverCrowdEditor"

namespace CVars
{
	bool bPrintBoneIndices = false;
	FAutoConsoleVariableRef CvarStruct
	(	
		TEXT("vat.PrintBoneIndices"), bPrintBoneIndices,
		TEXT("no help"), ECVF_Default
	);
}
void PrintBoneIndices(const USkeletalMesh* SkeletalMesh)
{
	if (!SkeletalMesh)
	{
		return;
	}

	const FReferenceSkeleton& GlobalRefSkeleton = SkeletalMesh->GetSkeleton()->GetReferenceSkeleton();
	const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();
	int32 NumGlobalBones = GlobalRefSkeleton.GetNum();

	UE_LOG(LogOverCrowdEditor, Warning, TEXT("SKELETON NAME: %s"), *SkeletalMesh->GetSkeleton()->GetName());
	for (int32 i = 0; i < NumGlobalBones; i++)
	{
		FString GlobalBoneName = GlobalRefSkeleton.GetBoneName(i).ToString();
		FString RefBoneName;

		if (i < RefSkeleton.GetNum())
		{
			RefBoneName = RefSkeleton.GetBoneName(i).ToString();
		}

		UE_LOG(LogOverCrowdEditor, Warning, TEXT("Bone Index: %d Global: %s   Ref: %s"), i, *GlobalBoneName, *RefBoneName);

	}
}

using namespace OverCrowd_Private;

UOverCrowdEditorLibrary::UOverCrowdEditorLibrary(const FObjectInitializer& ObjectInitializer)
: Super(ObjectInitializer)
{

}

int32 UOverCrowdEditorLibrary::GetBoneIndex(const USkeleton* Skeleton, FName BoneName)
{
	if (!Skeleton)
	{
		return 0;
	}
	const FReferenceSkeleton& GlobalRefSkeleton = Skeleton->GetReferenceSkeleton();

	return GlobalRefSkeleton.FindBoneIndex(BoneName);

}

bool UOverCrowdEditorLibrary::BakeAnimationToTextures(UVatAnimationDataAsset* VatAnimDataAsset, const TArray<UVatMeshPairing*>& SourceSkelMeshes, FName PartialBoneName, TArray<UVatMeshPairing*>& OutDestMeshes)
{
	if (!VatAnimDataAsset || !VatAnimDataAsset->AnimationsToBake)
	{
		return false;
	}
	bool bIsDirty = false;

	TArray<FSkelMeshToStaticMesh> MeshesToConvert;

	if (SourceSkelMeshes.Num())
	{
		MeshesToConvert.Empty();

		for (UVatMeshPairing* DmAsset : SourceSkelMeshes)
		{
			if (!DmAsset)
				continue;
			
			FSkelMeshToStaticMesh ToAdd;
			ToAdd.ConvertedMesh = Cast<UStaticMesh>(DmAsset->VatStaticMesh.LoadSynchronous());
			ToAdd.SourceMesh = Cast<USkeletalMesh>(DmAsset->SkelMesh.LoadSynchronous());

			if (ToAdd.ConvertedMesh && ToAdd.SourceMesh)
			{
				// if(PartialBoneName == NAME_None && DmAsset->BasePoseIndex != MeshesToConvert.Num())
				// {
				// 	DmAsset->BasePoseIndex = MeshesToConvert.Num();
				// 	DmAsset->MarkPackageDirty();
				// }
				// else if(PartialBoneName != NAME_None && DmAsset->AdditiveBasePoseIndex != MeshesToConvert.Num())
				// {
				// 	DmAsset->AdditiveBasePoseIndex = MeshesToConvert.Num();
				// 	DmAsset->MarkPackageDirty();
				// }
				
				MeshesToConvert.Add(ToAdd);
				OutDestMeshes.Add(DmAsset);
			}
		}
	}

	USkeletalMesh * AnimationRefrenceMesh = nullptr;
	AnimationRefrenceMesh = VatAnimDataAsset->AnimationsToBake->AnimationReferenceSkeleton.LoadSynchronous();

	if (TArray<FOverCrowdAnimSequenceInfo> Dummy; !GetValidAnimSequences(VatAnimDataAsset, MeshesToConvert, Dummy) )
	{
		return false;
	}

	TMap <USkinnedAsset*, TArray<FVector3f> > BoneRefPositionsMap;
	TMap <USkinnedAsset*, TArray<FVector4f> > BoneRefRotationsMap;

	TArray<FVector3f> BonePositions;
	TArray<FVector4f> BoneRotations;

	TArray<FVector3f> ConvertedRefPositions;
	TArray<FVector4f> ConvertedRefRotations;

	int32 NumBones = 0;
	int32 NumFrames = 0;
	int32 NumBasePoses = 1;// MeshesToConvert.Num();

	for (auto Tmp : MeshesToConvert)
	{
		auto SourceMesh = Tmp.SourceMesh;
		 
		if (!SourceMesh)
			continue;

		auto& BoneRefPositions = BoneRefPositionsMap.Add(SourceMesh);
		auto& BoneRefRotations = BoneRefRotationsMap.Add(SourceMesh);

		int32 NumLocalBones = GetRefBonePositionsAndRotations(SourceMesh, BoneRefPositions, BoneRefRotations);
		NumBones = FMath::Max(SourceMesh->GetSkeleton()->GetReferenceSkeleton().GetNum(), NumBones);

		MapLocalBonesToGlobalBones<FVector3f>(SourceMesh, BoneRefPositions, ConvertedRefPositions);
		MapLocalBonesToGlobalBones<FVector4f>(SourceMesh, BoneRefRotations, ConvertedRefRotations);
		
		if (CVars::bPrintBoneIndices)
		{
			PrintBoneIndices(SourceMesh);
		}
	}
	
	BonePositions.Append(ConvertedRefPositions);
	BoneRotations.Append(ConvertedRefRotations);

	check(GEditor);
	UWorld* World = GEditor->GetEditorWorldContext().World();
	check(World);
	
	TArray< USkeletalMeshComponent*> SkeletalMeshComponents;
	TArray< UPoseableMeshComponent*> DynamicMeshComponents;
	TArray< AActor*> ActorsToDestroy;

	for (auto Tmp : MeshesToConvert)
	{
		//if (Tmp.SourceMesh)
		{
			AActor* Actor = World->SpawnActor<AActor>();
			check(Actor);

			Tmp.SourceMesh->SetMinLod(0);

			USkeletalMeshComponent* SkeletalMeshComponent = NewObject<USkeletalMeshComponent>(Actor);
			check(SkeletalMeshComponent);
			SkeletalMeshComponent->SetSkeletalMesh(Tmp.SourceMesh);
			SkeletalMeshComponent->OverrideMinLOD(0);
			SkeletalMeshComponent->SetForcedLOD(0); // Force to LOD0;
			SkeletalMeshComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);
			SkeletalMeshComponent->SetUpdateAnimationInEditor(true);
			SkeletalMeshComponent->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
			SkeletalMeshComponent->RegisterComponent();

			SkeletalMeshComponents.Add(SkeletalMeshComponent);

			auto DynamicMeshComponent = NewObject<UPoseableMeshComponent>(Actor);
			DynamicMeshComponent->RegisterComponent();
			//DynamicMeshComponent->SetSkeletalMesh(SkeletalMeshComponent->GetSkeletalMeshAsset());
			DynamicMeshComponent->SetSkinnedAssetAndUpdate(SkeletalMeshComponent->GetSkeletalMeshAsset());
			DynamicMeshComponent->AllocateTransformData();
			
			DynamicMeshComponents.Add(DynamicMeshComponent);
			ActorsToDestroy.Add(Actor);
		}
	}

	if (VatAnimDataAsset->Animations.Num() != VatAnimDataAsset->AnimationsToBake->AnimSequences.Num() )//|| VatAnimDataAsset->NumBasePoses != NumBasePoses)
	{
		//VatAnimDataAsset->NumBasePoses = NumBasePoses;
		VatAnimDataAsset->Animations.SetNum(VatAnimDataAsset->AnimationsToBake->AnimSequences.Num());
		bIsDirty = VatAnimDataAsset->MarkPackageDirty();
	}
	
	int AnimIndex = 0;
	for (FOverCrowdAnimSequenceInfo& AnimSequenceInfo : VatAnimDataAsset->AnimationsToBake->AnimSequences)
	{
		UAnimSequence* AnimSequence = AnimSequenceInfo.AnimSequence;

		if (!AnimSequence)
		{
			continue;
		}

		USkeletalMeshComponent* ReferenceMeshComponent = nullptr;

		if(AnimationRefrenceMesh || AnimSequence->GetPreviewMesh())
		{
			AActor* Actor = World->SpawnActor<AActor>();
			ActorsToDestroy.Add(Actor);
			ReferenceMeshComponent = NewObject<USkeletalMeshComponent>(Actor);
			check(ReferenceMeshComponent);

			if(AnimationRefrenceMesh)
			{
				ReferenceMeshComponent->SetSkeletalMesh(AnimationRefrenceMesh);
				ReferenceMeshComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);
				ReferenceMeshComponent->SetUpdateAnimationInEditor(true);
				ReferenceMeshComponent->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
				ReferenceMeshComponent->RegisterComponent();
				ReferenceMeshComponent->SetAnimation(AnimSequence);
			}
			else
			{
				AnimationRefrenceMesh = AnimSequence->GetPreviewMesh();
				ReferenceMeshComponent->SetSkeletalMesh(AnimationRefrenceMesh);
				VatAnimDataAsset->AnimationsToBake->AnimationReferenceSkeleton = AnimationRefrenceMesh;

				bIsDirty = VatAnimDataAsset->AnimationsToBake->MarkPackageDirty();
			}
		}
		
		for (auto SkeletalMeshComponent : SkeletalMeshComponents)
		{
			SkeletalMeshComponent->SetAnimation(AnimSequence);
		}
		
		int32 AnimStartFrame;
		int32 AnimEndFrame;
		int32 SampleIndex = 0;
		//float SampleRate = 30;
		const float SampleInterval = 1.f / AnimSequenceInfo.SampleRate;

		const int32 AnimNumFrames = GetAnimationFrameRange(AnimSequenceInfo, AnimStartFrame, AnimEndFrame);
		const float AnimStartTime = AnimSequence->GetTimeAtFrame(AnimStartFrame);

		FFormatNamedArguments Args;
		Args.Add(TEXT("AnimSequence"), FText::FromString(*AnimSequence->GetFName().ToString()));
		Args.Add(TEXT("AnimIndex"), AnimIndex);
		Args.Add(TEXT("NumAnimSequences"), VatAnimDataAsset->AnimationsToBake->AnimSequences.Num());
		FScopedSlowTask AnimProgressBar(AnimNumFrames, FText::Format(LOCTEXT("ProcessingAnimSequence", "Processing AnimSequence: {AnimSequence} [{AnimIndex}/{NumAnimSequences}]"), Args), true /*Enabled*/);
		AnimProgressBar.MakeDialog(false /*bShowCancelButton*/, false /*bAllowInPIE*/);

		while (SampleIndex < AnimNumFrames)
		{
			AnimProgressBar.EnterProgressFrame();

			TArray<FVector3f> ConvertedBoneFramePositions;
			TArray<FVector4f> ConvertedBoneFrameRotations;

			const float Time = AnimStartTime + ((float)SampleIndex * SampleInterval);
			SampleIndex++;
			int Count = 0;
			for (auto SkeletalMeshComponent : SkeletalMeshComponents)
			{
				TArray<FVector3f> BoneFramePositions;
				TArray<FVector4f> BoneFrameRotations;

				USkeletalMeshComponent* AnimatedSkelMeshComp = ReferenceMeshComponent;
				if (!AnimatedSkelMeshComp)
				{
					AnimatedSkelMeshComp = SkeletalMeshComponent;
				}

				AnimatedSkelMeshComp->SetPosition(Time);
				AnimatedSkelMeshComp->TickAnimation(0.f, false);
				AnimatedSkelMeshComp->RefreshBoneTransforms();
				
				auto& BoneRefPositions = BoneRefPositionsMap[SkeletalMeshComponent->GetSkinnedAsset()];
				auto& BoneRefRotations = BoneRefRotationsMap[SkeletalMeshComponent->GetSkinnedAsset()];
				
				auto DynamicMeshComponent = DynamicMeshComponents[Count];
				
				DynamicMeshComponent->CopyPoseFromSkeletalComponent(AnimatedSkelMeshComp);
				DynamicMeshComponent->FillComponentSpaceTransforms();
				DynamicMeshComponent->FinalizeBoneTransform();

				if(VatAnimDataAsset->AdditveRootBoneName != NAME_None)
				{
					const FReferenceSkeleton& RefSkeleton = SkeletalMeshComponent->GetSkeletalMeshAsset()->GetRefSkeleton();
					int32 BoneIndex = RefSkeleton.FindBoneIndex(VatAnimDataAsset->AdditveRootBoneName);
					VatAnimDataAsset->AdditveRootBoneIndex = BoneIndex;
				}
				
				if (PartialBoneName != NAME_None)
				{
					TArray<int32> BoneIndices;
					const FReferenceSkeleton& RefSkeleton = SkeletalMeshComponent->GetSkeletalMeshAsset()->GetRefSkeleton();

					int32 BoneIndex = RefSkeleton.FindBoneIndex(PartialBoneName);

					while (BoneIndex != INDEX_NONE)
					{
						BoneIndices.Add(BoneIndex);
						BoneIndex = RefSkeleton.GetParentIndex(BoneIndex);
					}

					for (int32 i = BoneIndices.Num() - 1; i >= 0; i--)
					{
						BoneIndex = BoneIndices[i];
						DynamicMeshComponent->SetBoneLocationByName(PartialBoneName, FVector(BoneRefPositions[BoneIndex].X, BoneRefPositions[BoneIndex].Y, BoneRefPositions[BoneIndex].Z), EBoneSpaces::WorldSpace);
						DynamicMeshComponent->SetBoneRotationByName(PartialBoneName, FRotator(FQuat(FVector(BoneRefRotations[BoneIndex].X, BoneRefRotations[BoneIndex].Y, BoneRefRotations[BoneIndex].Z), BoneRefRotations[BoneIndex].W)), EBoneSpaces::WorldSpace);
					}

					DynamicMeshComponent->FillComponentSpaceTransforms();
					DynamicMeshComponent->FinalizeBoneTransform();
				}

				GetBonePositionsAndRotations(DynamicMeshComponent, BoneRefPositions, BoneFramePositions, BoneFrameRotations);

				MapLocalBonesToGlobalBones<FVector3f>(DynamicMeshComponent->GetSkinnedAsset(), BoneFramePositions, ConvertedBoneFramePositions);
				MapLocalBonesToGlobalBones<FVector4f>(DynamicMeshComponent->GetSkinnedAsset(), BoneFrameRotations, ConvertedBoneFrameRotations);
				
				Count++;
			}

			BonePositions.Append(ConvertedBoneFramePositions);
			BoneRotations.Append(ConvertedBoneFrameRotations);
		} 

		FOverCrowdAnimInfo NewAnimInfo;
		NewAnimInfo.StartFrame = NumFrames;
		NewAnimInfo.EndFrame = NumFrames + AnimNumFrames - 1;
		NewAnimInfo.SampleRate = AnimSequenceInfo.SampleRate;
		NewAnimInfo.AnimationTag = AnimSequenceInfo.AnimationTag;

		FOverCrowdAnimInfo &OldAnimInfo = VatAnimDataAsset->Animations[AnimIndex];

		if (	OldAnimInfo.StartFrame != NewAnimInfo.StartFrame
			||	OldAnimInfo.EndFrame != NewAnimInfo.EndFrame
			||	OldAnimInfo.AnimationTag != NewAnimInfo.AnimationTag
			||	OldAnimInfo.SampleRate != NewAnimInfo.SampleRate)
		{
			VatAnimDataAsset->Animations[AnimIndex] = NewAnimInfo;
			bIsDirty = VatAnimDataAsset->AnimationsToBake->MarkPackageDirty();
			bIsDirty = VatAnimDataAsset->MarkPackageDirty();
		}

		if(ReferenceMeshComponent)
		{
			ReferenceMeshComponent->UnregisterComponent();
			ReferenceMeshComponent->DestroyComponent();
		}

		NumFrames += AnimNumFrames;
		AnimIndex++;
	} 

	
	for (auto SkeletalMeshComponent : SkeletalMeshComponents)
	{
		SkeletalMeshComponent->UnregisterComponent();
		SkeletalMeshComponent->DestroyComponent();
	}
	for (auto DynamicMeshComponent : DynamicMeshComponents)
	{
		DynamicMeshComponent->UnregisterComponent();
		DynamicMeshComponent->DestroyComponent();
	}
	for (auto Actor : ActorsToDestroy)
	{
		Actor->Destroy();
	}

	int32 Height, Width;
	// Note we are adding +NumBasePoses frame for the ref poses and +1 for animation info
	auto MaxEl = FMath::Max(NumBones, (VatAnimDataAsset->AnimationsToBake->AnimSequences.Num() * 3));
	if (!FindBestResolution(NumFrames + NumBasePoses + 2, MaxEl, Height, Width))
	{
		UE_LOG(LogOverCrowdEditor, Warning, TEXT("Bone Animation data cannot be fit in a %ix%i texture."), Height, Width);
		return false;
	}
	
	TArray<FVector4f> NormalizedBoneRotations;
	NormalizeBoneData(BoneRotations, NormalizedBoneRotations);
	
	WriteUnNormalizedVectorsToTexture(VatAnimDataAsset, BonePositions, NumFrames + NumBasePoses, Height, Width * 3, VatAnimDataAsset->GetBonePositionTexture());
	WriteVectorsToTexture<FVector4f, FHighPrecision>(NormalizedBoneRotations, NumFrames + NumBasePoses, Height, Width, VatAnimDataAsset->GetBoneRotationTexture());

	return true;
}

bool UOverCrowdEditorLibrary::SanitizeForMask(UTexture2D* Texture)
{
	bool bModified = false;

	if (Texture->CompressionSettings == TextureCompressionSettings::TC_VectorDisplacementmap &&
		Texture->MipGenSettings == TextureMipGenSettings::TMGS_NoMipmaps &&
		Texture->SRGB == false)
	{
		return false;
	}

	Texture->CompressionSettings = TextureCompressionSettings::TC_VectorDisplacementmap;
	Texture->MipGenSettings = TextureMipGenSettings::TMGS_NoMipmaps;
	Texture->SRGB = false;
	Texture->UpdateResource();

	return true;
}

bool UOverCrowdEditorLibrary::WriteCrowdMemberDataToTexture(const TArray<FCrowdMemberData> &CrowdMembers, const TMap<FGameplayTag, FWardrobeVatAssetPairArray>& SortedBodyParts,
	const TArray<FGameplayTag> &WardrobeIndices, UTexture2D *OutTexture)
{
	FTexturePlatformData* PlatformData = OutTexture->GetPlatformData();

	int32 Width = 2048;
	int32 Height = CrowdMembers.Num()/64 +2;

	TArray<FColor> Pixels;
	Pixels.Init(FColor(0, 0, 0, 0), Width * Height);

	TMap<FGameplayTag, int32> BodyPartToIndex;
	for(int32 Index = 0; auto BodyPart : SortedBodyParts)
	{
		BodyPartToIndex.Add(BodyPart.Key, Index++);
	}

	
	for(int Index = 0; auto CrowdMember : CrowdMembers)
	{
		FCrowdMemberPackedData PackedData;
		PackedData.Init();
		
		PackedData.PositionX = (int32)(CrowdMember.Transform.GetLocation().X * 1000.0f);
		PackedData.PositionY = (int32)(CrowdMember.Transform.GetLocation().Y * 1000.0f);
		PackedData.PositionZ = (int32)(CrowdMember.Transform.GetLocation().Z * 1000.0f);

		UE::Math::TQuat<double> Rot = CrowdMember.Transform.GetRotation();

		PackedData.RotationX = Rot.X * 1000.0f;
		PackedData.RotationY = Rot.Y * 1000.0f;
		PackedData.RotationZ = Rot.Z * 1000.0f;
		PackedData.RotationW = Rot.W * 1000.0f;

		PackedData.Scale = CrowdMember.Transform.GetScale3D().X * 1000.0f;

		PackedData.RandomAnimationOffset = CrowdMember.RandomAnimationOffset.X * 1000.0f;

		FGameplayTag WardrobeID = CrowdMember.WardrobeID;
		if(!WardrobeID.IsValid())
		{
			UE_LOG(LogOverCrowdEditor,Error, TEXT("UOverCrowdEditorLibrary::WriteCrowdMemberDataToTexture --- WardrobeID is invalid"));
		}
		else
		{
			if(WardrobeIndices.Contains(WardrobeID))
			{
				PackedData.AnimationState.R = (int32)WardrobeIndices.Find(WardrobeID);	
			}
			
			FWardrobe Wardrobe;
			UOverCrowdRuntimeLibrary::GetWardrobeFromTag(Wardrobe,WardrobeID);
			if(Wardrobe.WardrobeAnimation && Wardrobe.WardrobeAnimation->AnimationsToBake)
			{
				PackedData.AnimationState.G = Wardrobe.WardrobeAnimation->AnimationsToBake->GetAnimationIndex(CrowdMember.AnimationState);
			}
		}

		for(int32 BodyPartCount = 0;auto BodyPart : CrowdMember.BodyPartInstances)
		{
			if(!BodyPart.Key)
				continue;
			
			FWardrobeVatAssetPair SearchFor;
			SearchFor.WardrobeID = WardrobeID;
			SearchFor.VatAsset = BodyPart.Key;

			FGameplayTag BodyPartID = BodyPart.Key->BodyPart;
			if(SortedBodyParts.Contains(BodyPartID))
			{
				auto MeshIndex = SortedBodyParts[BodyPartID].VatAssets.Find(SearchFor);
				int32 MeshRendererIndex = BodyPartToIndex[BodyPartID];
				if(MeshIndex != INDEX_NONE && (MeshRendererIndex/4 < PackedData.MeshIndices.Num()))
				{
					FColor& MeshIndexData = PackedData.MeshIndices[MeshRendererIndex/4];
					//MeshIndex += 1;
					if(MeshRendererIndex % 4 == 0)
						MeshIndexData.R = (uint8)MeshIndex;
					else if(MeshRendererIndex % 4 == 1)
						MeshIndexData.G = (uint8)MeshIndex;
					else if(MeshRendererIndex % 4 == 2)
						MeshIndexData.B = (uint8)MeshIndex;
					else if(MeshRendererIndex % 4 == 3)
						MeshIndexData.A = (uint8)MeshIndex;
				}
			}
			else
			{
				UE_LOG(LogOverCrowdEditor,Error, TEXT("WriteCrowdMemberDataToTexture --SortedBodyParts.Contains(BodyPartID): %s"), *BodyPartID.ToString());
			}
		}
		
		uint8* PixelPtr = (uint8*)&Pixels[Index];
		FMemory::Memcpy(PixelPtr, &PackedData, sizeof(FCrowdMemberPackedData));
		Index += 32;
	}
	
	// FCrowdMetaDataPacked PackedMetaData;
	// PackedMetaData.NumCrowdMembers = CrowdMembers.Num();
	// uint8* PixelPtr = (uint8*)&Pixels[Height*Width - Width];
	// FMemory::Memcpy(PixelPtr, &PackedMetaData, sizeof(FCrowdMetaDataPacked));
	
	WriteToTexture<FLowPrecision>(OutTexture, Height, Width, Pixels);

	OutTexture->MarkPackageDirty();

	return true;
}

bool UOverCrowdEditorLibrary::DoesPackageExist(FString PackageName)
{
	return FPackageName::DoesPackageExist(PackageName);
}

bool UOverCrowdEditorLibrary::FillOutClothingMask(UVatClothingMask* ClothingMask)
{
	if (!ClothingMask)
	{
		return false;
	}

	if (ClothingMask->ElementsOfCompositedMask.IsEmpty())
	{
		UE_LOG(LogOverCrowdEditor, Warning, TEXT("No elements to generate mask from"));
		return false;
	}

	UTexture2D* CurrentTexture = ClothingMask->ElementsOfCompositedMask[0].LoadSynchronous();
	if (!CurrentTexture || !ClothingMask->CompositedClothingMask)
	{
		UE_LOG(LogOverCrowdEditor, Warning, TEXT("No texture slots should be empty"));
	}

	FTexturePlatformData* PlatformData = CurrentTexture->GetPlatformData();

	int32 Width = PlatformData->SizeX;
	int32 Height = PlatformData->SizeY;

	TArray<FColor> Pixels;
	Pixels.Init(FColor(0, 0, 0, 255), Width * Height);
		
	int32 Index = 0;
	for (auto El : ClothingMask->ElementsOfCompositedMask)
	{
		UTexture2D* Texture = El.LoadSynchronous();
			
		if(!Texture)
			continue;

		uint32 BitMask = 0x00000001 << Index;

		PlatformData = Texture->GetPlatformData();

		float NormalizedWidth = (float)PlatformData->SizeX / (float)Width ;
		float NormalizedHeight = (float)PlatformData->SizeY / (float)Height;

		const FColor* FormatedImageData = static_cast<const FColor*>(PlatformData->Mips[0].BulkData.LockReadOnly());

		if (FormatedImageData)
		{
			for (int32 Y = 0, i = 0; Y < Height; Y++)
			{
				for (int32 X = 0; X < Width; X++, i++)
				{
					FColor PixelColor = FormatedImageData[(int)(Y * NormalizedHeight) * PlatformData->SizeX + (int)(X* NormalizedWidth)];

					if (PixelColor.R > 128 || PixelColor.G > 128 || PixelColor.B > 128)
					{
						Pixels[i].Bits |= BitMask;
					}
				}
			}
		
			PlatformData->Mips[0].BulkData.Unlock();
		}
		else
		{
			UE_LOG(LogOverCrowdEditor, Warning, TEXT("Something wrong with FormatedImageData"));
		}
		Index++;
		if (Index == 7 || Index == 15 || Index == 23)
		{
			Index++;
		}
			
	}
	
	WriteToTexture<FLowPrecision>(ClothingMask->CompositedClothingMask, Height, Width, Pixels);

	ClothingMask->MarkPackageDirty();

	return true;
}

bool UOverCrowdEditorLibrary::BakeSkinWeightsToStaticMesh(USkeletalMesh* SkelMesh, UStaticMesh* StaticMesh, int32 UVChannel, FName AttachToSocket)
{
	if (!StaticMesh || !SkelMesh)
	{
		return false;
	}
	
	int32 SocketIndex = -1;
	
	const FReferenceSkeleton& GlobalRefSkeleton = SkelMesh->GetSkeleton()->GetReferenceSkeleton();
	SocketIndex = GlobalRefSkeleton.FindBoneIndex(AttachToSocket);

	for (int32 CurrentLOD = 0; CurrentLOD < StaticMesh->GetNumLODs(); CurrentLOD++)
	{
		TArray<TVertexSkinWeight<4>> SkinWeights;

		FSourceMeshToDriverMesh Mapping;
		if (SocketIndex == INDEX_NONE)
		{
			FScopedSlowTask ProgressBar(1.f, FText::Format(LOCTEXT("ProcessingMapping", "Processing StaticMesh -> SkeletalMesh Mapping ...LOD:{0}"), CurrentLOD), true /*Enabled*/);
			ProgressBar.MakeDialog(false /*bShowCancelButton*/, false /*bAllowInPIE*/);

			int32 NumDriverTriangles = 1;
			float Sigma = 1.0f;
			if (!Mapping.Update(StaticMesh, CurrentLOD, SkelMesh, CurrentLOD))
			{
				UE_LOG(LogOverCrowdEditor, Warning, TEXT("Static Mesh Mapping Error, LOD has no vertices!"));
				continue;
			}

			// Project SkinWeights from SkeletalMesh to StaticMesh
			TArray<VertexSkinWeightMax> StaticMeshSkinWeights;
			Mapping.ProjectSkinWeights(StaticMeshSkinWeights);

			// Reduce Weights to 4 highest influences.
			ReduceSkinWeights(StaticMeshSkinWeights, SkinWeights);
		}
		else
		{
			if (!StaticMesh->GetMeshDescription(CurrentLOD))
			{
				StaticMesh->CreateMeshDescription(CurrentLOD);
			}

			const FMeshDescription* MeshDescription = StaticMesh->GetMeshDescription(CurrentLOD);
			const FVertexArray& Vertices = MeshDescription->Vertices();
			const int32 NumVertices = Vertices.Num();

			UE_LOG(LogOverCrowdEditor,Warning,TEXT("Static Mesh NumVertices %d"), NumVertices);
			SkinWeights.SetNumUninitialized(NumVertices);
			for (TVertexSkinWeight<4>&SkinWeight : SkinWeights)
			{
				SkinWeight.BoneWeights = TStaticArray<uint8, 4>(InPlace, 255);
				SkinWeight.BoneWeights[1] = 0;
				SkinWeight.BoneWeights[2] = 0;
				SkinWeight.BoneWeights[3] = 0;

				SkinWeight.MeshBoneIndices = TStaticArray<uint16, 4>(InPlace, SocketIndex);
			}
		}

		TArray<FColor> SkinnedBoneWeights;
		TArray<FVector4f> SkinnedBoneIndices;
		
		GetSkinWeightsToUV(SkinWeights, SkinnedBoneIndices, SkinnedBoneWeights);

		CreateUVChannelForBoneIndices(StaticMesh, CurrentLOD, UVChannel, SkinnedBoneIndices, SkinnedBoneWeights);

		StaticMesh->PostEditChange();
	}

	return StaticMesh->MarkPackageDirty();
}

bool UOverCrowdEditorLibrary::GetValidAnimSequences(const UVatAnimationDataAsset* DataAsset, const TArray<FSkelMeshToStaticMesh>& MeshesToConvert, TArray<FOverCrowdAnimSequenceInfo>& OutAnimSequences)
{
	if (!MeshesToConvert.Num() || !MeshesToConvert[0].SourceMesh)
	{
		UE_LOG(LogOverCrowdEditor, Warning, TEXT("Invalid SkeletalMesh"));
		return false;
	}
	if (!DataAsset->AnimationsToBake)
	{
		UE_LOG(LogOverCrowdEditor, Warning, TEXT("No Animations to Bake"));
		return false;
	}
	// Check Animations
	OutAnimSequences.Reset();
	for (const FOverCrowdAnimSequenceInfo& AnimSequenceInfo : DataAsset->AnimationsToBake->AnimSequences)
	{
		if (const UAnimSequence* AnimSequence = AnimSequenceInfo.AnimSequence)
		{
			// Make sure SkeletalMesh is compatible with AnimSequence
			if (!MeshesToConvert[0].SourceMesh->GetSkeleton()->IsCompatibleForEditor(AnimSequence->GetSkeleton()))
			{
				UE_LOG(LogOverCrowdEditor, Warning, TEXT("Invalid AnimSequence: %s for given SkeletalMesh: %s -- Animation maybe broken"), *AnimSequence->GetFName().ToString(), *MeshesToConvert[0].SourceMesh->GetFName().ToString());
			}

			OutAnimSequences.Add(AnimSequenceInfo);
		}
	}

	if (!OutAnimSequences.Num())
	{
		UE_LOG(LogOverCrowdEditor, Warning, TEXT("No valid AnimSequences found"));
		return false;
	}

	return true;
}

int32 UOverCrowdEditorLibrary::GetAnimationFrameRange(const FOverCrowdAnimSequenceInfo& Animation,int32& OutStartFrame, int32& OutEndFrame)
{
	if (!Animation.AnimSequence)
	{
		return INDEX_NONE;
	}
	OutStartFrame = 0;
	OutEndFrame = Animation.AnimSequence->GetNumberOfSampledKeys() - 1;
	
	FFrameRate SequenceFrameRate = Animation.AnimSequence->GetSamplingFrameRate();

	// Return Number of Frames
	return (OutEndFrame - OutStartFrame + 1) * (Animation.SampleRate / SequenceFrameRate.AsDecimal());
}


int32 UOverCrowdEditorLibrary::GetRefBonePositionsAndRotations(const USkeletalMesh* SkeletalMesh,	TArray<FVector3f>& OutBoneRefPositions, TArray<FVector4f>& OutBoneRefRotations)
{
	check(SkeletalMesh);

	OutBoneRefPositions.Reset();
	OutBoneRefRotations.Reset();

	// Get Number of RawBones (no virtual)
	const int32 NumBones = GetNumBones(SkeletalMesh);
	
	// Get Raw Ref Bone (no virtual)
	TArray<FTransform> RefBoneTransforms;
	GetRefBoneTransforms(SkeletalMesh, RefBoneTransforms);
	DecomposeTransformations(RefBoneTransforms, OutBoneRefPositions, OutBoneRefRotations);

	return NumBones;
}


int32 UOverCrowdEditorLibrary::GetBonePositionsAndRotations(const USkinnedMeshComponent* SkeletalMeshComponent, const TArray<FVector3f>& BoneRefPositions, TArray<FVector3f>& BonePositions, TArray<FVector4f>& BoneRotations)
{

	check(SkeletalMeshComponent);

	BonePositions.Reset();
	BoneRotations.Reset();

	// Get Relative Transforms
	// Note: Size is of Raw bones in SkeletalMesh. These are the original/raw bones of the asset, without Virtual Bones.
	TArray<FMatrix44f> RefToLocals;
	SkeletalMeshComponent->CacheRefToLocalMatrices(RefToLocals);
	const int32 NumBones = RefToLocals.Num();

	// check size
	check(NumBones == BoneRefPositions.Num());

	// Get Component Space Transforms
	// Note returns all transforms, including VirtualBones
	const TArray<FTransform>& CompSpaceTransforms = SkeletalMeshComponent->GetComponentSpaceTransforms();
	check(CompSpaceTransforms.Num() >= RefToLocals.Num());

	// Allocate
	BonePositions.SetNumUninitialized(NumBones);
	BoneRotations.SetNumUninitialized(NumBones);

	for (int32 BoneIndex = 0; BoneIndex < NumBones; BoneIndex++)
	{
		// Decompose Transformation (ComponentSpace)
		const FTransform& CompSpaceTransform = CompSpaceTransforms[BoneIndex];
		FVector3f BonePosition;
		FVector4f BoneRotation;
		DecomposeTransformation(CompSpaceTransform, BonePosition, BoneRotation);

		// Position Delta (from RefPose)
		const FVector3f Delta = BonePosition - BoneRefPositions[BoneIndex];

		// Decompose Transformation (Relative to RefPose)
		FVector3f BoneRelativePosition;
		FVector4f BoneRelativeRotation;
		const FMatrix RefToLocalMatrix(RefToLocals[BoneIndex]);
		const FTransform RelativeTransform(RefToLocalMatrix);
		DecomposeTransformation(RelativeTransform, BoneRelativePosition, BoneRelativeRotation);

		BonePositions[BoneIndex] = Delta;
		BoneRotations[BoneIndex] = BoneRelativeRotation;
	}

	return NumBones;
}

bool DoesParamExist(FName ParamName, class UMaterialInstanceConstant* MaterialInstance, const EMaterialParameterAssociation MaterialParameterAssociation)
{
	float DummyFloat = 0;
	FLinearColor DummyVector;
	UTexture* DummyTexture = nullptr;

	if (MaterialInstance->GetScalarParameterValue(FHashedMaterialParameterInfo(ParamName, MaterialParameterAssociation, MaterialParameterAssociation == EMaterialParameterAssociation::LayerParameter ? 0 : INDEX_NONE), DummyFloat))
	{
		return true;
	}
	if (MaterialInstance->GetVectorParameterValue(FHashedMaterialParameterInfo(ParamName, MaterialParameterAssociation, MaterialParameterAssociation == EMaterialParameterAssociation::LayerParameter ? 0 : INDEX_NONE), DummyVector))
	{
		return true;
	}
	if (MaterialInstance->GetTextureParameterValue(FHashedMaterialParameterInfo(ParamName, MaterialParameterAssociation, MaterialParameterAssociation == EMaterialParameterAssociation::LayerParameter ? 0 : INDEX_NONE), DummyTexture))
	{
		return true;
	}

	return false;
}

bool UOverCrowdEditorLibrary::UpdateTextureOnMaterialInstances(UTexture2D* Texture, FName ParamName, class UMaterialInstanceConstant* MaterialInstance, const EMaterialParameterAssociation MaterialParameterAssociation)
{
	if (Texture == UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(MaterialInstance, ParamName, MaterialParameterAssociation))
	{
		return false;
	}

	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MaterialInstance, ParamName, Texture, MaterialParameterAssociation);

	// Update Material
	UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);

	// Rebuild Material
	UMaterialEditingLibrary::RebuildMaterialInstanceEditors(MaterialInstance->GetMaterial());

	return MaterialInstance->MarkPackageDirty();
}

bool UOverCrowdEditorLibrary::UpdateScalarOnMaterialInstances(float Value, FName ParamName, class UMaterialInstanceConstant* MaterialInstance, const EMaterialParameterAssociation MaterialParameterAssociation)
{
	if (Value == UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(MaterialInstance, ParamName, MaterialParameterAssociation))
	{
		return false;
	}
	
	UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MaterialInstance, ParamName, Value, MaterialParameterAssociation);
	
	UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);
	
	UMaterialEditingLibrary::RebuildMaterialInstanceEditors(MaterialInstance->GetMaterial());

	return MaterialInstance->MarkPackageDirty();
}
//
// bool UOverCrowdEditorLibrary::UpdateMaterialInstances(const UVatAnimationDataAsset* DataAsset, UVatMeshPairing* VatMeshPairing, //int32 BasePoseIndex, int32 AdditiveBasePoseIndex,
// 														class UMaterialInstanceConstant* MaterialInstance, const EMaterialParameterAssociation MaterialParameterAssociation)
// {
// 	
// }

void UOverCrowdEditorLibrary::RemoveRepeatedVertexAnimatedTextures(TArray<UVatAnimationDataAsset*> DataAssets)
{
	TArray<UVatAnimationDataAsset*> AllDataAssets;
	TArray<UTexture2D*> AllTextures;
	TArray<UTexture2D*> TexturesToRemove;

	for(auto DataAsset : DataAssets)
	{
		AllDataAssets.Add(DataAsset);
		if(DataAsset->AdditiveVatAnimationDataAsset)
		{
			AllDataAssets.Add(DataAsset->AdditiveVatAnimationDataAsset);
		}
	}
	
	for(auto DataAsset : AllDataAssets)
	{
		UTexture2D* PosTex = DataAsset->GetBonePositionTexture();
		UTexture2D* RotTex = DataAsset->GetBonePositionTexture();
		
		if(AllTextures.Contains(PosTex) || AllTextures.Contains(RotTex))
		{
			TexturesToRemove.Add(PosTex);
			TexturesToRemove.Add(RotTex);
		}
		else
		{
			AllTextures.Add(PosTex);
			AllTextures.Add(RotTex);
		}
	}
	
	for(auto Texture : TexturesToRemove)
	{
		for(auto DataAsset : AllDataAssets)
		{
			UTexture2D* PosTex = DataAsset->GetBonePositionTexture();
			UTexture2D* RotTex = DataAsset->GetBonePositionTexture();

			if(PosTex == Texture || RotTex == Texture)
			{
				DataAsset->BoneRotationTexture = nullptr;
				DataAsset->BonePositionTexture = nullptr;

				DataAsset->MarkPackageDirty();
			}
		}
	}
}

void UOverCrowdEditorLibrary::PutStringIntoCopyPasteBuffer(FString Str)
{
	FPlatformApplicationMisc::ClipboardCopy( *Str );
}


bool UOverCrowdEditorLibrary::UpdateMaterialInstanceFromDataAsset(const UVatAnimationDataAsset* DataAsset, UVatMeshPairing* VatMeshPairing, UMaterialInstanceConstant* MaterialInstance,
                                                                  const EMaterialParameterAssociation MaterialParameterAssociation)
{
	//TODO: make sure the parameter exists otherwise it will make it dirty and force a save
	if (!DoesParamExist(OverCrowdParamNames::BonePositionTexture ,MaterialInstance, MaterialParameterAssociation))
	{
		return false;
	}
	
	check(VatMeshPairing);
	check(DataAsset);
	check(MaterialInstance);
	check(DataAsset->AnimationsToBake);
	
	bool bIsDirty = false;

	int32 VatUVChannel = VatMeshPairing->VatUVChannel;
	
	bool UseUvs[4];
	UseUvs[0] = UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::UseUV0, MaterialParameterAssociation);
	UseUvs[1] = UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::UseUV1, MaterialParameterAssociation);
	UseUvs[2] = UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::UseUV2, MaterialParameterAssociation);
	UseUvs[3] = UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::UseUV3, MaterialParameterAssociation);

	bool bEnableVat = UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::EnableVAT, MaterialParameterAssociation);
	bool bFrameInterpolation = UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::FrameInterpolation, MaterialParameterAssociation);
	bool bAnimationBlending = UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::AnimationBlending, MaterialParameterAssociation);
	
	auto TmpPositionTex = UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(MaterialInstance, OverCrowdParamNames::BonePositionTexture, MaterialParameterAssociation);
	auto TmpRotationTex = UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(MaterialInstance, OverCrowdParamNames::BoneRotationTexture, MaterialParameterAssociation);
	auto TmpAdditiveBoneIndex = UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(MaterialInstance, OverCrowdParamNames::AdditiveBoneIndex, MaterialParameterAssociation);
	
	auto TmpEnableAdditive = UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::EnableAdditive, MaterialParameterAssociation);
	auto TmpAdditiveBonePositionTexture = UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(MaterialInstance, OverCrowdParamNames::AdditiveBonePositionTexture, MaterialParameterAssociation);
	auto TmpAdditiveBoneRotationTexture = UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(MaterialInstance, OverCrowdParamNames::AdditiveBoneRotationTexture, MaterialParameterAssociation);

	if(DataAsset->AnimationsToBake->bFrameInterpolation != bFrameInterpolation || DataAsset->AnimationsToBake->bAnimationBlending != bAnimationBlending)
		bIsDirty = true;
	if (DataAsset->AdditiveVatAnimationDataAsset)
	{
		if(TmpAdditiveBonePositionTexture != DataAsset->AdditiveVatAnimationDataAsset->GetBonePositionTexture() || TmpAdditiveBoneRotationTexture != DataAsset->AdditiveVatAnimationDataAsset->GetBoneRotationTexture())
		{
			bIsDirty = true;	
		}
	}
	else if (TmpEnableAdditive == true)
	{
		bIsDirty = true;
	}
	
	if (	(DataAsset->GetBonePositionTexture() != TmpPositionTex) 
		||	(DataAsset->GetBoneRotationTexture() != TmpRotationTex) 
		||	(DataAsset->AdditveRootBoneIndex != TmpAdditiveBoneIndex)
		||	((VatUVChannel < 0 || VatUVChannel > 3) || (UseUvs[VatUVChannel] != true))	)
	{
		bIsDirty = true;
	}
	
	if (DataAsset->AdditiveVatAnimationDataAsset)
	{
		if (UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::EnableAdditive, MaterialParameterAssociation) == false)
		{
			bIsDirty = true;
		}
	}
	else if (UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::EnableAdditive, MaterialParameterAssociation) == true)
	{
		bIsDirty = true;
	}

	if (!bIsDirty)
	{
		return false;
	}
	
	// Set UVChannel
	switch (VatUVChannel)
	{
	case 0:
		

		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::UseUV0, true, MaterialParameterAssociation);
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::UseUV1, false, MaterialParameterAssociation);
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::UseUV2, false, MaterialParameterAssociation);
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::UseUV3, false, MaterialParameterAssociation);
		break;
	case 1:
		
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::UseUV0, false, MaterialParameterAssociation);
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::UseUV1, true, MaterialParameterAssociation);
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::UseUV2, false, MaterialParameterAssociation);
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::UseUV3, false, MaterialParameterAssociation);
		break;
	case 2:
		
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::UseUV0, false, MaterialParameterAssociation);
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::UseUV1, false, MaterialParameterAssociation);
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::UseUV2, true, MaterialParameterAssociation);
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::UseUV3, false, MaterialParameterAssociation);
		break;
	case 3:
		

		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::UseUV0, false, MaterialParameterAssociation);
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::UseUV1, false, MaterialParameterAssociation);
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::UseUV2, false, MaterialParameterAssociation);
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::UseUV3, true, MaterialParameterAssociation);
		break;
	default:
		
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::UseUV0, false, MaterialParameterAssociation);
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::UseUV1, true, MaterialParameterAssociation);
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::UseUV2, false, MaterialParameterAssociation);
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::UseUV3, false, MaterialParameterAssociation);
		break;
	}

	UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::EnableVAT, true, MaterialParameterAssociation);
	UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::FrameInterpolation, DataAsset->AnimationsToBake->bFrameInterpolation, MaterialParameterAssociation);
	UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::AnimationBlending, DataAsset->AnimationsToBake->bAnimationBlending, MaterialParameterAssociation);

	if (DataAsset->AdditiveVatAnimationDataAsset)
	{
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::EnableAdditive, true, MaterialParameterAssociation);
		UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MaterialInstance, OverCrowdParamNames::AdditiveBonePositionTexture, DataAsset->AdditiveVatAnimationDataAsset->GetBonePositionTexture(), MaterialParameterAssociation);
		UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MaterialInstance, OverCrowdParamNames::AdditiveBoneRotationTexture, DataAsset->AdditiveVatAnimationDataAsset->GetBoneRotationTexture(), MaterialParameterAssociation);
		UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MaterialInstance, OverCrowdParamNames::AdditiveBoneIndex, DataAsset->AdditveRootBoneIndex, MaterialParameterAssociation);
	}
	else
	{
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, OverCrowdParamNames::EnableAdditive, false, MaterialParameterAssociation);
	}

	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MaterialInstance, OverCrowdParamNames::BonePositionTexture, DataAsset->GetBonePositionTexture(), MaterialParameterAssociation);
	
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MaterialInstance, OverCrowdParamNames::BoneRotationTexture, DataAsset->GetBoneRotationTexture(), MaterialParameterAssociation);

	UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);

	UMaterialEditingLibrary::RebuildMaterialInstanceEditors(MaterialInstance->GetMaterial());

	return MaterialInstance->MarkPackageDirty();
}


bool UOverCrowdEditorLibrary::SetLightMapIndex(UStaticMesh* StaticMesh, const int32 LODIndex, const int32 LightmapIndex, bool bGenerateLightmapUVs)
{
	check(StaticMesh);

	if (!StaticMesh->IsSourceModelValid(LODIndex))
	{
		return false;
	}

	for (int32 Index=0; Index < LightmapIndex; Index++)
	{
		if (LightmapIndex > StaticMesh->GetNumUVChannels(LODIndex))
		{
			StaticMesh->AddUVChannel(LODIndex);
		}
	}

	// Set Build Settings
	FStaticMeshSourceModel& SourceModel = StaticMesh->GetSourceModel(LODIndex);
	SourceModel.BuildSettings.bGenerateLightmapUVs = bGenerateLightmapUVs;
	SourceModel.BuildSettings.DstLightmapIndex = LightmapIndex;
	StaticMesh->SetLightMapCoordinateIndex(LightmapIndex);

	// Build Mesh
	StaticMesh->Build(false);
	StaticMesh->PostEditChange();
	StaticMesh->MarkPackageDirty();

	return true;
}


bool UOverCrowdEditorLibrary::GenerateLODsForSkeletalMesh(USkeletalMesh* SkelMesh, const int32 NumLODs, int32 MinVertices, float ScreenSizeAtLowestLOD, float ScreenSizeAtHighestLOD)
{
	if (!SkelMesh)
	{
		return false;
	}
	{
		FScopedSkeletalMeshPostEditChange ScopedPostEditChange(SkelMesh);
		
		bool bGenerateBaseLOD = true;
		bool bRegenerateEvenIfImported = true;

		int32 CurrentNumLODs = SkelMesh->GetLODNum();
		
		SkelMesh->SetMinLod(0);
		USkeletalMeshLODSettings* BackUpSettings = SkelMesh->GetLODSettings();

		int32 NumVertices = GetNumVertices(SkelMesh,0);
		//int32 NumTris = GetNumTriangles(SkelMesh,0);
		if(NumLODs > 1 && NumVertices != INDEX_NONE)
		{
			
			USkeletalMeshLODSettingsAccess *LODSettings = NewObject<USkeletalMeshLODSettingsAccess>(GetTransientPackage(), NAME_None,RF_Public | RF_Transient);
			//USkeletalMeshLODSettingsAccess *LODSettings = (USkeletalMeshLODSettingsAccess*)BackUpSettings;
			LODSettings->SetLODSettingsFromMesh(SkelMesh);
			TArray<FSkeletalMeshLODGroupSettings>& LODGroups = LODSettings->GetLODGroups();
			LODGroups.SetNum(NumLODs);
			
			
			float ScreenSizeIncrement = FMath::Log2(ScreenSizeAtLowestLOD/ ScreenSizeAtHighestLOD) / static_cast<float>(NumLODs-1);
			float VertFactorIncrement = FMath::Log2(MinVertices/ static_cast<float>(NumVertices)) / static_cast<float>(NumLODs-1);
			
			for(int32 Index = 0; FSkeletalMeshLODGroupSettings& LODGroup : LODGroups)
			{
				float VertsFactor = FMath::Pow(2,VertFactorIncrement * Index);
				float ScreenSizeFactor = FMath::Pow(2,ScreenSizeIncrement * Index);;
				//LODGroup.ScreenSize = FMath::Pow(2,ScreenSizeIncrement * Index);;
				
				LODGroup.ScreenSize = ScreenSizeAtHighestLOD * ScreenSizeFactor;
				LODGroup.ReductionSettings.TerminationCriterion = SkeletalMeshTerminationCriterion::SMTC_NumOfVerts;
				LODGroup.ReductionSettings.MaxNumOfVerts = static_cast<int>((static_cast<double>(NumVertices) * VertsFactor) );
				LODGroup.ReductionSettings.NumOfVertPercentage = 1.0f;
				LODGroup.ReductionSettings.MaxNumOfVertsPercentage = LODGroup.ReductionSettings.MaxNumOfVerts;
				
				Index++;
			
				UE_LOG(LogOverCrowdEditor, Display, TEXT("LOD Group Settings, MaxVerts: %d,  LODGroup.ScreenSize: %.4f"), LODGroup.ReductionSettings.MaxNumOfVerts, LODGroup.ScreenSize.GetValue());
			}
			SkelMesh->SetLODSettings(LODSettings);
		}
		FLODUtilities::RegenerateLOD(SkelMesh, GetTargetPlatformManagerRef().GetRunningTargetPlatform(), NumLODs, bRegenerateEvenIfImported, bGenerateBaseLOD);

		SkelMesh->SetLODSettings(BackUpSettings);
	}
	return true;
}

bool UOverCrowdEditorLibrary::StripLODsForSkeletalMesh(USkeletalMesh* SkeletalMesh)
{
	//SkeletalMesh->LODSettings = nullptr;
	SkeletalMesh->SetLODSettings(nullptr);
	SkeletalMesh->SetMinLod(0);
			
	return FLODUtilities::RegenerateLOD(SkeletalMesh, GetTargetPlatformManagerRef().GetRunningTargetPlatform(), 1);
}

//
//bool UMegaHumansBPLibrary::ConvertAllSkeletalMeshesToStaticMeshes(TArray<FSkelMeshToStaticMesh>& MeshesToConvert, FString DestinationDirectory, bool bForceRegeneration)
//{
//	if (DestinationDirectory.EndsWith("/") || DestinationDirectory.EndsWith("\\"))
//	{
//		DestinationDirectory = DestinationDirectory.LeftChop(1);
//	}
//	
//	for (int32 i = 0; i < MeshesToConvert.Num(); i++)
//	{
//		if (MeshesToConvert[i].SourceMesh && (!MeshesToConvert[i].ConvertedMesh || bForceRegeneration))
//		{
//			FString PathPart, FilenamePart, Dummy;
//			FPaths::Split(MeshesToConvert[i].SourceMesh->GetPathName(), PathPart, FilenamePart, Dummy);
//			
//			MeshesToConvert[i].ConvertedMesh = ConvertSkeletalMeshToStaticMesh(MeshesToConvert[i].SourceMesh, DestinationDirectory + "/VatMesh_" + FilenamePart);
//			
//		}
//	}
//	
//	return true;
//}

//
//UStaticMesh* UMegaHumansBPLibrary::ConvertToStaticMesh(USkeletalMesh* SkeletalMesh, FString PackageDirectory)
//{
//	IMeshUtilities& MeshUtilities = FModuleManager::Get().LoadModuleChecked<IMeshUtilities>("MeshUtilities");
//
//	check(GEditor);
//	UWorld* World = GEditor->GetEditorWorldContext().World();
//
//	check(World);
//	AActor* Actor = World->SpawnActor<AActor>();
//	check(Actor);
//
//	// Create Temp SkeletalMesh Component
//	USkeletalMeshComponent* MeshComponent = NewObject<USkeletalMeshComponent>(Actor);
//	MeshComponent->RegisterComponent();
//	MeshComponent->SetSkeletalMesh(SkeletalMesh);
//	TArray<UMeshComponent*> MeshComponents = { MeshComponent };
//
//	UStaticMesh* Mesh = MeshUtilities.ConvertMeshesToStaticMesh(MeshComponents, MeshComponent->GetComponentToWorld());
//
//	// Destroy Temp Component and Actor
//	MeshComponent->UnregisterComponent();
//	MeshComponent->DestroyComponent();
//	Actor->Destroy();
//
//	return Mesh;
//}

UStaticMesh* UOverCrowdEditorLibrary::ConvertSkeletalMeshToStaticMesh(USkeletalMesh* SkeletalMesh, FString PackageDirectory, bool bOverwrite)
{
	check(SkeletalMesh);

	FString PackageName = PackageDirectory;

	if (PackageName.IsEmpty() || !FPackageName::IsValidObjectPath(PackageName))
	{
		FString PathPart, FilenamePart, Dummy;
		FPaths::Split(SkeletalMesh->GetPathName(),PathPart,FilenamePart,Dummy);

		PackageDirectory = PathPart;
		PackageName = PathPart + "/VatMesh_" + FilenamePart;
	}
	
	check(GEditor);
	UWorld* World = GEditor->GetEditorWorldContext().World();

	check(World);
	AActor* Actor = World->SpawnActor<AActor>();
	check(Actor);

	// Create Temp SkeletalMesh Component
	USkeletalMeshComponent* MeshComponent = NewObject<USkeletalMeshComponent>(Actor);
	MeshComponent->RegisterComponent();
	MeshComponent->SetSkeletalMesh(SkeletalMesh);
	TArray<UMeshComponent*> MeshComponents = { MeshComponent };

	UStaticMesh* OutStaticMesh = nullptr;
	bool bGeneratedCorrectly = true;

	// Create New StaticMesh
	if ((!FPackageName::DoesPackageExist(PackageName)) || bOverwrite)
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		TArray< FAssetData> AssetData;
		if (AssetRegistryModule.Get().GetAssetsByPackageName(FName(PackageName), AssetData))
		{
			ObjectTools::DeleteAssets(AssetData);
		}
		
		IMeshUtilities& MeshUtilities = FModuleManager::Get().LoadModuleChecked<IMeshUtilities>("MeshUtilities");
		OutStaticMesh = MeshUtilities.ConvertMeshesToStaticMesh(MeshComponents, FTransform::Identity, PackageName);
	}
	// Update Existing StaticMesh
	else
	{
		// Load Existing Mesh
		OutStaticMesh = LoadObject<UStaticMesh>(nullptr, *PackageName);
	}

	if (OutStaticMesh)
	{
		UPackage* TransientPackage = GetTransientPackage();
		IMeshUtilities& MeshUtilities = FModuleManager::Get().LoadModuleChecked<IMeshUtilities>("MeshUtilities");
			
		// Copy Materials
		auto Materials = SkeletalMesh->GetMaterials();
		TArray<FStaticMaterial> NewMaterials;

		for (auto Material : Materials)
		{
			UMaterialInterface* MaterialInterface = Material.MaterialInterface;

			if(MaterialInterface)
			{
				MaterialInterface = GetOrCreateChildMaterial(MaterialInterface);	
			}
			NewMaterials.Add(FStaticMaterial(MaterialInterface));
		}
		OutStaticMesh->SetStaticMaterials(NewMaterials);
		
		if (OutStaticMesh->GetNumSourceModels() > 0)
		{
			OutStaticMesh->GetSourceModel(0).ScreenSize.Default = 1.0f;
		}

		for (int32 LODIndex = 0; LODIndex < SkeletalMesh->GetLODNum() && LODIndex < OutStaticMesh->GetNumLODs(); LODIndex++)
		{
			OutStaticMesh->bAutoComputeLODScreenSize = false;

			OutStaticMesh->GetRenderData()->ScreenSize[LODIndex] = SkeletalMesh->GetLODInfo(LODIndex)->ScreenSize.GetValue();
			OutStaticMesh->GetRenderData()->ScreenSize[LODIndex].Default = SkeletalMesh->GetLODInfo(LODIndex)->ScreenSize.GetValue();
			
			OutStaticMesh->GetSourceModel(LODIndex).ScreenSize.Default = SkeletalMesh->GetLODInfo(LODIndex)->ScreenSize.GetValue();
			OutStaticMesh->GetSourceModel(LODIndex).ScreenSize = SkeletalMesh->GetLODInfo(LODIndex)->ScreenSize.GetValue();

			UE_LOG(LogOverCrowdEditor, Log, TEXT("LODIndex: %d, ScreenSize: %f"), LODIndex, OutStaticMesh->GetRenderData()->ScreenSize[LODIndex].GetValue());
			OutStaticMesh->bRequiresLODScreenSizeConversion = true;

			{
				FStaticMeshComponentRecreateRenderStateContext ReregisterContext(OutStaticMesh, false);
				OutStaticMesh->Modify();
			}
		}

		// Done
		TArray<FText> OutErrors;
		OutStaticMesh->Build(true, &OutErrors);
		OutStaticMesh->PostEditChange();
		OutStaticMesh->MarkPackageDirty();
	}

	// Destroy Temp Component and Actor
	MeshComponent->UnregisterComponent();
	MeshComponent->DestroyComponent();
	Actor->Destroy();

	return bGeneratedCorrectly ? OutStaticMesh : nullptr;
}

void UOverCrowdEditorLibrary::GetTexturesFromMaterial(UMaterialInterface* Material, TMap<FString, UTexture*>& OutTextures)
{
	if(!Material)
		return;

	TArray<UTexture*> MaterialTextures;
	Material->GetUsedTextures(MaterialTextures, EMaterialQualityLevel::Num, true, ERHIFeatureLevel::Num, true);

	for(auto Texture : MaterialTextures)
	{
		if(Texture)
			OutTextures[Texture->GetName()] = Texture;
	}
}

FString UOverCrowdEditorLibrary::GetMaterailPackageName(UMaterialInterface* Material)
{
	if(Material)
		return Material->GetPackage()->GetName();

	return FString();
}

UMaterialInstanceConstant* UOverCrowdEditorLibrary::GetOrCreateChildMaterial(UMaterialInterface* Material, FString Suffix)
{
	if(!Material)
		return nullptr;
	
	FString Name;
	FString PackageName;
	
	IAssetTools& AssetTools = FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	FString AssetName = Material->GetPackage()->GetName();
	FString NewName = AssetName + Suffix;

	UMaterialInstanceConstant* MIC;
	MIC = LoadObject<UMaterialInstanceConstant>(nullptr, *NewName);
	if(MIC)
		return MIC;
	
	AssetTools.CreateUniqueAssetName(NewName, TEXT(""), PackageName, Name);

	UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
	Factory->InitialParent = Material;

	UObject* NewAsset = AssetTools.CreateAsset(Name, FPackageName::GetLongPackagePath(PackageName), UMaterialInstanceConstant::StaticClass(), Factory);

	MIC = Cast<UMaterialInstanceConstant>(NewAsset);

	return MIC;
}

UMaterialInstanceConstant* UOverCrowdEditorLibrary::CreateChildMaterial(UMaterialInterface* Material, FString FullPackageName)
{
	if (!Material)
		return nullptr;

	FString Name;
	FString PackageName;

	IAssetTools& AssetTools = FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	FString AssetName = FullPackageName;
	FString NewName = AssetName + "_Inst";
	AssetTools.CreateUniqueAssetName(NewName, TEXT(""), PackageName, Name);

	UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
	Factory->InitialParent = Material;

	UObject* NewAsset = AssetTools.CreateAsset(Name, FPackageName::GetLongPackagePath(PackageName), UMaterialInstanceConstant::StaticClass(), Factory);

	UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(NewAsset);

	return MIC;
}
void UOverCrowdEditorLibrary::NormalizeBoneData(	const TArray<FVector4f>& Rotations,	TArray<FVector4f>& OutNormalizedRotations)
{
	// ---------------------------------------------------------------------------
	// Normalize Rotations
	// And move them to [0, 1]
	OutNormalizedRotations.SetNumUninitialized(Rotations.Num());
	for (int32 Index = 0; Index < Rotations.Num(); ++Index)
	{
		const FVector4f Axis = Rotations[Index];
		const float Angle = Rotations[Index].W; // Angle are returned in radians and they go from [0-pi*2]

		OutNormalizedRotations[Index] = (Axis.GetSafeNormal() + FVector3f::OneVector) * 0.5f;
		OutNormalizedRotations[Index].W = Angle / (PI * 2.f);
	}
}

bool UOverCrowdEditorLibrary::CreateUVChannelForBoneIndices(UStaticMesh* StaticMesh, const int32 LODIndex, const int32 UVChannelIndex, const TArray<FVector4f>& _InBoneIndices, const TArray< FColor>& _InBoneWeights)
{
	check(StaticMesh);

	if (!StaticMesh->IsSourceModelValid(LODIndex))
	{
		return false;
	}

	FMeshDescription* MeshDescription = StaticMesh->GetMeshDescription(LODIndex);
	check(MeshDescription);

	int32 NumUVChannels = StaticMesh->GetNumUVChannels(LODIndex);

	for(int32 i = NumUVChannels; i <= UVChannelIndex; i++)
	{
		if (!StaticMesh->InsertUVChannel(LODIndex, i))
		{
			UE_LOG(LogOverCrowdEditor, Warning, TEXT("Unable to Add UVChannel"));
			return false;
		}
	}

	TMap<FVector3f, FColor> VertexColorWeights;
	TMap<FVertexInstanceID, FVector2D> BoneInds;

	for (const FVertexInstanceID VertexInstanceID : MeshDescription->VertexInstances().GetElementIDs())
	{
		const FVertexID VertexID = MeshDescription->GetVertexInstanceVertex(VertexInstanceID);
		const int32 VertexIndex = VertexID.GetValue();
		FVector3f VertPos = MeshDescription->GetVertexPosition(VertexID);
		FColor Color = _InBoneWeights[VertexIndex];

		VertexColorWeights.Add(VertPos,Color);

		float U = ((uint32)_InBoneIndices[VertexIndex].X) | (((uint32)_InBoneIndices[VertexIndex].Y) << 12);
		float V = ((uint32)_InBoneIndices[VertexIndex].Z) | (((uint32)_InBoneIndices[VertexIndex].W) << 12);

		BoneInds.Add(VertexInstanceID, FVector2D(U, V));
	}

	SetFullPrecisionUVs(StaticMesh, LODIndex, true);

	if (StaticMesh->IsSourceModelValid(LODIndex))
	{
		if (StaticMesh->GetSourceModel(LODIndex).IsRawMeshEmpty() == false)
		{
			TVertexAttributesRef<FVector3f> Positions = FStaticMeshAttributes(*MeshDescription).GetVertexPositions();
			TVertexInstanceAttributesRef<FVector4f> Colors = FStaticMeshAttributes(*MeshDescription).GetVertexInstanceColors();

			for (FVertexInstanceID VertexInstanceID : MeshDescription->VertexInstances().GetElementIDs())
			{
				FVertexID VertexID = MeshDescription->GetVertexInstanceVertex(VertexInstanceID);
				const FVector3f& Position = Positions[VertexID];
				if (const FColor* Color = VertexColorWeights.Find(Position))
				{
					Colors[VertexInstanceID] = FVector4f(FLinearColor::FromSRGBColor(*Color));
				}
				else
				{
					Colors[VertexInstanceID] = FVector4f(FLinearColor::White);
				}
			}
			StaticMesh->CommitMeshDescription(LODIndex);
		}
	}
	if (!StaticMesh->SetUVChannel(LODIndex, UVChannelIndex, BoneInds))
	{
		UE_LOG(LogOverCrowdEditor, Warning, TEXT("Unable to Set UVChannel: %i. BoneWeights1: %i"), UVChannelIndex, BoneInds.Num());
		return false;
	}

	return true;
}

bool UOverCrowdEditorLibrary::FindBestResolution(const int32 NumFrames, const int32 NumElements, int32& OutHeight, int32& OutWidth)
{
	int32 OutRowsPerFrame = 1;
	{
		OutWidth = 2;
		while (OutWidth < NumElements )
		{
			OutWidth *= 2;
		}

		const int32 TargetHeight = NumFrames;
		OutHeight = 2;
		while (OutHeight < TargetHeight)
		{
			OutHeight *= 2;
		}
	}
	return true;
};

void UOverCrowdEditorLibrary::SetFullPrecisionUVs(UStaticMesh* StaticMesh, int32 LODIndex, bool bFullPrecision)
{
	check(StaticMesh);

	if (StaticMesh->IsSourceModelValid(LODIndex))
	{
		FStaticMeshSourceModel& SourceModel = StaticMesh->GetSourceModel(LODIndex);
		SourceModel.BuildSettings.bUseFullPrecisionUVs = bFullPrecision;
	}
}

bool UOverCrowdEditorLibrary::CreateNewGameplayTag(FString TagName, FGameplayTag& OutTag)
{
	const UGameplayTagsManager& Manager = UGameplayTagsManager::Get();

	FName TagSource = "OverCrowdGamePlayTags.ini";
	FString TagComment = "";

	// check to see if this is a valid tag
	// first check the base rules for all tags then look for any additional rules in the delegate
	FText ErrorMsg;
	if (!UGameplayTagsManager::Get().IsValidGameplayTagString(TagName, &ErrorMsg))
	{
		return false;
	}

	FName NewTagName = FName(*TagName);
	if (!Manager.IsDictionaryTag(NewTagName))
	{
		IGameplayTagsEditorModule::Get().AddNewGameplayTagToINI(TagName, TagComment, TagSource);
	}
	
	FName TmpTag = FName(*TagName);
	OutTag = UGameplayTagsManager::Get().RequestGameplayTag(TmpTag, false);

	return OutTag.IsValid();
}

void UOverCrowdEditorLibrary::CleanWardrobes(UWardrobeCollection* CrowdMemberType)
{
	bool bDirty = false;
	if (!CrowdMemberType)
	{
		return;
	}

	TMap<FGameplayTag, FWardrobe> &BespokeWardrobes = CrowdMemberType->BespokeWardrobes;

	if (BespokeWardrobes.Remove(FGameplayTag::EmptyTag))
	{
		bDirty = true;
	}

	for (auto& ElCombo : BespokeWardrobes)
	{
		FWardrobe &Combination = ElCombo.Value;
		
		for (auto& ElBodyParts : Combination.Compartments)
		{
			FVatAssetArray& BodyParts = ElBodyParts.Value;

			if (BodyParts.TaggedVatAssets.RemoveAll([](const UVatMeshPairing* Ptr) {return Ptr == nullptr;	}))
			{
				bDirty = true;
			}
		}
	}

	if (bDirty)
	{
		MarkDirty(CrowdMemberType);
	}
}

void UOverCrowdEditorLibrary::GetViewportCameraLocation(FVector& ViewPos, FRotator& ViewRot)
{
	FViewport* ActiveViewport = GEditor->GetActiveViewport();
	FEditorViewportClient* EditorViewClient = (ActiveViewport != nullptr) ? (FEditorViewportClient*)ActiveViewport->GetClient() : nullptr;
	if (EditorViewClient)
	{
		ViewPos = EditorViewClient->GetViewLocation();
		ViewRot = EditorViewClient->GetViewRotation();
	}
}

bool UOverCrowdEditorLibrary::CheckIfInEditorAndInPIE()
{
	if (!IsInGameThread())
	{
		return false;
	}
	if (!GIsEditor)
	{
		return false;
	}
	if (GEditor->PlayWorld || GIsPlayInEditorWorld)
	{
		return false;
	}

	return true;
}

bool UOverCrowdEditorLibrary::IsCtrlButtonDown()
{
	const FModifierKeysState ModifierKeys = FSlateApplication::Get().GetModifierKeys();
	return ModifierKeys.IsLeftControlDown();
}


void UOverCrowdEditorLibrary::GetNextLevelTagsWithParentTag(const FGameplayTag& InparentTag, TArray<FGameplayTag>& OutTags)
{
	const UGameplayTagsManager& Manager = UGameplayTagsManager::Get();

	FName TagSourceName = "OverCrowdGamePlayTags.ini";

	const FGameplayTagSource* TagSource = Manager.FindTagSource(TagSourceName);

	if (TagSource && TagSource->SourceTagList)
	{
		UGameplayTagsList* TagList = TagSource->SourceTagList;

		for (int32 i = 0; i < TagList->GameplayTagList.Num(); i++)
		{
			FGameplayTag CurrentTag = Manager.RequestGameplayTag(TagList->GameplayTagList[i].Tag);

			if (CurrentTag.MatchesTag(InparentTag))
			{
				FString TagStr = TagList->GameplayTagList[i].Tag.ToString();
				FString ParentStr = InparentTag.ToString();

				TArray<FString> ParentSegments;
				ParentStr.ParseIntoArray(ParentSegments, TEXT("."), true);

				TArray<FString> ChildSegments;
				TagStr.ParseIntoArray(ChildSegments, TEXT("."), true);

				if (ChildSegments.Num() <= ParentSegments.Num())
				{
					continue;
				}
				FString TagBuilder = ChildSegments[0];
				for (int32 j = 1; j <= ParentSegments.Num(); j++)
				{
					TagBuilder += "." + ChildSegments[j];
				}
				FGameplayTag TmpTag = UGameplayTagsManager::Get().RequestGameplayTag(FName(*TagBuilder), false);

				OutTags.AddUnique(TmpTag);
			}
		}
	}
}


void UOverCrowdEditorLibrary::CopyAssetThumbnail(UObject* FromAsset, UObject* ToAsset)
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::Get().LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	FAssetData AssetDataSource = AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(FromAsset));
	FAssetData AssetDataDest = AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ToAsset));

	//assign the thumbnail and dirty
	const FString ObjectFullName = AssetDataDest.GetFullName();
	const FString PackageName = AssetDataDest.PackageName.ToString();

	UPackage* AssetPackage = FindObject<UPackage>(NULL, *PackageName);
	if (ensure(AssetPackage))
	{

		FObjectThumbnail TempThumbnail;
		ThumbnailTools::LoadThumbnailFromPackage(AssetDataSource, TempThumbnail);

		//if (ensure(TempThumbnail))
		{
			FObjectThumbnail* NewThumbnail = ThumbnailTools::CacheThumbnail(ObjectFullName, &TempThumbnail, AssetPackage);
			if (ensure(NewThumbnail))
			{
				//we need to indicate that the package needs to be resaved
				AssetPackage->MarkPackageDirty();

				// Let the content browser know that we've changed the thumbnail
				NewThumbnail->MarkAsDirty();

				// Signal that the asset was changed if it is loaded so thumbnail pools will update
				if (AssetDataDest.IsAssetLoaded())
				{
					AssetDataDest.GetAsset()->PostEditChange();
				}

				//Set that thumbnail as a valid custom thumbnail so it'll be saved out
				NewThumbnail->SetCreatedAfterCustomThumbsEnabled();
			}
		}
	}
}

UEditorUtilityWidget* UOverCrowdEditorLibrary::OpenUtilityBlueprint(FString Fullpath, FName &Out_TabID)
{
	UBlueprint* UMGBP = LoadObject<UBlueprint>(nullptr, *Fullpath);

	UEditorUtilityWidgetBlueprint* EditorWidget = Cast<UEditorUtilityWidgetBlueprint>(UMGBP);
	
	if (EditorWidget)
	{
		UEditorUtilitySubsystem* EditorUtilitySubsystem = GEditor->GetEditorSubsystem<UEditorUtilitySubsystem>();

		return EditorUtilitySubsystem->SpawnAndRegisterTabAndGetID(EditorWidget, Out_TabID);
	}
	return nullptr;
}

void UOverCrowdEditorLibrary::CloseUtilityBlueprint(FName TabID)
{
	GEditor->GetEditorSubsystem<UEditorUtilitySubsystem>()->CloseTabByID(TabID);
}

void UOverCrowdEditorLibrary::MarkDirty(UObject* Obj)
{
	Obj->MarkPackageDirty();

}
bool UOverCrowdEditorLibrary::IsDirty(UObject* Obj)
{
	if(!Obj)
		return false;

	return Obj->GetPackage()->IsDirty();
}

void UOverCrowdEditorLibrary::NavToAssetInContentBrowser(UObject* ObjectToNavTo)
{
	if(!ObjectToNavTo)
		return;
	
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	TArray<UObject*> NavToAsset =  { ObjectToNavTo };
	ContentBrowserModule.Get().SyncBrowserToAssets(NavToAsset);
}

void UOverCrowdEditorLibrary::GetAllVatMeshPairingAssets(TArray<FAssetData>& AssetData)
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	
	const bool bSearchSubClasses = true;
	AssetRegistryModule.Get().GetAssetsByClass(UVatMeshPairing::StaticClass()->GetClassPathName(), AssetData, bSearchSubClasses);
}
//
// TMap<UVatAnimationDataAsset*, FVatAssetArray> UOverCrowdEditorLibrary::GetGroupedAnimationDataAssets(TArray<UVatMeshPairing*> FilterList)
// {
// 	TMap<UVatAnimationDataAsset*, FVatAssetArray> Res;
// 	TArray<FAssetData> AssetDataArray;
// 	GetAllVatMeshPairingAssets(AssetDataArray);
// 	
// 	for(auto AssetData : AssetDataArray)
// 	{
// 		UVatMeshPairing* VatMesh = Cast<UVatMeshPairing>(AssetData.GetAsset());
// 		if(VatMesh)
// 		{
// 			if(!FilterList.IsEmpty() && !FilterList.Contains(VatMesh))
// 				continue;
// 			
// 			UVatAnimationDataAsset* VatAnim = UOverCrowdRuntimeLibrary::FindAnimationDataAsset(VatMesh);
// 			if(VatAnim)
// 				Res.FindOrAdd(VatAnim).TaggedVatAssets.Add(VatMesh);
// 		}
// 	}
// 	return Res;
// }


TMap<UVatAnimationDataAsset*, FVatAssetArray> UOverCrowdEditorLibrary::GroupAnimationDataAssetsByBodyPart(TArray<UVatMeshPairing*> VatMeshToGroup, UWardrobeAnimation* WardrobeAnimation)
{
	TMap<UVatAnimationDataAsset*, FVatAssetArray> Res;
	//TArray<FAssetData> AssetDataArray;
	//GetAllVatMeshPairingAssets(AssetDataArray);
	if(!WardrobeAnimation)
		return Res;
	
	for(auto VatMesh : VatMeshToGroup)
	{
		//UVatMeshPairing* VatMesh = Cast<UVatMeshPairing>(AssetData.GetAsset());
		if(VatMesh)
		{
			//if(!FilterList.IsEmpty() && !FilterList.Contains(VatMesh))
			//	continue;
			UVatAnimationDataAsset* VatAnim = WardrobeAnimation->FindOrCreateAnimationAsset(VatMesh);
			//UVatAnimationDataAsset* VatAnim = UOverCrowdRuntimeLibrary::FindAnimationDataAsset(VatMesh);
			if(VatAnim)
				Res.FindOrAdd(VatAnim).TaggedVatAssets.Add(VatMesh);
		}
	}
	return Res;
}

#undef LOCTEXT_NAMESPACE
