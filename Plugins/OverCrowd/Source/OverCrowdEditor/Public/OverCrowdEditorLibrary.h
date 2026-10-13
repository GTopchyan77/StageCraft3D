// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved

#pragma once

#include "OverCrowdDataAsset.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Materials/MaterialLayersFunctions.h"

#include "Engine/SkeletalMeshLODSettings.h"

#include "OverCrowdMeshMapping.h"
#include "OverCrowdUtils.h"

#include "OverCrowdEditorLibrary.generated.h"

class UStaticMesh;
class USkeletalMesh;

class USkeletalMeshLODSettingsAccess : public USkeletalMeshLODSettings
{
public:
	TArray<FSkeletalMeshLODGroupSettings>& GetLODGroups(){return LODGroups;};
};

UCLASS()
class UOverCrowdEditorLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_UCLASS_BODY()

#if WITH_EDITOR

	UFUNCTION(BlueprintCallable, Category = "OverCrowd", meta = (PartialBoneIndex = "None"))
	static UPARAM(DisplayName = "bSuccess") bool BakeAnimationToTextures(UVatAnimationDataAsset* VatAnimDataAsset, const TArray<UVatMeshPairing*> &SourceSkelMeshes, FName PartialBoneName, TArray<UVatMeshPairing*> &OutDestMeshes);

	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	static UPARAM(DisplayName = "bSuccess") bool FillOutClothingMask(UVatClothingMask *ClothingMask);

	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	static UPARAM(DisplayName = "bSuccess") bool WriteCrowdMemberDataToTexture(const TArray<FCrowdMemberData> &CrowdMembers,const TMap<FGameplayTag, FWardrobeVatAssetPairArray>& SortedBodyParts,
		const TArray<FGameplayTag> &WardrobeIndices,UTexture2D *OutTexture);
	
	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	static bool DoesPackageExist(FString PackageName);

	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	static UPARAM(DisplayName = "bModified") bool SanitizeForMask(UTexture2D* Texture);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static UStaticMesh* ConvertSkeletalMeshToStaticMesh(USkeletalMesh* SkeletalMesh, FString PackageName, bool bOverwrite = false);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static UPARAM(DisplayName = "Index")int32 GetBoneIndex(const USkeleton* Skeleton, FName BoneName);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static UPARAM(DisplayName = "bSuccess") bool BakeSkinWeightsToStaticMesh(USkeletalMesh* SkelMesh, UStaticMesh* StaticMesh, int32 UVChannel, FName AttachToSocket);
	
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static UPARAM(DisplayName = "bSuccess") bool GenerateLODsForSkeletalMesh(USkeletalMesh* SkeletalMesh, const int32 NumLODs = 8, int32 MinVertices = 50, float ScreenSizeAtLowestLOD = 0.01, float ScreenSizeAtHighestLOD = 1.0f);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static UPARAM(DisplayName = "bSuccess") bool StripLODsForSkeletalMesh(USkeletalMesh* SkeletalMesh);

	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	static void PutStringIntoCopyPasteBuffer(FString Str);
	
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static UPARAM(DisplayName = "bSuccess") bool SetLightMapIndex(UStaticMesh* StaticMesh, const int32 LODIndex, const int32 LightmapIndex=1, bool bGenerateLightmapUVs=true);

	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	static UPARAM(DisplayName = "bSuccess") bool UpdateMaterialInstanceFromDataAsset(const UVatAnimationDataAsset* DataAsset, UVatMeshPairing* Mesh, class UMaterialInstanceConstant* MaterialInstance,
		const EMaterialParameterAssociation MaterialParameterAssociation = EMaterialParameterAssociation::LayerParameter);

	// UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	// static UPARAM(DisplayName = "bModified") bool UpdateMaterialInstances(const UVatAnimationDataAsset* DataAsset, UVatMeshPairing* VatAsset, class UMaterialInstanceConstant* MaterialInstance,
	// 	const EMaterialParameterAssociation MaterialParameterAssociation = EMaterialParameterAssociation::LayerParameter);

	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	static void RemoveRepeatedVertexAnimatedTextures(TArray<UVatAnimationDataAsset*> DataAssets);

	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	static UPARAM(DisplayName = "bModified") bool UpdateTextureOnMaterialInstances(UTexture2D* Texture, FName ParamName, class UMaterialInstanceConstant* MaterialInstance, const EMaterialParameterAssociation MaterialParameterAssociation);

	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	static UPARAM(DisplayName = "bModified") bool UpdateScalarOnMaterialInstances(float Value, FName ParamName, class UMaterialInstanceConstant* MaterialInstance, const EMaterialParameterAssociation MaterialParameterAssociation);

	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	static void CopyAssetThumbnail(UObject *FromAsset, UObject* ToAsset);

	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	static void GetAllVatMeshPairingAssets(TArray<FAssetData> &AssetData);

	
	//UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	//static TMap<UVatAnimationDataAsset*, FVatAssetArray > GetGroupedAnimationDataAssets(TArray<UVatMeshPairing*> FilterList);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static TMap<UVatAnimationDataAsset*, FVatAssetArray > GroupAnimationDataAssetsByBodyPart(TArray<UVatMeshPairing*> VatMeshToGroup, UWardrobeAnimation* WardrobeAnimation);


	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	static void CleanWardrobes(UWardrobeCollection* CrowdMemberType);

	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	static UEditorUtilityWidget* OpenUtilityBlueprint(FString FullPath, FName& Out_TabID);

	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	static void CloseUtilityBlueprint(FName TabID);

	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	static void MarkDirty(UObject *Obj);

	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	static bool IsDirty(UObject* Obj);

	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	static void NavToAssetInContentBrowser(UObject *ObjectToNavTo);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static bool CreateNewGameplayTag(FString TagName, FGameplayTag& OutTag);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static void GetViewportCameraLocation(FVector& ViewPos, FRotator& ViewRot);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static void GetNextLevelTagsWithParentTag(const FGameplayTag& InparentTag, TArray<FGameplayTag>& OutTags);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static bool CheckIfInEditorAndInPIE();

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static bool IsCtrlButtonDown();

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static void GetTexturesFromMaterial(UMaterialInterface* Material, TMap<FString, UTexture*>& OutTextures);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static UMaterialInstanceConstant* GetOrCreateChildMaterial(UMaterialInterface* Material, FString Suffix = "_VAT");

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static UMaterialInstanceConstant* CreateChildMaterial(UMaterialInterface* Material, FString PackageName);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static FString GetMaterailPackageName(UMaterialInterface* Material);
#endif // WITH_EDITOR

