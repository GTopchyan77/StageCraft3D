// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved


#include "OverCrowdRuntimeLibrary.h"
#include "OverCrowd.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraConstants.h"
#if WITH_EDITOR
	#include "IAssetTools.h"
	#include "AssetToolsModule.h"
	#include "Factories/DataAssetFactory.h"
	#include "EditorLevelUtils.h"
	#include "PackageHelperFunctions.h"
	#include "AssetRegistry/AssetRegistryModule.h"
	#include "Factories/MaterialInstanceConstantFactoryNew.h"

#endif // #if WITH_EDITOR

#include "Curves/CurveFloat.h"
#include "GameplayTagsManager.h"
#include "OverCrowdDeveloperSettings.h"
#include "GenericQuadTree.h"
#include "NiagaraCrowd.h"

#include "Materials/MaterialInstanceConstant.h"

#ifndef LOCTEXT_NAMESPACE
	#define LOCTEXT_NAMESPACE "VatRuntimeLibrary"
#endif

FWardrobe UOverCrowdRuntimeLibrary::CombineWardrobes(const FWardrobe& A, const FWardrobe& B)
{
	FWardrobe Res = A;

	Res.MaterialParametersOverrides.Append(B.MaterialParametersOverrides);

	for (auto El : B.Compartments)
	{
		FVatAssetArray AssArray = Res.Compartments.FindOrAdd(El.Key);
		AssArray += El.Value;
		Res.Compartments.Add(El.Key, AssArray);
	}
	return Res;
}

void UOverCrowdRuntimeLibrary::RunConstructionScript(AActor* Actor)
{
#if WITH_EDITOR
	Actor->RerunConstructionScripts();
#endif // #if WITH_EDITOR
}


UActorComponent* UOverCrowdRuntimeLibrary::SetCreationMethod(UActorComponent* ActorComponent, EComponentCreationMethod CreationMethod)
{
#if WITH_EDITOR
	if (ActorComponent != nullptr)
	{
		ActorComponent->CreationMethod = CreationMethod;
	}
	
#endif // #if WITH_EDITOR
	return ActorComponent;
}


void UOverCrowdRuntimeLibrary::MarkDirty(UObject* Obj)
{
	#if WITH_EDITOR
		if(Obj)
			bool bResult = Obj->MarkPackageDirty();
	#endif // #if WITH_EDITOR

}
bool UOverCrowdRuntimeLibrary::IsDirty(UObject* Obj)
{
	bool Res = false;
	#if WITH_EDITOR

		if (!Obj)
			return Res;

		Res =  Obj->GetPackage()->IsDirty();
	#endif // #if WITH_EDITOR

	return Res;
}

void UOverCrowdRuntimeLibrary::ExtractVatMeshArray(const FCrowdMemberData& CrowdMemberData, TArray<UVatMeshPairing*> &OutVatArray)
{
	for (auto BodyPart : CrowdMemberData.BodyPartInstances)
	{
		if (BodyPart.Key)
		{
			OutVatArray.Add(BodyPart.Key);
		}
	}
}


bool UOverCrowdRuntimeLibrary::GetMaterialParamWithTag(UPARAM(ref)TArray<FMaterialParamData>& MaterialParams, FGameplayTag Tag, FMaterialParamData& FoundParam)
{
	for (auto Param : MaterialParams)
	{
		if (Param.ParamName == Tag)
		{
			FoundParam = Param;

			return true;
		}
	}
	return false;
}

bool UOverCrowdRuntimeLibrary::GetParamIndexFromTag(FGameplayTag ParamTag, int32& Index)
{
	FString TagString = ParamTag.ToString();
	TArray<FString> StringArray;
	TagString.ParseIntoArray(StringArray, TEXT("."), true);

	for (auto Str : StringArray)
	{
		FString Left, Right;
		if (Str.Contains("Param"))
		{
			Str.Split(TEXT("Param"), &Left, &Right);
			Index = FCString::Atoi(*Right);

			return true;
		}
	}
	return false;
}

void UOverCrowdRuntimeLibrary::GetAvailableWardrobeTags(TArray<FGameplayTag>& Tags)
{
	for(auto WC: GetWardrobeCollections())
	{
		for(auto Wardrobe: WC->BespokeWardrobes)
		{
			Tags.AddUnique(Wardrobe.Key);
		}
	}
}

FString UOverCrowdRuntimeLibrary::GetPluginSaveDirectory()
{
	return UOverCrowdDeveloperSettings::Get()->SaveDirectory.Path;;
}

FGameplayTag UOverCrowdRuntimeLibrary::GetClothingMaskTag()
{
	FGameplayTag Out = UOverCrowdDeveloperSettings::Get()->ClothingMaskTag;
	
#if WITH_EDITOR
	if(FGameplayTag::EmptyTag == Out)
	{
		Out = UOverCrowdDeveloperSettings::Get()->ClothingMaskTag =  FGameplayTag::RequestGameplayTag(TEXT("OverCrowd.Material.Param8.ClothingMask"));
	}
#endif
	
	return Out;
}

//
// FVector UOverCrowdRuntimeLibrary::GetEditorCameraLocation()
// {
// 	FVector OutLocation {0,0,0};
// 	
// #if WITH_EDITOR
// 	// if(GEditor)
// 	// {
// 	// 	FViewport* ActiveViewport = GEditor->GetActiveViewport();
// 	// 	if(ActiveViewport)
// 	// 	{
// 	// 		if( FEditorViewportClient* EditorViewClient = (ActiveViewport != nullptr) ? static_cast<FEditorViewportClient*>(ActiveViewport->GetClient()) : nullptr )
// 	// 		{
// 	// 			OutLocation = EditorViewClient->GetViewLocation();
// 	// 		}	
// 	// 	}
// 	// }
// 	//
// #endif
// 	
// 	return OutLocation;
// }

UVatClothingMask* UOverCrowdRuntimeLibrary::GetClothingMaskDataAsset()
{
	UVatClothingMask *Out = UOverCrowdDeveloperSettings::Get()->ClothingMask.LoadSynchronous();

	if(!Out)
	{
		FString ClothingPackagePath = GetPluginSaveDirectory() + TEXT("/DataAssets/DA_ClothingMask_Gen");
		
		if(FPackageName::DoesPackageExist(ClothingPackagePath))
		{
			Out = LoadObject<UVatClothingMask>(nullptr, *ClothingPackagePath);	
		}
#if WITH_EDITOR
		else
		{
			IAssetTools& AssetTools = FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

			UFactory* DataAssetFactory = NewObject<UFactory>(GetTransientPackage(), UDataAssetFactory::StaticClass());
			UObject* NewAsset = AssetTools.CreateAsset(FString(FPathViews::GetCleanFilename(ClothingPackagePath)), FString(FPathViews::GetPath(ClothingPackagePath)), UVatClothingMask::StaticClass(), DataAssetFactory);
			Out =  Cast<UVatClothingMask>(NewAsset);
		}
#endif
		UOverCrowdDeveloperSettings::Get()->ClothingMask = Out;
	}
	
	return Out;
}


