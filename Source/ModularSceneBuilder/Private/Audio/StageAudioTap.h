// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include <atomic>

/**
 * Where every audio source hands its samples to the analyser (Docs/ADR/0029-audio-analysis-and-audio-reactive-effects.md §3):
 * a single-producer, single-consumer ring of mono samples plus a small ring of block markers that place each block on the clock.
 *
 * Threads: Write is called by exactly one producer at a time (the audio render thread, a capture thread or the analyser's own test
 * generator); Read by exactly one consumer (the analyser thread). Neither takes a lock, allocates or touches a UObject, so Write is
 * safe on the audio render thread. Init and Reset run only while no producer and no consumer are active (the owner stops the
 * source and the analyser first).
 *
 * Overrun: the producer never waits. When the consumer falls a whole ring behind, the oldest samples are lost: Read notices the
 * lap (before and after copying, as a sequence lock would) and skips to the oldest sample still intact, counting what it lost.
 */
class FStageAudioTap
{
public:
	/** Capacity in samples (rounded up to a power of two) and the source's sample rate. Allocates; nothing else does. */
	void Init(int32 CapacitySamples, int32 InSampleRate);

	/** Empties both rings. Only while nobody writes or reads. */
	void Reset();

	int32 GetSampleRate() const { return SampleRate; }
	int32 GetCapacity() const { return Samples.Num(); }

	/**
	 * Producer: down-mixes Frames frames of Channels interleaved floats to mono (times Gain) and appends them. TimeOfFirstFrame is
	 * when the first frame was tapped, on the FPlatformTime::Seconds clock. Null Interleaved appends silence (a silent packet).
	 */
	void Write(const float* Interleaved, int32 Frames, int32 Channels, double TimeOfFirstFrame, float Gain = 1.f);

	/**
	 * Consumer: copies up to Out.Num() of the oldest unread samples into Out and returns how many, with the clock time of the
	 * first one in OutTimeOfFirst (0 when no marker covers it yet).
	 */
	int32 Read(TArrayView<float> Out, double& OutTimeOfFirst);

	/** Samples written and not yet read (may exceed the capacity after an overrun until the next Read). */
	int64 GetAvailable() const;

	/** Samples lost to overruns since Init / Reset. */
	int64 GetDroppedSamples() const { return Dropped.load(std::memory_order_relaxed); }

	/** For tests: the ring's storage, which must not move after Init. */
	const float* GetStorageForTest() const { return Samples.GetData(); }

private:
	struct FMarker
	{
		int64 FirstSample = 0;
		double Time = 0.0;
	};

	/** The time of sample Index from the newest marker at or before it. 0 when none is known. */
	double TimeOfSample(int64 Index) const;

	TArray<float> Samples;
	int64 Mask = 0;
	int32 SampleRate = 48000;

	static constexpr int32 MarkerCapacity = 512;
	FMarker Markers[MarkerCapacity];

	/** Monotonic indices: the producer writes WriteIndex and MarkerWrite; the consumer ReadIndex. */
	std::atomic<int64> WriteIndex{ 0 };
	std::atomic<int64> MarkerWrite{ 0 };
	int64 ReadIndex = 0;
	std::atomic<int64> Dropped{ 0 };
};

/**
 * A lock-free hand-off of the newest value from one writer thread to one reader thread (ADR 0029 §3: the analyser publishes
 * FStageAudioFeatures, the game thread reads them). The writer never waits and the reader always gets a complete value, never
 * one being written. Three slots: the writer's, the reader's and the one in the middle that they swap with.
 */
template <typename T>
class TStageTripleBuffer
{
public:
	/** Writer thread only. */
	void Publish(const T& Value)
	{
		Slots[Back] = Value;
		const uint8 Previous = Middle.exchange(uint8(Back | FreshBit), std::memory_order_acq_rel);
		Back = Previous & IndexMask;
	}

	/** Reader thread only: the newest published value (or the last one read, when nothing new arrived). */
	const T& Read()
	{
		if (Middle.load(std::memory_order_acquire) & FreshBit)
		{
			const uint8 Previous = Middle.exchange(Front, std::memory_order_acq_rel);
			Front = Previous & IndexMask;
		}
		return Slots[Front];
	}

	/** Only while neither thread uses it. */
	void Reset(const T& Value = T())
	{
		for (T& Slot : Slots)
		{
			Slot = Value;
		}
		Back = 0;
		Middle.store(1, std::memory_order_release);
		Front = 2;
	}

private:
	static constexpr uint8 FreshBit = 0x4;
	static constexpr uint8 IndexMask = 0x3;
	T Slots[3];
	uint8 Back = 0;
	std::atomic<uint8> Middle{ 1 };
	uint8 Front = 2;
};
