// Copyright NGG. All Rights Reserved.
//
// Media Foundation backed H.264 / AAC encoder writing an MP4 container.
//
// Media Foundation ships with Windows, so a packaged build needs no extra redistributables. The stream setup
// below follows the engine's own MP4 encoder (MovieRenderPipelineMP4Encoder), which is the configuration Epic
// validated against both the hardware MFTs and the Microsoft software fallback. The differences here are
// deliberate and come from this being a realtime capture rather than an offline render:
//   - sample times are supplied per frame instead of derived from a frame counter, so a hitch in the game
//     shows up as a longer frame rather than as audio/video drift;
//   - audio samples are written as they arrive from the submix instead of being sliced per video frame.

#include "NGGVideoRecorderEncoder.h"
#include "NGGVideoRecorderLog.h"

#if PLATFORM_WINDOWS

#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/WindowsHWrapper.h"
#include "Windows/WindowsPlatformMisc.h"
#include "Microsoft/COMPointer.h"

THIRD_PARTY_INCLUDES_START
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
THIRD_PARTY_INCLUDES_END

namespace NGGVideoRecorderMF
{
	/** 100-nanosecond units, the unit Media Foundation uses for all sample timing. */
	static constexpr int64 NsPerHns = 100;

	static FString HrToString(HRESULT Hr)
	{
		TCHAR Buffer[1024] = {};
		FPlatformMisc::GetSystemErrorMessage(Buffer, UE_ARRAY_COUNT(Buffer), (uint32)Hr);
		return FString::Printf(TEXT("0x%08X (%s)"), (uint32)Hr, *FString(Buffer).TrimStartAndEnd());
	}

	/** Fills the output (encoded) video media type. */
	static HRESULT ConfigureVideoOut(IMFMediaType* Type, const FNGGEncoderConfig& Config)
	{
		HRESULT Hr = Type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		if (SUCCEEDED(Hr)) { Hr = Type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264); }
		if (SUCCEEDED(Hr)) { Hr = Type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); }
		if (SUCCEEDED(Hr)) { Hr = MFSetAttributeSize(Type, MF_MT_FRAME_SIZE, Config.Width, Config.Height); }
		if (SUCCEEDED(Hr)) { Hr = MFSetAttributeRatio(Type, MF_MT_FRAME_RATE, Config.FrameRate, 1); }
		if (SUCCEEDED(Hr)) { Hr = MFSetAttributeRatio(Type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1); }
		if (SUCCEEDED(Hr)) { Hr = Type->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)Config.VideoBitrate); }

		// Rec.709 limited range, which is what every player assumes for H.264.
		if (SUCCEEDED(Hr)) { Hr = Type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709); }
		if (SUCCEEDED(Hr)) { Hr = Type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_sRGB); }
		if (SUCCEEDED(Hr)) { Hr = Type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709); }
		if (SUCCEEDED(Hr)) { Hr = Type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235); }

		if (SUCCEEDED(Hr)) { Hr = Type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High); }
		if (SUCCEEDED(Hr)) { Hr = Type->SetUINT32(CODECAPI_AVEncH264CABACEnable, TRUE); }
		if (SUCCEEDED(Hr)) { Hr = Type->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_PeakConstrainedVBR); }
		return Hr;
	}

	/** Fills the input (raw frame) video media type. */
	static HRESULT ConfigureVideoIn(IMFMediaType* Type, const FNGGEncoderConfig& Config)
	{
		// ARGB32 is B,G,R,A in memory on little endian, which is exactly what the GPU readback produces.
		HRESULT Hr = Type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		if (SUCCEEDED(Hr)) { Hr = Type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32); }
		if (SUCCEEDED(Hr)) { Hr = Type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); }
		if (SUCCEEDED(Hr)) { Hr = MFSetAttributeSize(Type, MF_MT_FRAME_SIZE, Config.Width, Config.Height); }
		if (SUCCEEDED(Hr)) { Hr = MFSetAttributeRatio(Type, MF_MT_FRAME_RATE, Config.FrameRate, 1); }
		if (SUCCEEDED(Hr)) { Hr = MFSetAttributeRatio(Type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1); }
		if (SUCCEEDED(Hr)) { Hr = Type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE); }

		// A positive stride plus an explicit sample size is what stops some encoders flipping the image.
		const uint32 Stride = (uint32)Config.Width * 4;
		if (SUCCEEDED(Hr)) { Hr = Type->SetUINT32(MF_MT_DEFAULT_STRIDE, Stride); }
		if (SUCCEEDED(Hr)) { Hr = Type->SetUINT32(MF_MT_SAMPLE_SIZE, Stride * (uint32)Config.Height); }
		return Hr;
	}

	static HRESULT ConfigureAudioOut(IMFMediaType* Type, const FNGGEncoderConfig& Config)
	{
		HRESULT Hr = Type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		if (SUCCEEDED(Hr)) { Hr = Type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC); }
		if (SUCCEEDED(Hr)) { Hr = Type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16); }
		if (SUCCEEDED(Hr)) { Hr = Type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)Config.AudioSampleRate); }
		if (SUCCEEDED(Hr)) { Hr = Type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)Config.AudioChannels); }
		if (SUCCEEDED(Hr)) { Hr = Type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)(Config.AudioBitrate / 8)); }
		return Hr;
	}

	static HRESULT ConfigureAudioIn(IMFMediaType* Type, const FNGGEncoderConfig& Config)
	{
		const uint32 BlockAlign = (uint32)Config.AudioChannels * sizeof(int16);

		HRESULT Hr = Type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		if (SUCCEEDED(Hr)) { Hr = Type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM); }
		if (SUCCEEDED(Hr)) { Hr = Type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16); }
		if (SUCCEEDED(Hr)) { Hr = Type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)Config.AudioSampleRate); }
		if (SUCCEEDED(Hr)) { Hr = Type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)Config.AudioChannels); }
		if (SUCCEEDED(Hr)) { Hr = Type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE); }
		if (SUCCEEDED(Hr)) { Hr = Type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, BlockAlign); }
		if (SUCCEEDED(Hr)) { Hr = Type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, BlockAlign * (uint32)Config.AudioSampleRate); }
		return Hr;
	}
}

