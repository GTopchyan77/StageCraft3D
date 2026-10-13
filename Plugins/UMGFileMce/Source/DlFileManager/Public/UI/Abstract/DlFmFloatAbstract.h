// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DlFmFloatAbstract.generated.h"

/**
 * 
 */
UCLASS()
class DLFILEMANAGER_API UDlFmFloatAbstract : public UUserWidget
{
	GENERATED_BODY()
public:
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnFloatValueChangedEvent, float, Value);
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnFloatRangeChangedEvent, float, MinV, float, MaxV);

	UFUNCTION(BlueprintCallable, Category="API")
	virtual void SetEditable(bool bAbleEditV);

	virtual void SetLimitMaxValue(bool bUseV);
	virtual void SetLimitMinValue(bool bUseV);
	
	UFUNCTION(BlueprintCallable, Category="API")
	virtual void SetLimitMaxMinValue(bool bUseV);

	virtual void SetMaxValue(float NewMax);
	virtual void SetMinValue(float NewMin);
	
	virtual void SetArrange_Interval(FFloatInterval NewArrange);
	UFUNCTION(BlueprintCallable, Category="API")
	virtual void SetArrange_2Number(float NewMin, float NewMax);

	UFUNCTION(BlueprintCallable, Category="API")
	virtual float SetValue(float NewValue);
	UFUNCTION(BlueprintCallable, Category="API")
	void SetChangeStep(float NewStep);
	UFUNCTION(BlueprintCallable, Category="API")
	virtual float SetValue_Add() ;
	UFUNCTION(BlueprintCallable, Category="API")
	virtual float SetValue_Sub() ;
	UFUNCTION(BlueprintCallable, Category="API")
	virtual float SetValue_NoBroadcast(float NewValue);
	UFUNCTION(BlueprintCallable, Category="API")
	float GetValue() const;

	UPROPERTY(BlueprintAssignable, Category="Widget Event")
	FOnFloatValueChangedEvent OnValueChanged;
	UPROPERTY(BlueprintAssignable, Category="Widget Event")
	FOnFloatRangeChangedEvent OnRangeChanged; 
	
protected:
	UPROPERTY(EditAnywhere, Category="Behavior")
	bool bEditable=true;
	UPROPERTY(EditAnywhere, Category="Behavior")
	float CurValue=0;
	UPROPERTY(EditAnywhere, Category="Behavior")
	float ChangeStep=1;
	UPROPERTY(EditAnywhere, Category="Behavior")
	bool bMinLimited=false;
	UPROPERTY(EditAnywhere, Category="Behavior")
	bool bMaxLimited=false;
	UPROPERTY(EditAnywhere, Category="Behavior")
	FFloatInterval CurRange=FFloatInterval(0,1);
};

