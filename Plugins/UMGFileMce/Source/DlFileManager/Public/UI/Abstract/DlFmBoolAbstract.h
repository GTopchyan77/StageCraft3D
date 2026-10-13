// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DlFmBoolAbstract.generated.h"

/**
 * 
 */
UCLASS()
class DLFILEMANAGER_API UDlFmBoolAbstract : public UUserWidget
{
	GENERATED_BODY()
public:
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnBoolValueChangedEvent, bool, Value);

	UFUNCTION()
	virtual void SetEditable(bool bAbleEdit){}
	UFUNCTION()
	virtual void SetValue(bool NewValue){}
	UFUNCTION()
	virtual bool GetValue() const{return CurValue;}
	
	UPROPERTY(BlueprintAssignable, Category="Widget Event")
	FOnBoolValueChangedEvent OnValueChanged;
protected:
	bool CurValue;
};
