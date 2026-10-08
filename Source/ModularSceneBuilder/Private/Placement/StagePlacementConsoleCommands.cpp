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
#include "History/StageEditHistoryComponent.h"
#include "History/StageEditHistorySubsystem.h"
#include "ModularSceneBuilder.h"
#include "Placement/StagePlacementPreview.h"
#include "Placement/StagePlacementToolComponent.h"
#include "Placement/StageSnappingComponent.h"
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
			Preview && Preview->IsPreviewVisible() ? *FString::Printf(TEXT(" at %s, %d snap guides"), *Preview->GetActorLocation().ToCompactString(), Preview->GetVisibleSnapGuideCount()) : TEXT(""),
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

		// Every item of a multi-selection, primary last, with its transform: group moves are checked from these lines.
		const TArray<AActor*> SelectedSet = Controller->GetSelection()->GetSelectedActors();
		if (SelectedSet.Num() > 1)
		{
			for (int32 Index = 0; Index < SelectedSet.Num(); ++Index)
			{
				const AActor* Item = SelectedSet[Index];
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Edit.Status:   [%d/%d]%s %s at %s rot %s scale %s"), Index + 1, SelectedSet.Num(),
					Index == SelectedSet.Num() - 1 ? TEXT(" primary") : TEXT(""), *GetNameSafe(Item),
					Item ? *Item->GetActorLocation().ToCompactString() : TEXT("-"), Item ? *Item->GetActorRotation().ToCompactString() : TEXT("-"),
					Item ? *Item->GetActorScale3D().ToCompactString() : TEXT("-"));
			}
		}
	}

	void ListItems(const TArray<FString>& Args, UWorld* World)
	{
		for (TActorIterator<AModularBaseActor> It(World); It; ++It)
		{
			if (!It->IsActorBeingDestroyed())
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Edit.Items: %s \"%s\" (%s) id %s at %s rot %s scale %s%s"), *It->GetName(),
					*It->GetInstanceLabel().ToString(), *GetNameSafe(It->GetItemData()), *It->GetInstanceId().ToString(EGuidFormats::Short),
					*It->GetActorLocation().ToCompactString(), *It->GetActorRotation().ToCompactString(), *It->GetActorScale3D().ToCompactString(),
					It->IsSelected() ? TEXT(" [selected]") : TEXT(""));
			}
		}
	}

	void SelectAll(const TArray<FString>& Args, UWorld* World)
	{
		if (AModularPlayerController* Controller = GetController(World))
		{
			Controller->RequestSelectAll();
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Edit.SelectAll: %d selected."), Controller->GetSelection()->GetSelectionCount());
		}
	}

	void ClearStage(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		if (!Controller)
		{
			return;
		}
		// The menu asks the user; the console asks for the word, so a stray command can never wipe the stage.
		if (Args.IsEmpty() || !Args[0].Equals(TEXT("confirm"), ESearchCase::IgnoreCase))
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Edit.ClearStage confirm  (removes every placed item as one undoable step)"));
			return;
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Edit.ClearStage: removed %d items."), Controller->RequestClearStage());
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
			// "ctrl" is the Ctrl+Click path: toggle the item in the selection.
			const bool bToggle = !Args.IsEmpty() && Args[0].Equals(TEXT("ctrl"), ESearchCase::IgnoreCase);
			Controller->DevSimulatePrimaryPressed(bToggle ? EStageSelectionClick::Toggle : EStageSelectionClick::Replace);
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

	/** "Ctrl+Shift+Z" -> LeftControl, LeftShift, Z (modifiers first, in the order a person presses them). */
	bool ParseKeyChord(const FString& Chord, TArray<FKey>& OutKeys)
	{
		TArray<FString> Parts;
		Chord.ParseIntoArray(Parts, TEXT("+"));
		for (const FString& Part : Parts)
		{
			const FKey Key = Part.Equals(TEXT("Ctrl"), ESearchCase::IgnoreCase) ? EKeys::LeftControl
				: Part.Equals(TEXT("Shift"), ESearchCase::IgnoreCase) ? EKeys::LeftShift
				: Part.Equals(TEXT("Alt"), ESearchCase::IgnoreCase) ? EKeys::LeftAlt
				: FKey(FName(*Part));
			if (!Key.IsValid())
			{
				return false;
			}
			OutKeys.Add(Key);
		}
		return !OutKeys.IsEmpty();
	}

	FModifierKeysState MakeModifierState(TConstArrayView<FKey> HeldKeys)
	{
		const bool bShift = HeldKeys.Contains(EKeys::LeftShift);
		const bool bControl = HeldKeys.Contains(EKeys::LeftControl);
		const bool bAlt = HeldKeys.Contains(EKeys::LeftAlt);
		return FModifierKeysState(bShift, false, bControl, false, bAlt, false, false, false, false);
	}

	void Key(const TArray<FString>& Args, UWorld* World)
	{
		TArray<FKey> Keys;
		if (Args.IsEmpty() || !ParseKeyChord(Args[0], Keys) || !FSlateApplication::IsInitialized())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Edit.Key <KeyName|Chord>  (e.g. Escape, P, SpaceBar, Ctrl+Z, Ctrl+Shift+Z)"));
			return;
		}

		// Through Slate, like the OS would deliver it: focused widget -> game viewport -> Enhanced Input -> the action
		// bindings. Each key goes down in order and the whole chord is held for a few frames, like a real press, so
		// Enhanced Input evaluates the chord before anything is released.
		FSlateApplication& Slate = FSlateApplication::Get();
		const int32 UserIndex = Slate.GetUserIndexForKeyboard();
		// The keyboard device, as the platform message handler reports it, so the viewport maps the key to the local player.
		const FInputDeviceId Keyboard = IPlatformInputDeviceMapper::Get().GetDefaultInputDevice();
		for (int32 Index = 0; Index < Keys.Num(); ++Index)
		{
			const FModifierKeysState Modifiers = MakeModifierState(MakeArrayView(Keys.GetData(), Index + 1));
			Slate.ProcessKeyDownEvent(FKeyEvent(Keys[Index], Modifiers, Keyboard, false, 0, 0, UserIndex));
		}

		int32 FramesLeft = 4;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Keys, Keyboard, UserIndex, FramesLeft](float) mutable
		{
			if (--FramesLeft > 0)
			{
				return true;
			}
			if (FSlateApplication::IsInitialized())
			{
				for (int32 Index = Keys.Num() - 1; Index >= 0; --Index)
				{
					FSlateApplication::Get().ProcessKeyUpEvent(FKeyEvent(Keys[Index], MakeModifierState(MakeArrayView(Keys.GetData(), Index)), Keyboard, false, 0, 0, UserIndex));
				}
			}
			return false;
		}));
		const TSharedPtr<SWidget> Focused = Slate.GetUserFocusedWidget(UserIndex);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Edit.Key: pressed %s (keyboard focus: %s)."), *Args[0],
			Focused.IsValid() ? *Focused->GetTypeAsString() : TEXT("none"));
	}

	void History(const TArray<FString>& Args, UWorld* World)
	{
		const UStageEditHistorySubsystem* HistorySubsystem = UWorld::GetSubsystem<UStageEditHistorySubsystem>(World);
		if (!HistorySubsystem)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Edit.History: no edit history in this world."));
			return;
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Edit.History: %d undo (next \"%s\"), %d redo (next \"%s\")."),
			HistorySubsystem->GetUndoCount(), *HistorySubsystem->GetUndoDescription().ToString(),
			HistorySubsystem->GetRedoCount(), *HistorySubsystem->GetRedoDescription().ToString());
	}

	void Undo(const TArray<FString>& Args, UWorld* World)
	{
		if (AModularPlayerController* Controller = GetController(World))
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Edit.Undo: %s"), *UEnum::GetValueAsString(Controller->RequestUndo()));
		}
	}

	void Redo(const TArray<FString>& Args, UWorld* World)
	{
		if (AModularPlayerController* Controller = GetController(World))
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Edit.Redo: %s"), *UEnum::GetValueAsString(Controller->RequestRedo()));
		}
	}

	void Snap(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		UStageSnappingComponent* Snapping = Controller ? Controller->GetSnapping() : nullptr;
		if (!Snapping)
		{
			return;
		}
		if (!Args.IsEmpty())
		{
			Snapping->SetSnapToItemsEnabled(Args[0].Equals(TEXT("on"), ESearchCase::IgnoreCase) || Args[0] == TEXT("1"));
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Edit.Snap: snap to items %s."), Snapping->IsSnapToItemsEnabled() ? TEXT("on") : TEXT("off"));
	}

	/**
	 * A real Move-handle drag of the selection: presses at the handle's screen position, moves the simulated cursor
	 * along the axis over several frames (Enhanced Input's held events), then releases. Snapping and history see
	 * exactly what a mouse drag produces.
	 */
	void DragGizmo(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		AModularTransformGizmo* Gizmo = Controller ? Controller->GetGizmo() : nullptr;
		float DistanceValue = 0.f;
		const int32 Axis = Args.IsEmpty() ? INDEX_NONE : FString(TEXT("XYZ")).Find(Args[0].ToUpper());
		if (!Gizmo || !Gizmo->GetTarget() || Axis == INDEX_NONE || Args[0].Len() != 1 || !ParseFloat(Args, 1, DistanceValue))
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: select an item, then StageCraft.Edit.DragGizmo <X|Y|Z> <cm>"));
			return;
		}

		Gizmo->SetMode(EGizmoMode::Translate);
		const FVector Start = Gizmo->DevGetMoveHandleLocation(Axis);
		const FVector End = Start + FVector(Axis == 0, Axis == 1, Axis == 2) * DistanceValue;
		FVector2D StartScreen;
		FVector2D EndScreen;
		if (!Controller->ProjectWorldLocationToScreen(Start, StartScreen) || !Controller->ProjectWorldLocationToScreen(End, EndScreen))
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Edit.DragGizmo: the handle is not on screen."));
			return;
		}

		Controller->DevSetSimulatedCursor(StartScreen);
		Controller->DevSimulatePrimaryPressed();
		if (!Gizmo->IsDragging())
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Edit.DragGizmo: the press at %s did not grab the %s handle."), *StartScreen.ToString(), *Args[0]);
			Controller->DevSimulatePrimaryReleased();
			return;
		}

		constexpr int32 Steps = 20;
		const TWeakObjectPtr<AModularPlayerController> WeakController = Controller;
		int32 Step = 0;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakController, StartScreen, EndScreen, Step](float) mutable
		{
			AModularPlayerController* DragController = WeakController.Get();
			if (!DragController)
			{
				return false;
			}
			if (++Step <= Steps)
			{
				DragController->DevSetSimulatedCursor(FMath::Lerp(StartScreen, EndScreen, static_cast<double>(Step) / Steps));
				DragController->DevSimulatePrimaryHeld();
				return true;
			}
			DragController->DevSimulatePrimaryReleased();
			const AActor* Selected = DragController->GetSelection()->GetSelectedActor();
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Edit.DragGizmo: released; %s at %s."), *GetNameSafe(Selected),
				Selected ? *Selected->GetActorLocation().ToCompactString() : TEXT("-"));
			return false;
		}));
	}

	void Delete(const TArray<FString>& Args, UWorld* World)
	{
		if (AModularPlayerController* Controller = GetController(World))
		{
			// The same path as the Delete key: every selected item, one undo step.
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Edit.Delete: deleted %d items."), Controller->RequestDeleteSelection());
		}
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
	FAutoConsoleCommandWithWorldAndArgs ClickCommand(TEXT("StageCraft.Edit.Click"), TEXT("[ctrl] Left click (press + release) at the simulated cursor; ctrl = Ctrl+Click (toggle selection)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Click));
	FAutoConsoleCommandWithWorldAndArgs ItemsCommand(TEXT("StageCraft.Edit.Items"), TEXT("Logs every placed item with label, catalog item, instance id and transform."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ListItems));
	FAutoConsoleCommandWithWorldAndArgs SelectAllCommand(TEXT("StageCraft.Edit.SelectAll"), TEXT("Same as Edit > Select All."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SelectAll));
	FAutoConsoleCommandWithWorldAndArgs ClearStageCommand(TEXT("StageCraft.Edit.ClearStage"), TEXT("confirm  Same as Edit > Clear Stage (one undoable step)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ClearStage));
	FAutoConsoleCommandWithWorldAndArgs HoldCommand(TEXT("StageCraft.Edit.Hold"), TEXT("[Frames=60] Holds the left button at the simulated cursor, then releases."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Hold));
	FAutoConsoleCommandWithWorldAndArgs KeyCommand(TEXT("StageCraft.Edit.Key"), TEXT("<KeyName|Chord> Presses and releases a key or chord through Slate, the same path as the keyboard (Escape, P, Ctrl+Z, Ctrl+Shift+Z...)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Key));
	FAutoConsoleCommandWithWorldAndArgs HistoryCommand(TEXT("StageCraft.Edit.History"), TEXT("Logs the undo/redo stack sizes and the next steps."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&History));
	FAutoConsoleCommandWithWorldAndArgs UndoCommand(TEXT("StageCraft.Edit.Undo"), TEXT("Same as Edit > Undo."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Undo));
	FAutoConsoleCommandWithWorldAndArgs RedoCommand(TEXT("StageCraft.Edit.Redo"), TEXT("Same as Edit > Redo."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Redo));
	FAutoConsoleCommandWithWorldAndArgs SnapCommand(TEXT("StageCraft.Edit.Snap"), TEXT("[on|off] Sets or logs Snap to Items."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Snap));
	FAutoConsoleCommandWithWorldAndArgs DragGizmoCommand(TEXT("StageCraft.Edit.DragGizmo"), TEXT("<X|Y|Z> <cm> Drags the selection's Move handle through the real click path."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DragGizmo));
	FAutoConsoleCommandWithWorldAndArgs DeleteCommand(TEXT("StageCraft.Edit.Delete"), TEXT("Deletes the selection as an undoable step (the Delete key's path)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Delete));
	FAutoConsoleCommandWithWorldAndArgs GizmoModeCommand(TEXT("StageCraft.Edit.GizmoMode"), TEXT("[Move|Rotate|Scale] Sets the gizmo tool (no argument cycles)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&GizmoMode));
	FAutoConsoleCommandWithWorldAndArgs SetLocationCommand(TEXT("StageCraft.Edit.SetLocation"), TEXT("<X> <Y> <Z> Inspector-equivalent location edit of the selection."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SetLocation));
	FAutoConsoleCommandWithWorldAndArgs SetScaleCommand(TEXT("StageCraft.Edit.SetScale"), TEXT("<X> <Y> <Z> Inspector-equivalent scale edit of the selection."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SetScale));
}

#endif // !UE_BUILD_SHIPPING
