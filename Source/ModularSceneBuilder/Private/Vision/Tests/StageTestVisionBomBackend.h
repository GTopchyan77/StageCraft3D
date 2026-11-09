// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Vision/StageVisionBomBackend.h"
#include "StageTestVisionBomBackend.generated.h"

/**
 * Automation tests only: a Vision Inspector backend that answers with a response body the test sets, through the same parser as the
 * real one (StageVisionBom::ParseResponseBody), now or when the test says. Never configured outside tests; it ships as an
 * unreachable class (UHT cannot compile a UCLASS out), like UStageTestCommerceBackend. Game thread only.
 */
UCLASS(NotBlueprintable, Transient, HideDropdown)
class UStageTestVisionBomBackend : public UStageVisionBomBackend
{
	GENERATED_BODY()

public:
	//~ Begin UStageVisionBomBackend Interface
	virtual bool IsAvailable(FText& OutReason) const override;
	virtual FText GetDisplayName() const override { return FText::FromString(TEXT("Test reader")); }
	virtual bool Read(const TArray<uint8>& Jpeg, FOnStageVisionBomRead OnRead) override;
	virtual void Cancel() override { Held.Unbind(); }
	//~ End UStageVisionBomBackend Interface

	/** Answers the held request with the current HttpStatus and Body. False when none is held. */
	bool AnswerHeld();

	bool bAvailable = true;
	/** Answer inside Read instead of holding the request. */
	bool bAnswerNow = true;
	int32 HttpStatus = 200;
	FString Body;
	int32 Reads = 0;

private:
	FOnStageVisionBomRead Held;
};
