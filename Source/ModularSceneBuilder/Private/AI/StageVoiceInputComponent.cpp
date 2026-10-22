// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/StageVoiceInputComponent.h"

#include "AI/StageAIAssistantSettings.h"
#include "AI/StageSpeechToTextBackend.h"
#include "AI/StageVoiceRecorder.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "ModularSceneBuilder.h"
#include "TimerManager.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageVoiceInputComponent)

#define LOCTEXT_NAMESPACE "StageVoiceInputComponent"

UStageVoiceInputComponent::UStageVoiceInputComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

// Defined here, where FStageVoiceRecorder is complete, so TUniquePtr can delete it.
UStageVoiceInputComponent::~UStageVoiceInputComponent() = default;

bool UStageVoiceInputComponent::IsLocalPlayerTool() const
{
	const APlayerController* Controller = Cast<APlayerController>(GetOwner());
	return Controller && Controller->IsLocalController();
}

void UStageVoiceInputComponent::BeginPlay()
{
	Super::BeginPlay();
	if (GetDefault<UStageAIAssistantSettings>()->UsesVoiceInput() && IsLocalPlayerTool())
	{
		Backend = NewObject<UStageOpenAITranscriptionBackend>(this);
	}
}

void UStageVoiceInputComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Cancel();
	Backend = nullptr;
	OnVoiceInputChanged.Clear();
	OnTranscriptReady.Clear();
	Super::EndPlay(EndPlayReason);
}

bool UStageVoiceInputComponent::IsAvailable(FText& OutReason) const
{
	if (!GetDefault<UStageAIAssistantSettings>()->UsesVoiceInput())
	{
		OutReason = LOCTEXT("Off", "Voice prompts are off. Turn them on in Project Settings › StageCraft AI Assistant › Voice Input.");
		return false;
	}
	if (!Backend)
	{
		OutReason = LOCTEXT("NotLocal", "Voice prompts are only available to the local player.");
		return false;
	}
	return Backend->IsAvailable(OutReason);
}

void UStageVoiceInputComponent::SetState(EStageVoiceInputState NewState, const FText& NewStatus)
{
	State = NewState;
	Status = NewStatus;
	OnVoiceInputChanged.Broadcast();
}

bool UStageVoiceInputComponent::StartRecording()
{
	if (State != EStageVoiceInputState::Idle)
	{
		return false;
	}
	FText Reason;
	if (!IsAvailable(Reason))
	{
		SetState(EStageVoiceInputState::Idle, Reason);
		return false;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	const float MaxSeconds = GetDefault<UStageAIAssistantSettings>()->GetMaxRecordingSeconds();
	TUniquePtr<FStageVoiceRecorder> NewRecorder = MakeUnique<FStageVoiceRecorder>();
	if (!NewRecorder->Start(MaxSeconds, Reason))
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Voice: %s"), *Reason.ToString());
		SetState(EStageVoiceInputState::Idle, Reason);
		return false;
	}
	Recorder = MoveTemp(NewRecorder);
	World->GetTimerManager().SetTimer(LimitTimer, FTimerDelegate::CreateUObject(this, &ThisClass::StopRecording), MaxSeconds, /*bLoop*/ false);
	SetState(EStageVoiceInputState::Recording, FText::Format(LOCTEXT("Listening", "Listening… press STOP when you are done (at most {0} s)."), FText::AsNumber(FMath::RoundToInt(MaxSeconds))));
	return true;
}

void UStageVoiceInputComponent::StopRecording()
{
	if (State != EStageVoiceInputState::Recording || !Recorder.IsValid())
	{
		return;
	}
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(LimitTimer);
	}
	const FStageVoiceClip Clip = Recorder->Stop();
	Recorder.Reset();

	if (Clip.GetSeconds() < StageSpeechAudio::MinSeconds)
	{
		SetState(EStageVoiceInputState::Idle, LOCTEXT("TooShort", "The recording was too short. Press MIC, speak, then press STOP."));
		return;
	}
	const TArray<float> Speech = StageSpeechAudio::Resample(Clip.Samples, Clip.SampleRate, StageSpeechAudio::TranscriptionSampleRate);
	if (StageSpeechAudio::MeasureRms(Speech) < StageSpeechAudio::SilenceRms)
	{
		SetState(EStageVoiceInputState::Idle, LOCTEXT("Silent", "Nothing was heard: the microphone stayed silent. Check the recording device in Windows."));
		return;
	}
	TArray<uint8> Wav = StageSpeechAudio::EncodeWav(Speech, StageSpeechAudio::TranscriptionSampleRate);
	UE_LOG(LogStageCraft, Log, TEXT("Voice: %.1f s recorded, sending %d bytes for transcription."), Clip.GetSeconds(), Wav.Num());
	if (!Backend || !Backend->Transcribe(MoveTemp(Wav), GetDefault<UStageAIAssistantSettings>()->GetTranscriptionOptions(),
		FOnStageTranscriptionFinished::CreateUObject(this, &ThisClass::HandleTranscribed)))
	{
		SetState(EStageVoiceInputState::Idle, LOCTEXT("NotSent", "The recording could not be sent for transcription."));
		return;
	}
	SetState(EStageVoiceInputState::Transcribing, FText::Format(LOCTEXT("Transcribing", "Transcribing {0} s with {1}…"),
		FText::AsNumber(FMath::RoundToInt(Clip.GetSeconds())), Backend->GetDisplayName()));
}

void UStageVoiceInputComponent::HandleTranscribed(const FStageTranscriptionResult& Result)
{
	if (State != EStageVoiceInputState::Transcribing)
	{
		return;
	}
	if (!Result.bSucceeded)
	{
		SetState(EStageVoiceInputState::Idle, Result.Error);
		return;
	}
	SetState(EStageVoiceInputState::Idle, FText::Format(LOCTEXT("Heard", "Heard: \"{0}\""), FText::FromString(Result.Text)));
	OnTranscriptReady.Broadcast(Result.Text);
}

void UStageVoiceInputComponent::Cancel()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(LimitTimer);
	}
	if (Recorder.IsValid())
	{
		Recorder->Abort();
		Recorder.Reset();
	}
	if (Backend)
	{
		Backend->Cancel();
	}
	if (State != EStageVoiceInputState::Idle)
	{
		SetState(EStageVoiceInputState::Idle, LOCTEXT("Cancelled", "Voice prompt cancelled."));
	}
}

#undef LOCTEXT_NAMESPACE