TArray<UWardrobeCollection*> UOverCrowdRuntimeLibrary::GetWardrobeCollections()
{
	TArray<UWardrobeCollection*> Out;

	auto &WardrobeCollections = UOverCrowdDeveloperSettings::Get()->WardrobeCollections;


	if(!WardrobeCollections.Num())
	{
		FString PackagePath = GetPluginSaveDirectory() + TEXT("/DataAssets/DA_WardrobeCollection_Gen");

		UWardrobeCollection* Wc = nullptr;
		//if(FPackageName::DoesPackageExist(PackagePath))
		Wc = LoadObject<UWardrobeCollection>(nullptr, *PackagePath);
		if(Wc)
		{
			WardrobeCollections.Add(Wc);
		}
#if WITH_EDITOR
		else
		{
			IAssetTools& AssetTools = FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

			UFactory* DataAssetFactory = NewObject<UFactory>(GetTransientPackage(), UDataAssetFactory::StaticClass());
			UObject* NewAsset = AssetTools.CreateAsset(FString(FPathViews::GetCleanFilename(PackagePath)), FString(FPathViews::GetPath(PackagePath)), UWardrobeCollection::StaticClass(), DataAssetFactory);
			Wc = Cast<UWardrobeCollection>(NewAsset);

			WardrobeCollections.Add(Wc);
		}
#endif
	}
	
	for(auto WardrobeCollection : WardrobeCollections)
	{
		if(auto Wc = WardrobeCollection.LoadSynchronous())
		{
			Out.Add(Wc);
		}
	}
	
	return Out;
}

bool UOverCrowdRuntimeLibrary::IsSameSkeleton(const TArray<UVatMeshPairing*> &VatMeshes)
{
	USkeleton * PreviousSkeleton = nullptr;
	for(auto VatMesh : VatMeshes)
	{
		if(!VatMesh)
			continue;

		USkeleton* Skel = VatMesh->GetSkelMeshSkeleton();
		if(!Skel)
			continue;

		if(!PreviousSkeleton)
			PreviousSkeleton = Skel;
		else if(Skel != PreviousSkeleton)
			return false;
	}

	return true;
}

UVatAnimationDataAsset* UOverCrowdRuntimeLibrary::FindAnimationDataAsset(UVatMeshPairing* VatMesh, FGameplayTag WardrobeID)
{
	UWardrobeAnimation* WardrobeAnimation = CreateOrLoadWardrobeAnimation(WardrobeID);

	if(!VatMesh || !WardrobeAnimation)
		return nullptr;

	return WardrobeAnimation->FindOrCreateAnimationAsset(VatMesh);
}

UStaticMesh* UOverCrowdRuntimeLibrary::DuplicateStaticMeshAndCreatePackage(UStaticMesh *CopyFrom, FString NewAssetName)
{
	UStaticMesh* Out = nullptr;

	FString PackagePath = FString::Printf(TEXT("%s/NiagaraMeshes/%s"), *UOverCrowdRuntimeLibrary::GetPluginSaveDirectory(), *NewAssetName);

#if WITH_EDITOR
	//if(!Out)
	{
		
		//if(bPackageExists)
		UPackage* MeshPackage;
		MeshPackage = CreatePackage(*PackagePath);

		Out = Cast<UStaticMesh>(StaticDuplicateObject(CopyFrom, MeshPackage, *NewAssetName));
		//Out = DuplicateObject<UStaticMesh>(CopyFrom, MeshPackage, *NewAssetName);

		MeshPackage->MarkPackageDirty();
		//NewObject<UStaticMesh>(MeshPackage, UStaticMesh::StaticClass(), *NewAssetName, EObjectFlags::RF_Public | EObjectFlags::RF_Standalone);
		FAssetRegistryModule::AssetCreated(Out);

		FString PackageFilePath = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
		// bool bSaved = UPackage::SavePackage(MeshPackage, Out, EObjectFlags::RF_Public , *PackageFilePath);
		// if(bSaved)
		// {
		// 	UE_LOG(LogOverCrowdEditor, Display, TEXT("Duplicate package %s"), *PackageFilePath);
		// }
		// else
		// {
		// 	UE_LOG(LogOverCrowdEditor, Error, TEXT("Could not save package %s"), *PackageFilePath);
		// }
		// if (UPackage::SavePackage(MeshPackage, nullptr, EObjectFlags::RF_Public, *PackageFilePath))
		// {
		// 	
		// }
		
		SavePackageHelper(MeshPackage, PackageFilePath);
	}
#endif
	
	return Out;
}

UWardrobeAnimation* UOverCrowdRuntimeLibrary::CreateOrLoadWardrobeAnimation(FGameplayTag WardrobeID)
{
	UWardrobeAnimation* Out = nullptr;

	//FString SkeletonName = ForSkeleton->GetName();
	FString WardrobeName = GetSanitizedWardrobeName(WardrobeID);// + ".uasset";
	
	FString PackagePath = FString::Printf(TEXT("%s/DataAssets/WA_%s"), *UOverCrowdRuntimeLibrary::GetPluginSaveDirectory(), *WardrobeName);

	//if(FPackageName::DoesPackageExist(PackagePath))
	{
		Out = LoadObject<UWardrobeAnimation>(nullptr, *PackagePath);	
	}
#if WITH_EDITOR
	if(!Out)
	{
		IAssetTools& AssetTools = FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

		UFactory* DataAssetFactory = NewObject<UFactory>(GetTransientPackage(), UDataAssetFactory::StaticClass());
		UObject* NewAsset = AssetTools.CreateAsset(FString(FPathViews::GetCleanFilename(PackagePath)), FString(FPathViews::GetPath(PackagePath)), UWardrobeAnimation::StaticClass(), DataAssetFactory);
		Out =  Cast<UWardrobeAnimation>(NewAsset);
		Out->WardrobeID = WardrobeID;
	}
#endif
	return Out;
}

UAnimationsToBakeDataAsset* UOverCrowdRuntimeLibrary::CreateOrLoadAnimationsToBake(FGameplayTag WardrobeID)
{
	UAnimationsToBakeDataAsset* Out = nullptr;

	FString WardrobeName = GetSanitizedWardrobeName(WardrobeID);//  + ".uasset";;
	FString PackagePath = FString::Printf(TEXT("%s/DataAssets/A2B_%s"), *UOverCrowdRuntimeLibrary::GetPluginSaveDirectory(), *WardrobeName);

	//if(FPackageName::DoesPackageExist(PackagePath))
	{
		Out = LoadObject<UAnimationsToBakeDataAsset>(nullptr, *PackagePath);	
	}
#if WITH_EDITOR
	if(!Out)
	{
		IAssetTools& AssetTools = FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

		UFactory* DataAssetFactory = NewObject<UFactory>(GetTransientPackage(), UDataAssetFactory::StaticClass());
		UObject* NewAsset = AssetTools.CreateAsset(FString(FPathViews::GetCleanFilename(PackagePath)), FString(FPathViews::GetPath(PackagePath)), UAnimationsToBakeDataAsset::StaticClass(), DataAssetFactory);
		Out =  Cast<UAnimationsToBakeDataAsset>(NewAsset);
		
	}
#endif
	return Out;
}

