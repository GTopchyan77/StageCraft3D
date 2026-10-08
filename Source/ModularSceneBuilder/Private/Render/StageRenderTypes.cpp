// Copyright Epic Games, Inc. All Rights Reserved.

#include "Render/StageRenderTypes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageRenderTypes)

namespace StageRender
{
	FIntPoint GetOutputSize(EStageRenderResolution Resolution)
	{
		switch (Resolution)
		{
		case EStageRenderResolution::UltraHD4K:
			return FIntPoint(3840, 2160);
		case EStageRenderResolution::Square:
			return FIntPoint(2160, 2160);
		case EStageRenderResolution::FullHD:
		default:
			return FIntPoint(1920, 1080);
		}
	}

	double GetSupersampleFactor(const FStageRenderSettings& Settings)
	{
		if (Settings.AntiAliasing != EStageRenderAntiAliasing::Supersampled)
		{
			return 1.0;
		}
		const FIntPoint Output = GetOutputSize(Settings.Resolution);
		const double OutputPixels = double(Output.X) * double(Output.Y);
		return FMath::Clamp(FMath::Sqrt(MaxSupersampledPixels / OutputPixels), 1.0, 2.0);
	}

	FIntPoint GetInternalSize(const FStageRenderSettings& Settings)
	{
		const FIntPoint Output = GetOutputSize(Settings.Resolution);
		const double Factor = GetSupersampleFactor(Settings);
		// Even sizes keep the downsample filter symmetric.
		auto Scale = [Factor](int32 Pixels) { return FMath::Max(2, 2 * FMath::RoundToInt32(Pixels * Factor * 0.5)); };
		return FIntPoint(Scale(Output.X), Scale(Output.Y));
	}

	int32 GetWarmupFrames(const FStageRenderSettings& Settings)
	{
		const bool bTemporal = Settings.AntiAliasing == EStageRenderAntiAliasing::Temporal || Settings.AntiAliasing == EStageRenderAntiAliasing::Supersampled;
		const int32 Frames = bTemporal ? 16 : 4;
		return Settings.PostProcess == EStageRenderPostProcess::Cinematic ? Frames * 2 : Frames;
	}

	FString SanitizeFileComponent(const FString& Text)
	{
		FString Result;
		Result.Reserve(Text.Len());
		for (const TCHAR Char : Text.TrimStartAndEnd())
		{
			const bool bKeep = (Char >= TEXT('a') && Char <= TEXT('z')) || (Char >= TEXT('A') && Char <= TEXT('Z'))
				|| (Char >= TEXT('0') && Char <= TEXT('9')) || Char == TEXT('-') || Char == TEXT('_');
			Result.AppendChar(bKeep ? Char : TEXT('_'));
		}
		return Result.IsEmpty() ? FString(TEXT("Untitled")) : Result;
	}

	FString MakeFileName(const FString& SceneName, const FDateTime& LocalTime, const FIntPoint& Size)
	{
		return FString::Printf(TEXT("StageCraft_%s_%s_%dx%d.png"), *SanitizeFileComponent(SceneName),
			*LocalTime.ToString(TEXT("%Y%m%d-%H%M%S")), Size.X, Size.Y);
	}

	FString MakeWatermarkText(const FString& SceneName, const FDateTime& LocalTime, const FIntPoint& Size, int32 ItemCount)
	{
		return FString::Printf(TEXT("StageCraft 3D  ·  %s  ·  %s  ·  %d×%d  ·  %d %s"),
			SceneName.IsEmpty() ? TEXT("Untitled") : *SceneName, *LocalTime.ToString(TEXT("%Y-%m-%d %H:%M")),
			Size.X, Size.Y, ItemCount, ItemCount == 1 ? TEXT("item") : TEXT("items"));
	}

	FStageRenderSettings Sanitize(const FStageRenderSettings& Settings)
	{
		const FStageRenderSettings Defaults;
		FStageRenderSettings Result = Settings;
		if (uint8(Result.Resolution) > uint8(EStageRenderResolution::Square))
		{
			Result.Resolution = Defaults.Resolution;
		}
		if (uint8(Result.AntiAliasing) > uint8(EStageRenderAntiAliasing::Supersampled))
		{
			Result.AntiAliasing = Defaults.AntiAliasing;
		}
		if (uint8(Result.PostProcess) > uint8(EStageRenderPostProcess::Cinematic))
		{
			Result.PostProcess = Defaults.PostProcess;
		}
		return Result;
	}
}
