// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Engine/DataAsset.h"
#include "Animation/AnimSequence.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "GameplayTagContainer.h"
#include "PopulateScene.h"
#include "OverCrowdDataAsset.generated.h"

class USkeletalMesh;
class UStaticMesh;
class UTexture2D;
class UVatMeshPairing;
class UWardrobeAnimation;

USTRUCT(Blueprintable)
struct FMaterialParamData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "OverCrowd", BlueprintReadWrite)
	FGameplayTag ParamName;

	UPROPERTY(EditAnywhere, Category = "OverCrowd", BlueprintReadWrite)
	float Value = 0.0f;

	UPROPERTY(EditAnywhere, Category = "OverCrowd", BlueprintReadWrite)
	int32 ParamIndex = -1;

	//Convenience for replacing items in an array of FMaterialParamData
	//so only equal if the ParamNames are
	FORCEINLINE bool operator==(const FMaterialParamData& Other) const
	{
		return	(ParamName == Other.ParamName);// &&
				//(ParamIndex == Other.ParamIndex);
	}
};

USTRUCT(Blueprintable)
struct FParameterRange
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "OverCrowd", BlueprintReadWrite)
	FGameplayTag ParamID;

	UPROPERTY(EditAnywhere, Category = "OverCrowd", BlueprintReadWrite)
	UCurveFloat *ValueRange;

	UPROPERTY(EditAnywhere, Category = "OverCrowd", BlueprintReadWrite)
	bool bMustMatchToOverride = false;

	FORCEINLINE bool operator==(const FParameterRange& Other) const
	{
		return	(ParamID == Other.ParamID) &&
			(ValueRange == Other.ValueRange);
	}
};

USTRUCT(Blueprintable)
struct FPerInstanceOverrides
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere,BlueprintReadWrite, Category = "OverCrowd")
	TArray<FMaterialParamData> Overrides;

	int32 GetOverrideIndex(int32 ParamIndex)
	{
		for(int32 Index = 0; Index < Overrides.Num(); Index++)
		{
			if(Overrides[Index].ParamIndex == ParamIndex)
				return Index;
		}
		return INDEX_NONE;
	}
};

struct FCrowdMetaDataPacked
{
	int32 NumCrowdMembers;
	int32 NumWardrobes;
	int32 NumAnimations;
	
};

struct FCrowdMemberPackedData
{
	int32 PositionX;
	int32 PositionY;
	int32 PositionZ;
	
	int32 RotationX;
	int32 RotationY;
	int32 RotationZ;
	int32 RotationW; //size 7
	
	int32 Scale;
	int32 RandomAnimationOffset;
	FColor AnimationState; //size 10

	TStaticArray<FColor, 3>  MeshIndices;
	TStaticArray<FColor, 12> MeshOverrides; //size 23

	TStaticArray<FColor, 9> Metadata; //size 32

	void Init()
	{
		for(int i =0; i < 6; i++)
		{
			MeshIndices[i] = {255, 255, 255, 255};
		}
	}
};

USTRUCT(BlueprintType)
struct FCrowdMemberData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	TMap<UVatMeshPairing*, FPerInstanceOverrides> BodyPartInstances;

	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	FTransform Transform;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	FGameplayTag WardrobeID;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	FVector2D RandomAnimationOffset = {0,0};
	
	UPROPERTY(BlueprintReadWrite, Category = "OverCrowd")
	FGuid ID;

	UPROPERTY(BlueprintReadWrite, Category = "OverCrowd")
	TSubclassOf<class AActor>  SwapCharacterClass;

	UPROPERTY(BlueprintReadWrite, Category = "OverCrowd")
	FGameplayTag AnimationState;

	UPROPERTY(BlueprintReadWrite, Category = "OverCrowd")
	FGameplayTag AdditiveAnimationState;

	FGameplayTag PreviousAnimationState;
	float AnimStartTimeStamp = 0;
};

USTRUCT(Blueprintable)
struct FVatAssetArray
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "OverCrowd", BlueprintReadWrite)
	TArray<UVatMeshPairing*> TaggedVatAssets;

	//UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	//FGameplayTag WardrobeID;

	
	FORCEINLINE bool operator==(const FVatAssetArray& Other) const
	{
		return (TaggedVatAssets == Other.TaggedVatAssets);// && (WardrobeID == Other.WardrobeID);
	}

	FORCEINLINE FVatAssetArray operator+=(const FVatAssetArray& Other)
	{
		TaggedVatAssets.Append( Other.TaggedVatAssets);

		return  *this;
	}
};


