// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UI/Abstract/DlFmEnumAbstract.h"
#include "DlFmEnum_Panel.generated.h"

class UPanelWidget;
class UDlFmFilteredToggleButton;
class UDlFmEnum_Grid;


class UDlFmEnum_Panel;
UCLASS()
class DLFILEMANAGER_API UDlFmEnum_Panel_Item : public UUserWidget
{
	GENERATED_BODY()
public:
	UPROPERTY(meta = (BindWidget))
	UDlFmFilteredToggleButton* Toggle_Switch;
	UFUNCTION()
	void OnToggleChanged_Switch(bool NewV);
	UPROPERTY(EditAnywhere, Category="Behaivor")
	uint8 SelfIndex;//Within One UDlFmEnum_Grid, this value must distribute from 0 to N

	UDlFmEnum_Panel* Parent;

	virtual void NativeConstruct() override;
};
/**
 * 
 */
UCLASS()
class DLFILEMANAGER_API UDlFmEnum_Panel : public UDlFmEnumAbstract
{
	GENERATED_BODY()
public:

	virtual void NativeConstruct() override;
	virtual uint8 SetCurIndex(const uint8 NewValue) override;
	virtual uint8 SetCurIndex_NoBroadcast(const uint8 NewValue) override;
	virtual void SetTotalNum(const uint8 NewTotal) override{}
	virtual void SetTotalNum_NoBroadcast(const uint8 NewTotal) override{}
	virtual uint8 SetCurIndex_Next() override{return CurIndex;}
	virtual uint8 SetCurIndex_Previous() override{return CurIndex;}
	virtual void SetItemVisible(uint8 Item, ESlateVisibility Visible) const override;
private:
	
	UPROPERTY(meta = (BindWidget))
	UPanelWidget* Panel;

	UPROPERTY()
	//TArray<UDlFmEnum_Panel_Item*> ChildWidgetList;
	TMap<uint8,UDlFmEnum_Panel_Item*> ChildWidgetMap;
};
