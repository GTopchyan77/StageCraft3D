// Copyright Epic Games, Inc. All Rights Reserved.

#include "Sketch/StageSketchImageImport.h"

#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

#define LOCTEXT_NAMESPACE "StageSketchImageImport"

namespace StageSketchImageImport
{
	namespace
	{
		/** JPEG quality of the image sent for the online reading: small enough to upload quickly, sharp enough for pen lines. */
		constexpr int32 VisionJpegQuality = 85;
	}

	bool HasSupportedExtension(const FString& FilePath)
	{
		const FString Extension = FPaths::GetExtension(FilePath).ToLower();
		return Extension == TEXT("png") || Extension == TEXT("jpg") || Extension == TEXT("jpeg");
	}

	FString GetFileDialogFilter()
	{
		return TEXT("Stage drawings (*.png, *.jpg)|*.png;*.jpg;*.jpeg");
	}

	bool DecodeImage(TConstArrayView64<uint8> FileBytes, TArray<uint8>& OutBGRA, int32& OutWidth, int32& OutHeight, FText& OutError)
	{
		IImageWrapperModule& ImageWrapper = FModuleManager::GetModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const EImageFormat Format = ImageWrapper.DetectImageFormat(FileBytes.GetData(), FileBytes.Num());
		if (Format != EImageFormat::PNG && Format != EImageFormat::JPEG)
		{
			OutError = LOCTEXT("NotAnImage", "The file is not a PNG or JPEG image.");
			return false;
		}
		const TSharedPtr<IImageWrapper> Wrapper = ImageWrapper.CreateImageWrapper(Format);
		if (!Wrapper.IsValid() || !Wrapper->SetCompressed(FileBytes.GetData(), FileBytes.Num()))
		{
			OutError = LOCTEXT("Damaged", "The image could not be read; the file may be damaged.");
			return false;
		}
		const int64 Width = Wrapper->GetWidth();
		const int64 Height = Wrapper->GetHeight();
		if (Width < 8 || Height < 8)
		{
			OutError = LOCTEXT("TooSmall", "The image is too small to read.");
			return false;
		}
		if (Width > MaxDimension || Height > MaxDimension || Width * Height > MaxPixels)
		{
			OutError = FText::Format(LOCTEXT("TooLarge", "The image is {0} × {1} pixels; images up to {2} pixels wide and {3} megapixels can be read."),
				FText::AsNumber(Width), FText::AsNumber(Height), FText::AsNumber(MaxDimension), FText::AsNumber(MaxPixels / 1000000));
			return false;
		}
		TArray64<uint8> Raw;
		if (!Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw) || Raw.Num() != Width * Height * 4)
		{
			OutError = LOCTEXT("NotDecoded", "The image could not be decoded.");
			return false;
		}
		OutWidth = int32(Width);
		OutHeight = int32(Height);
		OutBGRA.SetNumUninitialized(int32(Raw.Num()));
		// Transparent parts of a drawing are paper: flatten the alpha onto white.
		for (int64 Pixel = 0; Pixel < Width * Height; ++Pixel)
		{
			const uint32 Alpha = Raw[Pixel * 4 + 3];
			for (int32 Channel = 0; Channel < 3; ++Channel)
			{
				OutBGRA[int32(Pixel * 4 + Channel)] = uint8((uint32(Raw[Pixel * 4 + Channel]) * Alpha + 255u * (255u - Alpha)) / 255u);
			}
			OutBGRA[int32(Pixel * 4 + 3)] = 255;
		}
		return true;
	}

	FStageSketchGrayImage ToGray(TConstArrayView<uint8> BGRA, int32 Width, int32 Height)
	{
		FStageSketchGrayImage Gray;
		if (Width <= 0 || Height <= 0 || BGRA.Num() != Width * Height * 4)
		{
			return Gray;
		}
		Gray.Width = Width;
		Gray.Height = Height;
		Gray.Pixels.SetNumUninitialized(Width * Height);
		for (int32 Pixel = 0; Pixel < Width * Height; ++Pixel)
		{
			const uint32 B = BGRA[Pixel * 4];
			const uint32 G = BGRA[Pixel * 4 + 1];
			const uint32 R = BGRA[Pixel * 4 + 2];
			Gray.Pixels[Pixel] = uint8((77u * R + 150u * G + 29u * B) >> 8);
		}
		return Gray;
	}

	TArray<uint8> DownscaleBGRA(TConstArrayView<uint8> BGRA, int32 Width, int32 Height, int32 MaxSide, int32& OutWidth, int32& OutHeight)
	{
		OutWidth = Width;
		OutHeight = Height;
		if (FMath::Max(Width, Height) <= MaxSide || MaxSide <= 0)
		{
			return TArray<uint8>(BGRA.GetData(), BGRA.Num());
		}
		const double Scale = double(MaxSide) / double(FMath::Max(Width, Height));
		OutWidth = FMath::Max(1, FMath::RoundToInt(Width * Scale));
		OutHeight = FMath::Max(1, FMath::RoundToInt(Height * Scale));
		TArray<uint8> Out;
		Out.SetNumUninitialized(OutWidth * OutHeight * 4);
		for (int32 Y = 0; Y < OutHeight; ++Y)
		{
			const int32 Y0 = int32(double(Y) * Height / OutHeight);
			const int32 Y1 = FMath::Clamp(int32(double(Y + 1) * Height / OutHeight), Y0 + 1, Height);
			for (int32 X = 0; X < OutWidth; ++X)
			{
				const int32 X0 = int32(double(X) * Width / OutWidth);
				const int32 X1 = FMath::Clamp(int32(double(X + 1) * Width / OutWidth), X0 + 1, Width);
				uint32 Sum[4] = { 0, 0, 0, 0 };
				for (int32 SY = Y0; SY < Y1; ++SY)
				{
					for (int32 SX = X0; SX < X1; ++SX)
					{
						for (int32 Channel = 0; Channel < 4; ++Channel)
						{
							Sum[Channel] += BGRA[(SY * Width + SX) * 4 + Channel];
						}
					}
				}
				const uint32 Count = uint32((Y1 - Y0) * (X1 - X0));
				for (int32 Channel = 0; Channel < 4; ++Channel)
				{
					Out[(Y * OutWidth + X) * 4 + Channel] = uint8(Sum[Channel] / Count);
				}
			}
		}
		return Out;
	}

	bool LoadImageFile(const FString& FilePath, TArray<uint8>& OutBGRA, int32& OutWidth, int32& OutHeight, FText& OutError)
	{
		const int64 Size = IFileManager::Get().FileSize(*FilePath);
		if (Size <= 0)
		{
			OutError = LOCTEXT("Unreadable", "The file could not be found or read.");
			return false;
		}
		if (Size > MaxFileBytes)
		{
			OutError = FText::Format(LOCTEXT("FileTooLarge", "The file is larger than {0} MB."), FText::AsNumber(MaxFileBytes / (1024 * 1024)));
			return false;
		}
		TArray64<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *FilePath))
		{
			OutError = LOCTEXT("Unreadable", "The file could not be found or read.");
			return false;
		}
		return DecodeImage(Bytes, OutBGRA, OutWidth, OutHeight, OutError);
	}

	FStageSketchImageMetrics MeasureImage(TConstArrayView<uint8> BGRA, int32 Width, int32 Height)
	{
		FStageSketchImageMetrics Metrics;
		if (Width <= 0 || Height <= 0 || BGRA.Num() != Width * Height * 4)
		{
			return Metrics;
		}
		int32 W = 0;
		int32 H = 0;
		const TArray<uint8> Small = DownscaleBGRA(BGRA, Width, Height, MeasureMaxSide, W, H);
		const int32 Count = W * H;

		// Luma, then a 3x3 box blur so paper grain and JPEG noise do not count as texture.
		TArray<float> Luma;
		Luma.SetNumUninitialized(Count);
		double SumRG = 0.0;
		double SumYB = 0.0;
		double SumRG2 = 0.0;
		double SumYB2 = 0.0;
		for (int32 Pixel = 0; Pixel < Count; ++Pixel)
		{
			const float B = Small[Pixel * 4];
			const float G = Small[Pixel * 4 + 1];
			const float R = Small[Pixel * 4 + 2];
			Luma[Pixel] = 0.299f * R + 0.587f * G + 0.114f * B;
			const double RG = R - G;
			const double YB = 0.5 * (R + G) - B;
			SumRG += RG;
			SumYB += YB;
			SumRG2 += RG * RG;
			SumYB2 += YB * YB;
		}
		TArray<float> Blurred;
		Blurred.SetNumUninitialized(Count);
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				float Sum = 0.f;
				int32 Taps = 0;
				for (int32 DY = -1; DY <= 1; ++DY)
				{
					for (int32 DX = -1; DX <= 1; ++DX)
					{
						const int32 SX = X + DX;
						const int32 SY = Y + DY;
						if (SX >= 0 && SY >= 0 && SX < W && SY < H)
						{
							Sum += Luma[SY * W + SX];
							++Taps;
						}
					}
				}
				Blurred[Y * W + X] = Sum / float(Taps);
			}
		}

		// Gradient (central differences, 0..510). Strong = a stroke edge; weak = texture, shading or grain that survived the blur.
		constexpr float WeakGradient = 6.f;
		constexpr float StrongGradient = 30.f;
		constexpr int32 StrokeReach = 2;
		TArray<float> Gradient;
		Gradient.SetNumZeroed(Count);
		for (int32 Y = 1; Y < H - 1; ++Y)
		{
			for (int32 X = 1; X < W - 1; ++X)
			{
				const int32 Index = Y * W + X;
				Gradient[Index] = FMath::Abs(Blurred[Index + 1] - Blurred[Index - 1]) + FMath::Abs(Blurred[Index + W] - Blurred[Index - W]);
			}
		}
		int32 Weak = 0;
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const float Value = Gradient[Y * W + X];
				if (Value < WeakGradient || Value >= StrongGradient)
				{
					continue;
				}
				// The soft rim of a drawn stroke is not texture.
				bool bNearStroke = false;
				for (int32 DY = -StrokeReach; DY <= StrokeReach && !bNearStroke; ++DY)
				{
					for (int32 DX = -StrokeReach; DX <= StrokeReach; ++DX)
					{
						const int32 SX = X + DX;
						const int32 SY = Y + DY;
						if (SX >= 0 && SY >= 0 && SX < W && SY < H && Gradient[SY * W + SX] >= StrongGradient)
						{
							bNearStroke = true;
							break;
						}
					}
				}
				Weak += bNearStroke ? 0 : 1;
			}
		}
		Metrics.WeakTextureShare = float(Weak) / float(Count);

		const double MeanRG = SumRG / Count;
		const double MeanYB = SumYB / Count;
		const double VarRG = FMath::Max(0.0, SumRG2 / Count - MeanRG * MeanRG);
		const double VarYB = FMath::Max(0.0, SumYB2 / Count - MeanYB * MeanYB);
		Metrics.Colourfulness = float(FMath::Sqrt(VarRG + VarYB) + 0.3 * FMath::Sqrt(MeanRG * MeanRG + MeanYB * MeanYB));
		return Metrics;
	}

	void Run(FStageSketchImportJob& Job)
	{
		TArray<uint8> BGRA;
		if (!LoadImageFile(Job.FilePath, BGRA, Job.SourceWidth, Job.SourceHeight, Job.Error))
		{
			return;
		}

		Job.Metrics = MeasureImage(BGRA, Job.SourceWidth, Job.SourceHeight);
		Job.bLooksLikePhoto = LooksLikePhoto(Job.Metrics);
		// Tracing a photograph makes random strokes from its texture (decks from shadows, fixtures from speckle): not done unless asked.
		const bool bTrace = !Job.bLooksLikePhoto || Job.bTraceIfPhoto;
		if (!bTrace && !Job.bWantVisionImage)
		{
			Job.bDecoded = true;
			return;
		}
		if (bTrace)
		{
			Job.Trace = StageSketchImageTrace::Trace(ToGray(BGRA, Job.SourceWidth, Job.SourceHeight), Job.Options);
		}
		Job.Preview = DownscaleBGRA(BGRA, Job.SourceWidth, Job.SourceHeight, PreviewMaxSide, Job.PreviewWidth, Job.PreviewHeight);
		if (Job.bWantVisionImage)
		{
			const TArray<uint8> Vision = DownscaleBGRA(BGRA, Job.SourceWidth, Job.SourceHeight, VisionMaxSide, Job.VisionWidth, Job.VisionHeight);
			IImageWrapperModule& ImageWrapper = FModuleManager::GetModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
			const TSharedPtr<IImageWrapper> Jpeg = ImageWrapper.CreateImageWrapper(EImageFormat::JPEG);
			if (Jpeg.IsValid() && Jpeg->SetRaw(Vision.GetData(), Vision.Num(), Job.VisionWidth, Job.VisionHeight, ERGBFormat::BGRA, 8))
			{
				const TArray64<uint8> Compressed = Jpeg->GetCompressed(VisionJpegQuality);
				Job.VisionJpeg = TArray<uint8>(Compressed.GetData(), int32(Compressed.Num()));
			}
		}
		Job.bDecoded = true;
	}
}

#undef LOCTEXT_NAMESPACE