private:

	
	
	// Runs some validations for the assets in DataAsset
	static bool GetValidAnimSequences(const UVatAnimationDataAsset* DataAsset, const TArray<FSkelMeshToStaticMesh> &MeshesToConvert , TArray<FOverCrowdAnimSequenceInfo>& OutAnimSequences);

	// Returns Start, EndFrame and NumFrames in Animation
	static int32 GetAnimationFrameRange(const FOverCrowdAnimSequenceInfo& Animation, int32& OutStartFrame, int32& OutEndFrame);

	// Gets RefPose Bone Position and Rotations.
	static int32 GetRefBonePositionsAndRotations(const USkeletalMesh* SkeletalMesh, 
	TArray<FVector3f>& OutBoneRefPositions, TArray<FVector4f>& OutBoneRefRotations);

	// Gets Bone Position and Rotations for Current Pose.	
	// The BonePosition is returned relative to the RefPose
	static int32 GetBonePositionsAndRotations(const USkinnedMeshComponent* SkeletalMeshComponent, const TArray<FVector3f>& BoneRefPositions, TArray<FVector3f>& BonePositions, TArray<FVector4f>& BoneRotations);

	// Normalizes Positions and Rotations between [0-1] with Bounding Box
	static void NormalizeBoneData(	const TArray<FVector4f>& Rotations, TArray<FVector4f>& OutNormalizedRotations);

	/* Returns best resolution for the given data. 
	*  Returns false if data doesnt fit in the the max range */
	static bool FindBestResolution(const int32 NumFrames, const int32 NumElements, int32& OutHeight, int32& OutWidth);

	/* Sets Static Mesh FullPrecisionUVs Property*/
	static void SetFullPrecisionUVs(UStaticMesh* StaticMesh, const int32 LODIndex, bool bFullPrecision=true);

	/* Creates UV Coord with vertices */
	static bool CreateUVChannelForBoneIndices(UStaticMesh* StaticMesh, const int32 LODIndex, const int32 UVChannelIndex, const TArray<FVector4f>& InBoneIndices, const TArray< FColor>& InBoneWeights);
};
