// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AI/StageAIAssistantBackend.h"
#include "StageAITestBackend.generated.h"

/**
 * Automation tests only: an online interpreter whose answer the test scripts (Docs/ADR/0030 §10, StageCraft.Assistant.OnlinePlan).
 * It answers inside Interpret, with NextResponse, exactly like a backend whose HTTP request completed at once; no network.
 * Installed with UStageAICommandSubsystem::SetBackendForTesting. Ships as an unreachable class (UHT cannot compile a UCLASS out),
 * like UStageTestCommerceBackend. Game thread only.
 */
UCLASS(NotBlueprintable, Transient, HideDropdown)
class UStageAITestBackend : public UStageAIAssistantBackend
{
	GENERATED_BODY()

public:
	/** What the next Interpret answers. */
	FStageAIModelResponse NextResponse;

	/** Prompts received, oldest first. */
	TArray<FString> Prompts;

	//~ Begin UStageAIAssistantBackend Interface
	virtual bool IsAvailable(FText& OutReason) const override { return true; }
	virtual FText GetDisplayName() const override { return INVTEXT("Test backend"); }
	virtual bool Interpret(const FStageAIBackendRequest& Request, FOnStageAIBackendFinished OnFinished) override
	{
		Prompts.Add(Request.Prompt);
		OnFinished.ExecuteIfBound(NextResponse);
		return true;
	}
	virtual void Cancel() override {}
	//~ End UStageAIAssistantBackend Interface
};
