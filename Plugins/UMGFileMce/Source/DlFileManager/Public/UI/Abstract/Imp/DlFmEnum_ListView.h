// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ListView.h"
#include "UI/Abstract/DlFmEnumAbstract.h"
#include "ModifyU/UI/DlFmButton.h"
#include "DlFmEnum_ListView.generated.h"

class UListView;
class UDlFmEnum_ListView;
class UDlFmButton;
/**
 * 
 */

UCLASS()
class UDlFmEnum_ListView_ItemData : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY()
	uint8 SelfIndex;
	UPROPERTY()
	ESlateVisibility Visible=ESlateVisibility::Visible;
	 
	UDlFmEnum_ListView* Parent;
};

UCLASS()
class UDlFmEnum_ListView_ItemWidget : public UUserWidget, public IUserObjectListEntry
{
	GENERATED_BODY()
public:
	virtual void NativeConstruct() override;
	virtual void NativeOnListItemObjectSet(UObject* ListItemObject) override;
private:
	UPROPERTY(meta = (BindWidget))
	UDlFmButton* Button_Select;
	UFUNCTION()
	void OnSelect_Pressed();

	UDlFmEnum_ListView_ItemData* DataPtr;
};

UCLASS()
class DLFILEMANAGER_API UDlFmEnum_ListView : public UDlFmEnumAbstract
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category = "Toggle")
	FSlateBrush WhenTrue;
	UPROPERTY(EditAnywhere, Category = "Toggle")
	FSlateBrush WhenHoveredTrue;
	UPROPERTY(EditAnywhere, Category = "Toggle")
	FSlateBrush WhenDisabledTrue;
	UPROPERTY(EditAnywhere, Category = "Toggle")
	FSlateBrush WhenFalse;
	UPROPERTY(EditAnywhere, Category = "Toggle")
	FSlateBrush WhenHoveredFalse;
	UPROPERTY(EditAnywhere, Category = "Toggle")
	FSlateBrush WhenDisabledFalse;
	
	virtual void NativeConstruct() override;
	virtual uint8 SetCurIndex(const uint8 NewValue) override;
	virtual uint8 SetCurIndex_NoBroadcast(const uint8 NewValue) override;
	virtual void SetTotalNum(const uint8 NewTotal) override;
	virtual void SetTotalNum_NoBroadcast(const uint8 NewTotal) override;
	virtual uint8 SetCurIndex_Next() override;
	virtual uint8 SetCurIndex_Previous() override;
	virtual void SetItemVisible(uint8 ItemIndexV, ESlateVisibility VisibleV) const override;
private:
	UPROPERTY(meta = (BindWidget))
	UListView* MainList;
	UPROPERTY()
	TArray<UDlFmEnum_ListView_ItemData*> ItemPool;

	void Set_ItemPoolNum(uint8 NewNum);
};