USTRUCT(Blueprintable)
struct FWardrobe
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Animation", BlueprintReadWrite)
	UWardrobeAnimation* WardrobeAnimation = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	FGameplayTag WardrobeID;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	TMap< FGameplayTag, FVatAssetArray> Compartments;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	TArray<FParameterRange> MaterialParametersOverrides;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	float ProbabilityWeight = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	FVector2D ScaleVariation = {1.0f,1.0f};

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	FColor WardrobeColor = FColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	TSubclassOf<class AActor>  SwapCharacterClass;
	
	FORCEINLINE bool operator==(const FWardrobe& Other) const
	{

		TArray<FGameplayTag> Keys;
		TArray<FGameplayTag> OtherKeys;
		Compartments.GenerateKeyArray(Keys);
		Other.Compartments.GenerateKeyArray(OtherKeys);

		TArray<FVatAssetArray> Values;
		TArray<FVatAssetArray> OtherValues;
		Compartments.GenerateValueArray(Values);
		Other.Compartments.GenerateValueArray(OtherValues);

		return((Keys == OtherKeys) &&
			(Values == OtherValues) &&
			//(AdditionalMetaData == Other.AdditionalMetaData) &&
			(MaterialParametersOverrides == Other.MaterialParametersOverrides));
	}

	bool Sanitize();
};


USTRUCT(Blueprintable)
struct FAnimationTagPair
{
	GENERATED_BODY()
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="AnimTag"), Category = "Animation")
	FGameplayTag From;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(DisplayName="AdditiveAnimTag"), Category = "Animation")
	FGameplayTag To;
	
	FORCEINLINE bool operator==(const FAnimationTagPair& Other) const
	{
		return((From == Other.From) &&
			(To == Other.To));
	}
};

FORCEINLINE uint32 GetTypeHash(const FAnimationTagPair& Thing)
{
	uint32 Hash = FCrc::MemCrc32(&Thing, sizeof(FAnimationTagPair));
	return Hash;
}


USTRUCT(Blueprintable)
struct FWardrobeVatAssetPair
{
	GENERATED_BODY()
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	FGameplayTag WardrobeID;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	UVatMeshPairing *VatAsset = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	UStaticMesh *DuplicatedMesh = nullptr;
	
	FORCEINLINE bool operator==(const FWardrobeVatAssetPair& Other) const
	{
		return((WardrobeID == Other.WardrobeID) &&
			(VatAsset == Other.VatAsset));
	}
};

FORCEINLINE uint32 GetTypeHash(const FWardrobeVatAssetPair& Thing)
{
	uint32 Hash = FCrc::MemCrc32(&Thing, sizeof(FWardrobeVatAssetPair));
	return Hash;
}


USTRUCT(Blueprintable)
struct FWardrobeVatAssetPairArray
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "OverCrowd", BlueprintReadWrite)
	TArray<FWardrobeVatAssetPair> VatAssets;
};

USTRUCT(Blueprintable)
struct FAnimationTagGroup
{
	GENERATED_BODY()
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation")
	TArray<FAnimationTagPair> Animation;
};

USTRUCT(Blueprintable)
struct FMaskSource
{
	GENERATED_BODY()
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	TArray<UVatMeshPairing*> SourcesForMask;

	UPROPERTY(EditAnywhere, Category = "OverCrowd", BlueprintReadWrite)
	TSoftObjectPtr<UTexture2D> MaskTexture;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadWrite, Category = "OverCrowd")
	int32 MaskIndex;
};
//
// USTRUCT(Blueprintable)
// struct FVatAssetDecorator
// {
// 	GENERATED_BODY()
//
// 	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
// 	UVatMeshPairing* VatAsset;
//
// 	UPROPERTY(BlueprintReadOnly)
// 	int32 Index;
// };


//DEPRECATED
USTRUCT(BlueprintType)
struct FIndexArray
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintReadWrite, Category = "OverCrowd", EditAnywhere, meta = (DisplayName = "IndexArray"))
	TArray<int32> IndexArray;
};

