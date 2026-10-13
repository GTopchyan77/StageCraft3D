// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
//#include "NiagaraMeshRendererProperties.h"
#include "OverCrowdDataAsset.h"
#include "OverCrowdRuntimeLibrary.generated.h"


/**
 * 
 */
UCLASS()
class OVERCROWD_API UOverCrowdRuntimeLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	
	
	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	static FWardrobe CombineWardrobes(const FWardrobe& A, const FWardrobe& B);

	UFUNCTION(BlueprintCallable, meta = (Category = "Util"))
	static void RunConstructionScript(AActor *Actor);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static UActorComponent* SetCreationMethod(UActorComponent* ActorComponent, EComponentCreationMethod method);

	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	static void MarkDirty(UObject* Obj);

	UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	static bool IsDirty(UObject* Obj);

	
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static void ExtractVatMeshArray(const FCrowdMemberData& CrowdMemberData, TArray<UVatMeshPairing*> &OutVatArray);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static void GetRandomVariationFromWardrobe(FWardrobe Wardrobe, TMap<FGameplayTag, UVatMeshPairing*>& OutBodyPartMeshes);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static void GetAllMeshPairingsFromWardrobe(FWardrobe Wardrobe, TArray<UVatMeshPairing*>& Out_VatMeshPairings);


	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static void GetMultipleVariationsFromWardrobes(TArray<FPerCharacterNiagaraData> &OutData, const UVatClothingMask* ClothingMask, const TArray<FWardrobe>& Wardrobes, const TMap<UVatMeshPairing*, int32>& IndexLookup, int32 NumVariations);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static void MakeSortedVatAssetArrayFromWardrobes(const TArray<FWardrobe>& Wardrobes, TMap<FGameplayTag, FWardrobeVatAssetPairArray>& Out_SortedBodyParts);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static void GetAllWardrobesFromCrowdMembers(const TArray<FCrowdMemberData> &NPCs, TArray<FGameplayTag> &Out_WardrobeIDs);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static void ExtractBodyPartIndicesIntoArray(const TArray<FPerCharacterNiagaraData> &PerCharacterData, FGameplayTag BodyPartTag, TArray<int32>& OutIndices);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static void ExtractBodyPartBitMaskIntoArray(const TArray<FPerCharacterNiagaraData>& PerCharacterData, FGameplayTag BodyPartTag, TArray<int32>& OutBitMasks);
	
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static bool CalculateAndSetBitMasks(UPARAM(ref) FCrowdMemberData &CrowdMemberData, const UVatClothingMask* ClothingMask, int32 MaskOverrideSlot = 8);
	
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static void GetAllWardrobes(TArray<FWardrobe>& OutWardrobes, TArray<FGameplayTag> ShowOnlyList, TArray<FGameplayTag> HideList);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static bool GetWardrobeFromTag(FWardrobe& OutWardrobe, FGameplayTag WardrobeID);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static bool BuildCrowdMemberDataAtLocations(const TArray<FAnimationTagGroup> AnimationGroups, const TArray<FWardrobe>& Wardrobes, const TArray<FTransform> &AtLocations, TArray<FCrowdMemberData> &OutNPCs);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static bool CreateRandomCrowdMemberData(const FWardrobe &Wardrobe,  FCrowdMemberData& OutNPC);

	//UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	//static void AddMeshRendererToNiagaraSystem(UNiagaraSystem* NiagaraSystem, UStaticMesh* Mesh, UMaterialInterface* Material);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static void GenerateMeshRenderers(ANiagaraCrowd* NiagaraCrowd, const TMap<FGameplayTag, FWardrobeVatAssetPairArray>& SortedBodyParts);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static TArray<FMaterialParamData> PickRandomMaterialOverrides(TArray<FParameterRange> PerInstanceParams, const TArray<FGameplayTag> FilterOut);
	
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static void GetAllBodyPartsFromWardrobes(const TArray<FWardrobe>& Wardrobes, TMap<FGameplayTag, FVatAssetArray>& OutBodyParts, TMap<UVatMeshPairing*, int32>& OutIndexLookup);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static TArray<FVector> RemoveLocationsThatIntersect(const TArray<FVector>& InLocations, float DistanceThreshold, FBox2D Extent);
	
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static bool GetMaterialParamWithTag(UPARAM(ref) TArray<FMaterialParamData>& MaterialParams, FGameplayTag Tag, FMaterialParamData& FoundParam);
	
	UFUNCTION(BlueprintPure, Category = "OverCrowd")
	static bool GetParamIndexFromTag(FGameplayTag ParamTag, int32& Index);

	UFUNCTION(BlueprintPure, Category = "OverCrowd")
	static void GetAvailableWardrobeTags(TArray<FGameplayTag> &Tags);
	
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static FString GetPluginSaveDirectory();

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static FGameplayTag GetClothingMaskTag();
	
	//UFUNCTION(BlueprintCallable)
	//static FVector GetEditorCameraLocation();
	
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "OverCrowd")
	static UVatClothingMask* GetClothingMaskDataAsset();

	//UFUNCTION(BlueprintCallable, BlueprintPure, Category = "OverCrowd")
	//static TArray<UVatAnimationDataAsset*> GetAnimationDataAssets();

	//UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	//static void PopulateAnimationDataAssetCollection(TArray<UVatMeshPairing*> VatMeshes);
	
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "OverCrowd")
	static UVatAnimationDataAsset* FindAnimationDataAsset(UVatMeshPairing* VatMesh, FGameplayTag WardrobeID);

	//UFUNCTION(BlueprintCallable, BlueprintPure, Category = "OverCrowd")
	//static bool IsUsingDefaultAnimationDataAsset(UVatMeshPairing* VatMesh);

	//UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	//static void UseDefaultAnimationDataAsset(UVatMeshPairing* VatMesh);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static TArray<UWardrobeCollection*>  GetWardrobeCollections();

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static bool IsSameSkeleton(const TArray<UVatMeshPairing*> &VatMeshes);

	//static UAnimationDataAssetCollection* CreateOrLoadDefaultAnimationDataAssetCollection();

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static UWardrobeAnimation* CreateOrLoadWardrobeAnimation(FGameplayTag WardrobeID);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static UStaticMesh* DuplicateStaticMeshAndCreatePackage(UStaticMesh *CopyFrom, FString NewAssetName);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static UAnimationsToBakeDataAsset* CreateOrLoadAnimationsToBake(FGameplayTag WardrobeID);

	static FString GetSanitizedWardrobeName(FGameplayTag WardrobeID);
	static FString GetSanitizedName(FGameplayTag OverCrowdTag, FString AddtionalStringToChop);

	UFUNCTION(BlueprintPure,BlueprintCallable, Category = "OverCrowd")
	static FString GetSanitizedPlacementGroupName(FGameplayTag PlacementGroup);

	//UFUNCTION(BlueprintCallable, meta = (Category = "OverCrowd"))
	//static UPARAM(DisplayName = "bSuccess") bool WriteCrowdMemberDataToTextureRuntime(const TArray<FCrowdMemberData> &CrowdMembers,const TMap<FGameplayTag, FWardrobeVatAssetPairArray>& SortedBodyParts, UTexture2D *OutTexture);


	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static UTexture2D* LoadTexture(FString AssetName, FString PackagePath);
	
#if WITH_EDITOR
	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static UMaterialInstanceConstant* GetOrCreateChildMaterial(UMaterialInterface* Material, FString Suffix);
#endif
	
	//UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	//static void AddAnimationDataAsset(UVatMeshPairing* VatMesh, UVatAnimationDataAsset* AnimAsset, bool SetAsDefault = false);

	UFUNCTION(BlueprintCallable, Category = "OverCrowd")
	static void MoveActorsToLevel(ULevel* Level, TArray<AActor*> Actors, AActor* AttachTo);
};
