// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Vision/StageVisionBom.h"
#include "StageVisionBomBackend.generated.h"

DECLARE_DELEGATE_OneParam(FOnStageVisionBomRead, const FStageVisionBomResult& /*Result*/);

/**
 * An online reader of stage photos for the Vision Inspector (Docs/ADR/0032 §8). It only lists what it sees: UStageVisionBomSubsystem
 * matches the answer to the catalog and the user checks it before anything is built. One request in flight at a time; owned by the
 * subsystem, which cancels it in Deinitialize. Game thread only.
 */
UCLASS(Abstract)
class MODULARSCENEBUILDER_API UStageVisionBomBackend : public UObject
{
	GENERATED_BODY()

public:
	/** Whether a photo can be sent now; OutReason says why not ("turned off", "no API key"). */
	virtual bool IsAvailable(FText& OutReason) const PURE_VIRTUAL(UStageVisionBomBackend::IsAvailable, return false;);

	/** User-facing name ("Claude (claude-opus-5-5)"). */
	virtual FText GetDisplayName() const PURE_VIRTUAL(UStageVisionBomBackend::GetDisplayName, return FText::GetEmpty(););

	/** Sends the JPEG to be read. OnRead runs exactly once, on the game thread, unless cancelled first. False when it cannot start. */
	virtual bool Read(const TArray<uint8>& Jpeg, FOnStageVisionBomRead OnRead) PURE_VIRTUAL(UStageVisionBomBackend::Read, return false;);

	/** Abandons the request in flight without calling back. Safe when there is none. */
	virtual void Cancel() PURE_VIRTUAL(UStageVisionBomBackend::Cancel, );
};

/**
 * Claude reading the photo (Messages API with the image and the strict stage_inventory tool, StageVisionBom). Uses the AI assistant's
 * online model settings (UStageAIAssistantSettings) and API key; off unless the online model is turned on there. The photo goes
 * from this machine to Anthropic only, never through StageCraft Cloud.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageClaudeVisionBomBackend : public UStageVisionBomBackend
{
	GENERATED_BODY()

public:
	//~ Begin UStageVisionBomBackend Interface
	virtual bool IsAvailable(FText& OutReason) const override;
	virtual FText GetDisplayName() const override;
	virtual bool Read(const TArray<uint8>& Jpeg, FOnStageVisionBomRead OnRead) override;
	virtual void Cancel() override;
	//~ End UStageVisionBomBackend Interface

	//~ Begin UObject Interface
	virtual void BeginDestroy() override;
	//~ End UObject Interface

private:
	/** The request in flight; its completion delegate is unbound on Cancel so it can never call back afterwards. */
	TSharedPtr<class IHttpRequest, ESPMode::ThreadSafe> PendingRequest;
};
