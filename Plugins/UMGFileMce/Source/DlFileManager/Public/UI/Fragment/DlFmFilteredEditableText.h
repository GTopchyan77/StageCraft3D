// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/EditableText.h"
#include "DlFmFilteredEditableText.generated.h"

/**
 * 
 */
UCLASS()
class DLFILEMANAGER_API UDlFmFilteredEditableText : public UEditableText
{
	GENERATED_UCLASS_BODY()
public:

	/** Called whenever the text is committed.  This happens when the user presses enter or the text box loses focus. */
	UPROPERTY(BlueprintAssignable, Category="Widget Event", meta=(DisplayName="OnFilteredTextCommitted (Editable Text)"))
	FOnEditableTextCommittedEvent OnFilteredTextCommitted;
	UPROPERTY(EditAnywhere, Category = "Filtered")
	float GapTime = 0.1;
 
private:
	float LastTextCommittedTime=0;
	UFUNCTION()
	void OnFilteredTextCommitted_InternalHandle(const FText& TextVal,ETextCommit::Type CommitMethodVal);
};