FString UOverCrowdRuntimeLibrary::GetSanitizedWardrobeName(FGameplayTag WardrobeID)
{
	FString WardrobeName = WardrobeID.ToString();
	WardrobeName = WardrobeName.Replace(TEXT("Overcrowd."),TEXT(""),ESearchCase::IgnoreCase);
	WardrobeName = WardrobeName.Replace(TEXT("Wardrobe."),TEXT(""),ESearchCase::IgnoreCase);
	//WardrobeName = WardrobeName.Replace(TEXT("."),TEXT("_"),ESearchCase::IgnoreCase);

	FString Left, Right;
	if(WardrobeName.Split(TEXT("."), &Left, &Right))
		WardrobeName = Left;

	return WardrobeName;
}

FString UOverCrowdRuntimeLibrary::GetSanitizedName(FGameplayTag OverCrowdTag, FString AddtionalStringToChop)
{
	FString SantitizedName = OverCrowdTag.ToString();
	if(AddtionalStringToChop.Len())
		AddtionalStringToChop += TEXT(".");
	SantitizedName = SantitizedName.Replace(TEXT("Overcrowd."),TEXT(""),ESearchCase::IgnoreCase);
	SantitizedName = SantitizedName.Replace(*AddtionalStringToChop,TEXT(""),ESearchCase::IgnoreCase);
	SantitizedName = SantitizedName.Replace(TEXT("."),TEXT("_"),ESearchCase::IgnoreCase);

	return SantitizedName;
}

FString UOverCrowdRuntimeLibrary::GetSanitizedPlacementGroupName(FGameplayTag PlacementGroup)
{
	//FString SantitizedName = PlacementGroup.ToString();
	// SantitizedName = SantitizedName.Replace(TEXT("Overcrowd."),TEXT(""),ESearchCase::IgnoreCase);
	// SantitizedName = SantitizedName.Replace(TEXT("PlacementGroup."),TEXT(""),ESearchCase::IgnoreCase);
	// SantitizedName = SantitizedName.Replace(TEXT("."),TEXT("_"),ESearchCase::IgnoreCase);

	// FString Left, Right;
	// if(SantitizedName.Split(TEXT("."), &Left, &Right))
	// 	SantitizedName = Left;

	return GetSanitizedName(PlacementGroup,TEXT("PlacementGroup"));
}

UTexture2D* UOverCrowdRuntimeLibrary::LoadTexture(FString AssetName, FString PackagePath)
{
	UTexture2D* Texture = nullptr;
#if WITH_EDITOR

	if(PackagePath[PackagePath.Len()-1] != TEXT('/'))
		PackagePath += TEXT('/');
	FString FullName = PackagePath + AssetName;

	Texture = LoadObject<UTexture2D>(nullptr, *FullName);
#endif

	return Texture;
}

//
// bool UOverCrowdRuntimeLibrary::WriteCrowdMemberDataToTextureRuntime(const TArray<FCrowdMemberData>& CrowdMembers,	const TMap<FGameplayTag, FWardrobeVatAssetPairArray>& SortedBodyParts, UTexture2D* OutTexture)
// {
// #if WITH_EDITOR
// 	return UOverCrowdEditorLibrary::WriteCrowdMemberDataToTexture(CrowdMembers, SortedBodyParts, OutTexture);
// #else
// 	return true;
// #endif
// }

#if WITH_EDITOR
UMaterialInstanceConstant* UOverCrowdRuntimeLibrary::GetOrCreateChildMaterial(UMaterialInterface* Material, FString Suffix)
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
#endif
//
// UAnimationDataAssetCollection* UOverCrowdRuntimeLibrary::CreateOrLoadDefaultAnimationDataAssetCollection()
// {
// 	UAnimationDataAssetCollection* Out = UOverCrowdDeveloperSettings::Get()->AnimationDataAssetCollection.LoadSynchronous();
//
// 	// UAnimationDataAssetCollection* Out = nullptr;
// 	// Out = UOverCrowdDeveloperSettings::Get()->AnimationDataAssetCollection.LoadSynchronous();
// 	// auto Settings = UOverCrowdDeveloperSettings::Get();
// 	//
// 	if(Out)
// 	{
// 		return Out;
// 	}
// 	
// 	if(!Out)
// 	{
// 		FString PackagePath = UOverCrowdRuntimeLibrary::GetPluginSaveDirectory() + TEXT("/DataAssets/DA_AnimationCollection_Gen");
// 		if(FPackageName::DoesPackageExist(PackagePath))
// 		{
// 			Out = LoadObject<UAnimationDataAssetCollection>(nullptr, *PackagePath);	
// 		}
// #if WITH_EDITOR
// 		else
// 		{
// 			IAssetTools& AssetTools = FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
//
// 			UFactory* DataAssetFactory = NewObject<UFactory>(GetTransientPackage(), UDataAssetFactory::StaticClass());
// 			UObject* NewAsset = AssetTools.CreateAsset(FString(FPathViews::GetCleanFilename(PackagePath)), FString(FPathViews::GetPath(PackagePath)), UAnimationDataAssetCollection::StaticClass(), DataAssetFactory);
// 			Out =  Cast<UAnimationDataAssetCollection>(NewAsset);
// 		}
// #endif
// 		UOverCrowdDeveloperSettings::Get()->AnimationDataAssetCollection = Out;
// 	}
//
// 	return Out;
// }
//
// TArray<UVatAnimationDataAsset*> UOverCrowdRuntimeLibrary::GetAnimationDataAssets()
// {
// 	TArray<UVatAnimationDataAsset*> Res;
// 	if(UAnimationDataAssetCollection* AnimationDataAssetCollection = CreateOrLoadDefaultAnimationDataAssetCollection())
// 	{
// 		AnimationDataAssetCollection->Defaults.GenerateValueArray(Res);
// 		for(auto VatAnim : AnimationDataAssetCollection->AnimationDataAssets)
// 		{
// 			if(VatAnim.Value)
// 			{
// 				Res.AddUnique(VatAnim.Value);	
// 			}
// 		}
// 	}
// 	return Res;
// }

