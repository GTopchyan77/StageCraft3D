// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/IUserObjectListEntry.h"
#include "Blueprint/UserWidget.h"
#include "VersionSync/DlFmVersion.h"
#include "DlFmRemapList_Lib.generated.h"

class UTextBlock;
class UComboBoxString;
class UDlFmRemapList;
class UDlFmBorder;
class UDlFmRemapList_OneWidget;

UCLASS()
class DLFILEMANAGER_API UDlFmRemapList_OneData : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY()
	int32 SelfIndex;
	UPROPERTY()
	UDlFmRemapList* Parent;
	UPROPERTY()
	UDlFmRemapList_OneWidget* Widget;

	void RefreshWidgetSelect() const;
};

UCLASS()
class DLFILEMANAGER_API UDlFmRemapList_OneWidget : public UUserWidget, public IUserObjectListEntry
{
	GENERATED_BODY()
public:
	
	UPROPERTY(meta=(BindWidget))
	UTextBlock* Text_KeyName;
	UPROPERTY(meta=(BindWidget))
	UComboBoxString* Combo_ValueName;
	UFUNCTION()
	void OnSelectChanged_ValueName(FString SelectedItem, ESelectInfo::Type SelectionType);
	UPROPERTY(meta=(BindWidgetOptional))
	UDlFmBorder* Border_IsNone;
	
	UPROPERTY(EditAnywhere, Category="Appearance")
	FSlateBrush Brush_IsNone;
	UPROPERTY(EditAnywhere, Category="Appearance")
	FSlateBrush Brush_NotNone;
	
	UPROPERTY()
	UDlFmRemapList_OneData* SelfData;

	FDlFmLiteVersion_Observer Ver_ValueList;

	virtual void NativeConstruct() override;
	virtual void NativeOnListItemObjectSet(UObject* ListItemObject) override;
	void SetUI_SelectIndex(int32 Index) const;
private:
	void SetStyle_SelectNone(bool Val) const;
};

