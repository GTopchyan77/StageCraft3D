// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DlFmVectorAbstract.generated.h"

/**
 * 
 */
UCLASS()
class DLFILEMANAGER_API UDlFmVectorAbstract : public UUserWidget
{
	GENERATED_BODY()
public:
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnVectorValueChangedEvent, FVector, Value);
	
	UFUNCTION(BlueprintCallable, Category="Behaivor")
	virtual void SetValue_NoBroadcast(const FVector& NewValue);
	UFUNCTION(BlueprintCallable, Category="Behaivor")
	virtual void SetValue(const FVector& NewValue);
	
	UFUNCTION(BlueprintCallable, Category="Behaivor")
	const FVector& GetValue() const{return CurValue;};
	FOnVectorValueChangedEvent OnVectorValueChanged;
protected:
	UPROPERTY(EditAnywhere, Category="Behaivor")
	FVector CurValue;
};