/**
 * Media Foundation sink writer wrapper.
 *
 * Every method runs on the encoder worker thread, which is also the thread that calls CoInitialize, so the
 * COM objects never cross a thread boundary.
 */
class FNGGMediaFoundationEncoder final : public FNGGVideoEncoder
{
public:
	virtual ~FNGGMediaFoundationEncoder() override
	{
		// Finalize is normally called explicitly; this only runs when the recording was abandoned.
		FString Unused;
		Finalize(Unused);
	}

	virtual bool Initialize(const FNGGEncoderConfig& InConfig, FString& OutError) override
	{
		using namespace NGGVideoRecorderMF;

		Config = InConfig;

		if (!FWindowsPlatformMisc::CoInitialize())
		{
			OutError = TEXT("Could not initialise COM on the encoder thread.");
			return false;
		}
		bComInitialized = true;

		HRESULT Hr = MFStartup(MF_VERSION);
		if (FAILED(Hr))
		{
			OutError = FString::Printf(TEXT("MFStartup failed: %s"), *HrToString(Hr));
			return false;
		}
		bMFStarted = true;

		// Writer-wide configuration. Hardware transforms use the GPU encoder (NVENC / Quick Sync / AMF) when
		// one is present, and throttling off keeps WriteSample from blocking the encoder thread.
		TComPtr<IMFAttributes> WriterAttributes;
		Hr = MFCreateAttributes(&WriterAttributes, 3);
		if (SUCCEEDED(Hr))
		{
			WriterAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
			WriterAttributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
			WriterAttributes->SetUINT32(CODECAPI_AVLowLatencyMode, FALSE);
		}

		if (SUCCEEDED(Hr))
		{
			Hr = MFCreateSinkWriterFromURL(*Config.FilePath, nullptr, WriterAttributes.Get(), &SinkWriter);
			if (FAILED(Hr))
			{
				OutError = FString::Printf(TEXT("Could not create the MP4 writer for '%s': %s"), *Config.FilePath, *HrToString(Hr));
				return false;
			}
		}

		// Video stream. Every stream has to be added before BeginWriting.
		TComPtr<IMFMediaType> VideoOut;
		if (SUCCEEDED(Hr)) { Hr = MFCreateMediaType(&VideoOut); }
		if (SUCCEEDED(Hr)) { Hr = ConfigureVideoOut(VideoOut.Get(), Config); }
		if (SUCCEEDED(Hr)) { Hr = SinkWriter->AddStream(VideoOut.Get(), &VideoStreamIndex); }
		if (FAILED(Hr))
		{
			OutError = FString::Printf(TEXT("Could not add the H.264 video stream (%dx%d @ %d fps): %s"),
				Config.Width, Config.Height, Config.FrameRate, *HrToString(Hr));
			return false;
		}

		TComPtr<IMFMediaType> AudioOut;
		if (Config.bHasAudio)
		{
			if (SUCCEEDED(Hr)) { Hr = MFCreateMediaType(&AudioOut); }
			if (SUCCEEDED(Hr)) { Hr = ConfigureAudioOut(AudioOut.Get(), Config); }
			if (SUCCEEDED(Hr)) { Hr = SinkWriter->AddStream(AudioOut.Get(), &AudioStreamIndex); }
			if (FAILED(Hr))
			{
				OutError = FString::Printf(TEXT("Could not add the AAC audio stream (%d Hz, %d ch): %s"),
					Config.AudioSampleRate, Config.AudioChannels, *HrToString(Hr));
				return false;
			}
		}

		// Rate control lives on the attributes handed to SetInputMediaType, not on the media type itself.
		TComPtr<IMFAttributes> EncoderAttributes;
		if (SUCCEEDED(Hr)) { Hr = MFCreateAttributes(&EncoderAttributes, 3); }
		if (SUCCEEDED(Hr))
		{
			EncoderAttributes->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, (UINT32)Config.VideoBitrate);
			EncoderAttributes->SetUINT32(CODECAPI_AVEncCommonMaxBitRate, (UINT32)(Config.VideoBitrate * 3 / 2));
			EncoderAttributes->SetUINT32(CODECAPI_AVEncCommonBufferSize, (UINT32)Config.VideoBitrate);
		}

