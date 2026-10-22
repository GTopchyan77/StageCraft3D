// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AI/StageSpeechTypes.h"
#include "Components/ActorComponent.h"
#include "StageVoiceInputComponent.generated.h"

/** Recording started or stopped, transcription started or ended, or the status line changed. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageVoiceInputChanged);

/** A recording was transcribed. The text is untrusted input: it goes into the prompt field like typed text. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageVoiceTranscript, const FString&, Text);

/**
 * Voice prompts for the AI assistant, one local player's tool (Docs/ADR/0017-multilingual-voice-and-photo-import.md §2.5): MIC
 * starts recording from the default microphone, a second press (or the length limit) stops it, and the recording is transcribed
 * by the speech service into text for the assistant's prompt field.
 *
 * State and ownership:
 *  - Idle -> Recording (StartRecording) -> Transcribing (StopRecording, or the limit's timer) -> Idle (the transcript or an error).
 *    Cancel returns to Idle from any state and drops the recording or the request.
 *  - The recorder (microphone stream) and the backend (HTTP request) are owned here and closed in EndPlay; the limit is a timer
 *    with a handle, cleared on every way out of Recording. No tick.
 *  - Off unless the project settings turn voice input on (UStageAIAssistantSettings), and only for a local player controller.
 *    Nothing is recorded before StartRecording, and the microphone closes as soon as recording stops.
 *  - The component never sends a prompt itself: OnTranscriptReady hands the text to the panel, which puts it in the field (or
 *    sends it through AModularPlayerController::RequestAssistantPrompt when the settings say so).
 */
UCLASS(ClassGroup = "StageCraft")
class MODULARSCENEBUILDER_API UStageVoiceInputComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UStageVoiceInputComponent();
	virtual ~UStageVoiceInputComponent() override;

	/** Whether MIC can be used now; OutReason says why not (turned off in the settings, no API key, not a local player). */
	bool IsAvailable(FText& OutReason) const;

	/** Opens the microphone and starts recording. False, with the reason as the status, when it cannot or is not Idle. */
	bool StartRecording();

	/** Stops recording and sends what was heard for transcription. A recording too short or silent is dropped, with a status. */
	void StopRecording();

	/** Drops a recording or a transcription in progress. */
	void Cancel();

	EStageVoiceInputState GetState() const { return State; }

	/** The latest status line ("Listening…", "Heard nothing", an error), empty when there is nothing to say. */
	const FText& GetStatus() const { return Status; }

	/** Fires on every state or status change. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Assistant")
	FOnStageVoiceInputChanged OnVoiceInputChanged;

	/** Fires once per successful transcription. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Assistant")
	FOnStageVoiceTranscript OnTranscriptReady;

protected:
	//~ Begin UActorComponent Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End UActorComponent Interface

private:
	void SetState(EStageVoiceInputState NewState, const FText& NewStatus);
	void HandleTranscribed(const FStageTranscriptionResult& Result);
	bool IsLocalPlayerTool() const;

	EStageVoiceInputState State = EStageVoiceInputState::Idle;
	FText Status;

	/** The open microphone while Recording; reset on every way out of it. */
	TUniquePtr<class FStageVoiceRecorder> Recorder;

	/** Null unless voice input is on in the settings. */
	UPROPERTY(Transient)
	TObjectPtr<class UStageSpeechToTextBackend> Backend = nullptr;

	/** Stops a recording at the length limit. */
	FTimerHandle LimitTimer;
};