USTRUCT(Blueprintable)
struct FPerCharacterNiagaraData
{
	GENERATED_BODY()
	
	UPROPERTY(BlueprintReadOnly, Category = "OverCrowd")
	TMap<FGameplayTag, int32> BodyPartIndices;

	UPROPERTY(BlueprintReadOnly, Category = "OverCrowd")
	TMap<FGameplayTag, int32> BitMasks;
};

USTRUCT(Blueprintable)
struct FMaskingObjects
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	TArray<UVatMeshPairing*> MaskTargets;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	TArray<FMaskSource> MaskSources;

};

UCLASS(Blueprintable)
class OVERCROWD_API UVatClothingMask : public UPrimaryDataAsset
{
	GENERATED_BODY()
public:

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	FName Title;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MaskSelectionList")
	TArray<TSoftObjectPtr<UTexture2D> > ElementsOfCompositedMask;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	UTexture2D* CompositedClothingMask;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	TArray<FMaskingObjects> MaskingGroup;

	// returns true if dirty
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	bool Collate();

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	void GetBitMaskValue(UVatMeshPairing* DrivingMesh, UPARAM(ref) TArray<UVatMeshPairing*>& BodyPartMeshes, int32& BitMask) const;

	UFUNCTION(BlueprintPure, Category = "OverCrowd")
	static int32 IndexToBitMaskValue(int32 Index);

	bool GetMaskingObjects(UVatMeshPairing* Source, FMaskingObjects& Out) const
	{
		if(!Source)
			return false;

		for (auto MaskingObject : MaskingGroup)
		{
			if (MaskingObject.MaskTargets.Contains(Source))
			{
				Out = MaskingObject;
				return true;
			}
		}
		return false;
	}
};

UCLASS(Blueprintable)
class OVERCROWD_API UVatMeshPairing : public UObject
{
	GENERATED_BODY()
public:

	UPROPERTY(EditAnywhere, Category = "OverCrowd", BlueprintReadWrite)
	FGameplayTag BodyPart;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	TArray<FParameterRange> PerInstanceParameters;

	UPROPERTY(BlueprintReadWrite, Category = "Overcrowd")
	USkeleton* Skeleton;

	UPROPERTY(EditAnywhere, Category = "OverCrowd", BlueprintReadWrite)
	int32 VatUVChannel = 1;

	
	//UPROPERTY(EditAnywhere, Category = "OverCrowd", BlueprintReadWrite)
	//int32 AdditiveBasePoseIndex = 0;

	UPROPERTY(EditAnywhere, Category = "OverCrowd", BlueprintReadWrite)
	FName AttachToSocket;

	UPROPERTY(EditAnywhere, Category = "OverCrowd", BlueprintReadWrite)
	TSoftObjectPtr<UStaticMesh> VatStaticMesh;

	UPROPERTY(VisibleAnywhere, Category = "OverCrowd", BlueprintReadWrite)
	TSoftObjectPtr<USkeletalMesh> SkelMesh;

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	USkeleton* GetSkelMeshSkeleton();
};

UCLASS(Blueprintable)
class OVERCROWD_API UVatStaticMesh : public UVatMeshPairing
{
	GENERATED_BODY()
public:
};

namespace OverCrowdParamNames
{
	//static const FName Frame = TEXT("Frame");
	static const FName AnimIndex = TEXT("AnimIndex");
	static const FName AdditiveSampleRate = TEXT("AdditiveSampleRate");
	//static const FName SampleRate = TEXT("SampleRate");
	static const FName BasePoseIndex = TEXT("BasePoseIndex");
	static const FName NumBasePoses = TEXT("NumBasePoses");
	static const FName AdditiveBasePoseIndex = TEXT("AdditiveBasePoseIndex");
	static const FName NumAdditiveBasePoses = TEXT("NumAdditiveBasePoses");
	static const FName BoneWeightRowsPerFrame = TEXT("BoneWeightsRowsPerFrame");
	static const FName VertexPositionTexture = TEXT("PositionTexture");
	static const FName VertexNormalTexture = TEXT("NormalTexture");
	static const FName BonePositionTexture = TEXT("BonePositionTexture");
	static const FName BoneRotationTexture = TEXT("BoneRotationTexture");
	static const FName AdditiveBonePositionTexture = TEXT("AdditiveBonePositionTexture");
	static const FName AdditiveBoneRotationTexture = TEXT("AdditiveBoneRotationTexture");
	static const FName AdditiveBoneIndex = TEXT("AdditiveBoneIndex");
	static const FName UseUV0 = TEXT("UseUV0");
	static const FName UseUV1 = TEXT("UseUV1");
	static const FName UseUV2 = TEXT("UseUV2");
	static const FName UseUV3 = TEXT("UseUV3");
	static const FName EnableVAT = TEXT("EnableVAT");
	static const FName FrameInterpolation = TEXT("FrameInterpolation");
	static const FName AnimationBlending = TEXT("AnimationBlending");
	static const FName EnableAdditive = TEXT("EnableAdditive");
	//EnableAdditive
}

