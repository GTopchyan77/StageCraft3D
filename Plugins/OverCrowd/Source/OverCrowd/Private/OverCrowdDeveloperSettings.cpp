// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved


#include "OverCrowdDeveloperSettings.h"
#include "GameplayTagsManager.h"

#if WITH_EDITOR
	#include "Editor.h"
#endif

#define LOCTEXT_NAMESPACE "OverCrowdSettings"

FName UOverCrowdDeveloperSettings::GetContainerName() const
{
	return Super::GetContainerName();
}

FName UOverCrowdDeveloperSettings::GetCategoryName() const
{
	return TEXT("OverCrowd");
}

UOverCrowdDeveloperSettings::UOverCrowdDeveloperSettings()
{
	UGameplayTagsManager& GameplayTagsManager =  UGameplayTagsManager::Get();
	ClothingMaskTag = GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.Material.Param8.ClothingMask"), FString("ClothingMask custom data param index"));

	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.BodyPart.Accessory"), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.BodyPart.Arms"     ), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.BodyPart.Body"     ), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.BodyPart.Dress"    ), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.BodyPart.Feet"     ), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.BodyPart.Hair"     ), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.BodyPart.Head"     ), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.BodyPart.Legs"     ), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.BodyPart.Torso"    ), FString(""));

	
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.Animation.Default"), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.Wardrobe.Default"), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.PlacementGroup.Default"), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.PlacementGroup.Niagara"), FString(""));
	
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.Material.Param09.Default"), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.Material.Param10.Default"), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.Material.Param11.Default"), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.Material.Param12.Default"), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.Material.Param13.Default"), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.Material.Param14.Default"), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.Material.Param15.Default"), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.Material.Param15.Default"), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.Material.Param16.Default"), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.Material.Param16.Default"), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.Material.Param17.Default"), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.Material.Param18.Default"), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.Material.Param19.Default"), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.Material.Param20.Default"), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.Material.Param21.Default"), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.Material.Param22.Default"), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.Material.Param23.Default"), FString(""));
	GameplayTagsManager.AddNativeGameplayTag(FName("OverCrowd.Material.Param24.Default"), FString(""));
}

FName UOverCrowdDeveloperSettings::GetSectionName() const
{
	return Super::GetSectionName();
}

UOverCrowdDeveloperSettings* UOverCrowdDeveloperSettings::Get()
{
	static UOverCrowdDeveloperSettings* Instance;

	if(Instance != nullptr)
	{
		return Instance;
	}

	for (TObjectIterator<UOverCrowdDeveloperSettings> SettingsIt(RF_NoFlags); SettingsIt; ++SettingsIt)
	{
		Instance = *SettingsIt;
		break;
	}
	
	return Instance;
}

#if WITH_EDITOR
FText UOverCrowdDeveloperSettings::GetSectionText() const
{
	return LOCTEXT("UOverCrowdDeveloperSettings::GetSectionText", "OverCrowd - VAT Toolkit");
}

FText UOverCrowdDeveloperSettings::GetSectionDescription() const
{
	return LOCTEXT("UOverCrowdDeveloperSettings::GetSectionDescription",
				   "Easily create massive amount of characters using vertex animation");
}

void UOverCrowdDeveloperSettings::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	
}

#endif
