// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "VersionSync/DlFmVersion.h"
#include "DlFmRemapList.generated.h"

class UListView;
class UDlFmButton;
class UDlFmRemapList_OneData;
class UDlFmBorder;
/**
 * 
 */
UCLASS()
class DLFILEMANAGER_API UDlFmRemapList : public UUserWidget
{
	GENERATED_BODY()
public:
	
	UPROPERTY(meta=(BindWidget))
	UListView* List_KeyValue;
	UPROPERTY(meta=(BindWidgetOptional))
	UDlFmButton* Button_ClearSelect;
	UFUNCTION()
	void OnClick_ClearSelect();
	UPROPERTY(meta=(BindWidgetOptional))
	UDlFmButton* Button_TryMatch;
	UFUNCTION()
	void OnClick_TryMatch();
	UPROPERTY(meta=(BindWidgetOptional))
	UDlFmBorder* Border_MatchStatus;
	UPROPERTY(EditAnywhere, Category="Appearance")
	FLinearColor Color_AllMatched;
	UPROPERTY(EditAnywhere, Category="Appearance")
	FLinearColor Color_ExistUnmatched;
	void Refresh_MatchStatus();

	
	FDlFmLiteVersion_Entity Ver_ValueList;
	
	void SetRemap(int32 KeyIndex,int32 ValueIndex);
	const TArray<int32>& GetRemap(){return Remap_Key2Value;}
	TArray<int32> GetReverseMap() const
	{
		TArray<int32> Tmp;
		Tmp.SetNum(GetValueList_Num());
		for (int32 I0=0;I0<Tmp.Num();I0++)
		{
			Tmp[I0]=Remap_Key2Value.Find(I0);
		}
		return Tmp;
	}
	void Refresh_AfterKvListChange();
	
	virtual void TryMatch(){}
	virtual int32 GetKeyList_Num()const{return 0;}
	virtual int32 GetValueList_Num()const{return 0;}
	virtual FString GetKey_ItemName(int32 Index){return FString();}
	virtual FString GetValue_ItemName(int32 Index){return FString();}

	virtual void NativeConstruct() override;
protected:
	UPROPERTY()
	TArray<int32> Remap_Key2Value;

	void SetVisiblePoolNum(int32 Val);
	UPROPERTY()
	TArray<UDlFmRemapList_OneData*> Visible_Pool;
	UPROPERTY()
	TArray<UDlFmRemapList_OneData*> Data_Pool;

	void ForceRefreshAllUISelect();
};