//
// void UOverCrowdRuntimeLibrary::PopulateAnimationDataAssetCollection(TArray<UVatMeshPairing*> VatMeshes)
// {
// 	for(auto VatMesh : VatMeshes)
// 	{
// 		FindAnimationDataAsset(VatMesh);
// 	}
// }
//
// UVatAnimationDataAsset* UOverCrowdRuntimeLibrary::FindAnimationDataAsset(UVatMeshPairing* VatMesh)
// {
// 	UVatAnimationDataAsset* Res = nullptr;
// 	if(!VatMesh)
// 		return Res;
// 	
// 	USkeleton* Skeleton  = VatMesh->GetSkelMeshSkeleton();
// 	if(!Skeleton)
// 		return Res;
// 	
// 	if(UAnimationDataAssetCollection* AnimationDataAssetCollection = CreateOrLoadDefaultAnimationDataAssetCollection())
// 	{
// 		UVatAnimationDataAsset** VatAnim = AnimationDataAssetCollection->AnimationDataAssets.Find(VatMesh);
// 		if(VatAnim)
// 			Res = *VatAnim;
// 		
// 		if(!Res)
// 		{
// 			VatAnim = AnimationDataAssetCollection->Defaults.Find(Skeleton);
// 			if(VatAnim)
// 				Res = *VatAnim;
// 			
// 		}
// 	}
// 	return Res;
// }
//
// bool UOverCrowdRuntimeLibrary::IsUsingDefaultAnimationDataAsset(UVatMeshPairing* VatMesh)
// {
// 	if(!VatMesh)
// 		return false;
// 	
// 	USkeleton* Skeleton  = VatMesh->GetSkelMeshSkeleton();
// 	if(!Skeleton)
// 		return false;
// 	
// 	if(UAnimationDataAssetCollection* AnimationDataAssetCollection = CreateOrLoadDefaultAnimationDataAssetCollection())
// 	{
// 		UVatAnimationDataAsset** VatAnim = AnimationDataAssetCollection->AnimationDataAssets.Find(VatMesh);
// 		if(VatAnim)
// 		 	return false;
// 		
// 		VatAnim = AnimationDataAssetCollection->Defaults.Find(Skeleton);
// 		if(VatAnim)
// 			return true;
// 	}
// 	return false;
// }
//
// void UOverCrowdRuntimeLibrary::UseDefaultAnimationDataAsset(UVatMeshPairing* VatMesh)
// {
// 	if(!VatMesh)
// 		return;
// 	
// 	if(UAnimationDataAssetCollection* AnimationDataAssetCollection = CreateOrLoadDefaultAnimationDataAssetCollection())
// 	{
// 		AnimationDataAssetCollection->AnimationDataAssets.Remove(VatMesh);
// 	}
// }
//
// void UOverCrowdRuntimeLibrary::AddAnimationDataAsset(UVatMeshPairing* VatMesh, UVatAnimationDataAsset* AnimAsset, bool SetAsDefault)
// {
// 	if(!VatMesh || !AnimAsset)
// 		return;
// 	
// 	USkeleton* Skeleton  = VatMesh->GetSkelMeshSkeleton();
// 	if(!Skeleton)
// 		return;
// 	
// 	UAnimationDataAssetCollection* AnimationDataAssetCollection = CreateOrLoadDefaultAnimationDataAssetCollection();
//
// 	if(SetAsDefault)
// 	{
// 		AnimationDataAssetCollection->Defaults.Add(Skeleton, AnimAsset);
// 	}
// 	else
// 	{
// 		UVatAnimationDataAsset* DefaultAnimAsset = AnimationDataAssetCollection->Defaults.FindOrAdd(Skeleton);
// 		if(!DefaultAnimAsset)
// 		{
// 			AnimationDataAssetCollection->Defaults.Add(Skeleton, AnimAsset);
// 		}
// 		else if(DefaultAnimAsset != AnimAsset)
// 		{
// 			AnimationDataAssetCollection->AnimationDataAssets.Add(VatMesh, AnimAsset);	
// 		}
// 	}
//
// 	MarkDirty(AnimationDataAssetCollection);
// }

void UOverCrowdRuntimeLibrary::GetRandomVariationFromWardrobe(FWardrobe Wardrobe, TMap <FGameplayTag, UVatMeshPairing*>& OutBodyPartMeshes)
{
	for (auto& BodyParts : Wardrobe.Compartments)
	{
		auto &TmpBodyParts = BodyParts.Value.TaggedVatAssets;
		
		if(TmpBodyParts.IsEmpty())
			continue;

		const int32 RandIndex = FMath::Rand() % TmpBodyParts.Num();

		OutBodyPartMeshes.Add(BodyParts.Key, TmpBodyParts[RandIndex]);
	}
}

void UOverCrowdRuntimeLibrary::GetAllMeshPairingsFromWardrobe(FWardrobe Wardrobe, TArray<UVatMeshPairing*>& Out_VatMeshPairings)
{
	for (auto& BodyParts : Wardrobe.Compartments)
	{
		for(auto VatPairing : BodyParts.Value.TaggedVatAssets)
		{
			if(VatPairing)
				Out_VatMeshPairings.Add(VatPairing);
		}
	}
}

void UOverCrowdRuntimeLibrary::GetMultipleVariationsFromWardrobes(TArray<FPerCharacterNiagaraData> &OutData, const UVatClothingMask* ClothingMask, const TArray<FWardrobe>& Wardrobes, const TMap<UVatMeshPairing*, int32>& IndexLookup, int32 NumVariations)
{
	if(Wardrobes.IsEmpty() || !ClothingMask)
		return;

	TArray<float> SelectionWeights;
	float TotalProbability = 0.0f;
	for(auto& Wardrobe : Wardrobes)
	{
		TotalProbability += Wardrobe.ProbabilityWeight;
		SelectionWeights.Add(TotalProbability);
	}
	
	OutData.SetNum(NumVariations);
	for (int32 i = 0; i < NumVariations; i++)
	{
		float Rando = FMath::FRandRange(0,TotalProbability);
		int32 Index = 0;
		for(float SelectionWeight: SelectionWeights)
		{
			if(Rando <= SelectionWeight)
				break;
			Index++;
		}
		const FWardrobe &Wardrobe = Wardrobes[Index];
		
		FPerCharacterNiagaraData &Data = OutData[i];

		TMap <FGameplayTag, UVatMeshPairing*> BodyParts;
		GetRandomVariationFromWardrobe(Wardrobe, BodyParts);

		TArray<UVatMeshPairing*> BodyPartMeshes;
		BodyParts.GenerateValueArray(BodyPartMeshes);

		for (auto BodyPart : BodyParts)
		{
			int32 BitMask;
			ClothingMask->GetBitMaskValue(BodyPart.Value, BodyPartMeshes,BitMask);
			
			if (const int32 *LookupIndex = IndexLookup.Find(BodyPart.Value))
			{
				Data.BodyPartIndices.Add(BodyPart.Key, *LookupIndex);
				Data.BitMasks.Add(BodyPart.Key, BitMask);
			}
		}
	}
}

void UOverCrowdRuntimeLibrary::MakeSortedVatAssetArrayFromWardrobes(const TArray<FWardrobe>& Wardrobes, TMap<FGameplayTag, FWardrobeVatAssetPairArray>& Out_SortedBodyParts)
{
	if(Wardrobes.IsEmpty() )
		return;

	for(auto Wardrobe : Wardrobes)
	{
		for(auto BodyParts: Wardrobe.Compartments)
		{
			FWardrobeVatAssetPairArray& BodyPartArray = Out_SortedBodyParts.FindOrAdd(BodyParts.Key);
			for(auto VatPairing : BodyParts.Value.TaggedVatAssets)
			{
				FWardrobeVatAssetPair WardrobeVatAssetPair;
				WardrobeVatAssetPair.WardrobeID = Wardrobe.WardrobeID;
				WardrobeVatAssetPair.VatAsset = VatPairing;

				BodyPartArray.VatAssets.Add(WardrobeVatAssetPair);
			}
		}
	}
}