USTRUCT(Blueprintable)
struct FOverCrowdAnimSequenceInfo
{
	GENERATED_BODY()
	
	UPROPERTY(EditAnywhere, Category = Default, BlueprintReadWrite, meta = (EditCondition = "bEnabled", EditConditionHides))
	TObjectPtr<UAnimSequence> AnimSequence = nullptr;
	
	UPROPERTY(EditAnywhere, Category = Default, BlueprintReadWrite)
	FGameplayTag AnimationTag;

	UPROPERTY(EditAnywhere, Category = Default, BlueprintReadOnly)
	float SampleRate = 30;
};

USTRUCT(Blueprintable)
struct FSkelMeshToStaticMesh
{
	GENERATED_BODY()

	//UPROPERTY(EditAnywhere, Category = Default, BlueprintReadWrite)
	//FGameplayTagContainer GameplayTags;

	UPROPERTY(EditAnywhere, Category = Default, BlueprintReadWrite)
	TObjectPtr<USkeletalMesh> SourceMesh;

	UPROPERTY(EditAnywhere, Category = Default, BlueprintReadWrite)
	TObjectPtr<UStaticMesh> ConvertedMesh;
};

USTRUCT(Blueprintable)
struct FOverCrowdAnimInfo
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = Default, BlueprintReadOnly)
	int32 StartFrame = 0;

	UPROPERTY(VisibleAnywhere, Category = Default, BlueprintReadOnly)
	int32 EndFrame = 0;

	UPROPERTY(VisibleAnywhere, Category = Default, BlueprintReadOnly)
	float SampleRate = 30;

	UPROPERTY(EditAnywhere, Category = Default, BlueprintReadWrite)
	FGameplayTag AnimationTag;

	UPROPERTY(EditAnywhere, Category = Default, BlueprintReadWrite)
	FGameplayTag AdditiveAnimationTag;
};


USTRUCT(Blueprintable)
struct FCachedAnimationIndices
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = Default, BlueprintReadOnly)
	TMap<FGameplayTag, int32> PrimaryAnimationIndices;

	UPROPERTY(VisibleAnywhere, Category = Default, BlueprintReadOnly)
	TMap<FGameplayTag, int32> AdditiveAnimationIndices;

	UPROPERTY(VisibleAnywhere, Category = Default, BlueprintReadOnly)
	TMap<FGameplayTag, float> IncurredAnimOffsets;

	UPROPERTY(VisibleAnywhere, Category = Default, BlueprintReadOnly)
	TMap<FGameplayTag, float> FirstAnimDurations;

	UPROPERTY(VisibleAnywhere, Category = Default, BlueprintReadOnly)
	TMap<FAnimationTagPair, FGameplayTag> AnimationChainTags;

	UPROPERTY(VisibleAnywhere, Category = Default, BlueprintReadOnly)
	TMap<FGameplayTag, FAnimationTagPair> AnimationChainBookEnds;
};

USTRUCT(Blueprintable)
struct FAnimationChain
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Animation")
	FGameplayTag AnimA;

	UPROPERTY(EditAnywhere, Category = "Animation")
	FGameplayTag AnimB;

	UPROPERTY(EditAnywhere, Category = "Animation")
	FGameplayTag AnimC;

	UPROPERTY(EditAnywhere, Category = "Animation")
	FGameplayTag AnimD;

	UPROPERTY(BlueprintReadOnly, Category = "Animation")
	float IncurredAnimOffset = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Animation")
	float FirstAnimDuration = 0;

	void GetBookEnds(FAnimationTagPair &BookEnd);

};


