// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for placement and transform editing (Docs/ADR/0002-placement-and-audio.md).
// They drive the controller's real click handlers at a simulated cursor position, so placement, selection
// and the "hold never places" rule can be verified from logs without a mouse. Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Actors/ModularBaseActor.h"
#include "Actors/ModularTransformGizmo.h"
#include "Components/SelectionComponent.h"
#include "Containers/Ticker.h"
#include "Data/BaseItemData.h"
#include "Data/StageParameterTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "HAL/IConsoleManager.h"
#include "ModularSceneBuilder.h"
#include "Placement/StagePlacementPreview.h"
#include "Placement/StagePlacementToolComponent.h"
#include "Player/ModularPlayerController.h"
#include "Subsystems/StageItemSubsystem.h"

namespace StagePlacementCommands
{
	AModularPlayerController* GetController(UWorld* World)
	{
		AModularPlayerController* Controller = World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
		if (!Controller)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Edit: no AModularPlayerController in this world."));
		}
		return Controller;
	}

	bool ParseFloat(const TArray<FString>& Args, int32 Index, float& Out)
	{
		return Args.IsValidIndex(Index) && LexTryParseString(Out, *Args[Index]);
	}

	int32 CountPlacedItems(UWorld* World)
	{
		int32 Count = 0;
		for (TActorIterator<AModularBaseActor> It(World); It; ++It)
		{
			Count += It->IsActorBeingDestroyed() ? 0 : 1;
		}
		return Count;
	}

	void Status(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		if (!Controller)
		{
			return;
		}

		const UStagePlacementToolComponent* Tool = Controller->GetPlacementTool();
		const AStagePlacementPreview* Preview = Tool->GetPreviewForDiagnostics();
		const AActor* Selected = Controller->GetSelection()->GetSelectedActor();
		const AModularTransformGizmo* Gizmo = Controller->GetGizmo();

		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Edit.Status: mode %s, armed %s, preview %s%s, items placed %d, gizmo %s"),
			*UEnum::GetValueAsString(Tool->GetEditMode()), *GetNameSafe(Tool->GetArmedItem()),
			*UEnum::GetValueAsString(Tool->GetPreviewState()),
			Preview && Preview->IsPreviewVisible() ? *FString::Printf(TEXT(" at %s"), *Preview->GetActorLocation().ToCompactString()) : TEXT(""),
			CountPlacedItems(World), Gizmo ? *UEnum::GetValueAsString(Gizmo->GetMode()) : TEXT("<none>"));

		if (Selected)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Edit.Status: selected %s at %s rot %s scale %s"), *Selected->GetName(),
				*Selected->GetActorLocation().ToCompactString(), *Selected->GetActorRotation().ToCompactString(), *Selected->GetActorScale3D().ToCompactString());
		}
		else
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Edit.Status: nothing selected"));
		}
	}

	void Arm(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		const UStageItemSubsystem* Items = GameInstance ? GameInstance->GetSubsystem<UStageItemSubsystem>() : nullptr;
		if (!Controller || !Items || Args.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Edit.Arm <ItemAssetName>"));
			return;
		}

		for (UBaseItemData* Item : Items->GetCatalog())
		{
			if (Item && Item->GetName().Equals(Args[0], ESearchCase::IgnoreCase))
			{
				const FStageEconomyResultInfo Result = Controller->RequestPlaceItem(Item);
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Edit.Arm %s: %s"), *Item->GetName(), *UEnum::GetValueAsString(Result.Code));
				return;
			}
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Edit.Arm: no catalog item named %s (catalog loaded: %s)."), *Args[0], Items->IsCatalogLoaded() ? TEXT("yes") : TEXT("no"));
	}

	void PlaceMode(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		if (!Controller)
		{
			return;
		}

		UStagePlacementToolComponent* Tool = Controller->GetPlacementTool();
		const FString Mode = Args.IsEmpty() ? TEXT("toggle") : Args[0];
		if (Mode.Equals(TEXT("on"), ESearchCase::IgnoreCase))
		{
			Tool->EnterPlaceMode();
		}
		else if (Mode.Equals(TEXT("off"), ESearchCase::IgnoreCase))
		{
			Tool->EnterSelectMode();
		}
		else
		{
			Tool->TogglePlaceMode();
		}
	}

	void PointerAt(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		float U = 0.f;
		float V = 0.f;
		int32 SizeX = 0;
		int32 SizeY = 0;
		if (!Controller || !ParseFloat(Args, 0, U) || !ParseFloat(Args, 1, V))
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Edit.PointerAt <U 0-1> <V 0-1>  (fraction of the viewport)"));
			return;
		}
		Controller->GetViewportSize(SizeX, SizeY);
		Controller->DevSetSimulatedCursor(FVector2D(U * SizeX, V * SizeY));
	}

	void PointerAtActor(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		if (!Controller || Args.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Edit.PointerAtActor <ActorName>"));
			return;
		}

		for (TActorIterator<AModularBaseActor> It(World); It; ++It)
		{
			FVector2D ScreenPosition;
			if (It->GetName().Equals(Args[0], ESearchCase::IgnoreCase) && Controller->ProjectWorldLocationToScreen(It->GetActorLocation(), ScreenPosition))
			{
				Controller->DevSetSimulatedCursor(ScreenPosition);
				return;
			}
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Edit.PointerAtActor: %s not found or not on screen."), *Args[0]);
	}

	void ReleasePointer(const TArray<FString>& Args, UWorld* World)
	{
		if (AModularPlayerController* Controller = GetController(World))
		{
			Controller->DevClearSimulatedCursor();
		}
	}

	void Click(const TArray<FString>& Args, UWorld* World)
	{
		if (AModularPlayerController* Controller = GetController(World))
		{
			Controller->DevSimulatePrimaryPressed();
			Controller->DevSimulatePrimaryReleased();
		}
	}

	void Hold(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		float FramesValue = 60.f;
		ParseFloat(Args, 0, FramesValue);
		if (!Controller)
		{
			return;
		}

		// Press now, report Triggered once per frame like Enhanced Input does while a button is held, then release.
		Controller->DevSimulatePrimaryPressed();
		const TWeakObjectPtr<AModularPlayerController> WeakController = Controller;
		int32 FramesLeft = FMath::Max(FMath::RoundToInt32(FramesValue), 1);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakController, FramesLeft](float) mutable
		{
			AModularPlayerController* HeldController = WeakController.Get();
			if (!HeldController)
			{
				return false;
			}
			if (--FramesLeft > 0)
			{
				HeldController->DevSimulatePrimaryHeld();
				return true;
			}
			HeldController->DevSimulatePrimaryReleased();
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Edit.Hold: released."));
			return false;
		}));
	}

	void Key(const TArray<FString>& Args, UWorld* World)
	{
		const FKey KeyToPress(Args.IsEmpty() ? NAME_None : FName(*Args[0]));
		if (!KeyToPress.IsValid() || !FSlateApplication::IsInitialized())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Edit.Key <KeyName>  (e.g. Escape, P, SpaceBar)"));
			return;
		}

		// Through Slate, like the OS would deliver it: focused widget -> game viewport -> Enhanced Input -> the action
		// bindings. Held for a few frames, like a real tap, so Enhanced Input evaluates the press before the release.
		FSlateApplication& Slate = FSlateApplication::Get();
		const int32 UserIndex = Slate.GetUserIndexForKeyboard();
		// The keyboard device, as the platform message handler reports it, so the viewport maps the key to the local player.
		const FInputDeviceId Keyboard = IPlatformInputDeviceMapper::Get().GetDefaultInputDevice();
		Slate.ProcessKeyDownEvent(FKeyEvent(KeyToPress, FModifierKeysState(), Keyboard, false, 0, 0, UserIndex));
		int32 FramesLeft = 4;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([KeyToPress, Keyboard, UserIndex, FramesLeft](float) mutable
		{
			if (--FramesLeft > 0)
			{
				return true;
			}
			if (FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().ProcessKeyUpEvent(FKeyEvent(KeyToPress, FModifierKeysState(), Keyboard, false, 0, 0, UserIndex));
			}
			return false;
		}));
		const TSharedPtr<SWidget> Focused = Slate.GetUserFocusedWidget(UserIndex);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Edit.Key: pressed %s (keyboard focus: %s)."), *KeyToPress.ToString(),
			Focused.IsValid() ? *Focused->GetTypeAsString() : TEXT("none"));
	}

	void GizmoMode(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		AModularTransformGizmo* Gizmo = Controller ? Controller->GetGizmo() : nullptr;
		const FString Mode = Args.IsEmpty() ? FString() : Args[0];
		if (!Gizmo)
		{
			return;
		}

		if (Mode.Equals(TEXT("Move"), ESearchCase::IgnoreCase))
		{
			Gizmo->SetMode(EGizmoMode::Translate);
		}
		else if (Mode.Equals(TEXT("Rotate"), ESearchCase::IgnoreCase))
		{
			Gizmo->SetMode(EGizmoMode::Rotate);
		}
		else if (Mode.Equals(TEXT("Scale"), ESearchCase::IgnoreCase))
		{
			Gizmo->SetMode(EGizmoMode::Scale);
		}
		else
		{
			Gizmo->CycleMode();
		}
	}

	void SetTransform(const TArray<FString>& Args, UWorld* World, const FGameplayTag& ParameterId)
	{
		AModularPlayerController* Controller = GetController(World);
		AActor* Selected = Controller ? Controller->GetSelection()->GetSelectedActor() : nullptr;
		float X = 0.f;
		float Y = 0.f;
		float Z = 0.f;
		if (!Selected || !ParseFloat(Args, 0, X) || !ParseFloat(Args, 1, Y) || !ParseFloat(Args, 2, Z))
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: select an item, then StageCraft.Edit.Set<Location|Scale> <X> <Y> <Z>"));
			return;
		}

		// The same validated path the Inspector's numeric fields use on Enter.
		const FStageEconomyResultInfo Result = Controller->RequestParameterChange(Selected, ParameterId, FStageParameterValue::MakeVector(FVector(X, Y, Z)));
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Edit: %s -> %s; now location %s scale %s"), *ParameterId.ToString(),
			*UEnum::GetValueAsString(Result.Code), *Selected->GetActorLocation().ToCompactString(), *Selected->GetActorScale3D().ToCompactString());
	}

	void SetLocation(const TArray<FString>& Args, UWorld* World)
	{
		SetTransform(Args, World, StageCraftTags::Param_Transform_Location);
	}

	void SetScale(const TArray<FString>& Args, UWorld* World)
	{
		SetTransform(Args, World, StageCraftTags::Param_Transform_Scale);
	}

	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.Edit.Status"), TEXT("Logs edit mode, armed item, preview, placed item count, gizmo mode and the selection."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
	FAutoConsoleCommandWithWorldAndArgs ArmCommand(TEXT("StageCraft.Edit.Arm"), TEXT("<ItemAssetName> Same as clicking the item in the Library: arm it and enter Place mode."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Arm));
	FAutoConsoleCommandWithWorldAndArgs PlaceModeCommand(TEXT("StageCraft.Edit.PlaceMode"), TEXT("[on|off|toggle] Enters or leaves Place mode."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&PlaceMode));
	FAutoConsoleCommandWithWorldAndArgs PointerAtCommand(TEXT("StageCraft.Edit.PointerAt"), TEXT("<U> <V> Simulates the cursor at a fraction of the viewport (0-1)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&PointerAt));
	FAutoConsoleCommandWithWorldAndArgs PointerAtActorCommand(TEXT("StageCraft.Edit.PointerAtActor"), TEXT("<ActorName> Simulates the cursor over a placed item."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&PointerAtActor));
	FAutoConsoleCommandWithWorldAndArgs ReleasePointerCommand(TEXT("StageCraft.Edit.ReleasePointer"), TEXT("Returns cursor control to the mouse."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ReleasePointer));
	FAutoConsoleCommandWithWorldAndArgs ClickCommand(TEXT("StageCraft.Edit.Click"), TEXT("Left click (press + release) at the simulated cursor."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Click));
	FAutoConsoleCommandWithWorldAndArgs HoldCommand(TEXT("StageCraft.Edit.Hold"), TEXT("[Frames=60] Holds the left button at the simulated cursor, then releases."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Hold));
	FAutoConsoleCommandWithWorldAndArgs KeyCommand(TEXT("StageCraft.Edit.Key"), TEXT("<KeyName> Presses and releases a key through Slate, the same path as the keyboard (Escape, P, SpaceBar...)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Key));
	FAutoConsoleCommandWithWorldAndArgs GizmoModeCommand(TEXT("StageCraft.Edit.GizmoMode"), TEXT("[Move|Rotate|Scale] Sets the gizmo tool (no argument cycles)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&GizmoMode));
	FAutoConsoleCommandWithWorldAndArgs SetLocationCommand(TEXT("StageCraft.Edit.SetLocation"), TEXT("<X> <Y> <Z> Inspector-equivalent location edit of the selection."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SetLocation));
	FAutoConsoleCommandWithWorldAndArgs SetScaleCommand(TEXT("StageCraft.Edit.SetScale"), TEXT("<X> <Y> <Z> Inspector-equivalent scale edit of the selection."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SetScale));
}

#endif // !UE_BUILD_SHIPPING
