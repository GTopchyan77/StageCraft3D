// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/StageAIToolProvider.h"
#include "StageMockAIToolProvider.generated.h"

/**
 * An offline stand-in for an external AI tool, used by tests and scripted runs (ADR 0023 §4). It accepts keys that start with
 * "mock-" and "receives" a model by copying it into GetReceivedDirectory(). Answers synchronously. Never configured by default:
 * it is registered only by tests and by the development command StageCraft.AITools.Mock.
 */
UCLASS()
class UStageMockAIToolProvider : public UStageAIToolProvider
{
	GENERATED_BODY()

public:
	static const FName ProviderId;
	static constexpr const TCHAR* AcceptedKeyPrefix = TEXT("mock-");

	static FString GetReceivedDirectory();

	virtual FName GetProviderId() const override { return ProviderId; }
	virtual FText GetDisplayName() const override;
	virtual EStageAIToolCapability GetCapabilities() const override { return EStageAIToolCapability::AcceptModel; }
	virtual void VerifyKey(const FString& ApiKey, FOnStageAIToolAnswer OnAnswer) override;
	virtual void SubmitModel(const FStageAIToolJob& Job, const FString& ApiKey, FOnStageAIToolAnswer OnAnswer) override;
};