		TComPtr<IMFMediaType> VideoIn;
		if (SUCCEEDED(Hr)) { Hr = MFCreateMediaType(&VideoIn); }
		if (SUCCEEDED(Hr)) { Hr = ConfigureVideoIn(VideoIn.Get(), Config); }
		if (SUCCEEDED(Hr)) { Hr = SinkWriter->SetInputMediaType(VideoStreamIndex, VideoIn.Get(), EncoderAttributes.Get()); }
		if (FAILED(Hr))
		{
			OutError = FString::Printf(TEXT("The system H.264 encoder rejected %dx%d @ %d fps: %s"),
				Config.Width, Config.Height, Config.FrameRate, *HrToString(Hr));
			return false;
		}

		if (Config.bHasAudio)
		{
			TComPtr<IMFMediaType> AudioIn;
			if (SUCCEEDED(Hr)) { Hr = MFCreateMediaType(&AudioIn); }
			if (SUCCEEDED(Hr)) { Hr = ConfigureAudioIn(AudioIn.Get(), Config); }
			if (SUCCEEDED(Hr)) { Hr = SinkWriter->SetInputMediaType(AudioStreamIndex, AudioIn.Get(), nullptr); }
			if (FAILED(Hr))
			{
				OutError = FString::Printf(TEXT("The system AAC encoder rejected %d Hz / %d channels: %s"),
					Config.AudioSampleRate, Config.AudioChannels, *HrToString(Hr));
				return false;
			}
		}

		Hr = SinkWriter->BeginWriting();
		if (FAILED(Hr))
		{
			OutError = FString::Printf(TEXT("BeginWriting failed: %s"), *HrToString(Hr));
			return false;
		}

