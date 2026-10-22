// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for importing a photo or scan into Sketch to Stage (Docs/ADR/0017-multilingual-voice-and-photo-import.md §2.6-2.7): the
// local reading on synthetic drawings (outlines, risers, open lines, dots, circles, uneven light, a blackboard, noise, text-like
// marks), decoding (PNG with transparency, JPEG, garbage), the online reading's schema and validation, and an import through
// UStageSketchSubsystem in a real game world (worker thread, reference photo, cancel, refusals).
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Sketch; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/TaskGraphInterfaces.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Math/RandomStream.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Placement/StageDeckMath.h"
#include "Sketch/StageSketchImageImport.h"
#include "Sketch/StageSketchImageTrace.h"
#include "Sketch/StageSketchMath.h"
#include "Sketch/StageSketchSubsystem.h"
#include "Sketch/StageSketchVision.h"

namespace StageSketchImageTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	/** A sheet of paper to draw test pictures on. */
	struct FSheet
	{
		FStageSketchGrayImage Image;

		FSheet(int32 Width, int32 Height, uint8 Paper = 245)
		{
			Image.Width = Width;
			Image.Height = Height;
			Image.Pixels.Init(Paper, Width * Height);
		}

		void Dot(double X, double Y, double Radius, uint8 Ink)
		{
			for (int32 PY = FMath::FloorToInt(Y - Radius); PY <= FMath::CeilToInt(Y + Radius); ++PY)
			{
				for (int32 PX = FMath::FloorToInt(X - Radius); PX <= FMath::CeilToInt(X + Radius); ++PX)
				{
					if (PX >= 0 && PY >= 0 && PX < Image.Width && PY < Image.Height && FVector2D::Distance(FVector2D(PX + 0.5, PY + 0.5), FVector2D(X, Y)) <= Radius)
					{
						Image.Pixels[PY * Image.Width + PX] = Ink;
					}
				}
			}
		}

		void Line(const FVector2D& A, const FVector2D& B, double Thickness = 4.0, uint8 Ink = 30)
		{
			const int32 Steps = FMath::Max(1, FMath::CeilToInt(FVector2D::Distance(A, B)));
			for (int32 Step = 0; Step <= Steps; ++Step)
			{
				const FVector2D P = FMath::Lerp(A, B, double(Step) / Steps);
				Dot(P.X, P.Y, Thickness * 0.5, Ink);
			}
		}

		void Polyline(TConstArrayView<FVector2D> Points, bool bClosed, double Thickness = 4.0, uint8 Ink = 30)
		{
			for (int32 Index = 0; Index + 1 < Points.Num(); ++Index)
			{
				Line(Points[Index], Points[Index + 1], Thickness, Ink);
			}
			if (bClosed && Points.Num() > 2)
			{
				Line(Points.Last(), Points[0], Thickness, Ink);
			}
		}

		void Rect(double X0, double Y0, double X1, double Y1, double Thickness = 4.0, uint8 Ink = 30)
		{
			Polyline({ FVector2D(X0, Y0), FVector2D(X1, Y0), FVector2D(X1, Y1), FVector2D(X0, Y1) }, true, Thickness, Ink);
		}

		void Ring(double X, double Y, double Radius, double Thickness = 2.5, uint8 Ink = 30)
		{
			constexpr int32 Segments = 48;
			TArray<FVector2D> Points;
			for (int32 Index = 0; Index < Segments; ++Index)
			{
				const double Angle = 2.0 * UE_DOUBLE_PI * Index / Segments;
				Points.Add(FVector2D(X + Radius * FMath::Cos(Angle), Y + Radius * FMath::Sin(Angle)));
			}
			Polyline(Points, true, Thickness, Ink);
		}
	};

	int32 CountKind(const FStageSketchTraceResult& Result, EStageSketchElement Element)
	{
		int32 Count = 0;
		for (const FStageSketchStroke& Stroke : Result.Strokes)
		{
			Count += Stroke.GetElement() == Element ? 1 : 0;
		}
		return Count;
	}

	const FStageSketchStroke* FirstOf(const FStageSketchTraceResult& Result, EStageSketchElement Element)
	{
		return Result.Strokes.FindByPredicate([Element](const FStageSketchStroke& Stroke) { return Stroke.GetElement() == Element; });
	}

	FBox2D BoundsOf(const FStageSketchStroke& Stroke)
	{
		FBox2D Box(ForceInit);
		for (const FVector2D& Point : Stroke.Points)
		{
			Box += Point;
		}
		return Box;
	}

	/** Plan cm of a pixel of a Width x Height sheet traced WidthCm wide. */
	FVector2D Plan(double X, double Y, int32 Width, int32 Height, double WidthCm = 2000.0)
	{
		return StageSketchImageTrace::PixelToPlan(FVector2D(X, Y), Width, Height, WidthCm);
	}

	FStageSketchTraceResult TraceSheet(const FSheet& Sheet, double WidthCm = 2000.0)
	{
		FStageSketchTraceOptions Options;
		Options.WidthCm = WidthCm;
		Options.DeckHeight = 60.f;
		return StageSketchImageTrace::Trace(Sheet.Image, Options);
	}

	/** BGRA8 pixels of a gray sheet, fully opaque. */
	TArray<uint8> ToBGRA(const FStageSketchGrayImage& Image)
	{
		TArray<uint8> BGRA;
		for (const uint8 Gray : Image.Pixels)
		{
			BGRA.Append({ Gray, Gray, Gray, 255 });
		}
		return BGRA;
	}

	TArray64<uint8> Encode(EImageFormat Format, const TArray<uint8>& BGRA, int32 Width, int32 Height)
	{
		IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(Format);
		Wrapper->SetRaw(BGRA.GetData(), BGRA.Num(), Width, Height, ERGBFormat::BGRA, 8);
		return Wrapper->GetCompressed(Format == EImageFormat::JPEG ? 90 : 0);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSketchImageTraceTest, "StageCraft.Sketch.Image.Trace", StageSketchImageTests::Flags)