void UOverCrowdRuntimeLibrary::GetAllWardrobesFromCrowdMembers(const TArray<FCrowdMemberData>& NPCs, TArray<FGameplayTag> &Out_WardrobeIDs)
{
	Out_WardrobeIDs.Empty();
	for(auto CrowdMemberData : NPCs)
	{
		if(!Out_WardrobeIDs.Contains(CrowdMemberData.WardrobeID))
		{
			Out_WardrobeIDs.AddUnique(CrowdMemberData.WardrobeID);
		}
	}
}

void UOverCrowdRuntimeLibrary::ExtractBodyPartIndicesIntoArray(const TArray<FPerCharacterNiagaraData>& PerCharacterData, FGameplayTag BodyPartTag, TArray<int32>& OutIndices)
{
	OutIndices.Reserve(PerCharacterData.Num());
	
	for(auto &PerCharacter : PerCharacterData)
	{
		if (auto Index = PerCharacter.BodyPartIndices.Find(BodyPartTag))
		{
			OutIndices.Emplace(*Index + 1);
		}
		else
		{
			OutIndices.Emplace(0);
		}
	}
}

void UOverCrowdRuntimeLibrary::ExtractBodyPartBitMaskIntoArray(const TArray<FPerCharacterNiagaraData>& PerCharacterData, FGameplayTag BodyPartTag, TArray<int32>& OutBitMasks)
{
	OutBitMasks.Reserve(PerCharacterData.Num());
	
	for(const auto & [BodyPartIndices, BitMasks] : PerCharacterData)
	{
		if (const int32* Mask = BitMasks.Find(BodyPartTag))
		{
			OutBitMasks.Emplace(*Mask);
		}
		else
		{
			OutBitMasks.Emplace(0);
		}
	}
}

bool UOverCrowdRuntimeLibrary::CalculateAndSetBitMasks(FCrowdMemberData& CrowdMemberData, const UVatClothingMask* ClothingMask, const int32 MaskOverrideSlot)
{
	bool bDirty = false;
	if(!ClothingMask)
		return bDirty;
	
	TArray<UVatMeshPairing*> BodyPartVatAssets;
	CrowdMemberData.BodyPartInstances.GetKeys(BodyPartVatAssets);
	
	for(auto &BodyPart : CrowdMemberData.BodyPartInstances)
	{
		int32 BitMask = 0;
		ClothingMask->GetBitMaskValue(BodyPart.Key, BodyPartVatAssets,BitMask);

		auto Index = BodyPart.Value.GetOverrideIndex(MaskOverrideSlot);
		if(Index != INDEX_NONE)
		{
			float OldMask = BodyPart.Value.Overrides[Index].Value;
			if(OldMask != BitMask)
			{
				BodyPart.Value.Overrides[Index].Value = BitMask;
				bDirty = true;
			}
		}
		else
		{
			BodyPart.Value.Overrides.Add(FMaterialParamData(GetClothingMaskTag(), BitMask,MaskOverrideSlot));
		}
	}
	return bDirty;
}

void UOverCrowdRuntimeLibrary::GetAllWardrobes(TArray<FWardrobe>& OutWardrobes, TArray<FGameplayTag> ShowOnlyList, TArray<FGameplayTag> HideList)
{
	auto WardrobeCollections = UOverCrowdRuntimeLibrary::GetWardrobeCollections();

	if(ShowOnlyList.IsEmpty())
	{
		for (auto CrowdType : WardrobeCollections)
		{
			if(!CrowdType)
				continue;

			for (auto Wardrobe : CrowdType->BespokeWardrobes)
			{
				
				if (!HideList.Contains(Wardrobe.Key))
				{
					//TODO: Remove only tmp
					Wardrobe.Value.WardrobeID = Wardrobe.Key;
					//-----------------------
					OutWardrobes.Add(Wardrobe.Value);
				}
			
				// else
				// {
				// 	if (ShowOnlyList.Contains(Wardrobe.Key))
				// 	{
				// 		//TODO: Remove only tmp
				// 		Wardrobe.Value.WardrobeID = Wardrobe.Key;
				// 		//-----------------------
				// 		OutWardrobes.Add(Wardrobe.Value);
				// 	}
				// }
			}
		}
	}
	else
	{
		for(auto ShowOnly : ShowOnlyList)
		{
			for (auto CrowdType : WardrobeCollections)
			{
				if(!CrowdType)
					continue;

				for (auto Wardrobe : CrowdType->BespokeWardrobes)
				{
				
					if(Wardrobe.Key == ShowOnly)
					{
						Wardrobe.Value.WardrobeID = Wardrobe.Key;
						OutWardrobes.Add(Wardrobe.Value);
					}
					// else
					// {
					// 	if (ShowOnlyList.Contains(Wardrobe.Key))
					// 	{
					// 		//TODO: Remove only tmp
					// 		Wardrobe.Value.WardrobeID = Wardrobe.Key;
					// 		//-----------------------
					// 		OutWardrobes.Add(Wardrobe.Value);
					// 	}
					// }
				}
			}
		}
		
	}
	
}

bool UOverCrowdRuntimeLibrary::GetWardrobeFromTag(FWardrobe& OutWardrobe, FGameplayTag WardrobeID)
{
	auto WardrobeCollections = UOverCrowdRuntimeLibrary::GetWardrobeCollections();
	for (auto CrowdType : WardrobeCollections)
	{
		if (!CrowdType)
			continue;

		if (CrowdType->BespokeWardrobes.Contains(WardrobeID))
		{
			OutWardrobe = CrowdType->BespokeWardrobes[WardrobeID];
			//TODO: Remove only tmp
			OutWardrobe.WardrobeID = WardrobeID;
			//-----------------------
			return true;
		}
	}
	return false;
}

bool UOverCrowdRuntimeLibrary::BuildCrowdMemberDataAtLocations(const TArray<FAnimationTagGroup> AnimationGroups, const TArray<FWardrobe>& Wardrobes, const TArray<FTransform>& AtLocations, TArray<FCrowdMemberData>& OutNPCs)
{
	if(Wardrobes.IsEmpty())
		return false;

	TArray<float> SelectionWeights;
	
	float TotalProbability = 0.0f;
	for(auto& Wardrobe : Wardrobes)
	{
		TotalProbability += Wardrobe.ProbabilityWeight;
		SelectionWeights.Add(TotalProbability);
	}
	
	
	OutNPCs.Reserve(AtLocations.Num());
	for(auto Location : AtLocations)
	{
		float Rando = FMath::FRandRange(0,TotalProbability);
		int32 Index = 0;
		for(float SelectionWeight: SelectionWeights)
		{
			if(Rando <= SelectionWeight)
				break;
			Index++;
		}
		
		if(AnimationGroups.IsEmpty())
		{
			FCrowdMemberData CrowdMemberData;
			const FWardrobe &Wardrobe = Wardrobes[Index];

			CrowdMemberData.Transform = Location;
			CreateRandomCrowdMemberData(Wardrobe,CrowdMemberData);
	
			OutNPCs.Emplace(CrowdMemberData);
		}
		else
		{
			uint32 RandomIndex = FMath::RandRange(0,AnimationGroups.Num() - 1);
			FAnimationTagGroup AnimGroup = AnimationGroups[RandomIndex];

			float RandOffset = FMath::RandRange(0.0f,255.5f);
			for(auto AnimTag: AnimGroup.Animation)
			{
				
				FCrowdMemberData CrowdMemberData;
				const FWardrobe &Wardrobe = Wardrobes[Index++];
				Index = Index % Wardrobes.Num();

				CrowdMemberData.Transform = Location;
				CreateRandomCrowdMemberData(Wardrobe,CrowdMemberData);
				CrowdMemberData.AnimationState = AnimTag.From;
				CrowdMemberData.AdditiveAnimationState = AnimTag.To;
				CrowdMemberData.RandomAnimationOffset.X = RandOffset;
	
				OutNPCs.Emplace(CrowdMemberData);
			}
		}
	}
	return true;
}

