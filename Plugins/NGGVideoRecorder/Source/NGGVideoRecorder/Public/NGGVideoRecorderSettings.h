// Copyright NGG. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "InputCoreTypes.h"
#include "NGGVideoRecorderTypes.h"
#include "NGGVideoRecorderSettings.generated.h"

/**
 * Project Settings -> Plugins -> NGG Video Recorder.
 *
 * Saved to DefaultGame.ini, so it is baked into packaged builds.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "NGG Video Recorder"))
class NGGVIDEORECORDER_API UNGGVideoRecorderSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UNGGVideoRecorderSettings();

	/** Settings used by StartRecordingWithDefaults, the hotkey and the console commands. */
	UPROPERTY(EditAnywhere, Config, Category = "Defaults", meta = (ShowOnlyInnerProperties))
	FNGGVideoRecorderOptions DefaultOptions;

	/** Let the plugin listen for its global hotkeys (recording and UI panel), with no input bindings needed in the game. */
	UPROPERTY(EditAnywhere, Config, Category = "Hotkey")
	bool bEnableToggleHotkey = true;

	/** Defaults to F9; assigned in the constructor because EKeys is only populated once InputCore starts. */
	UPROPERTY(EditAnywhere, Config, Category = "Hotkey", meta = (EditCondition = "bEnableToggleHotkey"))
	FKey ToggleRecordingKey;

	/** Global key that shows/hides the recorder control panel. Defaults to F10. */
	UPROPERTY(EditAnywhere, Config, Category = "Hotkey", meta = (EditCondition = "bEnableToggleHotkey"))
	FKey ToggleUIKey;

	/**
	 * Show the built-in recorder control panel (record/stop, elapsed time, export) in the bottom-right
	 * corner of the viewport as soon as the game starts. It can always be toggled with ToggleUIKey or
	 * the NGGRecorder.ToggleUI console command.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "UI")
	bool bShowRecorderUIOnStartup = true;

	/**
	 * Collapse the panel while recording so it is not burned into the video. Stop the recording with the
	 * hotkey (F9 by default) and the panel comes back.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "UI")
	bool bHidePanelWhileRecording = false;

	/**
	 * Print a "REC 00:12" overlay while recording.
	 *
	 * Note: the recorder captures the game window as it is presented, so this overlay is burned into the
	 * video as well. Leave it off for deliverables.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Feedback")
	bool bShowOnScreenIndicator = false;

	/** Log a line to the output log when a recording starts and finishes. */
	UPROPERTY(EditAnywhere, Config, Category = "Feedback")
	bool bLogRecordingEvents = true;

	/**
	 * How many captured frames may wait in the encoder queue. Each slot costs Width * Height * 4 bytes
	 * (~8 MB at 1080p). Raise it if you see dropped frames on a machine with a slow encoder.
	 */
	UPROPERTY(EditAnywhere, Config, AdvancedDisplay, Category = "Performance", meta = (ClampMin = "2", ClampMax = "64"))
	int32 MaxQueuedFrames = 8;

	static const UNGGVideoRecorderSettings& Get();
};
