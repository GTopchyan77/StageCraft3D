// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Audio/StageAudioReactiveTypes.h"
#include "Engine/TimerHandle.h"
#include "Subsystems/WorldSubsystem.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"
#include "StageAudioReactiveSubsystem.generated.h"

DECLARE_MULTICAST_DELEGATE(FOnStageAudioStatusChanged);

/**
 * Audio-reactive lighting's audio side for one world (Docs/ADR/0029-audio-analysis-and-audio-reactive-effects.md §3, §5): the
 * live source (File, Input, SystemAudio or TestSignal; exactly one), the tap every source writes into, the analyser thread, and
 * the file player. Owns them all; tears them down in order (source, analyser, tap) on every change and on Deinitialize.
 *
 * Readers call GetFeatures (one copy of the newest analysed frame, released when the audience hears it) and GetStatus.
 * Writers are AModularPlayerController's audio requests, judged by the GameMode; the dev console uses those requests too.
 *
 * Event-driven, no tick: sources that open on their own thread (a file, system audio) report back through the game-thread task
 * queue. While a source is starting or running, one looping timer (2 Hz) watches for a device that went away or a file that
 * ended, and tells OnAudioStatusChanged listeners. The timer does not exist while the source is Off or Failed.
 *
 * Privacy (ADR 0029 §6): captured audio is analysed in memory only, never recorded, saved, logged or sent. Capture stops on
 * every source change and when the stage closes.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageAudioReactiveSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	UStageAudioReactiveSubsystem();
	virtual ~UStageAudioReactiveSubsystem() override;

	//~ Begin USubsystem / UWorldSubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	//~ End USubsystem / UWorldSubsystem Interface

	/**
	 * Switches to Source (Off stops). File is refused here: OpenFile chooses the file. The choice (except File) is remembered on
	 * this PC and reopened with the next stage. False, with OutError, when the source cannot start; the state is then Failed.
	 * Input starts at once; SystemAudio is Starting until its device opens.
	 */
	bool SetSource(EStageAudioSource Source, FText& OutError);

	/** Switches to the File source and opens Path (Starting until the decoder has opened it; it then plays). */
	bool OpenFile(const FString& Path, FText& OutError);

	/** Play, pause, stop, seek, loop on the open file. False, with OutError, when no file is open or the command is invalid. */
	bool Transport(const FStageAudioTransportRequest& Request, FText& OutError);

	/** Replaces this PC's settings (sanitized, saved). A running source picks up the new timing, bands and input device. */
	bool ApplySettings(const FStageAudioReactiveSettings& Settings, FText& OutError);

	FStageAudioReactiveSettings GetSettings() const;

	EStageAudioSource GetSource() const { return Source; }
	EStageAudioSourceState GetState() const { return State; }

	/** The newest frame the audience has heard; a frame without signal while nothing runs. */
	FStageAudioFeatures GetFeatures();

	FStageAudioStatus GetStatus();

	/** State changes at once; counters and the file position at most every StatusIntervalSeconds while a source runs. */
	FOnStageAudioStatusChanged OnAudioStatusChanged;

	static constexpr float StatusIntervalSeconds = 0.5f;

#if WITH_DEV_AUTOMATION_TESTS
	/** Tests: read and write Settings instead of this PC's saved settings, and never save. Null restores the default. */
	void UseSettingsForTests(class UStageCraftUserSettings* Settings);
#endif

protected:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

private:
	class UStageCraftUserSettings* GetUserSettings() const;
	void SaveSettings(const FStageAudioReactiveSettings& Settings);
	void RememberSource(EStageAudioSource Remembered);

	/** Stops the source, the analyser and the file player, in that order; forgets callbacks still in flight. Idempotent. */
	void StopAll();

	bool StartAnalyzer(FText& OutError);
	bool StartTestSignal(FText& OutError);
	bool StartInput(FText& OutError);
	bool StartSystemAudio(FText& OutError);

	/** Opens the current file at StartSeconds; playback starts paused when bPaused. */
	bool StartFile(double StartSeconds, bool bPaused, FText& OutError);
	void HandleFileOpened(uint32 ForGeneration, bool bOk, const FText& Error, const struct FStageAudioFileInfo& Info);
	void HandleSystemAudioOpened(uint32 ForGeneration, bool bOk, const FText& Error, const FString& Description);

	double GetFilePosition() const;

	void SetState(EStageAudioSourceState NewState, const FText& Error = FText::GetEmpty());
	void Fail(const FText& Error);
	void HandleStatusTimer();

	UPROPERTY(Transient)
	TObjectPtr<class UAudioComponent> FileComponent = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageAudioFileSound> FileSound = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageCraftUserSettings> TestSettings = nullptr;

	/** Used when there are no user settings (commandlets): sources work but are not remembered. */
	FStageAudioReactiveSettings FallbackSettings;

	EStageAudioSource Source = EStageAudioSource::Off;
	EStageAudioSourceState State = EStageAudioSourceState::Off;
	FText LastError;
	FString Description;
	int32 SampleRate = 0;
	int32 Channels = 0;

	/** Bumped on every stop: a source that reports back for an older generation is ignored. */
	uint32 Generation = 0;

	TSharedPtr<class FStageAudioTap> Tap;
	TUniquePtr<class FStageAudioAnalyzer> Analyzer;
	TSharedPtr<class FStageAudioFileDecoder> Decoder;
	TUniquePtr<class FStageLoopbackCapture> Loopback;
	TUniquePtr<class FStageAudioInputCapture> Input;

	/** File transport. */
	FString FilePath;
	double FileStartSeconds = 0.0;
	double FileDuration = 0.0;
	bool bFilePaused = false;
	bool bFileEnded = false;
	/** Seeking restarts the decoder; the new player starts paused when the old one was. */
	bool bStartPaused = false;

	FTimerHandle StatusTimer;
};
