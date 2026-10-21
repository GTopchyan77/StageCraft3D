// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for Sketch to Stage (Docs/ADR/0015-sketch-to-stage.md): draw strokes into the sketch through the same
// validated AddStroke the canvas uses, change settings, report the plan, and generate through the controller's request (the same
// path as the GENERATE 3D button). Coordinates are plan metres (X up, Y right in the canvas). Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Actors/StageDeckActor.h"
#include "Data/BaseItemData.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UI/StageSketchCanvasWidget.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/SWindow.h"
#include "ModularSceneBuilder.h"
#include "Player/ModularPlayerController.h"
#include "Sketch/StageSketchGenerator.h"
#include "Sketch/StageSketchSubsystem.h"
#include "Subsystems/StageSessionSubsystem.h"

namespace StageSketchCommands
{
	UStageSketchSubsystem* GetSketch(UWorld* World)
	{
		UStageSketchSubsystem* Sketch = UWorld::GetSubsystem<UStageSketchSubsystem>(World);
		if (!Sketch)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Sketch: no sketch in this world (Game and PIE worlds only)."));
		}
		return Sketch;
	}

	/** "Shape 0,0 4,0 4,2 0,2 [h=80]": the kind, plan points in metres, and an optional deck height in cm. */
	void Add(const TArray<FString>& Args, UWorld* World)
	{
		UStageSketchSubsystem* Sketch = GetSketch(World);
		if (!Sketch || Args.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Sketch.Add Freehand|Shape|Truss|Fixture x,y [x,y ...] [closed] [h=<cm>]   (x, y in metres; x:y also works inside -ExecCmds)"));
			return;
		}
		FStageSketchStroke Stroke;
		const int64 Kind = StaticEnum<EStageSketchStrokeKind>()->GetValueByNameString(Args[0]);
		if (Kind == INDEX_NONE)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Sketch.Add: unknown kind \"%s\"."), *Args[0]);
			return;
		}
		Stroke.Kind = EStageSketchStrokeKind(Kind);
		Stroke.DeckHeight = Sketch->GetSettings().DeckHeight;
		for (int32 Index = 1; Index < Args.Num(); ++Index)
		{
			FString X, Y;
			if (Args[Index].Equals(TEXT("closed"), ESearchCase::IgnoreCase))
			{
				Stroke.bClosed = true;
			}
			else if (Args[Index].StartsWith(TEXT("h=")))
			{
				Stroke.DeckHeight = FCString::Atof(*Args[Index].RightChop(2));
			}
			else if (Args[Index].Replace(TEXT(":"), TEXT(",")).Split(TEXT(","), &X, &Y))
			{
				Stroke.Points.Add(FVector2D(FCString::Atod(*X) * 100.0, FCString::Atod(*Y) * 100.0));
			}
		}
		if (Stroke.bClosed && Stroke.Kind == EStageSketchStrokeKind::Truss && !Stroke.Points.IsEmpty())
		{
			const FVector2D First = Stroke.Points[0];
			Stroke.Points.Add(First); // How the canvas closes a truss loop: back on the first point.
		}
		const bool bAdded = Sketch->AddStroke(Stroke);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Sketch.Add %s: %s (%d strokes)."), *Args[0], bAdded ? TEXT("added") : TEXT("refused"), Sketch->GetStrokes().Num());
	}

	void Status(const TArray<FString>& Args, UWorld* World)
	{
		const UStageSketchSubsystem* Sketch = GetSketch(World);
		if (!Sketch)
		{
			return;
		}
		const FStageSketchSettings& Settings = Sketch->GetSettings();
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Sketch.Status: %d strokes; deck %.0f cm, truss %.0f cm, grid %.0f cm, snap %s, smoothing %.2f; items deck \"%s\", truss \"%s\", fixture \"%s\"; %s."),
			Sketch->GetStrokes().Num(), Settings.DeckHeight, Settings.TrussHeight, Settings.GridStep, Settings.bSnapToGrid ? TEXT("on") : TEXT("off"), Settings.Smoothing,
			*GetNameSafe(Sketch->GetItem(EStageSketchElement::Deck)), *GetNameSafe(Sketch->GetItem(EStageSketchElement::Truss)), *GetNameSafe(Sketch->GetItem(EStageSketchElement::Fixture)),
			Sketch->IsGenerationPending() ? TEXT("generation pending") : TEXT("idle"));
		for (int32 Index = 0; Index < Sketch->GetStrokes().Num(); ++Index)
		{
			const FStageSketchStroke& Stroke = Sketch->GetStrokes()[Index];
			UE_LOG(LogStageCraft, Display, TEXT("  stroke %d: %s, %d points%s, becomes %s"), Index + 1, *UEnum::GetValueAsString(Stroke.Kind), Stroke.Points.Num(),
				Stroke.bClosed ? TEXT(", closed") : TEXT(""), *UEnum::GetValueAsString(Stroke.GetElement()));
		}
		const FStageSketchBuildPlan Plan = Sketch->MakePlan();
		UE_LOG(LogStageCraft, Display, TEXT("  plan: %s"), *StageSketchGenerator::DescribePlan(Plan).ToString());
		for (const FText& Warning : Plan.Warnings)
		{
			UE_LOG(LogStageCraft, Display, TEXT("  warning: %s"), *Warning.ToString());
		}
		if (const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(World))
		{
			for (const AModularBaseActor* Item : Session->GetPlacedItems())
			{
				if (const AStageDeckActor* Deck = Cast<AStageDeckActor>(Item))
				{
					UE_LOG(LogStageCraft, Display, TEXT("  deck %s at %s: %d corners, %.2f m2, height %.0f cm, %d panels, %d triangles."), *Deck->GetName(),
						*Deck->GetActorLocation().ToCompactString(), Deck->GetOutline().Num(), Deck->GetDeckArea() * 0.0001, Deck->GetDeckHeight(), Deck->GetQuantity(), Deck->GetMeshTriangleCount());
				}
			}
		}
	}

	void Set(const TArray<FString>& Args, UWorld* World)
	{
		UStageSketchSubsystem* Sketch = GetSketch(World);
		if (!Sketch || Args.Num() < 2)
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Sketch.Set DeckHeight|TrussHeight|Grid <cm> | Snap 0|1 | Smoothing <0..0.9>"));
			return;
		}
		FStageSketchSettings Settings = Sketch->GetSettings();
		const float Value = FCString::Atof(*Args[1]);
		if (Args[0] == TEXT("DeckHeight")) { Settings.DeckHeight = Value; }
		else if (Args[0] == TEXT("TrussHeight")) { Settings.TrussHeight = Value; }
		else if (Args[0] == TEXT("Grid")) { Settings.GridStep = Value; }
		else if (Args[0] == TEXT("Snap")) { Settings.bSnapToGrid = Value != 0.f; }
		else if (Args[0] == TEXT("Smoothing")) { Settings.Smoothing = Value; }
		Sketch->SetSettings(Settings);
		Status({}, World);
	}

	void Clear(const TArray<FString>& Args, UWorld* World)
	{
		if (UStageSketchSubsystem* Sketch = GetSketch(World))
		{
			Sketch->ClearStrokes();
		}
	}

	void UndoStroke(const TArray<FString>& Args, UWorld* World)
	{
		if (UStageSketchSubsystem* Sketch = GetSketch(World))
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Sketch.UndoStroke: %s."), Sketch->RemoveLastStroke() ? TEXT("removed") : TEXT("nothing to remove"));
		}
	}

	void Generate(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Sketch.Generate: %s."),
			Controller ? (Controller->RequestGenerateFromSketch() ? TEXT("started") : TEXT("refused")) : TEXT("no AModularPlayerController"));
	}

	/** Saves the window that hosts the Sketch panel (a floating pop-up window by default) as Saved/Screenshots/Sketch_<time>.png. */
	void Screenshot(const TArray<FString>& Args, UWorld* World)
	{
		for (TObjectIterator<UStageSketchCanvasWidget> It; It; ++It)
		{
			const TSharedPtr<SWidget> Widget = It->GetCachedWidget();
			const TSharedPtr<SWindow> Window = Widget.IsValid() ? FSlateApplication::Get().FindWidgetWindow(Widget.ToSharedRef()) : nullptr;
			if (!Window.IsValid())
			{
				continue;
			}
			TArray<FColor> Pixels;
			FIntVector Size;
			if (!FSlateApplication::Get().TakeScreenshot(Window.ToSharedRef(), Pixels, Size) || Pixels.IsEmpty())
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Sketch.Screenshot: capture failed."));
				return;
			}
			for (FColor& Pixel : Pixels)
			{
				Pixel.A = 255;
			}
			TArray64<uint8> Png;
			FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
			const FString File = FPaths::ProjectSavedDir() / TEXT("Screenshots") / FString::Printf(TEXT("Sketch_%s.png"), *FDateTime::Now().ToString(TEXT("%H%M%S")));
			const bool bSaved = FFileHelper::SaveArrayToFile(Png, *File);
			const bool bMain = Window == FSlateApplication::Get().GetActiveTopLevelRegularWindow() && Window->GetTitle().ToString().Contains(TEXT("ModularSceneBuilder"));
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Sketch.Screenshot: window \"%s\" %dx%d at %s%s, %s %s."), *Window->GetTitle().ToString(), Size.X, Size.Y,
				*Window->GetPositionInScreen().ToString(), bMain ? TEXT(" (main window)") : TEXT(""), bSaved ? TEXT("saved") : TEXT("NOT saved"), *File);
			return;
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Sketch.Screenshot: the Sketch window is not open."));
	}

	FAutoConsoleCommandWithWorldAndArgs ScreenshotCommand(TEXT("StageCraft.Sketch.Screenshot"),
		TEXT("Saves the Sketch window (wherever it is docked or floating) to Saved/Screenshots."), FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Screenshot));
	FAutoConsoleCommandWithWorldAndArgs AddCommand(TEXT("StageCraft.Sketch.Add"),
		TEXT("Freehand|Shape|Truss|Fixture x,y [x,y ...] [closed] [h=<cm>]  Adds a stroke to the sketch (plan metres)."), FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Add));
	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.Sketch.Status"),
		TEXT("Lists the sketch, its settings, the plan and its warnings, and the decks on stage."), FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
	FAutoConsoleCommandWithWorldAndArgs SetCommand(TEXT("StageCraft.Sketch.Set"),
		TEXT("DeckHeight|TrussHeight|Grid <cm> | Snap 0|1 | Smoothing <0..0.9>  Changes a sketch setting."), FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Set));
	FAutoConsoleCommandWithWorldAndArgs ClearCommand(TEXT("StageCraft.Sketch.Clear"),
		TEXT("Empties the sketch (the stage is unchanged)."), FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Clear));
	FAutoConsoleCommandWithWorldAndArgs UndoStrokeCommand(TEXT("StageCraft.Sketch.UndoStroke"),
		TEXT("Removes the newest stroke."), FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&UndoStroke));
	FAutoConsoleCommandWithWorldAndArgs GenerateCommand(TEXT("StageCraft.Sketch.Generate"),
		TEXT("Generates the sketch through the controller (rules, all or nothing, one undo step)."), FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Generate));
}

#endif // !UE_BUILD_SHIPPING
