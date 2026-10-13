// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UI/Abstract/DlFmVectorAbstract.h"
#include "DlFmVector_Edit.generated.h"

class UDlFmFloat_EditSimple;
/**
 * 
 */
UCLASS()
class DLFILEMANAGER_API UDlFmVector_Edit : public UDlFmVectorAbstract
{
	GENERATED_BODY()
public:
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnVectorValueChangedEvent, FVector, Value);

	FOnVectorValueChangedEvent OnValueChangedEvent;
	virtual void NativeConstruct() override;

	virtual void SetValue_NoBroadcast(const FVector& NewValue) override;
	
	virtual void SetValue(const FVector& NewValue) override;

private:
	
	UPROPERTY(meta = (BindWidget))
	UDlFmFloat_EditSimple* Float_X;
	UFUNCTION()
	void OnValueChanged_X(float Val,ETextCommit::Type CommitMethod);
	UPROPERTY(meta = (BindWidget))
	UDlFmFloat_EditSimple* Float_Y;
	UFUNCTION()
	void OnValueChanged_Y(float Val,ETextCommit::Type CommitMethod);
	UPROPERTY(meta = (BindWidget))
	UDlFmFloat_EditSimple* Float_Z;
	UFUNCTION()
	void OnValueChanged_Z(float Val,ETextCommit::Type CommitMethod);
	
};
