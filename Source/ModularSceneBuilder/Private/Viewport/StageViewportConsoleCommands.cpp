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
#include "HAL/IConsoleManager.h"
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
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.View.Status: grid %s (%d lines); snap grid %.0f cm, angle %.0f deg; gizmo snap %.0f cm / %.0f deg; placement grid %.0f cm."),
			Grid && Grid->IsGridShown() ? TEXT("shown") : TEXT("hidden"), Grid ? Grid->GetLineCount() : 0,
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
	FAutoConsoleCommandWithWorldAndArgs SnapGridCommand(TEXT("StageCraft.Snap.Grid"), TEXT("<cm> Sets the toolbar's grid snap (a preset)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SnapGrid));
	FAutoConsoleCommandWithWorldAndArgs SnapAngleCommand(TEXT("StageCraft.Snap.Angle"), TEXT("<degrees> Sets the toolbar's angle snap (a preset)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SnapAngle));
}

#endif // !UE_BUILD_SHIPPING
