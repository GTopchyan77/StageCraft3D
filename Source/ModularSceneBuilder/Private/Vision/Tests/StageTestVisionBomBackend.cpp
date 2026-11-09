// Copyright Epic Games, Inc. All Rights Reserved.

#include "Vision/Tests/StageTestVisionBomBackend.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageTestVisionBomBackend)

bool UStageTestVisionBomBackend::IsAvailable(FText& OutReason) const
{
	if (!bAvailable)
	{
		OutReason = FText::FromString(TEXT("Test reader is off."));
	}
	return bAvailable;
}

bool UStageTestVisionBomBackend::Read(const TArray<uint8>& Jpeg, FOnStageVisionBomRead OnRead)
{
	if (!bAvailable || Jpeg.IsEmpty() || Held.IsBound())
	{
		return false;
	}
	++Reads;
	Held = MoveTemp(OnRead);
	if (bAnswerNow)
	{
		AnswerHeld();
	}
	return true;
}

bool UStageTestVisionBomBackend::AnswerHeld()
{
	if (!Held.IsBound())
	{
		return false;
	}
	FOnStageVisionBomRead Callback = MoveTemp(Held);
	Held.Unbind();
	Callback.ExecuteIfBound(StageVisionBom::ParseResponseBody(HttpStatus, Body));
	return true;
}
