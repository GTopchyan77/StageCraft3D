// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved


#include "OverCrowdUtilityWidget.h"
#include "OverCrowdEditor.h"
#include "EditWardrobe.h"
#include "GameplayTagContainer.h"
//#include "SGameplayTagPicker.h"
#include "AssetToolsModule.h"
#include "Interfaces/IMainFrameModule.h"
#include "ToolMenuSection.h"
#include "EditWardrobe.h"
#include "IAssetTools.h"
#include "OverCrowdRuntimeLibrary.h"
#include "VatAssetArrayWidget.h"
#include "VatAssetWidget.h"
#include "Factories/DataAssetFactory.h"


void UOverCrowdUtilityWidget::AddRemoveWardrobes(FGameplayTagContainer CurrentWardrobes)
{
	WardrobeIDs = CurrentWardrobes.GetGameplayTagArray();
	
	TSharedRef<SWindow> Window = SNew(SWindow)
		.ClientSize(FVector2D(500, 600))
		[
			SNew(SEditWardrobe)
			.InOverCrowdUtilityWidget(this)
			//.WardrobeContainer(CurrentWardrobes)
			.OnWardrobeEdited_Lambda([&](bool DoIt)
			{
				UE_LOG(LogOverCrowdEditor, Warning, TEXT("OnWardrobeEdited_Lambda"));
				if(DoIt)
				{
					FGameplayTagContainer OutContainer;
					for(auto Wardrobe : WardrobeIDs)
					{
						OutContainer.AddTag(Wardrobe);
					}
					GameplayTagSelected(OutContainer);	
				}
				// if(CurrentWardrobeWindow.)
				// {
				// 	CurrentWardrobeWindow.Pin()->DestroyWindowImmediately();	
				// }
			})
		];

	//CurrentWardrobeWindow = Window;
	
	IMainFrameModule& MainFrameModule = FModuleManager::LoadModuleChecked<IMainFrameModule>(TEXT("MainFrame"));
	if (MainFrameModule.GetParentWindow().IsValid())
	{
		FSlateApplication::Get().AddWindowAsNativeChild(Window, MainFrameModule.GetParentWindow().ToSharedRef());
	}
	else
	{
		FSlateApplication::Get().AddWindow(Window);
	}
}

UVatAnimationDataAsset* UOverCrowdUtilityWidget::CreateAdditveVatAnimationDataAsset(UVatMeshPairing* VatMesh)
{
	if(!VatMesh)
		return nullptr;
	
	auto Skel = VatMesh->GetSkelMeshSkeleton();
	if (!Skel)
		return nullptr;

	UVatAnimationDataAsset *OutDataAsset = nullptr;
	
	FString VatMeshName = Skel->GetName();
	VatMeshName = "VA_Additive_" + VatMeshName;

	IAssetTools& AssetTools = FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	auto FullPackagePath = UOverCrowdRuntimeLibrary::GetPluginSaveDirectory() + "/"+ VatMeshName;
	OutDataAsset = LoadObject<UVatAnimationDataAsset>(nullptr, *FullPackagePath);
	if(!OutDataAsset)
	{
		FString OutPackageName, OutName;
		AssetTools.CreateUniqueAssetName(FullPackagePath, TEXT(""), OutPackageName, OutName);
		UFactory* DataAssetFactory = NewObject<UFactory>(GetTransientPackage(), UDataAssetFactory::StaticClass());
		UObject* NewAsset = AssetTools.CreateAsset(OutName, OutPackageName, UVatAnimationDataAsset::StaticClass(), DataAssetFactory);

		OutDataAsset =  Cast<UVatAnimationDataAsset>(NewAsset);
	}
	
	return OutDataAsset;
}

UAnimationsToBakeDataAsset* UOverCrowdUtilityWidget::CreateAdditveAnimToBakeDataAsset(UVatAnimationDataAsset* VatAnimAsset)
{
	if(!VatAnimAsset)
		return nullptr;

	UAnimationsToBakeDataAsset *OutDataAsset = nullptr;
	
	FString VatAssetName = VatAnimAsset->GetName();
	FString AnimSetName = "A2B_" + VatAssetName;
	AnimSetName = AnimSetName.Replace(TEXT("VA_"),TEXT(""),ESearchCase::IgnoreCase);
	
	IAssetTools& AssetTools = FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	auto FullPackagePath = UOverCrowdRuntimeLibrary::GetPluginSaveDirectory() + "/"+ AnimSetName;
	FString OutPackageName, OutName;
	AssetTools.CreateUniqueAssetName(FullPackagePath, TEXT(""), OutPackageName, OutName);

	UFactory* DataAssetFactory = NewObject<UFactory>(GetTransientPackage(), UDataAssetFactory::StaticClass());
	UObject* NewAsset = AssetTools.CreateAsset(OutName, FullPackagePath, UAnimationsToBakeDataAsset::StaticClass(), DataAssetFactory);

	OutDataAsset =  Cast<UAnimationsToBakeDataAsset>(NewAsset);
	VatAnimAsset->AnimationsToBake = OutDataAsset;

	VatAnimAsset->MarkPackageDirty();
	
	return OutDataAsset;
}

void UOverCrowdUtilityWidget::OnPropertyEdited(FName Name)
{
}

void UOverCrowdUtilityWidget::GameplayTagSelected_Implementation(FGameplayTagContainer NewSelection)
{
}
