// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.


#include "UI/Fragment/DlFmFilteredEditableText.h"
#include "Engine/World.h"

UDlFmFilteredEditableText::UDlFmFilteredEditableText(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	OnTextCommitted.AddUniqueDynamic(this,&UDlFmFilteredEditableText::OnFilteredTextCommitted_InternalHandle);
}

void UDlFmFilteredEditableText::OnFilteredTextCommitted_InternalHandle(const FText& TextVal, ETextCommit::Type CommitMethodVal)
{
	if (GetWorld()->RealTimeSeconds >= LastTextCommittedTime+ GapTime)
	{
		LastTextCommittedTime = GetWorld()->RealTimeSeconds;
		OnFilteredTextCommitted.Broadcast(TextVal,CommitMethodVal);
	}
}
