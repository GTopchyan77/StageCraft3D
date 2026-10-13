// Copyright(c) 2022 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DlFmLib.h"
#include "Blueprint/IUserObjectListEntry.h"
#include "DlFm_Item_Logic.generated.h"

class UDlFmButton;
class UEditableText;
class UDlFmFilteredEditableText;
class UTextBlock;
class UImage;
class UDlFmFilteredToggleButton;
class UDlFm_Item_Data;
struct FDlUI_FileExplorer_ItemInfo;

UCLASS()
class DLFILEMANAGER_API UDlFm_Item_Logic : public UUserWidget, public IUserObjectListEntry
{
	GENERATED_BODY()
public:
	virtual void NativeConstruct() override;

	virtual void NativeOnListItemObjectSet(UObject* ListItemObject) override;
	void SetStatus(const FDlUI_FileExplorer_ItemInfo& Item, const int32& FixedIndexVal);
	void SetModeStyle(const FDlUI_FileExplorer_ItemInfo& Info, FDlUI_FileExplorer_Lib::ESelectMode SelectMode,
	                  FDlUI_FileExplorer_Lib::EExploreMode ExploreMode);

	UFUNCTION(BlueprintNativeEvent)
	void SetIconBrush(const FDlUI_FileExplorer_ItemInfo& Item,UImage* Target_Image_Icon);
	virtual void SetIconBrush_Implementation(const FDlUI_FileExplorer_ItemInfo& Item,UImage* Target_Image_Icon);


private:
	UPROPERTY(meta = (BindWidget))
	UDlFmFilteredToggleButton* Button_Background;
	UFUNCTION()
	void OnInternalClicked_Background(bool Val);
	UFUNCTION()
	void OnInternalCommit_Name(const FText& Text, ETextCommit::Type CommitMethod);
	UFUNCTION()
	void OnInternalChanged_Select(bool Val);
	UFUNCTION()
	void OnInternalClicked_FunctionButton();

	UPROPERTY(EditAnywhere, Category="Apperance")
	FSlateBrush IconBrush_Directory;
	UPROPERTY(EditAnywhere, Category="Apperance")
	FSlateBrush IconBrush_NormalFile;
	UPROPERTY(EditAnywhere, Category="Apperance")
	TMap<FString, FSlateBrush> IconBrush_SpecialFile;

	UPROPERTY(meta = (BindWidget))
	UDlFmFilteredEditableText* Text_ItemName;
	UPROPERTY(meta = (BindWidget))
	UTextBlock* Text_ItemName_Show;
	void Internal_SetNameEditable(bool Val) const;
	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* Text_ItemPath;
	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* Text_Extension;
	UPROPERTY(meta = (BindWidgetOptional))
	UImage* Image_Icon;
	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* Text_AccessDate;
	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* Text_CreationDate;
	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* Text_ModificationDate;
	UPROPERTY(meta = (BindWidgetOptional))
	UWidget* WShow_ReadOnly;
	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* Text_Size;
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmFilteredToggleButton* Toggle_Selectable_InMultiMode;

	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_Function;

	UDlFm_Item_Data* CurrentItem;
};
