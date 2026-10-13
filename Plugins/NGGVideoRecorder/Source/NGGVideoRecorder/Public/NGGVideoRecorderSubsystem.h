// Copyright NGG. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "NGGVideoRecorderTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "NGGVideoRecorderSubsystem.generated.h"

class APlayerController;
class FNGGRecorderFrameCapture;
class FNGGRecorderHotkeyProcessor;
class SWidget;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNGGRecordingStarted, const FString&, FilePath);

/**
 * Fired synchronously the instant a start is requested, before any capture setup — the actual capture
 * hook is only installed one engine tick later (see StartRecording), so this is the one safe place to
 * hide UI that must never appear in the recording. Hide it here; a listener that instead waits for
 * OnRecordingStarted is too late, since the capture hook is already live by then.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNGGRecordingWillStart, const FString&, FilePath);

/**
 * @param bSuccess    True when the MP4 was finalised and is playable.
 * @param FilePath    Absolute path of the file. Still set on failure so you can clean up a partial file.
 * @param Error       Empty on success, otherwise a human readable reason.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FNGGRecordingFinished, bool, bSuccess, const FString&, FilePath, const FString&, Error);

/**
 * Records the game viewport to an MP4 file.
 *
 * Grab it from Blueprint with "Get Game Instance Subsystem" -> NGG Video Recorder Subsystem, or from C++ with
 * UGameInstance::GetSubsystem<UNGGVideoRecorderSubsystem>(). It lives on the game instance, so a recording keeps
 * running across level transitions and is finalised automatically when the game shuts down.
 *
 * Also available without any Blueprint work:
 *   - the toggle hotkey configured in Project Settings (F9 by default)
 *   - console commands NGGRecorder.Start / .Stop / .Toggle / .Status
 */
UCLASS(DisplayName = "NGG Video Recorder")
class NGGVIDEORECORDER_API UNGGVideoRecorderSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ USubsystem
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem

	/**
	 * Begin recording the game viewport to an MP4.
	 *
	 * @param Options       Resolution, frame rate, quality and output path for this recording.
	 * @param OutFilePath   Absolute path of the file being written.
	 * @param OutError      Reason the recording could not start.
	 * @return              True if recording started.
	 */
	UFUNCTION(BlueprintCallable, Category = "NGG Video Recorder",
		meta = (AutoCreateRefTerm = "Options", DisplayName = "Start Recording"))
	bool StartRecording(const FNGGVideoRecorderOptions& Options, FString& OutFilePath, FString& OutError);

	/** Begin recording using the settings from Project Settings -> Plugins -> NGG Video Recorder. */
	UFUNCTION(BlueprintCallable, Category = "NGG Video Recorder", meta = (DisplayName = "Start Recording (Project Defaults)"))
	bool StartRecordingWithDefaults(FString& OutFilePath, FString& OutError);

	/**
	 * Finish the recording and flush the MP4 to disk.
	 *
	 * This blocks until the encoder has drained its queue, usually a few hundred milliseconds.
	 * OnRecordingFinished fires before this returns.
	 */
	UFUNCTION(BlueprintCallable, Category = "NGG Video Recorder")
	void StopRecording();

	/** Start if idle, stop if recording. Returns true if a recording is running afterwards. */
	UFUNCTION(BlueprintCallable, Category = "NGG Video Recorder")
	bool ToggleRecording();

	UFUNCTION(BlueprintPure, Category = "NGG Video Recorder")
	bool IsRecording() const;

	/** Seconds of video captured so far, 0 when idle. */
	UFUNCTION(BlueprintPure, Category = "NGG Video Recorder")
	float GetRecordingDuration() const;

	/** The recording duration formatted as MM:SS, for HUDs. "00:00" when idle. */
	UFUNCTION(BlueprintPure, Category = "NGG Video Recorder")
	FString GetRecordingTimecode() const;

	/** Absolute path of the current or most recent recording. */
	UFUNCTION(BlueprintPure, Category = "NGG Video Recorder")
	FString GetLastRecordingPath() const;

	/** Copy-free version of GetLastRecordingPath for C++ callers that poll every frame. */
	const FString& GetLastRecordingPathRef() const { return CurrentFilePath; }

	UFUNCTION(BlueprintPure, Category = "NGG Video Recorder")
	FNGGVideoRecorderStats GetStats() const;

	/** Opens the folder containing the last recording in Windows Explorer. Editor and development builds only. */
	UFUNCTION(BlueprintCallable, Category = "NGG Video Recorder")
	void RevealLastRecordingInExplorer() const;

	/**
	 * Show the built-in recorder control panel in the bottom-right corner of the game viewport.
	 *
	 * Shown automatically on startup when bShowRecorderUIOnStartup is set (the default), and toggled by
	 * the ToggleUIKey hotkey (F10 by default) and the NGGRecorder.ToggleUI console command. If the mouse
	 * cursor is hidden the panel makes it visible, and restores it when the panel is closed.
	 */
	UFUNCTION(BlueprintCallable, Category = "NGG Video Recorder|UI")
	void ShowRecorderUI();

	/** Remove the recorder control panel from the viewport. */
	UFUNCTION(BlueprintCallable, Category = "NGG Video Recorder|UI")
	void HideRecorderUI();

	/** Show if hidden, hide if shown. Returns true if the panel is visible afterwards. */
	UFUNCTION(BlueprintCallable, Category = "NGG Video Recorder|UI")
	bool ToggleRecorderUI();

	UFUNCTION(BlueprintPure, Category = "NGG Video Recorder|UI")
	bool IsRecorderUIVisible() const;

	UPROPERTY(BlueprintAssignable, Category = "NGG Video Recorder")
	FNGGRecordingWillStart OnRecordingWillStart;

	UPROPERTY(BlueprintAssignable, Category = "NGG Video Recorder")
	FNGGRecordingStarted OnRecordingStarted;

	UPROPERTY(BlueprintAssignable, Category = "NGG Video Recorder")
	FNGGRecordingFinished OnRecordingFinished;

