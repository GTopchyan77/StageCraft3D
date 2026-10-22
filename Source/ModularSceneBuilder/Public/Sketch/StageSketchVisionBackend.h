// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Sketch/StageSketchVision.h"
#include "UObject/Object.h"
#include "StageSketchVisionBackend.generated.h"

DECLARE_DELEGATE_OneParam(FOnStageSketchVisionFinished, const FStageSketchVisionResult& /*Result*/);

/**
 * An online reader of stage drawings (Docs/ADR/0017-multilingual-voice-and-photo-import.md §2.7). It only describes the drawing:
 * UStageSketchSubsystem validates the answer and adds it to the sketch, where the user checks it before generating. One request
 * in flight at a time; owned by the subsystem, which cancels it in Deinitialize. Game thread only.
 */
UCLASS(Abstract)
class MODULARSCENEBUILDER_API UStageSketchVisionBackend : public UObject
{
	GENERATED_BODY()

public:
	/** Whether a drawing can be sent now; OutReason says why not ("turned off", "no API key"). */
	virtual bool IsAvailable(FText& OutReason) const PURE_VIRTUAL(UStageSketchVisionBackend::IsAvailable, return false;);

	/** User-facing name ("Claude (claude-opus-5-5)"). */
	virtual FText GetDisplayName() const PURE_VIRTUAL(UStageSketchVisionBackend::GetDisplayName, return FText::GetEmpty(););

	/** Sends the JPEG to be read. OnFinished runs exactly once, on the game thread, unless cancelled first. False when it cannot start. */
	virtual bool Read(const TArray<uint8>& Jpeg, FOnStageSketchVisionFinished OnFinished) PURE_VIRTUAL(UStageSketchVisionBackend::Read, return false;);

	/** Abandons the request in flight without calling back. Safe when there is none. */
	virtual void Cancel() PURE_VIRTUAL(UStageSketchVisionBackend::Cancel, );
};

/**
 * Claude reading the drawing (Messages API with the image and the strict sketch_plan tool, StageSketchVision). Uses the AI
 * assistant's online model settings (UStageAIAssistantSettings) and the ANTHROPIC_API_KEY environment variable; off unless the
 * online model is turned on there.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageClaudeSketchVisionBackend : public UStageSketchVisionBackend
{
	GENERATED_BODY()

public:
	//~ Begin UStageSketchVisionBackend Interface
	virtual bool IsAvailable(FText& OutReason) const override;
	virtual FText GetDisplayName() const override;
	virtual bool Read(const TArray<uint8>& Jpeg, FOnStageSketchVisionFinished OnFinished) override;
	virtual void Cancel() override;
	//~ End UStageSketchVisionBackend Interface

	//~ Begin UObject Interface
	virtual void BeginDestroy() override;
	//~ End UObject Interface

private:
	/** The request in flight; its completion delegate is unbound on Cancel so it can never call back afterwards. */
	TSharedPtr<class IHttpRequest, ESPMode::ThreadSafe> PendingRequest;
};