		bInitialized = true;
		return true;
	}

	virtual bool WriteVideoFrame(const uint8* BGRAData, int64 TimeNs, int64 DurationNs, FString& OutError) override
	{
		using namespace NGGVideoRecorderMF;

		if (!bInitialized || !SinkWriter)
		{
			OutError = TEXT("WriteVideoFrame called on an encoder that is not initialised.");
			return false;
		}

		const LONG Stride = Config.Width * 4;
		const DWORD BufferSize = (DWORD)(Stride * Config.Height);

		TComPtr<IMFMediaBuffer> Buffer;
		HRESULT Hr = MFCreateMemoryBuffer(BufferSize, &Buffer);
		if (FAILED(Hr))
		{
			OutError = FString::Printf(TEXT("MFCreateMemoryBuffer(%u) failed: %s"), BufferSize, *HrToString(Hr));
			return false;
		}

		BYTE* Dest = nullptr;
		Hr = Buffer->Lock(&Dest, nullptr, nullptr);
		if (SUCCEEDED(Hr))
		{
			// MFCopyImage is a plain SSE memcpy that understands strides.
			Hr = MFCopyImage(Dest, Stride, (const BYTE*)BGRAData, Stride, Stride, Config.Height);
			Buffer->Unlock();
		}
		if (SUCCEEDED(Hr)) { Hr = Buffer->SetCurrentLength(BufferSize); }

		TComPtr<IMFSample> Sample;
		if (SUCCEEDED(Hr)) { Hr = MFCreateSample(&Sample); }
		if (SUCCEEDED(Hr)) { Hr = Sample->AddBuffer(Buffer.Get()); }
		if (SUCCEEDED(Hr)) { Hr = Sample->SetSampleTime(TimeNs / NsPerHns); }
		if (SUCCEEDED(Hr)) { Hr = Sample->SetSampleDuration(FMath::Max<int64>(1, DurationNs / NsPerHns)); }
		if (SUCCEEDED(Hr)) { Hr = SinkWriter->WriteSample(VideoStreamIndex, Sample.Get()); }

		if (FAILED(Hr))
		{
			OutError = FString::Printf(TEXT("Writing a video frame failed: %s"), *HrToString(Hr));
			return false;
		}
		return true;
	}

	virtual bool WriteAudio(const int16* Interleaved, int32 NumFrames, int64 TimeNs, FString& OutError) override
	{
		using namespace NGGVideoRecorderMF;

		if (!bInitialized || !SinkWriter || !Config.bHasAudio)
		{
			return true;
		}
		if (NumFrames <= 0)
		{
			return true;
		}

		const DWORD BufferSize = (DWORD)(NumFrames * Config.AudioChannels * sizeof(int16));

		TComPtr<IMFMediaBuffer> Buffer;
		HRESULT Hr = MFCreateMemoryBuffer(BufferSize, &Buffer);

		BYTE* Dest = nullptr;
		if (SUCCEEDED(Hr)) { Hr = Buffer->Lock(&Dest, nullptr, nullptr); }
		if (SUCCEEDED(Hr))
		{
			FMemory::Memcpy(Dest, Interleaved, BufferSize);
			Buffer->Unlock();
		}
		if (SUCCEEDED(Hr)) { Hr = Buffer->SetCurrentLength(BufferSize); }

		const int64 DurationNs = ((int64)NumFrames * 1000000000LL) / FMath::Max(1, Config.AudioSampleRate);

		TComPtr<IMFSample> Sample;
		if (SUCCEEDED(Hr)) { Hr = MFCreateSample(&Sample); }
		if (SUCCEEDED(Hr)) { Hr = Sample->AddBuffer(Buffer.Get()); }
		if (SUCCEEDED(Hr)) { Hr = Sample->SetSampleTime(TimeNs / NsPerHns); }
		if (SUCCEEDED(Hr)) { Hr = Sample->SetSampleDuration(FMath::Max<int64>(1, DurationNs / NsPerHns)); }
		if (SUCCEEDED(Hr)) { Hr = SinkWriter->WriteSample(AudioStreamIndex, Sample.Get()); }

		if (FAILED(Hr))
		{
			OutError = FString::Printf(TEXT("Writing an audio sample failed: %s"), *HrToString(Hr));
			return false;
		}
		return true;
	}

	virtual bool Finalize(FString& OutError) override
	{
		using namespace NGGVideoRecorderMF;

		bool bSuccess = true;

		if (SinkWriter)
		{
			if (bInitialized)
			{
				const HRESULT Hr = SinkWriter->Finalize();
				if (FAILED(Hr))
				{
					OutError = FString::Printf(TEXT("Finalising the MP4 failed: %s"), *HrToString(Hr));
					bSuccess = false;
				}
			}
			SinkWriter.Reset();
		}

		if (bMFStarted)
		{
			MFShutdown();
			bMFStarted = false;
		}
		if (bComInitialized)
		{
			FWindowsPlatformMisc::CoUninitialize();
			bComInitialized = false;
		}

		bInitialized = false;
		return bSuccess;
	}

private:
	FNGGEncoderConfig Config;

	TComPtr<IMFSinkWriter> SinkWriter;
	DWORD VideoStreamIndex = 0;
	DWORD AudioStreamIndex = 0;

	bool bComInitialized = false;
	bool bMFStarted = false;
	bool bInitialized = false;
};

#include "Windows/HideWindowsPlatformTypes.h"

#endif // PLATFORM_WINDOWS

TUniquePtr<FNGGVideoEncoder> CreateNGGVideoEncoder(FString& OutError)
{
#if PLATFORM_WINDOWS
	return MakeUnique<FNGGMediaFoundationEncoder>();
#else
	OutError = TEXT("NGG Video Recorder can only write MP4 files on Windows.");
	return nullptr;
#endif
}
