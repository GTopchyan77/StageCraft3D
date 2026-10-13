// Copyright(c) 2022 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DlFmLib.h"
#include "DlFmDialog_Base_Logic.generated.h"

class UDlFmButton;
class UTextBlock;
/**
 * 
 */

UENUM()
enum class EDlFm_DialogEventType:uint8
{
	Click_Close=0,
	Click_Cancel=1,
	Click_OK=2,
	Click_Yes=3,
	Click_YesForAll=4,
	Click_No=5,
	Click_NoForAll=6,
	Click_SelectNewerModified=50,
	Click_SelectNewerModified_ForAll=51,
	Click_SelectLarger=52,
	Click_SelectLarger_ForAll=53,
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam( FOnDialogClicked, EDlFm_DialogEventType, EventType );

UCLASS()
class DLFILEMANAGER_API UDlFmDialog_Base_Logic : public UUserWidget
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category="Dialog Behavior")
	bool bAutoClose=true;
	virtual void DoWork_Close();
	virtual void DoWork_ChooseYes();
	virtual void DoWork_ChooseNo();
	virtual void DoWork_ChooseYesForAll();
	virtual void DoWork_ChooseNoForAll();
	virtual void DoWork_ChooseCancel();
	
	virtual void DoWork_SelectNewerModified();
	virtual void DoWork_SelectNewerModified_ForAll();
	virtual void DoWork_SelectLarger();
	virtual void DoWork_SelectLarger_ForAll();
	
	virtual void NativeConstruct() override;
	FOnDialogClicked OnDialogClicked;
private:
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_Close;
	UFUNCTION()
	void OnInternalClick_Close();
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_Yes;
	UFUNCTION()
	void OnInternalClick_Yes();
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_No;
	UFUNCTION()
	void OnInternalClick_No();
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_YesForAll;
	UFUNCTION()
	void OnInternalClick_YesForAll();
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_NoForAll;
	UFUNCTION()
	void OnInternalClick_NoForAll();
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_Cancel;
	UFUNCTION()
	void OnInternalClick_Cancel();
	
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_SelectNewerModified;
	UFUNCTION()
	void OnInternalClick_SelectNewerModified();
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_SelectNewerModified_ForAll;
	UFUNCTION()
	void OnInternalClick_SelectNewerModified_ForAll();
	
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_SelectLarger;
	UFUNCTION()
	void OnInternalClick_SelectLarger();
	UPROPERTY(meta = (BindWidgetOptional))
	UDlFmButton* Button_SelectLarger_ForAll;
	UFUNCTION()
	void OnInternalClick_SelectLarger_ForAll();
};

