// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UI/Abstract/DlFmEnumAbstract.h"
#include "DlFmEnum_OneLine.generated.h"

class UDlFmButton;
class UTextBlock;
/**
 * 
 */
UCLASS()
class DLFILEMANAGER_API UDlFmEnum_OneLine : public UDlFmEnumAbstract
{
	GENERATED_BODY()
public:

	virtual void NativeConstruct() override;
	
	virtual uint8 SetCurIndex(const uint8 NewValue) override;
	virtual uint8 SetCurIndex_NoBroadcast(const uint8 NewValue) override;
	virtual void SetTotalNum(const uint8 NewTotal) override;
	virtual void SetTotalNum_NoBroadcast(const uint8 NewTotal) override;
	virtual uint8 SetCurIndex_Next() override;
	virtual uint8 SetCurIndex_Previous() override;
	virtual void SetItemVisible(uint8 Item, ESlateVisibility Visible) const override{}

	void InitFromEnum(const UEnum* Val, uint8 TotalNumV);
private:
	UPROPERTY(meta = (BindWidget))
	UDlFmButton* Button_Previous;
	UFUNCTION()
	void OnPress_Previous();
	UPROPERTY(meta = (BindWidget))
	UDlFmButton* Button_Next;
	UFUNCTION()
	void OnPress_Next();
	UPROPERTY(meta = (BindWidget))
	UTextBlock* Text_Show;
	 
	UPROPERTY(EditAnywhere, Category="Behaivor")
	TArray<FText> ItemTextList = {FText::FromName(TEXT("Default"))};
};
