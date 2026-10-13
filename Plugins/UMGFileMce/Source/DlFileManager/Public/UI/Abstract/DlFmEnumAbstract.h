// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DlFmEnumAbstract.generated.h"

/**
 * 
 */
UCLASS()
class DLFILEMANAGER_API UDlFmEnumAbstract : public UUserWidget
{
	//The Enum should start from 0 to TotalNum,
	GENERATED_BODY()
public:
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnUint8ValueChangedEvent, uint8, Value);
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnUint8ValueChangedFromToEvent, uint8, OldVal,uint8 ,NewVal);

	UFUNCTION()
	virtual uint8 SetCurIndex(const uint8 NewValue);
	UFUNCTION()
	virtual uint8 SetCurIndex_NoBroadcast(const uint8 NewValue);
	UFUNCTION()
	uint8 GetCurIndex() const ;
	UFUNCTION()
	virtual void SetTotalNum(const uint8 NewTotal);
	UFUNCTION()
	virtual void SetTotalNum_NoBroadcast(const uint8 NewTotal);
	UFUNCTION()
	virtual uint8 SetCurIndex_Next();
	UFUNCTION()
	virtual uint8 SetCurIndex_Previous();
	UFUNCTION()
	bool IsFirst() const;
	UFUNCTION()
	bool IsLast() const;
	UFUNCTION()
	virtual void SetItemVisible(uint8 Item, ESlateVisibility Visible) const{}
	
	UPROPERTY(BlueprintAssignable, Category="Widget Event")
	FOnUint8ValueChangedEvent OnValueChanged;
	FOnUint8ValueChangedFromToEvent OnValueChangedFromTo;
protected:
	UPROPERTY(EditAnywhere, Category="Behaivor")
	uint8 CurIndex=0;
	UPROPERTY(EditAnywhere, Category="Behaivor")
	uint8 TotalNum=1;

};
