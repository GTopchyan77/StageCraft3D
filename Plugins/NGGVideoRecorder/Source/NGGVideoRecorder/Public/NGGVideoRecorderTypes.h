// Copyright NGG. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NGGVideoRecorderTypes.generated.h"

/** Output resolution of the recorded video. */
UENUM(BlueprintType)
enum class ENGGRecorderResolution : uint8
{
	/** Record at whatever size the game window currently is (rounded down to an even number of pixels). */
	MatchViewport	UMETA(DisplayName = "Match Viewport"),
	HD720			UMETA(DisplayName = "720p  (1280 x 720)"),
	HD1080			UMETA(DisplayName = "1080p (1920 x 1080)"),
	QHD1440			UMETA(DisplayName = "1440p (2560 x 1440)"),
	UHD2160			UMETA(DisplayName = "4K    (3840 x 2160)"),
	Custom			UMETA(DisplayName = "Custom"),
};

/** Video bitrate preset. Bitrate scales with resolution and frame rate, so these stay sensible at any size. */
UENUM(BlueprintType)
enum class ENGGRecorderQuality : uint8
{
	/** Small files, visible compression on fast motion. */
	Low				UMETA(DisplayName = "Low"),
	/** Good default for screen-share and review clips. */
	Medium			UMETA(DisplayName = "Medium"),
	/** Recommended for client deliverables. */
	High			UMETA(DisplayName = "High"),
	/** Near-transparent, large files. */
	VeryHigh		UMETA(DisplayName = "Very High"),
	/** Use CustomBitrateMbps instead of a preset. */
	Custom			UMETA(DisplayName = "Custom Bitrate"),
};

/**
 * Everything that describes one recording. Passed to UNGGVideoRecorderSubsystem::StartRecording.
 * Defaults here match the "sensible clip" case; project-wide defaults live in Project Settings.
 */
USTRUCT(BlueprintType)
struct NGGVIDEORECORDER_API FNGGVideoRecorderOptions
{
	GENERATED_BODY()

	/** Output frame rate of the MP4. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Video", meta = (ClampMin = "1", ClampMax = "240"))
	int32 FrameRate = 60;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Video")
	ENGGRecorderResolution Resolution = ENGGRecorderResolution::MatchViewport;

	/** Only used when Resolution is Custom. Both values are rounded down to an even number. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Video",
		meta = (EditCondition = "Resolution == ENGGRecorderResolution::Custom", ClampMin = "16"))
	FIntPoint CustomResolution = FIntPoint(1920, 1080);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Video")
	ENGGRecorderQuality Quality = ENGGRecorderQuality::High;

	/** Only used when Quality is Custom. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Video",
		meta = (EditCondition = "Quality == ENGGRecorderQuality::Custom", ClampMin = "0.5", ClampMax = "200"))
	float CustomBitrateMbps = 20.0f;

	/**
	 * When the source and output aspect ratios differ, letterbox with black bars instead of stretching.
	 * Only has an effect when Resolution is not Match Viewport.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Video")
	bool bPreserveAspectRatio = true;

	/** Record the game audio (main submix) into the MP4 as AAC. Ignored when bLockToFixedFrameRate is on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Audio")
	bool bCaptureAudio = true;

	/**
	 * Run the simulation at a fixed time step of exactly 1/FrameRate per rendered frame.
	 *
	 * The game will no longer run in real time - it advances one output frame per rendered frame - but the
	 * resulting video is perfectly smooth with no dropped or duplicated frames even if the scene is too heavy
	 * to render at FrameRate. This is what you want for a client deliverable of a heavy simulation.
	 * Audio capture is disabled in this mode because the audio engine still runs in real time.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capture")
	bool bLockToFixedFrameRate = false;

	/**
	 * Directory the MP4 is written to. Relative paths resolve against the project directory.
	 * Leave empty for <Project>/Saved/VideoCaptures.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	FString OutputDirectory;

	/**
	 * File name without extension. Supports the tokens {project} {map} {date} {time}.
	 * Leave empty for "{project}_{map}_{date}_{time}". A numeric suffix is appended if the file already exists.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	FString FileName;
};

/** Live status of the recorder, for HUDs and logging. */
USTRUCT(BlueprintType)
struct NGGVIDEORECORDER_API FNGGVideoRecorderStats
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "NGG Video Recorder")
	bool bIsRecording = false;

	/** Seconds of video captured so far. */
	UPROPERTY(BlueprintReadOnly, Category = "NGG Video Recorder")
	float ElapsedSeconds = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "NGG Video Recorder")
	int32 FramesCaptured = 0;

	/** Frames the GPU produced but the encoder could not keep up with. Should stay at 0. */
	UPROPERTY(BlueprintReadOnly, Category = "NGG Video Recorder")
	int32 FramesDropped = 0;

	UPROPERTY(BlueprintReadOnly, Category = "NGG Video Recorder")
	FIntPoint Resolution = FIntPoint::ZeroValue;

	UPROPERTY(BlueprintReadOnly, Category = "NGG Video Recorder")
	int32 FrameRate = 0;

	/** Absolute path of the file being written, or of the last finished recording. */
	UPROPERTY(BlueprintReadOnly, Category = "NGG Video Recorder")
	FString FilePath;
};
