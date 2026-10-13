// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved


#include "NiagaraCrowd.h"
#include "OverCrowd.h"
#include "NiagaraSystem.h"
#include "OverCrowdDataAsset.h"
#include "OverCrowdRuntimeLibrary.h"

ANiagaraCrowd::ANiagaraCrowd() 
{
	//try
	{

		PlacementGroup = FGameplayTag::RequestGameplayTag(FName("OverCrowd.PlacementGroup.Niagara"));

	}
	

	PrimaryActorTick.bCanEverTick = true;
	
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("RootComponent"));

	// Create a Static Mesh Component
	CrowdEmitter = CreateDefaultSubobject<UNiagaraComponent>(TEXT("NiagaraComponent"));

	// Attach the Static Mesh to the Root Component
	CrowdEmitter->SetupAttachment(RootComponent);

}

#if WITH_EDITOR
void ANiagaraCrowd::GetPackageNamesForDuplicates(const TMap<FGameplayTag, FWardrobeVatAssetPairArray>& NewSortedBodyParts, TArray<FString>& Out_PackageNames)
{
	for(auto& SortedBodyPart : NewSortedBodyParts)
	{
		FWardrobeVatAssetPairArray VatAssets = SortedBodyPart.Value;
		
		for (int MeshIndex = 0;FWardrobeVatAssetPair& WardrobeVatAsset : VatAssets.VatAssets)
		{
			FWardrobe Wardrobe;
			UOverCrowdRuntimeLibrary::GetWardrobeFromTag(Wardrobe, WardrobeVatAsset.WardrobeID);
		
			if(Wardrobe.WardrobeAnimation)
			{
				if(Wardrobe.WardrobeAnimation->MaterialReplacements.Contains(WardrobeVatAsset.VatAsset) && WardrobeVatAsset.VatAsset)
				{
					FString NewPackageName = UOverCrowdRuntimeLibrary::GetSanitizedWardrobeName(WardrobeVatAsset.WardrobeID) +
							UOverCrowdRuntimeLibrary::GetSanitizedName(SortedBodyPart.Key,"BodyPart") + FString::Printf(TEXT("%d"),MeshIndex);

					Out_PackageNames.Add(NewPackageName);
				}
			}
			MeshIndex++;
		}
	}
}

bool ANiagaraCrowd::IsSortedBodyPartsMapEqual(const TMap<FGameplayTag, FWardrobeVatAssetPairArray>& SortedBodyPartsA, const TMap<FGameplayTag, FWardrobeVatAssetPairArray>& SortedBodyPartsB)
{
	if(SortedBodyPartsA.Num() != SortedBodyPartsB.Num())
		return false;
	
	for(auto& SortedBodyPartA : SortedBodyPartsA)
	{
		auto BodyPartKey = SortedBodyPartA.Key;
		if(!SortedBodyPartsB.Contains(BodyPartKey))
			return false;

		const auto VatAssetsB = SortedBodyPartsB[BodyPartKey].VatAssets;
		const auto VatAssetsA = SortedBodyPartA.Value.VatAssets;

		if(VatAssetsA.Num() != VatAssetsB.Num())
			return false;
		
		for(int Index = 0; Index < VatAssetsA.Num(); Index++)
		{
			if(VatAssetsA[Index].VatAsset != VatAssetsB[Index].VatAsset)
				return false;
			
			if(VatAssetsA[Index].DuplicatedMesh != VatAssetsB[Index].DuplicatedMesh)
				return false;

			if(VatAssetsA[Index].VatAsset && !VatAssetsA[Index].VatAsset->VatStaticMesh.IsNull() && !VatAssetsB[Index].DuplicatedMesh)
				return false;
		}
	}
	return true;
}
#endif

// Called when the game starts or when spawned
void ANiagaraCrowd::BeginPlay()
{
	Super::BeginPlay();
}

// Called every frame
void ANiagaraCrowd::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}

bool ANiagaraCrowd::SetNewSortedBodyParts(const TMap<FGameplayTag, FWardrobeVatAssetPairArray>& NewSortedBodyParts)
{
	UObject *NewOwner = nullptr;

#if WITH_EDITOR
	if(IsSortedBodyPartsMapEqual(SortedBodyParts ,NewSortedBodyParts))
		return true;
#endif
	SortedBodyParts = NewSortedBodyParts;
	for(auto& SortedBodyPart : SortedBodyParts)
	{
		FWardrobeVatAssetPairArray& VatAssets = SortedBodyPart.Value;
		
		for (int MeshIndex = -1;FWardrobeVatAssetPair& WardrobeVatAsset : VatAssets.VatAssets)
		{
			MeshIndex++;
			UE_LOG(LogOverCrowdRuntime, Display, TEXT("Sorted BodyParts %s"), *WardrobeVatAsset.VatAsset->GetName());
			
			FWardrobe Wardrobe;
			UOverCrowdRuntimeLibrary::GetWardrobeFromTag(Wardrobe, WardrobeVatAsset.WardrobeID);
		
			UStaticMesh *SrcMesh = nullptr;
			UStaticMesh *DuplicatedMesh = nullptr;
			if(!Wardrobe.WardrobeAnimation)
				continue;

			if(!(Wardrobe.WardrobeAnimation->MaterialReplacements.Contains(WardrobeVatAsset.VatAsset) && WardrobeVatAsset.VatAsset))
				continue;
			
			DuplicatedMesh = WardrobeVatAsset.DuplicatedMesh; 
			if(!DuplicatedMesh)
			{
				SrcMesh = WardrobeVatAsset.VatAsset->VatStaticMesh.LoadSynchronous();
				if(!SrcMesh)
					continue;

				FString NewMeshName = UOverCrowdRuntimeLibrary::GetSanitizedWardrobeName(WardrobeVatAsset.WardrobeID) +
					UOverCrowdRuntimeLibrary::GetSanitizedName(SortedBodyPart.Key,"BodyPart") + FString::Printf(TEXT("%d"),MeshIndex);
				DuplicatedMesh = UOverCrowdRuntimeLibrary::DuplicateStaticMeshAndCreatePackage(SrcMesh, NewMeshName);

				auto MaterialReplacements = Wardrobe.WardrobeAnimation->MaterialReplacements[WardrobeVatAsset.VatAsset];
				TArray<FStaticMaterial> NewMaterials;
				for (auto Material : MaterialReplacements.Materials)
				{
					NewMaterials.Add(FStaticMaterial(Material));
				}
				DuplicatedMesh->SetStaticMaterials(NewMaterials);
				
				UOverCrowdRuntimeLibrary::MarkDirty(DuplicatedMesh);

				WardrobeVatAsset.DuplicatedMesh = DuplicatedMesh; 
			}
		}
	}
	return false;
}