bool UOverCrowdRuntimeLibrary::CreateRandomCrowdMemberData(const FWardrobe& Wardrobe, FCrowdMemberData& CrowdMemberData)
{
	CrowdMemberData.BodyPartInstances.Empty();
	float Scale = FMath::FRandRange(Wardrobe.ScaleVariation.X, Wardrobe.ScaleVariation.Y);
	FVector Scale3D(Scale, Scale, Scale);

	const auto MaterialParamsRanges = Wardrobe.MaterialParametersOverrides;
	TArray<FGameplayTag> FilterOut;
	for (auto Ranges : MaterialParamsRanges)
	{
		if (Ranges.bMustMatchToOverride)
			FilterOut.Add(Ranges.ParamID);
	}

	TArray<FMaterialParamData> MaterialOverrides = PickRandomMaterialOverrides(MaterialParamsRanges, FilterOut);

	for (auto& Compartment : Wardrobe.Compartments)
	{
		if (Compartment.Value.TaggedVatAssets.IsEmpty())
			continue;

		if (auto VatAsset = Compartment.Value.TaggedVatAssets[FMath::Rand() % Compartment.Value.TaggedVatAssets.Num()])
		{
			TArray<FMaterialParamData> TmpMaterialOverrides;
			TmpMaterialOverrides = PickRandomMaterialOverrides(VatAsset->PerInstanceParameters, FilterOut);

			for (auto Override : MaterialOverrides)
			{
				//bit of a hack replacing old value with new
				TmpMaterialOverrides.Remove(Override);
				TmpMaterialOverrides.Add(Override);
			}

			CrowdMemberData.BodyPartInstances.Add(VatAsset, FPerInstanceOverrides(TmpMaterialOverrides));
		}
		
		CrowdMemberData.Transform.SetScale3D(Scale3D);
		CrowdMemberData.RandomAnimationOffset.X = FMath::FRandRange(0.0f, 256.0f);
		CrowdMemberData.RandomAnimationOffset.Y = FMath::FRandRange(0.0f, 256.0f);
		CrowdMemberData.SwapCharacterClass = Wardrobe.SwapCharacterClass;
		CrowdMemberData.WardrobeID = Wardrobe.WardrobeID;
		CrowdMemberData.RandomAnimationOffset = CrowdMemberData.RandomAnimationOffset;
	}
	return true;
}
//
// void UOverCrowdRuntimeLibrary::AddMeshRendererToNiagaraSystem(UNiagaraSystem* NiagaraSystem, UStaticMesh* Mesh, UMaterialInterface* Material)
// {
// 	if (!NiagaraSystem || !Mesh)
// 	{
// 		UE_LOG(LogOverCrowdEditor, Warning, TEXT("Invalid Niagara System or Mesh."));
// 		return;
// 	}
//
// 	if (NiagaraSystem->GetEmitterHandles().Num() > 0)
// 	{
// 		const FNiagaraEmitterHandle& EmitterHandle = NiagaraSystem->GetEmitterHandles()[0];
// 		FVersionedNiagaraEmitter VersionedEmitter =  EmitterHandle.GetInstance();
// 		
// 		UNiagaraEmitter* Emitter = EmitterHandle.GetInstance().Emitter;
// 		if (Emitter)
// 		{
// 			FVersionedNiagaraEmitterData* EmitterData = Emitter->GetLatestEmitterData();
// 			;
//
// 			UNiagaraMeshRendererProperties* CopyFrom = nullptr;// = DuplicateObject<UNiagaraRendererProperties>(SourceProperties,Outer);
// 			for (UNiagaraRendererProperties* EmitterRenderer : EmitterData->GetRenderers())
// 			{
// 				if (UNiagaraMeshRendererProperties* MeshRenderer = Cast<UNiagaraMeshRendererProperties>(EmitterRenderer))
// 				{
// 					CopyFrom = MeshRenderer;
// 					break;
// 				}
// 			}
// 			
// 			UNiagaraMeshRendererProperties* NewMeshRenderer = DuplicateObject<UNiagaraMeshRendererProperties>(CopyFrom,Emitter);//NewObject<UNiagaraMeshRendererProperties>(Emitter);
// 			if (NewMeshRenderer)
// 			{
// 				
// 				// Assign the mesh to the renderer
// 				TArray<FNiagaraMeshRendererMeshProperties> MeshPropertiesArray;
// 				TArray<FStaticMaterial> Mats = {Material};
//
// 				FNiagaraMeshRendererMeshProperties TmpMeshProperties;
// 				TmpMeshProperties.Mesh = DuplicateObject<UStaticMesh>(Mesh,Emitter);
// 				TmpMeshProperties.Mesh->SetStaticMaterials(Mats);
// 				MeshPropertiesArray.Add(TmpMeshProperties);
// 				NewMeshRenderer->Meshes = MeshPropertiesArray;
//
// 				TArray<FNiagaraVariable> BoundVariables = NewMeshRenderer->GetBoundAttributes();
// 				FNiagaraVariableAttributeBinding Binding = FNiagaraConstants::GetAttributeDefaultBinding(SYS_PARAM_PARTICLES_MESH_INDEX);
// 				
// 				NewMeshRenderer->MeshIndexBinding.SetValue(TEXT("Particles.Bollocks"),VersionedEmitter,ENiagaraRendererSourceDataMode::Particles);
//
// 				Emitter->AddRenderer(NewMeshRenderer, EmitterData->Version.VersionGuid);
//
// 				UOverCrowdRuntimeLibrary::MarkDirty(NiagaraSystem);
// 				//Emitter->MarkPackageDirty();
// 			}
// 		}
// 	}
// }

