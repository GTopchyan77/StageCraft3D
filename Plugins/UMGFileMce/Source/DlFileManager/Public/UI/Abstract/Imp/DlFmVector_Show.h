// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UI/Abstract/DlFmVectorAbstract.h"
#include "DlFmVector_Show.generated.h"

class UDlFmFloat_ShowSimple;
/**
 * 
 */
UCLASS() 
class DLFILEMANAGER_API UDlFmVector_Show : public UDlFmVectorAbstract
{
	GENERATED_BODY()
public:
	virtual void SetValue(const FVector& NewValue) override;
private:
	
	UPROPERTY(meta = (BindWidget))
	UDlFmFloat_ShowSimple* Float_X;
	UPROPERTY(meta = (BindWidget))
	UDlFmFloat_ShowSimple* Float_Y;
	UPROPERTY(meta = (BindWidget))
	UDlFmFloat_ShowSimple* Float_Z;
	
};
