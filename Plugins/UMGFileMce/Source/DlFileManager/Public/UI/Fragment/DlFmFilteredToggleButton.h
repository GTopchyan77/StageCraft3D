// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ModifyU/UI/DlFmButton.h"
#include "Components/CheckBox.h"
#include "DlFmFilteredToggleButton.generated.h"

USTRUCT()
struct FDlFmFilteredToggleButton_Config
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
	UPROPERTY(EditAnywhere, Category = "Toggle")
	bool InitBool = true;
	
};
/**
 * 
 */
UCLASS()
class DLFILEMANAGER_API UDlFmFilteredToggleButton : public UDlFmButton
{
	GENERATED_UCLASS_BODY()
public:	
	UPROPERTY(BlueprintAssignable, Category="CheckBox|Event")
	FOnCheckBoxComponentStateChanged OnCheckStateChanged;
	UFUNCTION(BlueprintCallable, Category = "Widget")
	void SetIsChecked(bool InIsChecked);
	//Called by DlCombo, will not trigger Event, subscribe combo instead to get info
	void SetIsChecked_NoBroadcast(bool InIsChecked);
	UFUNCTION(BlueprintCallable, Category = "Widget")
	bool IsChecked() const;

	virtual void SynchronizeProperties() override;


private:
	UPROPERTY(EditAnywhere, Category = "Toggle")
	FDlFmFilteredToggleButton_Config Config;
	UFUNCTION()
	void DlFilteredClick_UpperHandle();

	bool CurrentBool;
	//float lastChangeTime = 0;
	FButtonStyle CacheButtonStyle;
	void UpdateStyle();
};