void UOverCrowdRuntimeLibrary::GenerateMeshRenderers(ANiagaraCrowd* NiagaraCrowd, const TMap<FGameplayTag, FWardrobeVatAssetPairArray>& SortedBodyParts)
{
	if (!NiagaraCrowd || !NiagaraCrowd->CrowdEmitter)
	{
		UE_LOG(LogOverCrowdRuntime, Warning, TEXT("Invalid NiagaraCrowd"));
		return;
	}
	UNiagaraSystem* NiagaraSystem = NiagaraCrowd->CrowdEmitter->GetAsset(); 
	if(!NiagaraSystem)
	{
		UE_LOG(LogOverCrowdRuntime, Warning, TEXT("Invalid Niagara System"));
		return;
	}
		

	if (NiagaraSystem->GetEmitterHandles().Num() > 0)
	{
		const FNiagaraEmitterHandle& EmitterHandle = NiagaraSystem->GetEmitterHandles()[0];
		FVersionedNiagaraEmitter VersionedEmitter =  EmitterHandle.GetInstance();
		
		UNiagaraEmitter* Emitter = EmitterHandle.GetInstance().Emitter;
		if (!Emitter)
			return;

#if WITH_EDITOR
		FNiagaraVariable TextureParam;
		TextureParam.SetName(TEXT("CrowdData"));
		TextureParam.SetType(FNiagaraTypeDefinition::GetUTextureDef());
		NiagaraSystem->GetExposedParameters().AddParameter(TextureParam);
		
		FNiagaraVariable CountParam;
		CountParam.SetName(TEXT("CrowdCount"));
		CountParam.SetType(FNiagaraTypeDefinition::GetIntDef());
		NiagaraSystem->GetExposedParameters().AddParameter(CountParam);
		
		FNiagaraVariable NumGridCellsXParam;
		NumGridCellsXParam.SetName(TEXT("NumGridCellsX"));
		NumGridCellsXParam.SetType(FNiagaraTypeDefinition::GetIntDef());
		NiagaraSystem->GetExposedParameters().AddParameter(NumGridCellsXParam);
		
		FNiagaraVariable NumGridCellsYParam;
		NumGridCellsYParam.SetName(TEXT("NumGridCellsY"));
		NumGridCellsYParam.SetType(FNiagaraTypeDefinition::GetIntDef());
		NiagaraSystem->GetExposedParameters().AddParameter(NumGridCellsYParam);
		
		FNiagaraVariable NumGridCellsZParam;
		NumGridCellsZParam.SetName(TEXT("NumGridCellsZ"));
		NumGridCellsZParam.SetType(FNiagaraTypeDefinition::GetIntDef());
		NiagaraSystem->GetExposedParameters().AddParameter(NumGridCellsZParam);
		
		FNiagaraVariable MaxGridNeighborsParam;
		MaxGridNeighborsParam.SetName(TEXT("MaxGridNeighbors"));
		MaxGridNeighborsParam.SetType(FNiagaraTypeDefinition::GetIntDef());
		NiagaraSystem->GetExposedParameters().AddParameter(MaxGridNeighborsParam);
		
		FNiagaraVariable GridSizeParam;
		GridSizeParam.SetName(TEXT("GridSize"));
		GridSizeParam.SetType(FNiagaraTypeDefinition::GetVec3Struct());
		NiagaraSystem->GetExposedParameters().AddParameter(GridSizeParam);

		// FNiagaraVariable CrowdDataSampleParam;
		// CrowdDataSampleParam.SetName(TEXT("CrowdDataSample"));
		// CrowdDataSampleParam.SetType(FNiagaraTypeDefinition::Get);
		// NiagaraSystem->GetExposedParameters().AddParameter(CrowdDataSampleParam);
#endif
		
		int MeshRenderersNeeded = SortedBodyParts.Num();
		FVersionedNiagaraEmitterData* EmitterData = Emitter->GetLatestEmitterData();
		TArray<UNiagaraMeshRendererProperties*> ToDelete;
		UNiagaraMeshRendererProperties* CopyFrom = nullptr;// = DuplicateObject<UNiagaraRendererProperties>(SourceProperties,Outer);

		//int MeshRendererCount =  0;
		// TArray<UNiagaraMeshRendererProperties*> MeshRenderers;
		// for (UNiagaraRendererProperties* EmitterRenderer : EmitterData->GetRenderers())
		// {
		// 	if (UNiagaraMeshRendererProperties* MeshRenderer = Cast<UNiagaraMeshRendererProperties>(EmitterRenderer))
		// 	{
		// 		MeshRenderer->Meshes.Empty();
		// 		if(!CopyFrom)
		// 			CopyFrom = MeshRenderer;
		// 		
		// 		MeshRenderers.Add(MeshRenderer);
		// 	}
		// 		
		// }
		for (UNiagaraRendererProperties* EmitterRenderer : EmitterData->GetRenderers())
		{
			if (UNiagaraMeshRendererProperties* MeshRenderer = Cast<UNiagaraMeshRendererProperties>(EmitterRenderer))
			{
				
				if(!CopyFrom)
				{
					CopyFrom = MeshRenderer;
					CopyFrom->Meshes.Empty();
				}
				else
				{
					MeshRenderer->Meshes.Empty();
					ToDelete.Add(MeshRenderer);
				}
			}
		}
		for (UNiagaraRendererProperties* EmitterRenderer : ToDelete)
		{
			Emitter->RemoveRenderer(EmitterRenderer, EmitterData->Version.VersionGuid);
		}

		if(!CopyFrom)
			CopyFrom = NewObject<UNiagaraMeshRendererProperties>(GetTransientPackage());
		// while(MeshRenderers.Num() < MeshRenderersNeeded)
		// {
		// 	UNiagaraMeshRendererProperties* NewMeshRenderer = DuplicateObject<UNiagaraMeshRendererProperties>(CopyFrom,Emitter);
		// 	MeshRenderers.Add(NewMeshRenderer);
		//
		// 	Emitter->AddRenderer(NewMeshRenderer, EmitterData->Version.VersionGuid);
		// }
		//UNiagaraMeshRendererProperties* NewMeshRenderer = DuplicateObject<UNiagaraMeshRendererProperties>(CopyFrom,Emitter);
			//if (!NewMeshRenderer)
			//	continue;

			//Emitter->AddRenderer(NewMeshRenderer, EmitterData->Version.VersionGuid);
		
		for(int Index = -1; auto& SortedBodyPart : SortedBodyParts)
		{
			Index++;
			
			//UNiagaraMeshRendererProperties* NewMeshRenderer = MeshRenderers[Index];
			UNiagaraMeshRendererProperties* NewMeshRenderer = DuplicateObject<UNiagaraMeshRendererProperties>(CopyFrom,Emitter);
			if (!NewMeshRenderer)
				continue;

			Emitter->AddRenderer(NewMeshRenderer, EmitterData->Version.VersionGuid);
			
			FName MeshIndexName = FName(*FString::Printf(TEXT("Particles.MeshIndex%d"), Index));
			NewMeshRenderer->MeshIndexBinding.SetValue(MeshIndexName,VersionedEmitter,ENiagaraRendererSourceDataMode::Particles);
			
			TArray<FNiagaraMeshRendererMeshProperties> MeshPropertiesArray;
			FWardrobeVatAssetPairArray VatAssets = SortedBodyPart.Value;

			//int32 NumValidMeshes = 0;
			for (FWardrobeVatAssetPair& WardrobeVatAsset : VatAssets.VatAssets)
			{
				FWardrobe Wardrobe;
				GetWardrobeFromTag(Wardrobe, WardrobeVatAsset.WardrobeID);
			
				// UStaticMesh *SrcMesh = nullptr;
				// //UStaticMesh *NewMesh = nullptr;
				// if(Wardrobe.WardrobeAnimation)
				// {
				// 	if(Wardrobe.WardrobeAnimation->MaterialReplacements.Contains(WardrobeVatAsset.VatAsset) && WardrobeVatAsset.VatAsset && !WardrobeVatAsset.VatAsset->VatStaticMesh.IsNull())
				// 	{
				// 		if(!WardrobeVatAsset.DuplicatedMesh)
				// 		{
				// 			if((SrcMesh = WardrobeVatAsset.VatAsset->VatStaticMesh.LoadSynchronous()) != nullptr)
				// 			{
				// 				FString NewMeshName = GetSanitizedWardrobeName(WardrobeVatAsset.WardrobeID) + GetSanitizedName(SortedBodyPart.Key,"BodyPart") + FString::Printf(TEXT("%d"),MeshPropertiesArray.Num());
				// 				WardrobeVatAsset.DuplicatedMesh = DuplicateObject<UStaticMesh>(SrcMesh,NewMeshRenderer, *NewMeshName);	
				// 			}
				// 		}
				// 		if(WardrobeVatAsset.DuplicatedMesh)
				// 		{
				// 			auto MaterialReplacements = Wardrobe.WardrobeAnimation->MaterialReplacements[WardrobeVatAsset.VatAsset];
				// 			TArray<FStaticMaterial> NewMaterials;
				// 			for (auto Material : MaterialReplacements.Materials)
				// 			{
				// 				NewMaterials.Add(FStaticMaterial(Material));
				// 			}
				// 			WardrobeVatAsset.DuplicatedMesh->SetStaticMaterials(NewMaterials);	
				// 		}
				// 	}
				// }
				FNiagaraMeshRendererMeshProperties TmpMeshProperties;
				TmpMeshProperties.LODMode = ENiagaraMeshLODMode::PerParticle;
				TmpMeshProperties.Mesh = WardrobeVatAsset.DuplicatedMesh;
				TmpMeshProperties.PivotOffset.Z = -100.0f;

				//if(NewMesh)
				//	NumValidMeshes++;
				MeshPropertiesArray.Add(TmpMeshProperties);
			}
			
			NewMeshRenderer->Meshes = MeshPropertiesArray;

			//FString NewMeshRenderName = "Blah" + FString::Printf(TEXT("%d"),Index);;
			//NewMeshRenderer->Rename(*NewMeshRenderName);

			//if(NumValidMeshes)
			{
				
				
			}
		}
		UOverCrowdRuntimeLibrary::MarkDirty(NiagaraSystem);
	}
			
				
//GetWardrobeFromTag(VatAssets)


// UNiagaraMeshRendererProperties* NewMeshRenderer = DuplicateObject<UNiagaraMeshRendererProperties>(CopyFrom,Emitter);//NewObject<UNiagaraMeshRendererProperties>(Emitter);
// if (NewMeshRenderer)
// {
// 	
// 	// Assign the mesh to the renderer
// 	TArray<FNiagaraMeshRendererMeshProperties> MeshPropertiesArray;
// 	TArray<FStaticMaterial> Mats = {Material};
//
// 	FNiagaraMeshRendererMeshProperties TmpMeshProperties;
// 	TmpMeshProperties.Mesh = DuplicateObject<UStaticMesh>(Mesh,Emitter);
// 	TmpMeshProperties.Mesh->SetStaticMaterials(Mats);
// 	MeshPropertiesArray.Add(TmpMeshProperties);
// 	NewMeshRenderer->Meshes = MeshPropertiesArray;
//
// 	TArray<FNiagaraVariable> BoundVariables = NewMeshRenderer->GetBoundAttributes();
// 	FNiagaraVariableAttributeBinding Binding = FNiagaraConstants::GetAttributeDefaultBinding(SYS_PARAM_PARTICLES_MESH_INDEX);
// 	
// 	NewMeshRenderer->MeshIndexBinding.SetValue(TEXT("Particles.Bollocks"),VersionedEmitter,ENiagaraRendererSourceDataMode::Particles);
//
// 	Emitter->AddRenderer(NewMeshRenderer, EmitterData->Version.VersionGuid);
//
// 	UOverCrowdRuntimeLibrary::MarkDirty(NiagaraSystem);
// 	//
// }

//}
}

