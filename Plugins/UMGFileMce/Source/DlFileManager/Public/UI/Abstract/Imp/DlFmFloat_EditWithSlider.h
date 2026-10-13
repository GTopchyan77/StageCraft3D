// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UI/Abstract/DlFmFloatAbstract.h"
#include "DlFmFloat_EditWithSlider.generated.h"

class USlider;
class UDlFmFloat_EditSimple;
/**
 * 
 */
UCLASS()
class DLFILEMANAGER_API UDlFmFloat_EditWithSlider : public UDlFmFloatAbstract
{
	GENERATED_BODY()
public:
	
	UPROPERTY(meta = (BindWidget))
	UDlFmFloat_EditSimple* Float_Number;
	UFUNCTION()
	void OnCommit_Number(float Val, ETextCommit::Type CommitMethod);
	UPROPERTY(meta = (BindWidget))
	USlider* Slider_Percent;
	UFUNCTION()
	void OnValueChanged_Percent(float NewVal);

	virtual void NativeConstruct() override;
	virtual void SetEditable(bool bAbleEditV) override;
	virtual float SetValue(float NewValue) override;
	virtual float SetValue_NoBroadcast(float NewValue) override;
	
	virtual void SetLimitMaxValue(bool bUseV) override {}
	virtual void SetLimitMinValue(bool bUseV) override {}
	virtual void SetLimitMaxMinValue(bool bUseV) override {}
	
	virtual void SetMaxValue(float NewMax) override;
	virtual void SetMinValue(float NewMin) override;
	
	virtual void SetArrange_Interval(FFloatInterval NewArrange) override;
	virtual void SetArrange_2Number(float NewMin, float NewMax) override;
};
