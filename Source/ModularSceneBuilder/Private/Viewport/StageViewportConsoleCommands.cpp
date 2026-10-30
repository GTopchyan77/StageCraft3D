// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for the viewport toolbar (Docs/ADR/0008). Each one calls the same setter the toolbar calls,
// so a scripted run exercises the real path. Zoom and Pan are the wheel's and the right-drag's own calls, with the
// deprojected world point logged before and after so the zoom anchor can be checked. Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Actors/ModularTransformGizmo.h"
#include "Camera/PlayerCameraManager.h"
#include "Containers/Ticker.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Pawn.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "DynamicRHI.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "LegacyScreenPercentageDriver.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "Widgets/SViewport.h"
#include "ModularSceneBuilder.h"
#include "Placement/StagePlacementToolComponent.h"
#include "Placement/StageSnappingComponent.h"
#include "Player/ModularPlayerController.h"
#include "Viewport/StageGridActor.h"
#include "Viewport/StageViewportComponent.h"

namespace StageViewportCommands
{
	AModularPlayerController* GetController(UWorld* World)
	{
		AModularPlayerController* Controller = World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
		if (!Controller || !Controller->GetViewportView())
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.View: no AModularPlayerController in this world."));
			return nullptr;
		}
		return Controller;
	}

	template <typename EnumType>
	bool ParseEnum(const FString& Name, EnumType& OutValue)
	{
		const UEnum* Enum = StaticEnum<EnumType>();
		const int64 Value = Enum ? Enum->GetValueByNameString(Name, EGetByNameFlags::None) : INDEX_NONE;
		if (Value == INDEX_NONE)
		{
			return false;
		}
		OutValue = static_cast<EnumType>(Value);
		return true;
	}

	/** Where the screen point at (FractionX, FractionY) of the viewport meets the view's centre plane, for anchor checks. */
	FVector WorldAtScreenFraction(const AModularPlayerController& Controller, double FractionX, double FractionY)
	{
		int32 SizeX = 0;
		int32 SizeY = 0;
		Controller.GetViewportSize(SizeX, SizeY);
		FVector Origin;
		FVector Direction;
		Controller.DeprojectScreenPositionToWorld(SizeX * FractionX, SizeY * FractionY, Origin, Direction);
		return Origin;
	}

	void Status(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		if (!Controller)
		{
			return;
		}
		const UStageViewportComponent* View = Controller->GetViewportView();
		const UStageSnappingComponent* Snapping = Controller->GetSnapping();
		const AModularTransformGizmo* Gizmo = Controller->GetGizmo();
		const FMinimalViewInfo& Camera = Controller->PlayerCameraManager ? Controller->PlayerCameraManager->GetCameraCacheView() : FMinimalViewInfo();
		const AStageGridActor* Grid = View->GetGridActor();

		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.View.Status: projection %s, mode %s, ortho width %.0f cm; camera %s at %s rot %s, POV %s width %.0f."),
			*UEnum::GetValueAsString(View->GetProjection()), *UEnum::GetValueAsString(View->GetViewMode()), View->GetOrthoWidth(),
			*GetNameSafe(Controller->GetPawn()), Controller->GetPawn() ? *Controller->GetPawn()->GetActorLocation().ToCompactString() : TEXT("-"),
			*Controller->GetControlRotation().ToCompactString(),
			Camera.ProjectionMode == ECameraProjectionMode::Orthographic ? TEXT("orthographic") : TEXT("perspective"), Camera.OrthoWidth);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.View.Status: grid %s (%.0f cm cells); snap grid %.0f cm, angle %.0f deg; gizmo snap %.0f cm / %.0f deg; placement grid %.0f cm."),
			Grid && Grid->IsGridShown() ? TEXT("shown") : TEXT("hidden"), Grid ? Grid->GetCellSize() : 0.0,
			Snapping ? Snapping->GetGridSnap() : -1.f, Snapping ? Snapping->GetAngleSnap() : -1.f,
			Gizmo ? Gizmo->GetTranslationSnap() : -1.f, Gizmo ? Gizmo->GetRotationSnapDegrees() : -1.f,
			Controller->GetPlacementTool() ? Controller->GetPlacementTool()->GetGridSnap() : -1.0);
	}

	void Projection(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		EStageViewProjection Value;
		if (!Controller || Args.IsEmpty() || !ParseEnum(Args[0], Value))
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.View.Projection <Perspective|Top|Front|Back|Left|Right>"));
			return;
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.View.Projection %s: %s."), *Args[0], Controller->GetViewportView()->SetProjection(Value) ? TEXT("ok") : TEXT("refused"));
	}

	void Mode(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		EStageViewMode Value;
		if (!Controller || Args.IsEmpty() || !ParseEnum(Args[0], Value))
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.View.Mode <Lit|Unlit|Lines|LightingOnly|DetailLighting|Reflections>"));
			return;
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.View.Mode %s: %s."), *Args[0], Controller->GetViewportView()->SetViewMode(Value) ? TEXT("ok") : TEXT("refused"));
	}

	void Grid(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		if (!Controller)
		{
			return;
		}
		UStageViewportComponent* View = Controller->GetViewportView();
		const bool bVisible = Args.IsEmpty() ? !View->IsGridVisible() : Args[0].Equals(TEXT("on"), ESearchCase::IgnoreCase);
		View->SetGridVisible(bVisible);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.View.Grid: %s."), View->IsGridVisible() ? TEXT("shown") : TEXT("hidden"));
	}

	/** <steps> [x y]: wheel notches at a screen point given as viewport fractions (default the centre). */
	void Zoom(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		if (!Controller || Args.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.View.Zoom <steps> [x y as viewport fractions]"));
			return;
		}
		const double FractionX = Args.Num() >= 3 ? FCString::Atod(*Args[1]) : 0.5;
		const double FractionY = Args.Num() >= 3 ? FCString::Atod(*Args[2]) : 0.5;
		int32 SizeX = 0;
		int32 SizeY = 0;
		Controller->GetViewportSize(SizeX, SizeY);

		UStageViewportComponent* View = Controller->GetViewportView();
		const FVector Before = WorldAtScreenFraction(*Controller, FractionX, FractionY);
		const bool bZoomed = View->ZoomOrtho(FCString::Atof(*Args[0]), FVector2D(SizeX * FractionX, SizeY * FractionY));
		// The camera cache updates on the next frame; deproject against the new state by refreshing it now.
		Controller->PlayerCameraManager->UpdateCamera(0.f);
		const FVector After = WorldAtScreenFraction(*Controller, FractionX, FractionY);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.View.Zoom %s: %s, width %.0f cm; point under (%.2f, %.2f) before %s after %s (moved %.2f cm in the view plane)."),
			*Args[0], bZoomed ? TEXT("ok") : TEXT("refused (perspective)"), View->GetOrthoWidth(), FractionX, FractionY,
			*Before.ToCompactString(), *After.ToCompactString(), FVector::VectorPlaneProject(After - Before, Controller->PlayerCameraManager->GetCameraCacheView().Rotation.Vector()).Size());
	}

	/** <dx> <dy>: a right-drag of this many pixels (X right, Y up). */
	void Pan(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		if (!Controller || Args.Num() < 2)
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.View.Pan <dx> <dy> (pixels, Y up)"));
			return;
		}
		const FVector Before = Controller->GetPawn() ? Controller->GetPawn()->GetActorLocation() : FVector::ZeroVector;
		const bool bPanned = Controller->GetViewportView()->PanOrtho(FVector2D(FCString::Atod(*Args[0]), FCString::Atod(*Args[1])));
		const FVector After = Controller->GetPawn() ? Controller->GetPawn()->GetActorLocation() : FVector::ZeroVector;
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.View.Pan %s %s: %s, camera moved %s."), *Args[0], *Args[1], bPanned ? TEXT("ok") : TEXT("refused (perspective)"),
			*(After - Before).ToCompactString());
	}

	/**
	 * StageCraft.View.MouseDrag <Left|Right|Middle> <dx> <dy> [Frames=10] [Alt|Key]: presses the button over the viewport centre
	 * (holding Alt, or a key such as W for a right-button flight, for the whole drag),
	 * feeds the raw mouse motion through FSlateApplication::OnRawMouseMove spread over Frames frames (the platform's convention,
	 * +dy = mouse moved toward the user, i.e. down), releases, and logs how the camera moved once it has settled. The whole
	 * Slate -> FSceneViewport -> Enhanced Input -> navigation path runs, so sign and scale are what a real mouse produces.
	 */
	void MouseDrag(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		const FKey Button = Args.Num() < 3 ? FKey()
			: Args[0].Equals(TEXT("Left"), ESearchCase::IgnoreCase) ? EKeys::LeftMouseButton
			: Args[0].Equals(TEXT("Right"), ESearchCase::IgnoreCase) ? EKeys::RightMouseButton
			: Args[0].Equals(TEXT("Middle"), ESearchCase::IgnoreCase) ? EKeys::MiddleMouseButton : FKey();
		const UGameViewportClient* ViewportClient = World ? World->GetGameViewport() : nullptr;
		const TSharedPtr<SViewport> ViewportWidget = ViewportClient ? ViewportClient->GetGameViewportWidget() : nullptr;
		if (!Controller || !Button.IsValid() || !ViewportWidget.IsValid() || !FSlateApplication::IsInitialized())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.View.MouseDrag <Left|Right|Middle> <dx> <dy> [Frames=10] [Alt|Key]  (raw counts, +dy = down)"));
			return;
		}
		const FIntPoint Delta(FCString::Atoi(*Args[1]), FCString::Atoi(*Args[2]));
		const int32 Frames = Args.IsValidIndex(3) ? FMath::Clamp(FCString::Atoi(*Args[3]), 1, 600) : 10;
		const FKey HeldKey = !Args.IsValidIndex(4) ? FKey() : Args[4].Equals(TEXT("Alt"), ESearchCase::IgnoreCase) ? EKeys::LeftAlt : FKey(*Args[4]);
		const FModifierKeysState Modifiers(false, false, false, false, HeldKey == EKeys::LeftAlt, false, false, false, false);

		FSlateApplication& Slate = FSlateApplication::Get();
		const FInputDeviceId Mouse = IPlatformInputDeviceMapper::Get().GetDefaultInputDevice();
		const FVector2D Center = FVector2D(ViewportWidget->GetCachedGeometry().GetAbsolutePositionAtCoordinates(FVector2f(0.5f, 0.5f)));
		if (HeldKey.IsValid())
		{
			Slate.ProcessKeyDownEvent(FKeyEvent(HeldKey, Modifiers, Mouse, false, 0, 0, Slate.GetUserIndexForKeyboard()));
		}
		Slate.SetCursorPos(Center);
		Slate.ProcessMouseButtonDownEvent(nullptr, FPointerEvent(Mouse, FSlateApplicationBase::CursorPointerIndex, Center, Center,
			TSet<FKey>({ Button }), Button, 0.f, Modifiers));

		const TWeakObjectPtr<AModularPlayerController> WeakController(Controller);
		const FRotator StartRotation = Controller->GetControlRotation();
		const FVector StartLocation = Controller->GetPawn() ? Controller->GetPawn()->GetActorLocation() : FVector::ZeroVector;
		// Frames of motion, one to release, then a second for a flight to coast to a stop.
		constexpr int32 SettleFrames = 60;
		int32 Frame = 0;
		FIntPoint Sent = FIntPoint::ZeroValue;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[WeakController, Button, Mouse, Modifiers, HeldKey, Delta, Frames, StartRotation, StartLocation, Args, Frame, Sent, ReleaseLocation = StartLocation](float) mutable
		{
			++Frame;
			if (!FSlateApplication::IsInitialized())
			{
				return false;
			}
			FSlateApplication& Slate = FSlateApplication::Get();
			if (Frame <= Frames)
			{
				// Spread evenly, whole counts like a real device.
				const FIntPoint Target(Delta.X * Frame / Frames, Delta.Y * Frame / Frames);
				Slate.OnRawMouseMove(Target.X - Sent.X, Target.Y - Sent.Y);
				Sent = Target;
				return true;
			}
			if (Frame == Frames + 1)
			{
				if (const AModularPlayerController* Controller = WeakController.Get(); Controller && Controller->GetPawn())
				{
					ReleaseLocation = Controller->GetPawn()->GetActorLocation();
				}
				const FVector2D Cursor = Slate.GetCursorPos();
				Slate.ProcessMouseButtonUpEvent(FPointerEvent(Mouse, FSlateApplicationBase::CursorPointerIndex, Cursor, Cursor, TSet<FKey>(), Button, 0.f, Modifiers));
				if (HeldKey.IsValid())
				{
					Slate.ProcessKeyUpEvent(FKeyEvent(HeldKey, FModifierKeysState(), Mouse, false, 0, 0, Slate.GetUserIndexForKeyboard()));
				}
				return true;
			}
			if (Frame < Frames + 1 + SettleFrames)
			{
				return true;
			}
			if (const AModularPlayerController* Controller = WeakController.Get())
			{
				const FRotator Turned = (Controller->GetControlRotation() - StartRotation).GetNormalized();
				const FVector Moved = (Controller->GetPawn() ? Controller->GetPawn()->GetActorLocation() : StartLocation) - StartLocation;
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.View.MouseDrag %s: pitch %+.2f yaw %+.2f deg, moved %s (%.1f cm, %.1f of it after release), camera now at %s rot %s, ticking %s."),
					*FString::Join(Args, TEXT(" ")), Turned.Pitch, Turned.Yaw, *Moved.ToCompactString(), Moved.Size(), FVector::Dist(ReleaseLocation, StartLocation + Moved),
					Controller->GetPawn() ? *Controller->GetPawn()->GetActorLocation().ToCompactString() : TEXT("-"), *Controller->GetControlRotation().ToCompactString(),
					Controller->GetPawn() && Controller->GetPawn()->IsActorTickEnabled() ? TEXT("yes") : TEXT("no"));
			}
			return false;
		}));
	}

	int32 GetCVarInt(const TCHAR* Name)
	{
		const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
		return Variable ? Variable->GetInt() : INDEX_NONE;
	}

	/**
	 * StageCraft.View.MeasureStability [Frames=60] [TimingFrames=60]: measures how much the still image shimmers (STATE.md #48).
	 * Logs the resolution the scene is actually rendered at (the engine's own heuristic, so an automatic screen percentage
	 * is resolved) and the AA settings, then averages the GPU frame time over TimingFrames with nothing else running, then
	 * reads the viewport back for Frames frames and reports the per-pixel temporal standard deviation of luma (0-255).
	 * With the camera at rest every bit of that deviation is TSR jitter not being resolved: the crawl on thin geometry.
	 * Readback stalls the GPU, which is why the timing pass runs first. Keep the mouse still while it runs.
	 */
	void MeasureStability(const TArray<FString>& Args, UWorld* World)
	{
		const UGameViewportClient* ViewportClient = World ? World->GetGameViewport() : nullptr;
		FViewport* Viewport = ViewportClient ? ViewportClient->Viewport : nullptr;
		if (!Viewport || Viewport->GetSizeXY().GetMin() <= 0)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.View.MeasureStability: no game viewport."));
			return;
		}
		const int32 Frames = Args.IsValidIndex(0) ? FMath::Clamp(FCString::Atoi(*Args[0]), 8, 600) : 60;
		const int32 TimingFrames = Args.IsValidIndex(1) ? FMath::Clamp(FCString::Atoi(*Args[1]), 1, 600) : 60;
		const FIntPoint Size = Viewport->GetSizeXY();

		FStaticResolutionFractionHeuristic Heuristic;
		Heuristic.Settings.PullRunTimeRenderingSettings(EViewStatusForScreenPercentage::Desktop);
		Heuristic.TotalDisplayedPixelCount = Size.X * Size.Y;
		const float Fraction = Heuristic.ResolveResolutionFraction();
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.View.MeasureStability: viewport %dx%d rendered at %.1f %% (%dx%d), screen percentage mode %s; r.ScreenPercentage %d, AA method %d, sg.AntiAliasingQuality %d, r.TSR.History.ScreenPercentage %d, r.TSR.ThinGeometryDetection %d, sg.ShadowQuality %d, sg.GlobalIlluminationQuality %d."),
			Size.X, Size.Y, Fraction * 100.f, FMath::RoundToInt(Size.X * Fraction), FMath::RoundToInt(Size.Y * Fraction),
			*UEnum::GetValueAsString(Heuristic.Settings.Mode), GetCVarInt(TEXT("r.ScreenPercentage")), GetCVarInt(TEXT("r.AntiAliasingMethod")),
			GetCVarInt(TEXT("sg.AntiAliasingQuality")), GetCVarInt(TEXT("r.TSR.History.ScreenPercentage")), GetCVarInt(TEXT("r.TSR.ThinGeometryDetection")),
			GetCVarInt(TEXT("sg.ShadowQuality")), GetCVarInt(TEXT("sg.GlobalIlluminationQuality")));

		struct FAccumulator
		{
			TArray<float> Sum;
			TArray<float> SumSquares;
			TArray<FColor> Pixels;
			double GpuMilliseconds = 0.0;
			int32 Frame = 0;
			int32 Captured = 0;
		};
		const TSharedRef<FAccumulator> Accumulator = MakeShared<FAccumulator>();
		Accumulator->Sum.SetNumZeroed(Size.X * Size.Y);
		Accumulator->SumSquares.SetNumZeroed(Size.X * Size.Y);
		const TWeakObjectPtr<const UGameViewportClient> WeakClient(ViewportClient);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakClient, Accumulator, Frames, TimingFrames, Size](float)
		{
			const UGameViewportClient* Client = WeakClient.Get();
			FViewport* Viewport = Client ? Client->Viewport : nullptr;
			if (!Viewport || Viewport->GetSizeXY() != Size)
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.View.MeasureStability: the viewport closed or resized; cancelled."));
				return false;
			}
			FAccumulator& Data = *Accumulator;
			++Data.Frame;
			// The first frames are skipped so the frame that ran the console command is not counted.
			constexpr int32 SkippedFrames = 4;
			if (Data.Frame <= SkippedFrames)
			{
				return true;
			}
			if (Data.Frame <= SkippedFrames + TimingFrames)
			{
				Data.GpuMilliseconds += FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles());
				return true;
			}
			if (!Viewport->ReadPixels(Data.Pixels) || Data.Pixels.Num() != Data.Sum.Num())
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.View.MeasureStability: viewport readback failed; cancelled."));
				return false;
			}
			for (int32 Index = 0; Index < Data.Pixels.Num(); ++Index)
			{
				const FColor& Pixel = Data.Pixels[Index];
				const float Luma = 0.2126f * Pixel.R + 0.7152f * Pixel.G + 0.0722f * Pixel.B;
				Data.Sum[Index] += Luma;
				Data.SumSquares[Index] += Luma * Luma;
			}
			if (++Data.Captured < Frames)
			{
				return true;
			}

			TArray<float> Deviations;
			Deviations.SetNumUninitialized(Data.Sum.Num());
			double Total = 0.0;
			int32 AboveOne = 0;
			int32 AboveFour = 0;
			for (int32 Index = 0; Index < Data.Sum.Num(); ++Index)
			{
				const float Mean = Data.Sum[Index] / Frames;
				const float Deviation = FMath::Sqrt(FMath::Max(0.f, Data.SumSquares[Index] / Frames - Mean * Mean));
				Deviations[Index] = Deviation;
				Total += Deviation;
				AboveOne += Deviation > 1.f ? 1 : 0;
				AboveFour += Deviation > 4.f ? 1 : 0;
			}
			// A heat map of where the image is unstable (white = a deviation of 16 or more), next to the screenshots.
			TArray<FColor> HeatMap;
			HeatMap.SetNumUninitialized(Deviations.Num());
			for (int32 Index = 0; Index < Deviations.Num(); ++Index)
			{
				const uint8 Level = uint8(FMath::Clamp(Deviations[Index] * 16.f, 0.f, 255.f));
				HeatMap[Index] = FColor(Level, Level, Level, 255);
			}
			const FString HeatMapPath = FPaths::ScreenShotDir() / FString::Printf(TEXT("Stability_%s.png"), *FDateTime::Now().ToString());
			const bool bSaved = FImageUtils::SaveImageByExtension(*HeatMapPath, FImageView(HeatMap.GetData(), Size.X, Size.Y));
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.View.MeasureStability: heat map %s %s."), bSaved ? TEXT("saved to") : TEXT("could not be saved to"), *HeatMapPath);

			Deviations.Sort();
			const double PixelCount = Deviations.Num();
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.View.MeasureStability: GPU %.2f ms (mean of %d frames); over %d still frames luma deviation mean %.3f, p99 %.2f, p99.9 %.2f, max %.2f; pixels above 1: %.2f %%, above 4: %.3f %%."),
				Data.GpuMilliseconds / TimingFrames, TimingFrames, Frames, Total / PixelCount,
				Deviations[FMath::Min(Deviations.Num() - 1, int32(PixelCount * 0.99))], Deviations[FMath::Min(Deviations.Num() - 1, int32(PixelCount * 0.999))],
				Deviations.Last(), 100.0 * AboveOne / PixelCount, 100.0 * AboveFour / PixelCount);
			return false;
		}));
	}

	void SnapGrid(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		if (!Controller || Args.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Snap.Grid <0|10|50|100> (cm, 0 = off)"));
			return;
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Snap.Grid %s: %s."), *Args[0], Controller->GetSnapping()->SetGridSnap(FCString::Atof(*Args[0])) ? TEXT("ok") : TEXT("refused"));
	}

	void SnapAngle(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		if (!Controller || Args.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Snap.Angle <0|5|15|45> (degrees, 0 = off)"));
			return;
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Snap.Angle %s: %s."), *Args[0], Controller->GetSnapping()->SetAngleSnap(FCString::Atof(*Args[0])) ? TEXT("ok") : TEXT("refused"));
	}

	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.View.Status"), TEXT("Logs projection, view mode, camera, grid and snaps."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
	FAutoConsoleCommandWithWorldAndArgs ProjectionCommand(TEXT("StageCraft.View.Projection"), TEXT("<Perspective|Top|Front|Back|Left|Right> The toolbar's projection switch."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Projection));
	FAutoConsoleCommandWithWorldAndArgs ModeCommand(TEXT("StageCraft.View.Mode"), TEXT("<Lit|Unlit|Lines> The toolbar's view mode switch."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Mode));
	FAutoConsoleCommandWithWorldAndArgs GridCommand(TEXT("StageCraft.View.Grid"), TEXT("[on|off] Shows, hides or toggles the reference grid."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Grid));
	FAutoConsoleCommandWithWorldAndArgs ZoomCommand(TEXT("StageCraft.View.Zoom"), TEXT("<steps> [x y] Orthographic wheel zoom at a screen point (viewport fractions); logs the anchor."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Zoom));
	FAutoConsoleCommandWithWorldAndArgs PanCommand(TEXT("StageCraft.View.Pan"), TEXT("<dx> <dy> Orthographic right-drag pan in pixels (Y up)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Pan));
	FAutoConsoleCommandWithWorldAndArgs MouseDragCommand(TEXT("StageCraft.View.MouseDrag"), TEXT("<Left|Right|Middle> <dx> <dy> [Frames] [Alt|Key] A real mouse drag through Slate over the viewport (raw counts, +dy = down); logs the camera's motion."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&MouseDrag));
	FAutoConsoleCommandWithWorldAndArgs MeasureStabilityCommand(TEXT("StageCraft.View.MeasureStability"), TEXT("[Frames=60] [TimingFrames=60] Logs the rendered resolution, AA settings, GPU ms and how much the still image shimmers (per-pixel luma deviation)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&MeasureStability));
	FAutoConsoleCommandWithWorldAndArgs SnapGridCommand(TEXT("StageCraft.Snap.Grid"), TEXT("<cm> Sets the toolbar's grid snap (a preset)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SnapGrid));
	FAutoConsoleCommandWithWorldAndArgs SnapAngleCommand(TEXT("StageCraft.Snap.Angle"), TEXT("<degrees> Sets the toolbar's angle snap (a preset)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SnapAngle));
}

#endif // !UE_BUILD_SHIPPING
