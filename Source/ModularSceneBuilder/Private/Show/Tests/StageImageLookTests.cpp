// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for STATE.md #86: the sketch's photo check (StageSketchImageImport::MeasureImage / LooksLikePhoto) on synthetic drawings and
// photo-like images, the photo colour palette (StageImagePalette), a music strobe that rests open in silence
// (StageEffects::GetSilentBand), and the FOLLOW MUSIC look (StageEffectRules::MakeMusicLook). The controller requests that use them
// (USE COLOURS, FOLLOW MUSIC, the Sketch refusal) are exercised by the scripted -game run recorded in STATE.md #86.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.ImageLook; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/StageParameterTypes.h"
#include "Math/RandomStream.h"
#include "Misc/AutomationTest.h"
#include "Show/StageEffects.h"
#include "Show/StageEffectTypes.h"
#include "Show/StageImagePalette.h"
#include "Sketch/StageSketchImageImport.h"

namespace StageImageLookTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	/** A BGRA8 picture filled by a per-pixel function returning sRGB 0..255. */
	struct FPicture
	{
		int32 Width = 0;
		int32 Height = 0;
		TArray<uint8> BGRA;

		FPicture(int32 InWidth, int32 InHeight, TFunctionRef<FVector3f(int32 X, int32 Y)> Pixel)
			: Width(InWidth), Height(InHeight)
		{
			BGRA.SetNumUninitialized(Width * Height * 4);
			for (int32 Y = 0; Y < Height; ++Y)
			{
				for (int32 X = 0; X < Width; ++X)
				{
					const FVector3f Rgb = Pixel(X, Y);
					const int32 Index = (Y * Width + X) * 4;
					BGRA[Index] = uint8(FMath::Clamp(FMath::RoundToInt(Rgb.Z), 0, 255));
					BGRA[Index + 1] = uint8(FMath::Clamp(FMath::RoundToInt(Rgb.Y), 0, 255));
					BGRA[Index + 2] = uint8(FMath::Clamp(FMath::RoundToInt(Rgb.X), 0, 255));
					BGRA[Index + 3] = 255;
				}
			}
		}

		FStageSketchImageMetrics Measure() const { return StageSketchImageImport::MeasureImage(BGRA, Width, Height); }
	};

	/** A stage plan: a deck outline, a truss line and a fixture dot, Stroke pixels wide, on Paper(X, Y). */
	bool IsPlanInk(int32 X, int32 Y, int32 W, int32 H, int32 Stroke)
	{
		const auto Near = [Stroke](int32 A, int32 B) { return FMath::Abs(A - B) < Stroke; };
		const int32 L = W / 6, R = W * 5 / 6, T = H / 6, B = H * 5 / 6;
		const bool bOutline = ((Near(X, L) || Near(X, R)) && Y >= T && Y <= B) || ((Near(Y, T) || Near(Y, B)) && X >= L && X <= R);
		const bool bTruss = Near(Y, H / 2) && X > W / 3 && X < W * 2 / 3;
		const bool bDot = FMath::Square(X - W / 2) + FMath::Square(Y - H / 3) < FMath::Square(Stroke * 3);
		return bOutline || bTruss || bDot;
	}

	/** Smooth value noise in 0..1 (bilinear over a random lattice), for photo-like texture. */
	float ValueNoise(const TArray<float>& Lattice, int32 LatticeSize, float X, float Y)
	{
		const int32 X0 = FMath::FloorToInt(X) % LatticeSize;
		const int32 Y0 = FMath::FloorToInt(Y) % LatticeSize;
		const int32 X1 = (X0 + 1) % LatticeSize;
		const int32 Y1 = (Y0 + 1) % LatticeSize;
		const float FX = X - FMath::FloorToFloat(X);
		const float FY = Y - FMath::FloorToFloat(Y);
		const float Top = FMath::Lerp(Lattice[Y0 * LatticeSize + X0], Lattice[Y0 * LatticeSize + X1], FX);
		const float Bottom = FMath::Lerp(Lattice[Y1 * LatticeSize + X0], Lattice[Y1 * LatticeSize + X1], FX);
		return FMath::Lerp(Top, Bottom, FY);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageImageLookPhotoCheckTest, "StageCraft.ImageLook.PhotoCheck", StageImageLookTests::Flags)

