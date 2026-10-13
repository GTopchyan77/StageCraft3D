// Copyright NGG. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Everything the encoder needs to open the output file. Fixed for the lifetime of one recording. */
struct FNGGEncoderConfig
{
	/** Absolute path of the .mp4 to write. The containing directory must already exist. */
	FString FilePath;

	/** Output frame size in pixels. Both values must be even (H.264 requirement). */
	int32 Width = 0;
	int32 Height = 0;

	/** Nominal frame rate written into the container. */
	int32 FrameRate = 60;

	/** Target video bitrate in bits per second. */
	int32 VideoBitrate = 20 * 1000 * 1000;

	/** When false the file has no audio track and WriteAudio is never called. */
	bool bHasAudio = false;

	/** Media Foundation's AAC encoder only accepts 44100 or 48000 Hz, 1 or 2 channels. */
	int32 AudioSampleRate = 48000;
	int32 AudioChannels = 2;
	int32 AudioBitrate = 192000;
};

/**
 * Sink that turns raw BGRA frames and PCM audio into a finished video file.
 *
 * All calls happen on the encoder's own worker thread, never on the game or render thread.
 */
class FNGGVideoEncoder
{
public:
	virtual ~FNGGVideoEncoder() = default;

	/** Open the file and configure the streams. On failure nothing needs to be released by the caller. */
	virtual bool Initialize(const FNGGEncoderConfig& Config, FString& OutError) = 0;

	/**
	 * @param BGRAData    Width * Height * 4 bytes, top-down, tightly packed.
	 * @param TimeNs      Presentation time in nanoseconds from the start of the recording. Must increase.
	 * @param DurationNs  How long this frame is shown.
	 */
	virtual bool WriteVideoFrame(const uint8* BGRAData, int64 TimeNs, int64 DurationNs, FString& OutError) = 0;

	/**
	 * @param Interleaved  NumFrames * AudioChannels signed 16 bit samples.
	 * @param NumFrames    Sample frames, not individual samples.
	 * @param TimeNs       Presentation time in nanoseconds from the start of the recording.
	 */
	virtual bool WriteAudio(const int16* Interleaved, int32 NumFrames, int64 TimeNs, FString& OutError) = 0;

	/** Write the container index and close the file. The encoder is unusable afterwards. */
	virtual bool Finalize(FString& OutError) = 0;
};

/** Returns the platform encoder, or nullptr with OutError set if this platform cannot write MP4 files. */
TUniquePtr<FNGGVideoEncoder> CreateNGGVideoEncoder(FString& OutError);
