// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UI/Abstract/DlFmVector4Abstract.h"
#include "DlFmVector4_Show.generated.h"

class UDlFmFloat_ShowSimple;
/**
 * 
 */
UCLASS()
class DLFILEMANAGER_API UDlFmVector4_Show : public UDlFmVector4Abstract
{
	GENERATED_BODY()
public:
	virtual void SetValue(const FVector4& NewValue) override;
private:
	
	UPROPERTY(meta = (BindWidget))
	UDlFmFloat_ShowSimple* Float_X;
	UPROPERTY(meta = (BindWidget))
	UDlFmFloat_ShowSimple* Float_Y;
	UPROPERTY(meta = (BindWidget))
	UDlFmFloat_ShowSimple* Float_Z;
	UPROPERTY(meta = (BindWidget))
	UDlFmFloat_ShowSimple* Float_W;
};