bool FStageSketchImageTraceTest::RunTest(const FString& Parameters)
{
	using namespace StageSketchImageTests;
	constexpr int32 W = 600;
	constexpr int32 H = 400;
	const double Scale = 2000.0 / W;

	// --- Mapping ---
	TestTrue(TEXT("Map: the centre is the origin"), Plan(300, 200, W, H).Equals(FVector2D::ZeroVector));
	TestTrue(TEXT("Map: image up is plan +X, image right plan +Y"), Plan(300, 0, W, H).Equals(FVector2D(200 * Scale, 0)) && Plan(600, 200, W, H).Equals(FVector2D(0, 300 * Scale)));

	// --- One deck outline ---
	{
		FSheet Sheet(W, H);
		Sheet.Rect(100, 100, 500, 300);
		const FStageSketchTraceResult Result = TraceSheet(Sheet);
		TestEqual(TEXT("Deck: one shape"), CountKind(Result, EStageSketchElement::Deck), 1);
		TestEqual(TEXT("Deck: nothing else"), Result.Strokes.Num(), 1);
		if (const FStageSketchStroke* Deck = FirstOf(Result, EStageSketchElement::Deck))
		{
			TestTrue(TEXT("Deck: closed shape"), Deck->Kind == EStageSketchStrokeKind::Shape && Deck->bClosed);
			TestTrue(TEXT("Deck: a rectangle is simplified to a few corners"), Deck->Points.Num() >= 4 && Deck->Points.Num() <= 8);
			const FBox2D Box = BoundsOf(*Deck);
			// The inside edge of a 4 px line around 100..500 x 100..300 px, at 3.33 cm per pixel.
			const FBox2D Expected(Plan(102, 298, W, H), Plan(498, 102, W, H));
			TestTrue(FString::Printf(TEXT("Deck: placed where it was drawn (%s vs %s)"), *Box.ToString(), *Expected.ToString()),
				Box.Min.Equals(Expected.Min, 25.0) && Box.Max.Equals(Expected.Max, 25.0));
			const double Area = FMath::Abs(StageDeckMath::GetSignedArea(Deck->Points));
			const double ExpectedArea = Expected.GetArea();
			TestTrue(FString::Printf(TEXT("Deck: area within 8%% (%.0f vs %.0f cm2)"), Area, ExpectedArea), FMath::Abs(Area - ExpectedArea) < 0.08 * ExpectedArea);
			TestEqual(TEXT("Deck: the panel's height"), Deck->DeckHeight, 60.f);
			FStageSketchStroke Clean = *Deck;
			TestTrue(TEXT("Deck: survives the sketch's own sanitizing"), StageSketchMath::SanitizeStroke(Clean) && StageDeckMath::IsSimplePolygon(Clean.Points));
		}
		const FStageSketchTraceResult Again = TraceSheet(Sheet);
		bool bSame = Again.Strokes.Num() == Result.Strokes.Num();
		for (int32 Index = 0; bSame && Index < Result.Strokes.Num(); ++Index)
		{
			bSame = Again.Strokes[Index].Points == Result.Strokes[Index].Points;
		}
		TestTrue(TEXT("Deterministic"), bSame);
		TestEqual(TEXT("Scale: twice the width doubles the plan"), BoundsOf(TraceSheet(Sheet, 4000.0).Strokes[0]).GetSize().X, BoundsOf(Result.Strokes[0]).GetSize().X * 2.0, 1.0);
	}

	// --- Wide side up: the enclosed area's first pixel can only be re-entered from the east, which once traced the edge forever ---
	{
		FSheet Sheet(W, H);
		// The sides slant 2 px inward per row, so below the first enclosed pixel and to its lower right there is ink.
		const TArray<FVector2D> Corners = { FVector2D(100, 80), FVector2D(500, 80), FVector2D(380, 140), FVector2D(220, 140) };
		Sheet.Polyline(Corners, true);
		const FStageSketchTraceResult Result = TraceSheet(Sheet);
		TestEqual(TEXT("Trapezoid: one deck"), CountKind(Result, EStageSketchElement::Deck), 1);
		if (const FStageSketchStroke* Deck = FirstOf(Result, EStageSketchElement::Deck))
		{
			TestTrue(FString::Printf(TEXT("Trapezoid: traced once round (%d corners)"), Deck->Points.Num()), Deck->Points.Num() >= 4 && Deck->Points.Num() <= 10);
			TArray<FVector2D> PlanCorners;
			for (const FVector2D& Corner : Corners)
			{
				PlanCorners.Add(Plan(Corner.X, Corner.Y, W, H));
			}
			const double Area = FMath::Abs(StageDeckMath::GetSignedArea(Deck->Points));
			const double Drawn = FMath::Abs(StageDeckMath::GetSignedArea(PlanCorners));
			// The inside edge of the line is a little smaller than the drawn centre line.
			TestTrue(FString::Printf(TEXT("Trapezoid: area (%.0f vs %.0f cm2)"), Area, Drawn), Area > 0.85 * Drawn && Area < Drawn);
		}
	}

	// --- A riser inside a stage ---
	{
		FSheet Sheet(W, H);
		Sheet.Rect(60, 60, 540, 340);
		Sheet.Rect(220, 140, 380, 260);
		const FStageSketchTraceResult Result = TraceSheet(Sheet);
		TestEqual(TEXT("Riser: two decks"), CountKind(Result, EStageSketchElement::Deck), 2);
		TArray<float> Heights;
		for (const FStageSketchStroke& Stroke : Result.Strokes)
		{
			Heights.Add(Stroke.DeckHeight);
		}
		Heights.Sort();
		TestTrue(TEXT("Riser: the inner deck stands one level higher"), Heights.Num() == 2 && Heights[0] == 60.f && Heights[1] == 120.f);
	}

	// --- An open line with a bend: a truss run ---
	{
		FSheet Sheet(W, H);
		Sheet.Polyline({ FVector2D(80, 320), FVector2D(80, 80), FVector2D(520, 80) }, false);
		const FStageSketchTraceResult Result = TraceSheet(Sheet);
		TestEqual(TEXT("Truss: one run"), CountKind(Result, EStageSketchElement::Truss), 1);
		TestEqual(TEXT("Truss: no deck"), CountKind(Result, EStageSketchElement::Deck), 0);
		if (const FStageSketchStroke* Truss = FirstOf(Result, EStageSketchElement::Truss))
		{
			TestEqual(TEXT("Truss: ends and the bend"), Truss->Points.Num(), 3);
			const FVector2D Ends[] = { Plan(80, 320, W, H), Plan(520, 80, W, H) };
			const bool bForward = Truss->Points[0].Equals(Ends[0], 30.0) && Truss->Points.Last().Equals(Ends[1], 30.0);
			const bool bBackward = Truss->Points[0].Equals(Ends[1], 30.0) && Truss->Points.Last().Equals(Ends[0], 30.0);
			TestTrue(TEXT("Truss: ends where it was drawn"), bForward || bBackward);
			TestTrue(TEXT("Truss: the bend"), Truss->Points.Num() == 3 && Truss->Points[1].Equals(Plan(80, 80, W, H), 30.0));
		}
	}

	// --- Fixtures: filled dots and a drawn circle ---
	{
		FSheet Sheet(W, H);
		const FVector2D Dots[] = { FVector2D(150, 200), FVector2D(300, 200), FVector2D(450, 200) };
		for (const FVector2D& Dot : Dots)
		{
			Sheet.Dot(Dot.X, Dot.Y, 6.0, 30);
		}
		Sheet.Ring(300, 320, 7.0);
		const FStageSketchTraceResult Result = TraceSheet(Sheet);
		TestEqual(TEXT("Fixtures: three dots and a circle"), CountKind(Result, EStageSketchElement::Fixture), 4);
		TestEqual(TEXT("Fixtures: nothing else"), Result.Strokes.Num(), 4);
		for (const FVector2D& Dot : Dots)
		{
			const FVector2D Expected = Plan(Dot.X, Dot.Y, W, H);
			TestTrue(TEXT("Fixtures: at the dot's centre"), Result.Strokes.ContainsByPredicate([&Expected](const FStageSketchStroke& Stroke) { return Stroke.Points[0].Equals(Expected, 8.0); }));
		}
	}

	// --- A deck with a fixture inside and a truss leaving its edge ---
	{
		FSheet Sheet(W, H);
		Sheet.Rect(100, 150, 350, 330);
		Sheet.Dot(225, 240, 6.0, 30);
		Sheet.Line(FVector2D(350, 240), FVector2D(560, 240));
		const FStageSketchTraceResult Result = TraceSheet(Sheet);
		TestEqual(TEXT("Mixed: one deck"), CountKind(Result, EStageSketchElement::Deck), 1);
		TestEqual(TEXT("Mixed: the dot inside the deck is a fixture, not part of the outline"), CountKind(Result, EStageSketchElement::Fixture), 1);
		TestEqual(TEXT("Mixed: the line leaving the deck is a truss run"), CountKind(Result, EStageSketchElement::Truss), 1);
		if (const FStageSketchStroke* Truss = FirstOf(Result, EStageSketchElement::Truss))
		{
			const FBox2D Box = BoundsOf(*Truss);
			TestTrue(FString::Printf(TEXT("Mixed: the run covers the line outside the deck (%s)"), *Box.ToString()), Box.Max.Y > Plan(540, 240, W, H).Y && Box.Min.Y > Plan(340, 240, W, H).Y);
		}
	}

	// --- Light, paper and noise ---
	{
		FSheet Sheet(W, H);
		// A shadow across the page: from white paper to mid grey, with the pen line 60 levels darker than the paper around it.
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				Sheet.Image.Pixels[Y * W + X] = uint8(250 - 130 * X / W);
			}
		}
		TArray<FVector2D> Corners = { FVector2D(100, 100), FVector2D(500, 100), FVector2D(500, 300), FVector2D(100, 300) };
		for (int32 Index = 0; Index < 4; ++Index)
		{
			const FVector2D A = Corners[Index];
			const FVector2D B = Corners[(Index + 1) % 4];
			const int32 Steps = int32(FVector2D::Distance(A, B));
			for (int32 Step = 0; Step <= Steps; ++Step)
			{
				const FVector2D P = FMath::Lerp(A, B, double(Step) / Steps);
				Sheet.Dot(P.X, P.Y, 2.0, uint8(250 - 130 * int32(P.X) / W - 60));
			}
		}
		FRandomStream Noise(7);
		for (int32 Index = 0; Index < 300; ++Index)
		{
			Sheet.Image.Pixels[Noise.RandRange(0, W * H - 1)] = 40;
		}
		const FStageSketchTraceResult Result = TraceSheet(Sheet);
		TestEqual(TEXT("Shadow and dust: still one deck"), CountKind(Result, EStageSketchElement::Deck), 1);
		TestEqual(TEXT("Shadow and dust: dust is not fixtures"), CountKind(Result, EStageSketchElement::Fixture), 0);
		TestEqual(TEXT("Shadow and dust: dust is not truss"), CountKind(Result, EStageSketchElement::Truss), 0);
	}
	{
		FSheet Board(W, H, 35);
		Board.Rect(100, 100, 500, 300, 4.0, 225);
		const FStageSketchTraceResult Result = TraceSheet(Board);
		TestTrue(TEXT("Blackboard: read inverted"), Result.bInverted);
		TestEqual(TEXT("Blackboard: one deck"), CountKind(Result, EStageSketchElement::Deck), 1);
	}
	{
		FSheet Sheet(W, H);
		// A handwritten "Z" next to a deck: small, open and thin, so neither a fixture nor a truss run.
		Sheet.Rect(100, 100, 400, 300);
		Sheet.Polyline({ FVector2D(470, 150), FVector2D(488, 150), FVector2D(470, 170), FVector2D(488, 170) }, false, 2.0);
		const FStageSketchTraceResult Result = TraceSheet(Sheet);
		TestEqual(TEXT("Text: the deck"), CountKind(Result, EStageSketchElement::Deck), 1);
		TestEqual(TEXT("Text: no fixture or truss"), Result.Strokes.Num(), 1);
		TestTrue(TEXT("Text: counted as left out"), Result.IgnoredMarks >= 1 && !Result.Warnings.IsEmpty());
	}
	{
		const FStageSketchTraceResult Blank = TraceSheet(FSheet(W, H));
		TestTrue(TEXT("Blank: nothing, with a hint"), Blank.Strokes.IsEmpty() && !Blank.Warnings.IsEmpty());
		const FStageSketchTraceResult Tiny = TraceSheet(FSheet(5, 5));
		TestTrue(TEXT("Tiny: refused with a reason"), Tiny.Strokes.IsEmpty() && !Tiny.Warnings.IsEmpty());
		TestTrue(TEXT("Invalid image: no ink"), [] { bool bInverted = false; return StageSketchImageTrace::FindInk(FStageSketchGrayImage(), bInverted).IsEmpty(); }());
	}

	// --- Scaling down ---
	{
		FSheet Large(2000, 1000);
		for (int32 X = 0; X < 1000; ++X)
		{
			for (int32 Y = 0; Y < 1000; ++Y)
			{
				Large.Image.Pixels[Y * 2000 + X] = 0;
			}
		}
		const FStageSketchGrayImage Small = StageSketchImageTrace::Downscale(Large.Image, 800);
		TestTrue(TEXT("Downscale: longest side 800, aspect kept"), Small.Width == 800 && Small.Height == 400 && Small.IsValid());
		TestTrue(TEXT("Downscale: averages"), Small.At(10, 10) == 0 && Small.At(790, 10) == 245);
		TestEqual(TEXT("Downscale: a small image is kept"), StageSketchImageTrace::Downscale(FSheet(300, 200).Image, 800).Width, 300);

		FSheet Big(1600, 1200);
		Big.Rect(400, 300, 1200, 900, 8.0);
		const FStageSketchTraceResult Result = TraceSheet(Big);
		TestEqual(TEXT("Large photo: traced after scaling down"), CountKind(Result, EStageSketchElement::Deck), 1);
		if (Result.Strokes.Num() == 1)
		{
			const FBox2D Box = BoundsOf(Result.Strokes[0]);
			TestTrue(FString::Printf(TEXT("Large photo: the same plan size (%s)"), *Box.ToString()), FMath::IsNearlyEqual(Box.GetSize().Y, 1000.0, 40.0));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSketchImageFileTest, "StageCraft.Sketch.Image.Files", StageSketchImageTests::Flags)

bool FStageSketchImageFileTest::RunTest(const FString& Parameters)
{
	using namespace StageSketchImageTests;
	FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

	FSheet Sheet(400, 300);
	Sheet.Rect(80, 60, 320, 240);
	const TArray<uint8> BGRA = ToBGRA(Sheet.Image);

	// PNG with a transparent background: transparent is paper.
	{
		TArray<uint8> Transparent = BGRA;
		for (int32 Pixel = 0; Pixel < 400 * 300; ++Pixel)
		{
			if (Sheet.Image.Pixels[Pixel] > 128)
			{
				Transparent[Pixel * 4 + 0] = Transparent[Pixel * 4 + 1] = Transparent[Pixel * 4 + 2] = 0;
				Transparent[Pixel * 4 + 3] = 0;
			}
		}
		const TArray64<uint8> Png = Encode(EImageFormat::PNG, Transparent, 400, 300);
		TArray<uint8> Decoded;
		int32 Width = 0;
		int32 Height = 0;
		FText Error;
		TestTrue(TEXT("PNG: decoded"), StageSketchImageImport::DecodeImage(Png, Decoded, Width, Height, Error) && Width == 400 && Height == 300);
		TestTrue(TEXT("PNG: transparent paper became white"), Decoded.Num() == 400 * 300 * 4 && Decoded[0] == 255 && Decoded[3] == 255);
		const FStageSketchTraceResult Result = StageSketchImageTrace::Trace(StageSketchImageImport::ToGray(Decoded, Width, Height), FStageSketchTraceOptions());
		TestEqual(TEXT("PNG: traced"), CountKind(Result, EStageSketchElement::Deck), 1);
	}
	{
		const TArray64<uint8> Jpeg = Encode(EImageFormat::JPEG, BGRA, 400, 300);
		TArray<uint8> Decoded;
		int32 Width = 0;
		int32 Height = 0;
		FText Error;
		TestTrue(TEXT("JPEG: decoded"), StageSketchImageImport::DecodeImage(Jpeg, Decoded, Width, Height, Error));
		TestEqual(TEXT("JPEG: traced"), CountKind(StageSketchImageTrace::Trace(StageSketchImageImport::ToGray(Decoded, Width, Height), FStageSketchTraceOptions()), EStageSketchElement::Deck), 1);
	}
	{
		const TArray<uint8> Garbage = { 'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'a', 'g', 'e' };
		TArray<uint8> Decoded;
		int32 Width = 0;
		int32 Height = 0;
		FText Error;
		TestFalse(TEXT("Garbage: refused"), StageSketchImageImport::DecodeImage(TArray64<uint8>(Garbage.GetData(), Garbage.Num()), Decoded, Width, Height, Error));
		TestTrue(TEXT("Garbage: says why"), Error.ToString().Contains(TEXT("PNG")));
		TestTrue(TEXT("Extensions"), StageSketchImageImport::HasSupportedExtension(TEXT("C:/a/plan.JPG")) && StageSketchImageImport::HasSupportedExtension(TEXT("plan.png"))
			&& !StageSketchImageImport::HasSupportedExtension(TEXT("plan.gif")) && !StageSketchImageImport::HasSupportedExtension(TEXT("plan")));
	}

	// The whole worker job on a file.
	const FString Folder = FPaths::ProjectSavedDir() / TEXT("Automation") / TEXT("SketchImage");
	const FString File = Folder / TEXT("Plan.png");
	TestTrue(TEXT("Job: test file written"), FFileHelper::SaveArrayToFile(Encode(EImageFormat::PNG, BGRA, 400, 300), *File));
	{
		FStageSketchImportJob Job;
		Job.FilePath = File;
		Job.bWantVisionImage = true;
		StageSketchImageImport::Run(Job);
		TestTrue(TEXT("Job: decoded"), Job.bDecoded && Job.SourceWidth == 400 && Job.SourceHeight == 300);
		TestTrue(TEXT("Job: preview"), Job.PreviewWidth == 400 && Job.Preview.Num() == 400 * 300 * 4);
		TestTrue(TEXT("Job: image for the online reading"), Job.VisionJpeg.Num() > 100 && Job.VisionJpeg[0] == 0xFF && Job.VisionJpeg[1] == 0xD8);
		TestEqual(TEXT("Job: traced"), Job.Trace.DeckCount, 1);

		FStageSketchImportJob Missing;
		Missing.FilePath = Folder / TEXT("Missing.png");
		StageSketchImageImport::Run(Missing);
		TestTrue(TEXT("Job: a missing file fails with a reason"), !Missing.bDecoded && !Missing.Error.IsEmpty());
	}
	IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists*/ false, /*Tree*/ true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSketchVisionTest, "StageCraft.Sketch.Image.Vision", StageSketchImageTests::Flags)

bool FStageSketchVisionTest::RunTest(const FString& Parameters)
{
	using EStatus = FStageSketchVisionResult::EStatus;
	const FString Tool = StageSketchVision::MakeToolDefinitionJson();
	TestTrue(TEXT("Tool: name, strict, closed schema"), Tool.Contains(TEXT("\"name\":\"sketch_plan\"")) && Tool.Contains(TEXT("\"strict\":true")) && Tool.Contains(TEXT("\"additionalProperties\":false")));
	TestTrue(TEXT("Tool: decks, truss and fixtures required"), Tool.Contains(TEXT("\"required\":[\"decks\",\"truss\",\"fixtures\"]")));

	FStageAIModelOptions Options;
	const FString Body = StageSketchVision::BuildRequestBody(Options, TEXT("QUJD"));
	TestTrue(TEXT("Request: the image"), Body.Contains(TEXT("\"type\":\"image\"")) && Body.Contains(TEXT("\"media_type\":\"image/jpeg\"")) && Body.Contains(TEXT("\"data\":\"QUJD\"")));
	TestTrue(TEXT("Request: model, fallbacks, auto tool choice"), Body.Contains(TEXT("\"model\":\"claude-opus-5-5\"")) && Body.Contains(TEXT("\"fallbacks\":\"default\""))
		&& Body.Contains(TEXT("\"type\":\"auto\"")) && !Body.Contains(TEXT("\"thinking\"")));
	TestTrue(TEXT("Request: reading a drawing is never low effort"), Body.Contains(TEXT("\"effort\":\"medium\"")));
	Options.Effort = TEXT("high");
	TestTrue(TEXT("Request: a higher effort is kept"), StageSketchVision::BuildRequestBody(Options, TEXT("x")).Contains(TEXT("\"effort\":\"high\"")));

	const auto Respond = [](const FString& Input)
	{
		return StageSketchVision::ParseResponseBody(200, FString::Printf(TEXT("{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"text\",\"text\":\"Reading.\"},")
			TEXT("{\"type\":\"tool_use\",\"id\":\"t\",\"name\":\"sketch_plan\",\"input\":%s}]}"), *Input));
	};
	{
		const FStageSketchVisionResult Plan = Respond(TEXT("{\"decks\":[{\"corners\":[{\"x\":0.1,\"y\":0.2},{\"x\":0.9,\"y\":0.2},{\"x\":0.9,\"y\":0.8},{\"x\":0.1,\"y\":0.8}],\"height_cm\":80}],")
			TEXT("\"truss\":[{\"points\":[{\"x\":0.05,\"y\":0.1},{\"x\":0.95,\"y\":0.1}],\"closed\":false}],\"fixtures\":[{\"x\":0.5,\"y\":0.5},{\"x\":1.01,\"y\":0.5}],\"notes\":\"Left riser unclear.\"}"));
		TestTrue(TEXT("Plan: read"), Plan.Status == EStatus::Plan && Plan.Strokes.Num() == 4);
		TestTrue(TEXT("Plan: deck with its height"), Plan.Strokes.Num() > 0 && Plan.Strokes[0].Kind == EStageSketchStrokeKind::Shape && Plan.Strokes[0].DeckHeight == 80.f);
		TestTrue(TEXT("Plan: slightly outside is clamped"), Plan.Strokes.Num() == 4 && Plan.Strokes[3].Points[0].Equals(FVector2D(1.0, 0.5)));
		TestEqual(TEXT("Plan: notes"), Plan.Text, FString(TEXT("Left riser unclear.")));
		const TArray<FStageSketchStroke> OnPlan = StageSketchVision::ToPlan(Plan.Strokes, 600, 400, 2000.0, 60.f);
		TestTrue(TEXT("Plan: the centre is the origin"), OnPlan.Num() == 4 && OnPlan[2].Points[0].Equals(FVector2D::ZeroVector));
		TestTrue(TEXT("Plan: same placement as the local reading"), OnPlan.Num() == 4 && OnPlan[0].Points[0].Equals(StageSketchImageTrace::PixelToPlan(FVector2D(60, 80), 600, 400, 2000.0)));
	}
	{
		const FStageSketchVisionResult Plan = Respond(TEXT("{\"decks\":[{\"corners\":[{\"x\":0.1,\"y\":0.2},{\"x\":0.9,\"y\":0.2}]},{\"corners\":[{\"x\":0.1,\"y\":0.2},{\"x\":5,\"y\":0.2},{\"x\":0.5,\"y\":0.9},{\"x\":0.1,\"y\":0.9}]}],")
			TEXT("\"truss\":[],\"fixtures\":[{\"x\":\"left\",\"y\":0.5},{\"x\":0.3,\"y\":0.3}]}"));
		TestTrue(TEXT("Hostile: bad elements dropped, good ones kept"), Plan.Status == EStatus::Plan && Plan.Strokes.Num() == 2 && Plan.Dropped == 3);
		const FStageSketchVisionResult Height = Respond(TEXT("{\"decks\":[{\"corners\":[{\"x\":0.1,\"y\":0.1},{\"x\":0.5,\"y\":0.1},{\"x\":0.5,\"y\":0.5}],\"height_cm\":-5}],\"truss\":[],\"fixtures\":[]}"));
		TestTrue(TEXT("Hostile: a negative height means not given"), Height.Strokes.Num() == 1 && Height.Strokes[0].DeckHeight == 0.f);
		TestEqual(TEXT("Hostile: no height becomes the panel's"), StageSketchVision::ToPlan(Height.Strokes, 100, 100, 1000.0, 60.f)[0].DeckHeight, 60.f);
		TestTrue(TEXT("Hostile: unknown field"), Respond(TEXT("{\"decks\":[],\"truss\":[],\"fixtures\":[],\"script\":\"x\"}")).Status == EStatus::Failed);
		FString ManyFixtures = TEXT("{\"decks\":[],\"truss\":[],\"fixtures\":[");
		for (int32 Index = 0; Index < 120; ++Index)
		{
			ManyFixtures += FString::Printf(TEXT("%s{\"x\":0.5,\"y\":0.5}"), Index == 0 ? TEXT("") : TEXT(","));
		}
		ManyFixtures += TEXT("]}");
		const FStageSketchVisionResult Many = Respond(ManyFixtures);
		TestTrue(TEXT("Hostile: capped"), Many.Strokes.Num() == StageSketchImageTrace::MaxFixtures && Many.Dropped == 120 - StageSketchImageTrace::MaxFixtures);
	}
	TestTrue(TEXT("Text answer"), StageSketchVision::ParseResponseBody(200, TEXT("{\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"This is a photo of a cat.\"}]}")).Status == EStatus::Text);
	TestTrue(TEXT("Refusal"), StageSketchVision::ParseResponseBody(200, TEXT("{\"stop_reason\":\"refusal\",\"content\":[]}")).Status == EStatus::Refused);
	const FStageSketchVisionResult Unauthorized = StageSketchVision::ParseResponseBody(401, TEXT("{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\",\"message\":\"invalid x-api-key\"}}"));
	TestTrue(TEXT("401 with the message"), Unauthorized.Status == EStatus::Failed && Unauthorized.Error.ToString().Contains(TEXT("invalid x-api-key")));
	TestTrue(TEXT("Malformed"), StageSketchVision::ParseResponseBody(200, TEXT("not json")).Status == EStatus::Failed);
	TestTrue(TEXT("System prompt: Armenian labels"), StageSketchVision::GetSystemPrompt().Contains(TEXT("Armenian")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSketchImportWorldTest, "StageCraft.Sketch.Image.Import", StageSketchImageTests::Flags)

bool FStageSketchImportWorldTest::RunTest(const FString& Parameters)
{
	using namespace StageSketchImageTests;
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageSketchImportTestWorld"));
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	World->InitializeActorsForPlay(FURL());
	World->BeginPlay();
	ON_SCOPE_EXIT
	{
		World->BeginTearingDown();
		World->EndPlay(EEndPlayReason::Quit);
		World->DestroyWorld(/*bInformEngineOfWorld*/ false);
		GEngine->DestroyWorldContext(World);
	};
	UStageSketchSubsystem* Sketch = UWorld::GetSubsystem<UStageSketchSubsystem>(World);
	if (!TestNotNull(TEXT("The sketch exists in a game world"), Sketch))
	{
		return false;
	}

	// The worker's result comes back as a game-thread task: run them until the import ends.
	const auto WaitForImport = [Sketch]()
	{
		const double Deadline = FPlatformTime::Seconds() + 20.0;
		while (Sketch->IsImportPending() && FPlatformTime::Seconds() < Deadline)
		{
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FPlatformProcess::Sleep(0.005f);
		}
		return !Sketch->IsImportPending();
	};

	FSheet Sheet(600, 400);
	Sheet.Rect(100, 100, 500, 300);
	Sheet.Line(FVector2D(60, 50), FVector2D(540, 50));
	Sheet.Dot(300, 200, 6.0, 30);
	const FString Folder = FPaths::ProjectSavedDir() / TEXT("Automation") / TEXT("SketchImport");
	const FString File = Folder / TEXT("Stage.png");
	FFileHelper::SaveArrayToFile(Encode(EImageFormat::PNG, ToBGRA(Sheet.Image), 600, 400), *File);

	FStageSketchSettings Settings = Sketch->GetSettings();
	Settings.PhotoWidth = 1200.f;
	Sketch->SetSettings(Settings);

	FText Error;
	TestTrue(TEXT("Import: started"), Sketch->ImportImage(File, EStageSketchReader::Local, Error));
	TestTrue(TEXT("Import: pending while the worker reads"), Sketch->IsImportPending());
	TestFalse(TEXT("Import: a second one is refused"), Sketch->ImportImage(File, EStageSketchReader::Local, Error));
	TestTrue(TEXT("Import: finished"), WaitForImport());
	TestTrue(FString::Printf(TEXT("Import: succeeded (%s)"), *Sketch->GetImportMessage().ToString()), Sketch->WasImportSuccessful());
	TestEqual(TEXT("Import: a deck"), Sketch->CountStrokes(EStageSketchElement::Deck), 1);
	TestEqual(TEXT("Import: a truss run"), Sketch->CountStrokes(EStageSketchElement::Truss), 1);
	TestEqual(TEXT("Import: a fixture"), Sketch->CountStrokes(EStageSketchElement::Fixture), 1);
	TestTrue(TEXT("Import: the photo is under the canvas"), Sketch->GetReferenceImage() && Sketch->IsReferenceVisible());
	TestTrue(TEXT("Import: the photo spans the chosen width"), FMath::IsNearlyEqual(Sketch->GetReferenceBounds().GetSize().Y, 1200.0, 1.0)
		&& FMath::IsNearlyEqual(Sketch->GetReferenceBounds().GetSize().X, 800.0, 1.0));
	const uint32 Finished = Sketch->GetImportsFinished();

	Sketch->SetReferenceVisible(false);
	TestFalse(TEXT("Photo: hidden"), Sketch->IsReferenceVisible());
	Sketch->ClearReference();
	TestNull(TEXT("Photo: removed, strokes kept"), Sketch->GetReferenceImage());
	TestEqual(TEXT("Photo: strokes kept"), Sketch->GetStrokes().Num(), 3);

	// Cancelled: the worker's late result is dropped.
	const int32 Before = Sketch->GetStrokes().Num();
	TestTrue(TEXT("Cancel: started"), Sketch->ImportImage(File, EStageSketchReader::Local, Error));
	Sketch->CancelImport();
	TestFalse(TEXT("Cancel: idle at once"), Sketch->IsImportPending());
	FPlatformProcess::Sleep(0.3f);
	FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	TestEqual(TEXT("Cancel: nothing added"), Sketch->GetStrokes().Num(), Before);
	TestTrue(TEXT("Cancel: counted as finished"), Sketch->GetImportsFinished() == Finished + 1 && !Sketch->WasImportSuccessful());

	TestFalse(TEXT("Refused: unsupported format"), Sketch->ImportImage(Folder / TEXT("Stage.gif"), EStageSketchReader::Local, Error));
	TestFalse(TEXT("Refused: missing file"), Sketch->ImportImage(Folder / TEXT("Missing.png"), EStageSketchReader::Local, Error));
	FText Offline;
	if (!Sketch->CanReadOnline(Offline))
	{
		TestFalse(TEXT("Refused: Claude while the online model is off"), Sketch->ImportImage(File, EStageSketchReader::Claude, Error));
		TestTrue(TEXT("Refused: says why"), Error.EqualTo(Offline));
	}

	// A file that is not an image fails on the worker with a reason, and adds nothing.
	const FString Fake = Folder / TEXT("Fake.jpg");
	FFileHelper::SaveStringToFile(TEXT("not a picture"), *Fake);
	TestTrue(TEXT("Damaged: started"), Sketch->ImportImage(Fake, EStageSketchReader::Local, Error));
	TestTrue(TEXT("Damaged: finished"), WaitForImport());
	TestTrue(TEXT("Damaged: failed with a reason"), !Sketch->WasImportSuccessful() && Sketch->GetImportMessage().ToString().Contains(TEXT("Fake.jpg")));
	TestEqual(TEXT("Damaged: nothing added"), Sketch->GetStrokes().Num(), Before);

	IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists*/ false, /*Tree*/ true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