bool FStageImageLookPhotoCheckTest::RunTest(const FString& Parameters)
{
	using namespace StageImageLookTests;
	constexpr int32 W = 512;
	constexpr int32 H = 360;
	FRandomStream Random(86);

	// --- Drawings: never photos, however they are lit or filled ---
	const FPicture Clean(W, H, [](int32 X, int32 Y) { const float V = IsPlanInk(X, Y, W, H, 3) ? 30.f : 245.f; return FVector3f(V); });
	const FPicture Uneven(W, H, [](int32 X, int32 Y)
	{
		const float Paper = 245.f - 90.f * X / W - 40.f * Y / H;
		return FVector3f(IsPlanInk(X, Y, W, H, 3) ? Paper - 170.f : Paper);
	});
	const FPicture Blackboard(W, H, [](int32 X, int32 Y) { const float V = IsPlanInk(X, Y, W, H, 3) ? 235.f : 40.f; return FVector3f(V); });
	const FPicture Dense(W, H, [](int32 X, int32 Y)
	{
		const bool bHatch = (X % 24 < 3 && Y > H / 4 && Y < H * 3 / 4) || (Y % 28 < 3 && X > W / 5 && X < W * 4 / 5);
		return FVector3f(IsPlanInk(X, Y, W, H, 3) || bHatch ? 30.f : 245.f);
	});
	const FPicture Filled(W, H, [](int32 X, int32 Y)
	{
		if (IsPlanInk(X, Y, W, H, 3))
		{
			return FVector3f(30.f);
		}
		const bool bRoom = X > W / 4 && X < W * 3 / 4 && Y > H / 4 && Y < H * 3 / 4;
		return bRoom ? (X < W / 2 ? FVector3f(120.f, 170.f, 230.f) : FVector3f(240.f, 200.f, 90.f)) : FVector3f(245.f);
	});
	// A phone photo of a pencil plan: grey strokes on grainy, unevenly lit paper, at twice the size (the check reduces it).
	const FPicture Pencil(W * 2, H * 2, [&Random](int32 X, int32 Y)
	{
		const float Paper = 215.f - 60.f * FMath::Abs(X - W) / W - 40.f * Y / (H * 2);
		const float Ink = IsPlanInk(X / 2, Y / 2, W, H, 3) ? -90.f : 0.f;
		return FVector3f(Paper + Ink + Random.FRandRange(-20.f, 20.f));
	});
	const TPair<const TCHAR*, const FPicture*> Drawings[] =
	{
		{ TEXT("clean"), &Clean }, { TEXT("unevenly lit"), &Uneven }, { TEXT("blackboard"), &Blackboard },
		{ TEXT("densely hatched"), &Dense }, { TEXT("colour-filled"), &Filled }, { TEXT("pencil photo"), &Pencil },
	};
	for (const TPair<const TCHAR*, const FPicture*>& Drawing : Drawings)
	{
		const FStageSketchImageMetrics Metrics = Drawing.Value->Measure();
		TestFalse(FString::Printf(TEXT("A %s drawing is not a photo (weak texture %.3f)"), Drawing.Key, Metrics.WeakTextureShare),
			StageSketchImageImport::LooksLikePhoto(Metrics));
	}

	// --- Photo-like: texture at several scales everywhere, in colour and in grey ---
	constexpr int32 LatticeSize = 64;
	TArray<float> Lattice;
	for (int32 Index = 0; Index < LatticeSize * LatticeSize; ++Index)
	{
		Lattice.Add(Random.FRand());
	}
	const auto Texture = [&Lattice](int32 X, int32 Y)
	{
		return 0.5f * ValueNoise(Lattice, LatticeSize, X / 40.f, Y / 40.f) + 0.3f * ValueNoise(Lattice, LatticeSize, X / 9.f + 17.f, Y / 9.f)
			+ 0.2f * ValueNoise(Lattice, LatticeSize, X / 3.f, Y / 3.f + 29.f);
	};
	const FPicture ColourPhoto(W, H, [&Texture](int32 X, int32 Y)
	{
		const float T = Texture(X, Y);
		return FVector3f(60.f + 160.f * T, 90.f + 120.f * Texture(X + 300, Y), 50.f + 150.f * (1.f - T));
	});
	const FPicture GreyPhoto(W, H, [&Texture](int32 X, int32 Y) { return FVector3f(30.f + 200.f * Texture(X, Y)); });
	const FStageSketchImageMetrics ColourMetrics = ColourPhoto.Measure();
	const FStageSketchImageMetrics GreyMetrics = GreyPhoto.Measure();
	TestTrue(FString::Printf(TEXT("A colour photo is a photo (weak texture %.3f)"), ColourMetrics.WeakTextureShare), StageSketchImageImport::LooksLikePhoto(ColourMetrics));
	TestTrue(FString::Printf(TEXT("A grey photo is a photo (weak texture %.3f)"), GreyMetrics.WeakTextureShare), StageSketchImageImport::LooksLikePhoto(GreyMetrics));
	TestTrue(TEXT("Colourfulness tells colour from grey"), ColourMetrics.Colourfulness > 20.f && GreyMetrics.Colourfulness < 1.f);

	// --- Invalid input ---
	TestEqual(TEXT("An empty image measures zero"), StageSketchImageImport::MeasureImage({}, 0, 0).WeakTextureShare, 0.f);
	TestEqual(TEXT("A size mismatch measures zero"), StageSketchImageImport::MeasureImage(Clean.BGRA, W + 1, H).WeakTextureShare, 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageImageLookPaletteTest, "StageCraft.ImageLook.Palette", StageImageLookTests::Flags)

bool FStageImageLookPaletteTest::RunTest(const FString& Parameters)
{
	using namespace StageImageLookTests;
	constexpr float Tolerance = 0.02f;

	// Three quarters deep red, one quarter dim blue.
	const FPicture TwoColours(400, 200, [](int32 X, int32 Y) { return X < 300 ? FVector3f(200.f, 20.f, 20.f) : FVector3f(10.f, 20.f, 90.f); });
	const TArray<FStageImageSwatch> Swatches = StageImagePalette::Extract(TwoColours.BGRA, TwoColours.Width, TwoColours.Height, 2);
	if (!TestEqual(TEXT("Two colours found"), Swatches.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("Largest share first"), Swatches[0].Share, 0.75f, Tolerance);
	TestEqual(TEXT("Second share"), Swatches[1].Share, 0.25f, Tolerance);
	TestEqual(TEXT("Red keeps its display colour"), Swatches[0].DisplayColor.R, 200.f / 255.f, Tolerance);
	TestEqual(TEXT("Red light is full red"), Swatches[0].LightColor.R, 1.f, Tolerance);
	TestTrue(TEXT("Red light has little green and blue"), Swatches[0].LightColor.G < 0.05f && Swatches[0].LightColor.B < 0.05f);
	TestEqual(TEXT("Blue light is full blue"), Swatches[1].LightColor.B, 1.f, Tolerance);
	TestTrue(TEXT("Red is brighter than the dim blue"), Swatches[0].Luminance > Swatches[1].Luminance);

	// Deterministic (Rules.md §54): the same pixels give the same palette.
	const TArray<FStageImageSwatch> Again = StageImagePalette::Extract(TwoColours.BGRA, TwoColours.Width, TwoColours.Height, 2);
	TestTrue(TEXT("Same pixels, same palette"), Again.Num() == Swatches.Num() && Again[0].DisplayColor.Equals(Swatches[0].DisplayColor)
		&& Again[1].DisplayColor.Equals(Swatches[1].DisplayColor));

	// Asking for more colours than the picture has: tiny clusters are dropped, never padded.
	const TArray<FStageImageSwatch> Many = StageImagePalette::Extract(TwoColours.BGRA, TwoColours.Width, TwoColours.Height, StageImagePalette::MaxColors);
	float Total = 0.f;
	for (const FStageImageSwatch& Swatch : Many)
	{
		Total += Swatch.Share;
		TestTrue(TEXT("No swatch under 2 %"), Swatch.Share >= 0.02f);
	}
	TestTrue(TEXT("At most the limit"), Many.Num() <= StageImagePalette::MaxColors);
	TestEqual(TEXT("Shares cover the picture"), Total, 1.f, Tolerance);

	// Black has no hue: white light at the lowest dimmer.
	const FPicture Black(16, 16, [](int32, int32) { return FVector3f(0.f); });
	const TArray<FStageImageSwatch> BlackSwatches = StageImagePalette::Extract(Black.BGRA, 16, 16, 1);
	TestTrue(TEXT("Black becomes white light"), BlackSwatches.Num() == 1 && BlackSwatches[0].LightColor.Equals(FLinearColor::White));

	// Dimmer from brightness.
	FStageImageSwatch Bright;
	Bright.Luminance = 0.5f;
	FStageImageSwatch Dark;
	Dark.Luminance = 0.f;
	TestEqual(TEXT("The brightest colour is full"), StageImagePalette::DimmerFor(Bright, 0.5f), 1.f, Tolerance);
	TestEqual(TEXT("The darkest keeps the floor"), StageImagePalette::DimmerFor(Dark, 0.5f), 0.25f, Tolerance);
	TestEqual(TEXT("No brightness at all is full"), StageImagePalette::DimmerFor(Dark, 0.f), 1.f, Tolerance);

	// Invalid input.
	TestEqual(TEXT("An empty image has no colours"), StageImagePalette::Extract({}, 0, 0, 3).Num(), 0);
	TestEqual(TEXT("A size mismatch has no colours"), StageImagePalette::Extract(Black.BGRA, 17, 16, 3).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageImageLookMusicStrobeTest, "StageCraft.ImageLook.MusicLook", StageImageLookTests::Flags)

bool FStageImageLookMusicStrobeTest::RunTest(const FString& Parameters)
{
	const TArray<FStageEffect> Look = StageEffectRules::MakeMusicLook();
	if (!TestEqual(TEXT("Three effects"), Look.Num(), 3))
	{
		return false;
	}
	TestTrue(TEXT("Bass drives the intensity"), Look[0].Target == EStageEffectTarget::Intensity && Look[0].Audio.Mode == EStageEffectAudioMode::Modulate
		&& Look[0].Audio.Band == EStageAudioBand::Bass);
	TestTrue(TEXT("Mids drive the colour's rate"), Look[1].Target == EStageEffectTarget::Color && Look[1].Audio.Band == EStageAudioBand::Mid
		&& Look[1].Audio.Modulates == EStageEffectAudioTarget::Rate);
	TestTrue(TEXT("Highs drive the shutter"), Look[2].Target == EStageEffectTarget::Beam && Look[2].Attribute == StageCraftTags::Attribute_Shutter
		&& Look[2].Audio.Band == EStageAudioBand::High && Look[2].Audio.Modulates == EStageEffectAudioTarget::Level);
	for (const FStageEffect& Effect : Look)
	{
		TestTrue(FString::Printf(TEXT("%s follows the music"), *Effect.Name.ToString()), Effect.UsesAudio());
	}

	// In silence (no signal, KILL or master 0) a music strobe rests open; every other effect keeps reading the band as full.
	const FStageEffect& Strobe = Look[2];
	TestEqual(TEXT("A music strobe is silent at 0"), StageEffects::GetSilentBand(Strobe), 0.f);
	TestEqual(TEXT("A bass pulse is silent at 1"), StageEffects::GetSilentBand(Look[0]), 1.f);
	TestEqual(TEXT("Master 0 rests the strobe"), StageEffects::ApplyAudioMaster(0.8f, 0.f, 0.f), 0.f);
	TestEqual(TEXT("Master 1 keeps the band"), StageEffects::ApplyAudioMaster(0.8f, 1.f, 0.f), 0.8f);
	TestEqual(TEXT("The old default still reads 1"), StageEffects::ApplyAudioMaster(0.2f, 0.f), 1.f);
	FStageEffect Modulated = Strobe;
	StageEffects::ApplyModulation(Modulated, StageEffects::GetSilentBand(Strobe));
	TestEqual(TEXT("A silent strobe's top is its low (open)"), Modulated.High, Strobe.Low);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
