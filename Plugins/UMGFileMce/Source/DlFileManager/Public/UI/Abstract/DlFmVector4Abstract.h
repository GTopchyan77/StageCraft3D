// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DlFmVector4Abstract.generated.h"

/**
 * 
 */
UCLASS()
class DLFILEMANAGER_API UDlFmVector4Abstract : public UUserWidget
{
	GENERATED_BODY()
public:
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnFloatValueChangedEvent, FVector4, Value);
	UFUNCTION(BlueprintCallable, Category="Behaivor")
	virtual void SetValue(const FVector4& NewValue);
	UFUNCTION(BlueprintCallable, Category="Behaivor")
	const FVector4& GetValue() const{return CurValue;};
protected:
	UPROPERTY(EditAnywhere, Category="Behaivor")
	FVector4 CurValue;
};
