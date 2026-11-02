// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/StageAIToolProvider.h"
#include "StageTestAIToolProvider.generated.h"

/**
 * Automation tests only: an AI tool whose answers the test controls (now, a refused key, or never), and which records what the
 * subsystem handed it, so a test can prove the key travelled from the vault to the tool and nowhere else (ADR 0023 §2, §4).
 *
 * The recorded key is a test key; real keys never reach this class (it is registered only by tests, through
 * UStageAIToolSubsystem::RegisterProviderForDevelopment). Ships unreachable, like UStageMockAIToolProvider. Game thread only.
 */
UCLASS(NotBlueprintable, Transient, HideDropdown)
class UStageTestAIToolProvider : public UStageAIToolProvider
{
	GENERATED_BODY()

public:
	enum class EMode : uint8
	{
		/** Accepts inside the call. */
		AnswerNow,
		/** Refuses the key inside the call. */
		RejectKey,
		/** Keeps the call and never answers on its own (Cancel ends it). */
		Hold,
	};

	static const FName ProviderId;

	//~ Begin UStageAIToolProvider Interface
	virtual FName GetProviderId() const override { return ProviderId; }
	virtual FText GetDisplayName() const override { return FText::FromString(TEXT("Test tool")); }
	virtual EStageAIToolCapability GetCapabilities() const override { return EStageAIToolCapability::AcceptModel; }
	virtual void VerifyKey(const FString& ApiKey, FOnStageAIToolAnswer OnAnswer) override;
	virtual void SubmitModel(const FStageAIToolJob& Job, const FString& ApiKey, FOnStageAIToolAnswer OnAnswer) override;
	virtual void Cancel() override;
	//~ End UStageAIToolProvider Interface

	EMode Mode = EMode::AnswerNow;

	/** What the last call received (test keys only). */
	FString LastKey;
	FStageAIToolJob LastJob;
	int32 SubmitCount = 0;
	int32 VerifyCount = 0;
	int32 CancelCount = 0;

private:
	void Answer(FOnStageAIToolAnswer& OnAnswer);

	/** A held call; cleared by Cancel. */
	FOnStageAIToolAnswer Held;
};
