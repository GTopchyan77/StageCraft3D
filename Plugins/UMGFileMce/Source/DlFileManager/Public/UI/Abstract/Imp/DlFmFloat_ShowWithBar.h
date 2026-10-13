// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UI/Abstract/DlFmFloatAbstract.h"
#include "DlFmFloat_ShowWithBar.generated.h"

class UDlFmFloat_ShowSimple;
class UProgressBar;
/**
 * 
 */
UCLASS()
class DLFILEMANAGER_API UDlFmFloat_ShowWithBar : public UDlFmFloatAbstract
{
	GENERATED_BODY()
public:
	
	UPROPERTY(meta = (BindWidget))
	UDlFmFloat_ShowSimple* Float_Number;
	UPROPERTY(meta = (BindWidget))
	UProgressBar* Bar_Percent;
	
	virtual void NativeConstruct() override;
	virtual float SetValue(float NewValue) override;
	virtual float SetValue_NoBroadcast(float NewValue) override;

	
	virtual void SetLimitMaxValue(bool bUseV) override {}
	virtual void SetLimitMinValue(bool bUseV) override {}
	virtual void SetLimitMaxMinValue(bool bUseV) override {}
	
};