private:
	/** Resolves tokens, makes the directory and returns a path that does not collide with an existing file. */
	FString BuildOutputPath(const FNGGVideoRecorderOptions& Options) const;

	/** Applies/restores FApp fixed time step for bLockToFixedFrameRate. */
	void ApplyFixedTimeStep(int32 FrameRate);
	void RestoreFixedTimeStep();

	bool TickIndicator(float DeltaTime);

	/**
	 * Runs one tick after StartRecording broadcasts OnRecordingWillStart — installs the actual capture
	 * hook only now, so anything that hid itself in response to OnRecordingWillStart has already had a
	 * full render+present cycle to do so before the first frame can possibly be captured.
	 */
	bool TickDeferredStart(float DeltaTime);

	/**
	 * Waits until the game viewport and local player exist, then shows the startup panel once.
	 * A ticker because no engine delegate covers PIE: PostLoadMapWithWorld only fires for real
	 * LoadMap calls, and PIE duplicates the editor world instead of loading one.
	 */
	bool TickAutoShowUI(float DeltaTime);

	/** Re-adds the panel after a map load wiped the viewport's widgets. */
	void HandlePostLoadMap(UWorld* LoadedWorld);

	void FinishRecording(bool bSuccess, const FString& Error);

	TSharedPtr<FNGGRecorderFrameCapture> Capture;
	TSharedPtr<FNGGRecorderHotkeyProcessor> HotkeyProcessor;

	TSharedPtr<SWidget> PanelWidget;

	/** Whether the panel should be on screen — survives map loads, which destroy the actual widget. */
	bool bWantsRecorderUI = false;

	/** The controller whose cursor the panel turned on, so hiding the panel can turn it back off. */
	TWeakObjectPtr<APlayerController> CursorForcedPC;

	FTSTicker::FDelegateHandle IndicatorTickHandle;
	FTSTicker::FDelegateHandle AutoShowUITickHandle;
	FTSTicker::FDelegateHandle DeferredStartTickHandle;
	FDelegateHandle PostLoadMapHandle;

	/** True from OnRecordingWillStart's broadcast until TickDeferredStart actually installs the capture hook. */
	bool bStartPending = false;
	FNGGVideoRecorderOptions PendingOptions;
	FString PendingFilePath;

	/**
	 * GFrameCounter at the moment OnRecordingWillStart was broadcast. FTSTicker's "next tick" delay of
	 * 0.0f only means "as soon as possible" — if the ticker is added before the core ticker's own dispatch
	 * point runs THIS frame, it can fire again in this same frame, not the next one. TickDeferredStart
	 * checks GFrameCounter against this and keeps re-arming itself until a real frame boundary has
	 * actually passed, which is the one guarantee OnRecordingWillStart's listeners are relying on.
	 */
	uint64 PendingRequestFrame = 0;

	FString CurrentFilePath;
	double RecordingStartTime = 0.0;

	/** Snapshot taken when a recording ends, so GetStats still reports something useful afterwards. */
	FNGGVideoRecorderStats CachedStats;

	bool bFixedTimeStepApplied = false;
	bool bPreviousUseFixedTimeStep = false;
	double PreviousFixedDeltaTime = 0.0;
};