TArray<FMaterialParamData> UOverCrowdRuntimeLibrary::PickRandomMaterialOverrides(TArray<FParameterRange> PerInstanceParams, const TArray<FGameplayTag> FilterOut)
{
	TArray<FMaterialParamData> Out;
	for(auto [ParamID, ValueRange, bMustMatchToOverride] : PerInstanceParams)
	{
		if(int32 ParamIndex = -1; GetParamIndexFromTag(ParamID, ParamIndex))
		{
			const float Time = static_cast<float>(FMath::Rand())/RAND_MAX;
			if(!FilterOut.Contains(ParamID))
			{
				if(ValueRange)
					Out.Add(FMaterialParamData(ParamID, ValueRange->GetFloatValue(Time), ParamIndex));
				else
					Out.Add(FMaterialParamData(ParamID, Time, ParamIndex));
			}
		}
	}
	return Out;
}

void UOverCrowdRuntimeLibrary::GetAllBodyPartsFromWardrobes(const TArray<FWardrobe>& Wardrobes, TMap<FGameplayTag, FVatAssetArray>& OutBodyParts, TMap<UVatMeshPairing*, int32>& OutIndexLookup)
{
	for (auto& Wardrobe : Wardrobes)
	{
		for (auto& BodyParts : Wardrobe.Compartments)
		{
			FVatAssetArray& AssetArray = OutBodyParts.FindOrAdd(BodyParts.Key);

			for (auto BodyPart : BodyParts.Value.TaggedVatAssets)
			{
				if (BodyPart && !AssetArray.TaggedVatAssets.Contains(BodyPart))
				{
					int32 Index = AssetArray.TaggedVatAssets.Add(BodyPart);
					OutIndexLookup.Add(BodyPart, Index);
				}
			}
		}
	}
}

TArray<FVector> UOverCrowdRuntimeLibrary::RemoveLocationsThatIntersect(const TArray<FVector>& InLocations, float DistanceThreshold, FBox2D Extent)
{
	TArray<FVector> Out;

	float QuadTreeMinSize = 10;
	TQuadTree<FVector, 100> QuadTree(Extent, QuadTreeMinSize);
	for (FVector Location : InLocations)
	{
		TArray<FVector> Results;
		
		FBox2D Box(FVector2D(Location.X - DistanceThreshold, Location.Y - DistanceThreshold) , FVector2D(Location.X + DistanceThreshold, Location.Y + DistanceThreshold));

		QuadTree.GetElements(Box, Results);

		if (!Results.Num())
		{
			Out.Add(Location);
			QuadTree.Insert(Location, Box);
		}
	}
	return Out;
}


void UOverCrowdRuntimeLibrary::MoveActorsToLevel(ULevel* Level, TArray<AActor*> Actors,  AActor* AttachTo)
{
#if WITH_EDITOR
	UEditorLevelUtils::MoveActorsToLevel(Actors, Level);

	if(AttachTo)
	{
		for(AActor* Actor : Actors)
		{
			Actor->AttachToActor(AttachTo, FAttachmentTransformRules(EAttachmentRule::KeepWorld,false));
		}
	}
	//UEditorLevelUtils::MoveSelectedActorsToLevel(GetWorld()->GetCurrentLevel());
#endif
}