UCLASS(Blueprintable)
class OVERCROWD_API UAnimationsToBakeDataAsset : public UPrimaryDataAsset
{
public:
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
private:
	GENERATED_BODY()

public:
	/** If the skeletal meshes have different yet compatible skeletons a mismatch can occur so a common reference skeleton is needed,
		otherwise (if this is left empty) it will be filled by the first animation sequence's preview mesh*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation")
	TSoftObjectPtr<USkeletalMesh> AnimationReferenceSkeleton;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation", Meta = (DisplayName = "Animation Sources"))
	TArray<FOverCrowdAnimSequenceInfo> AnimSequences;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation")
	TMap<FGameplayTag,FAnimationChain> AnimationChains;

	UPROPERTY(EditAnywhere, Category = "Material Static Switches", BlueprintReadWrite)
	bool bFrameInterpolation = true;

	UPROPERTY(EditAnywhere, Category = "Material Static Switches", BlueprintReadWrite)
	bool bAnimationBlending = true;

	UFUNCTION(BlueprintCallable, Category = "Animation")
	void GetCachedAnimationIndices(TMap<FGameplayTag, int32> &Out_AnimationIndices);

	int32 GetAnimationIndex(FGameplayTag FindAnimationTag);
	
	UFUNCTION(BlueprintCallable, Category = "Animation")
	void GetCachedAnimationChains(TMap<FAnimationTagPair, FGameplayTag> &Out_BookEndsToAnimationChains, TMap<FGameplayTag, FAnimationTagPair> &Out_AnimationChainToBookEnds);

	UFUNCTION(BlueprintCallable, Category = "Animation")
	void GetCachedIncurredAnimOffsets(TMap<FGameplayTag, float> &Out_AnimationOffsets, TMap<FGameplayTag, float>& Out_FirstAnimationDurations);

	UFUNCTION(BlueprintCallable, Category = "Animation")
	void GetFirstAnimationDuration(TMap<FGameplayTag, float> &Out_Durations);
	
};

UCLASS(Blueprintable, BlueprintType)
class OVERCROWD_API UVatAnimationDataAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	TSoftObjectPtr<UTexture2D> BonePositionTexture;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	TSoftObjectPtr<UTexture2D> BoneRotationTexture;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Additve Animation", meta = (EditCondition = "AdditiveVatAnimationDataAsset"))
	FName AdditveRootBoneName = "head";

	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "Additve Animation")
	int32 AdditveRootBoneIndex = 0;

	//UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "Additve Animation")
	//int32 NumBasePoses = 1;

	//UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "Additve Animation")
	//int32 NumAdditveBasePoses = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Additve Animation")
	UVatAnimationDataAsset* AdditiveVatAnimationDataAsset;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation")
	UAnimationsToBakeDataAsset* AnimationsToBake;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Info")
	TArray<FOverCrowdAnimInfo> Animations;
	
	UFUNCTION(BlueprintCallable, Category = Default)
	UPARAM(DisplayName = "AnimSequence") UAnimSequence* GetAnimationSequenceWithTag(FGameplayTagContainer AnimationTags,  int32& AnimSequenceIndex, int32& StartFrame, int32& EndFrame, float &SampleRate);

	//UFUNCTION(BlueprintCallable, Category = Default)
	//FCachedAnimationIndices GetCachedAnimationIndices();

	// If we weren't in a plugin, we could unify this in a base class
	template<typename AssetType>
	static AssetType* GetAsset(const TSoftObjectPtr<AssetType>& AssetPointer)
	{
		AssetType* ReturnVal = nullptr;
		if (AssetPointer.ToSoftObjectPath().IsValid())
		{
			ReturnVal = AssetPointer.Get();
			if (!ReturnVal)
			{
				AssetType* LoadedAsset = Cast<AssetType>(AssetPointer.ToSoftObjectPath().TryLoad());
				if (ensureMsgf(LoadedAsset, TEXT("Failed to load asset pointer %s"), *AssetPointer.ToString()))
				{
					ReturnVal = LoadedAsset;
				}
			}
		}
		return ReturnVal;
	}



#define VatAnimationDataAsset_ASSET_ACCESSOR(ClassName, PropertyName) \
	FORCEINLINE ClassName* Get##PropertyName() const { return GetAsset(PropertyName); }

	VatAnimationDataAsset_ASSET_ACCESSOR(UTexture2D, BonePositionTexture);
	VatAnimationDataAsset_ASSET_ACCESSOR(UTexture2D, BoneRotationTexture);

	// UFUNCTION(BlueprintPure, Category = Default, meta = (DisplayName = "Get Bone Position Texture"))
	// UTexture2D* BP_GetBonePositionTexture() { return GetBonePositionTexture(); }
	//
	// UFUNCTION(BlueprintPure, Category = Default, meta = (DisplayName = "Get Bone Rotation Texture"))
	// UTexture2D* BP_GetBoneRotationTexture() { return GetBoneRotationTexture(); }

};

UCLASS(Blueprintable)
class OVERCROWD_API UWardrobeCollection : public UPrimaryDataAsset
{
	GENERATED_BODY()
public:
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OverCrowd")
	TMap<FGameplayTag, FWardrobe> BespokeWardrobes;
	
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	bool AddOrUpdateWardrobe(FGameplayTag Key, const FWardrobe& Wardrobe);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	bool Sanitize();
	
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	bool RemoveWardrobesNotInList(TArray<FGameplayTag> ToKeep);
};
//
// UCLASS(Blueprintable)
// class OVERCROWD_API UAnimationDataAssetCollection : public UPrimaryDataAsset
// {
// 	GENERATED_BODY()
// public:
//
// 	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings")
// 	TMap<USkeleton*, UVatAnimationDataAsset*> Defaults;
//
// 	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings")
// 	TMap<UVatMeshPairing*, UVatAnimationDataAsset*> AnimationDataAssets;
// 	
// };
//

UCLASS(Blueprintable)
class OVERCROWD_API USaveSettings : public UPrimaryDataAsset
{
	GENERATED_BODY()
public:

	// UPROPERTY(EditAnywhere, Category = "Settings", BlueprintReadWrite)
	// FGameplayTag FilterIn;
	//
	// UPROPERTY(EditAnywhere, Category = "Settings", BlueprintReadWrite)
	// FGameplayTag FilterOut;
	//
	// UPROPERTY(EditAnywhere, Category = "Settings", BlueprintReadWrite)
	// USkeleton* FilterSkeleton;

	UPROPERTY(EditAnywhere, Category = "Settings", BlueprintReadWrite)
	TSubclassOf<class APopulateScene>  PopulateSceneClass;

	UPROPERTY(EditAnywhere, Category = "Settings", BlueprintReadWrite)
	UWardrobeCollection* CrowdMemberType;

	UPROPERTY(EditAnywhere, Category = "Settings", BlueprintReadWrite)
	FString SelectedWardrobe;
};

USTRUCT(BlueprintType)
struct FHISMData
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OverCrowd", meta = (DisplayName = "HISM", MakeStructureDefaultValue = "None"))
	TObjectPtr<UHierarchicalInstancedStaticMeshComponent> HISM;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OverCrowd", meta = (DisplayName = "CrowdMemberInstanceIndices", MakeStructureDefaultValue = "()"))
	TMap<FGuid, int32> CrowdMemberInstanceIndices;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OverCrowd", meta = (DisplayName = "DrivingMesh"))
	TObjectPtr<UVatMeshPairing> DrivingMesh;
};


USTRUCT(Blueprintable)
struct FMaterialReplacements
{
	GENERATED_BODY()
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Material")
	TArray<UMaterialInterface*> Materials;
};

UCLASS(Blueprintable)
class OVERCROWD_API UWardrobeAnimation : public UPrimaryDataAsset
{
	GENERATED_BODY()
public:

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation")
	FGameplayTag WardrobeID;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation")
	TMap<USkeleton*, UVatAnimationDataAsset*> AnimAssets;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation")
	UAnimationsToBakeDataAsset* AnimationsToBake;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation")
	TMap<UVatMeshPairing*, FMaterialReplacements> MaterialReplacements;

	UFUNCTION(BlueprintCallable, Category = "Animation")
	bool FindOrCreateMaterialReplacements(UVatMeshPairing* ForVatMesh, FMaterialReplacements& Out);

	UFUNCTION(BlueprintCallable, Category = "Animation")
	UVatAnimationDataAsset* FindOrCreateAnimationAsset(UVatMeshPairing* ForVatMesh);
};



