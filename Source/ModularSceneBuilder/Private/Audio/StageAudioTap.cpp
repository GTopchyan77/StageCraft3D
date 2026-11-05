// Copyright Epic Games, Inc. All Rights Reserved.

#include "Audio/StageAudioTap.h"

void FStageAudioTap::Init(int32 CapacitySamples, int32 InSampleRate)
{
	const int32 Capacity = int32(FMath::RoundUpToPowerOfTwo(uint32(FMath::Clamp(CapacitySamples, 1024, 1 << 22))));
	Samples.SetNumZeroed(Capacity);
	Mask = int64(Capacity) - 1;
	SampleRate = FMath::Max(InSampleRate, 1);
	Reset();
}

void FStageAudioTap::Reset()
{
	if (Samples.Num() > 0)
	{
		FMemory::Memzero(Samples.GetData(), Samples.Num() * sizeof(float));
	}
	FMemory::Memzero(Markers, sizeof(Markers));
	WriteIndex.store(0, std::memory_order_relaxed);
	MarkerWrite.store(0, std::memory_order_relaxed);
	ReadIndex = 0;
	Dropped.store(0, std::memory_order_relaxed);
}

void FStageAudioTap::Write(const float* Interleaved, int32 Frames, int32 Channels, double TimeOfFirstFrame, float Gain)
{
	if (Frames <= 0 || Samples.Num() == 0)
	{
		return;
	}
	const int64 First = WriteIndex.load(std::memory_order_relaxed);

	// The block's place on the clock is published before its samples, so the reader always finds a marker for what it reads.
	const int64 Marker = MarkerWrite.load(std::memory_order_relaxed);
	Markers[Marker & (MarkerCapacity - 1)] = { First, TimeOfFirstFrame };
	MarkerWrite.store(Marker + 1, std::memory_order_release);

	float* Ring = Samples.GetData();
	if (!Interleaved || Channels <= 0)
	{
		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			Ring[(First + Frame) & Mask] = 0.f;
		}
	}
	else
	{
		const float Scale = Gain / float(Channels);
		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			const float* In = Interleaved + int64(Frame) * Channels;
			float Sum = 0.f;
			for (int32 Channel = 0; Channel < Channels; ++Channel)
			{
				Sum += In[Channel];
			}
			Ring[(First + Frame) & Mask] = Sum * Scale;
		}
	}
	WriteIndex.store(First + Frames, std::memory_order_release);
}

int64 FStageAudioTap::GetAvailable() const
{
	return WriteIndex.load(std::memory_order_acquire) - ReadIndex;
}

double FStageAudioTap::TimeOfSample(int64 Index) const
{
	const int64 Newest = MarkerWrite.load(std::memory_order_acquire) - 1;
	const int64 Oldest = FMath::Max<int64>(0, Newest - MarkerCapacity + 2); // one slot of margin: the producer may be rewriting the oldest
	for (int64 Marker = Newest; Marker >= Oldest; --Marker)
	{
		const FMarker& Entry = Markers[Marker & (MarkerCapacity - 1)];
		if (Entry.FirstSample <= Index)
		{
			return Entry.Time + double(Index - Entry.FirstSample) / double(SampleRate);
		}
	}
	return 0.0;
}

int32 FStageAudioTap::Read(TArrayView<float> Out, double& OutTimeOfFirst)
{
	OutTimeOfFirst = 0.0;
	const int64 Capacity = Mask + 1;
	if (Capacity <= 1 || Out.Num() == 0)
	{
		return 0;
	}
	// The producer may be filling the slots just past WriteIndex (a block it has not published yet), which hold the oldest samples:
	// a quarter of the ring is kept as a guard against reading those.
	const int64 Safe = Capacity - Capacity / 4;

	const int64 Written = WriteIndex.load(std::memory_order_acquire);
	if (Written - ReadIndex > Safe)
	{
		Dropped.fetch_add(Written - Safe - ReadIndex, std::memory_order_relaxed);
		ReadIndex = Written - Safe;
	}
	int64 Count = FMath::Min<int64>(Out.Num(), Written - ReadIndex);
	if (Count <= 0)
	{
		return 0;
	}
	const float* Ring = Samples.GetData();
	const int64 Start = ReadIndex & Mask;
	const int64 FirstPart = FMath::Min(Count, Capacity - Start);
	FMemory::Memcpy(Out.GetData(), Ring + Start, FirstPart * sizeof(float));
	if (Count > FirstPart)
	{
		FMemory::Memcpy(Out.GetData() + FirstPart, Ring, (Count - FirstPart) * sizeof(float));
	}

	// Validate after copying: had the producer lapped us meanwhile, the oldest part of the copy may be newer audio. Drop it.
	const int64 After = WriteIndex.load(std::memory_order_acquire);
	if (After - ReadIndex > Safe)
	{
		const int64 Lost = After - Safe - ReadIndex;
		Dropped.fetch_add(Lost, std::memory_order_relaxed);
		if (Lost >= Count)
		{
			ReadIndex = After - Safe;
			return 0;
		}
		FMemory::Memmove(Out.GetData(), Out.GetData() + Lost, (Count - Lost) * sizeof(float));
		ReadIndex += Lost;
		Count -= Lost;
	}
	OutTimeOfFirst = TimeOfSample(ReadIndex);
	ReadIndex += Count;
	return int32(Count);
}
