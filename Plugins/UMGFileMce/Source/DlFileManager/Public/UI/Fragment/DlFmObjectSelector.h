// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ComboBoxString.h"
#include "DlFmObjectSelector.generated.h"

/**
 * 
 */
UENUM()
enum class EDlFmObjectSelector_Range:uint8
{
	ActorInWorld,
	AllObject,
	ObjectInWorld,
};

UENUM()
enum class EDlFmObjectSelector_NameType:uint8
{
	ObjectName,
	ObjectPathName,
	ActorLabel,
};
UCLASS()
class DLFILEMANAGER_API UDlFmObjectSelector : public UComboBoxString
{
	GENERATED_BODY()
public:
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSelectedObjectChangedEvent, UObject*, SelectedItem, ESelectInfo::Type, SelectionType);

	UPROPERTY(EditAnywhere, Category="Behavior")
	EDlFmObjectSelector_Range Range = EDlFmObjectSelector_Range::ActorInWorld;
	UPROPERTY(EditAnywhere, Category="Behavior")
	UClass* FilterClass = AActor::StaticClass();
	UPROPERTY(EditAnywhere, Category="Behavior")
	EDlFmObjectSelector_NameType NameType=EDlFmObjectSelector_NameType::ObjectName;
	//bool bTryGetLabel=false;
	UPROPERTY(EditAnywhere, Category="Behavior")
	bool bSortOptions=true;

	FOnSelectedObjectChangedEvent OnSelectedObjectChangedEvent;

	virtual void HandleOpening() override;
	virtual void HandleSelectionChanged(TSharedPtr<FString> Item, ESelectInfo::Type SelectionType) override;

	template<typename ReturnType>
	ReturnType* GetSelectedObject() const {return Cast<ReturnType>(Cache_SelectedObject);}
	UObject* GetSelectedObject() const {return Cache_SelectedObject;}

	void Refresh_SelectString();
	FString GetShowNameFromObj(UObject* Val) const;
	void SetSelectedObject(UObject* Val, bool bBroadcast);
private:
	bool bDlFmAction_HandleSelectionChanged=true;
	void SetSelectedObject_Pri(UObject* Val,ESelectInfo::Type SelectionType = ESelectInfo::Type::Direct);

	void SetOrCreateSelectedOption(const FString& Option);
	UPROPERTY()
	UObject* Cache_SelectedObject;

	TArray<FString> Cache_OptionString;
};


