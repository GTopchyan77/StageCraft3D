// Copyright NGG. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ISubmixBufferListener.h"

#include <atomic>

class FNGGRecorderWriter;
class UWorld;

/**
 * Taps the main submix and feeds 16 bit stereo PCM to the encoder.
 *
 * OnNewSubmixBuffer runs on the audio render thread, so this does the bare minimum: downmix, convert and
 * enqueue. Timestamps come from a running sample count rather than a clock, which keeps the audio track
 * exactly the right length no matter how the video frames are paced.
 */
class FNGGRecorderAudioCapture : public ISubmixBufferListener
{
public:
	/**
	 * Queries the world's audio device and registers a listener on the main submix.
	 *
	 * @param OutSampleRate  Rate the encoder must be configured with.
	 * @param OutError       Why audio cannot be recorded. The caller should carry on without an audio track.
	 * @return               The listener, or null if audio is unavailable.
	 */
	static TSharedPtr<FNGGRecorderAudioCapture> CreateAndRegister(
		UWorld* World,
		const TSharedPtr<FNGGRecorderWriter>& InWriter,
		int32& OutSampleRate,
		FString& OutError);

	/** Stops feeding the encoder and detaches from the submix. Safe to call more than once. */
	void Unregister();

	//~ ISubmixBufferListener
	virtual void OnNewSubmixBuffer(
		const USoundSubmix* OwningSubmix,
		float* AudioData,
		int32 NumSamples,
		int32 NumChannels,
		const int32 SampleRate,
		double AudioClock) override;

	virtual const FString& GetListenerName() const override;
	//~ End ISubmixBufferListener

	/** The encoder always receives stereo; anything wider is downmixed. */
	static constexpr int32 OutputChannels = 2;

private:
	TSharedPtr<FNGGRecorderWriter> Writer;

	TWeakObjectPtr<UWorld> CapturedWorld;

	int32 ExpectedSampleRate = 48000;

	/** Sample frames handed to the encoder so far; drives the audio timestamps. */
	int64 FramesSubmitted = 0;

	std::atomic<bool> bRunning{ false };
};
