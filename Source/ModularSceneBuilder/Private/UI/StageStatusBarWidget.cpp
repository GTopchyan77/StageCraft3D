// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageStatusBarWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/SelectionComponent.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Data/BaseItemData.h"
#include "Economy/StageProductData.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "History/StageEditHistorySubsystem.h"
#include "Engine/LocalPlayer.h"
#include "Import/StageMeshImportSubsystem.h"
#include "Input/StageInputPreferencesSubsystem.h"
#include "Misc/Paths.h"
#include "Placement/StagePlacementToolComponent.h"
#include "Player/ModularPlayerController.h"
#include "Player/StageCameraPawn.h"
#include "Render/StageRenderSubsystem.h"
#include "Report/StageCableScheduleSubsystem.h"
#include "Report/StagePatchSheetSubsystem.h"
#include "Scene/StageSceneComponent.h"
#include "Subsystems/StageEconomySubsystem.h"
#include "TimerManager.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageStatusBarWidget)

#define LOCTEXT_NAMESPACE "StageStatusBar"

namespace StageStatusBar
{
	namespace
	{
		/** "Esc or P", either one alone, or empty when neither is bound. */
		FText EitherKey(const FText& First, const FText& Second)
		{
			if (First.IsEmpty() || Second.IsEmpty())
			{
				return First.IsEmpty() ? Second : First;
			}
			return FText::Format(LOCTEXT("EitherKey", "{0} or {1}"), First, Second);
		}

		/** Joins the parts that are not empty with the hint separator; a part whose key is unbound is left out by its caller. */
		FText JoinParts(TConstArrayView<FText> Parts)
		{
			TArray<FText> Present;
			for (const FText& Part : Parts)
			{
				if (!Part.IsEmpty())
				{
					Present.Add(Part);
				}
			}
			return FText::Join(FText::FromString(TEXT("   ·   ")), Present);
		}

		/** "Keys: action", or empty when Keys is (the command is unbound). */
		FText KeyPart(const FText& Keys, const FText& Action)
		{
			return Keys.IsEmpty() ? FText::GetEmpty() : FText::Format(LOCTEXT("KeyPart", "{0}: {1}"), Keys, Action);
		}
	}

	FHintKeys FHintKeys::FromBindings(const FStageKeyBindings& Bindings)
	{
		// Only the primary chord, to keep the hint short; the menus and the Preferences panel show both.
		const auto Primary = [&Bindings](EStageInputCommand Command)
		{
			const FText Main = Bindings.Get(Command, EStageKeySlot::Primary).ToText();
			return Main.IsEmpty() ? Bindings.Get(Command, EStageKeySlot::Secondary).ToText() : Main;
		};
		FHintKeys Keys;
		Keys.Cancel = Primary(EStageInputCommand::Cancel);
		Keys.TogglePlace = Primary(EStageInputCommand::TogglePlaceMode);
		Keys.CycleTool = Primary(EStageInputCommand::CycleTransformTool);
		Keys.Delete = Primary(EStageInputCommand::DeleteSelection);
		Keys.FinishPath = Primary(EStageInputCommand::FinishPath);
		Keys.RemovePoint = Primary(EStageInputCommand::RemovePathPoint);
		Keys.Fly = StageKeyBindings::DescribeFlyKeys(Bindings);
		return Keys;
	}

	FText MakeHint(EStageEditMode Mode, const FText& ArmedItemName, int32 SelectedCount, const FHintKeys& Keys)
	{
		const FText LeavePlace = EitherKey(Keys.Cancel, Keys.TogglePlace);
		if (Mode == EStageEditMode::Place)
		{
			return ArmedItemName.IsEmpty()
				? JoinParts({ LOCTEXT("PlaceNoItem", "Pick an item in the Library to place it"), KeyPart(LeavePlace, LOCTEXT("BackToSelect", "back to Select")) })
				: JoinParts({ FText::Format(LOCTEXT("PlaceItem", "Placing {0}: each click places a copy"), ArmedItemName), KeyPart(LeavePlace, LOCTEXT("Finish", "finish")) });
		}
		if (SelectedCount > 1)
		{
			return JoinParts({ FText::Format(LOCTEXT("SelectGroup", "{0} items selected: the gizmo moves them together"), FText::AsNumber(SelectedCount)),
				KeyPart(Keys.Delete, LOCTEXT("RemoveAll", "remove all")), LOCTEXT("CtrlClickToggle", "Ctrl+Click: add / remove") });
		}
		const FText Tools = KeyPart(Keys.CycleTool, LOCTEXT("CycleTools", "Move / Rotate / Scale"));
		if (ArmedItemName.IsEmpty())
		{
			const FText Fly = Keys.Fly.IsEmpty() ? LOCTEXT("LookAround", "Right mouse: look around") : FText::Format(LOCTEXT("Fly", "Right mouse + {0}: fly"), Keys.Fly);
			return JoinParts({ LOCTEXT("ClickSelect", "Click an item to select it"), LOCTEXT("CtrlClickSeveral", "Ctrl+Click: select several"), Tools, Fly });
		}
		return JoinParts({ LOCTEXT("ClickSelect", "Click an item to select it"), LOCTEXT("CtrlClickSeveral", "Ctrl+Click: select several"), Tools,
			KeyPart(Keys.TogglePlace, FText::Format(LOCTEXT("PlaceArmed", "place {0}"), ArmedItemName)) });
	}

	FText MakeSpanHint(const FStageSpanToolStatus& Status, const FHintKeys& Keys)
	{
		if (Status.Phase == EStageSpanToolPhase::PickStart)
		{
			return JoinParts({ FText::Format(LOCTEXT("SpanStart", "Drawing {0}: click where the path starts"), Status.ItemName),
				KeyPart(EitherKey(Keys.Cancel, Keys.TogglePlace), LOCTEXT("Finish", "finish")) });
		}
		const FText RemovePoint = KeyPart(Keys.RemovePoint, LOCTEXT("RemoveLastPoint", "remove last point"));
		const FText DropPath = KeyPart(Keys.Cancel, LOCTEXT("DropPath", "drop the path"));
		if (!Status.bHasRun)
		{
			return JoinParts({ FText::Format(LOCTEXT("SpanEndNoRun", "Drawing {0}: click the next point"), Status.ItemName), RemovePoint, DropPath });
		}

		FNumberFormattingOptions TwoDecimals;
		TwoDecimals.SetMinimumFractionalDigits(2).SetMaximumFractionalDigits(2);
		const FText Run = FText::Format(LOCTEXT("SpanRun", "{0} m, {1} {1}|plural(one=segment,other=segments)"),
			FText::AsNumber(Status.Length * 0.01, &TwoDecimals), Status.SegmentCount);
		const FText Shape = Status.LegCount > 1
			? FText::Format(LOCTEXT("SpanLegs", "{0} in {1} legs"), Run, Status.LegCount)
			: Run;
		// Checked before bRefused: a loop that cannot close also shows a refused ghost, but the limits are not the reason.
		if (Status.bCloseBlocked)
		{
			return Keys.FinishPath.IsEmpty()
				? FText::Format(LOCTEXT("SpanCloseBlockedNoKey", "{0}: the loop does not close in whole sections   ·   adjust a side"), Status.ItemName)
				: FText::Format(LOCTEXT("SpanCloseBlocked", "{0}: the loop does not close in whole sections   ·   adjust a side, or press {1} to place it open"), Status.ItemName, Keys.FinishPath);
		}
		if (Status.bRefused)
		{
			return JoinParts({ FText::Format(LOCTEXT("SpanRefused", "{0}: {1}, not allowed by this stage's limits"), Status.ItemName, Shape), RemovePoint, DropPath });
		}
		if (Status.bClosing)
		{
			return FText::Format(LOCTEXT("SpanClosing", "{0}: {1}   ·   click to close the loop and place it"), Status.ItemName, Shape);
		}
		// The finish key needs a placed point beyond the start; the leg under the cursor only becomes one with the next click.
		const FText Head = FText::Format(LOCTEXT("SpanRunHead", "{0}: {1}"), Status.ItemName, Shape);
		const FText Drop = KeyPart(Keys.Cancel, LOCTEXT("Drop", "drop"));
		if (Status.PointCount >= 2)
		{
			const FText Place = Keys.FinishPath.IsEmpty()
				? LOCTEXT("DoubleClickPlace", "double-click: place")
				: FText::Format(LOCTEXT("FinishOrDoubleClick", "{0} or double-click: place"), Keys.FinishPath);
			return JoinParts({ Head, LOCTEXT("ClickAddPoint", "click: add point"), Place, KeyPart(Keys.RemovePoint, LOCTEXT("UndoPoint", "undo point")), Drop });
		}
		return JoinParts({ Head, LOCTEXT("ClickAddPointRun", "click: add point (double-click: place this run)"), Drop });
	}
}

void UStageStatusBarWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
}

const UStageCraftUITheme& UStageStatusBarWidget::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

void UStageStatusBarWidget::BuildTree()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	UVerticalBox* Root = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Root"));
	WidgetTree->RootWidget = Root;

	// A hairline separates the bar from the panels above, like the editor's status bar.
	UImage* Divider = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("TopDivider"));
	Divider->SetBrush(MakeBox(Palette.Divider));
	Divider->SetDesiredSizeOverride(FVector2D(1.f, 1.f));
	Root->AddChildToVerticalBox(Divider);

	UBorder* Bar = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Bar"));
	Bar->SetBrush(MakeBox(Palette.WindowBackground));
	Bar->SetPadding(FMargin(8.f, 3.f));
	Root->AddChildToVerticalBox(Bar);

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("Row"));
	Bar->SetContent(Row);

	ModeChip = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("ModeChip"));
	ModeChip->SetPadding(FMargin(8.f, 1.f));
	if (UHorizontalBoxSlot* ChipSlot = Row->AddChildToHorizontalBox(ModeChip))
	{
		ChipSlot->SetVerticalAlignment(VAlign_Center);
	}

	ModeText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ModeText"));
	ModeText->SetFont(MakeFont(8, TEXT("Bold")));
	ModeChip->SetContent(ModeText);

	HintText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("HintText"));
	HintText->SetFont(MakeFont(8));
	HintText->SetColorAndOpacity(FSlateColor(Palette.TextSecondary));
	HintText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	if (UHorizontalBoxSlot* HintSlot = Row->AddChildToHorizontalBox(HintText))
	{
		HintSlot->SetVerticalAlignment(VAlign_Center);
		HintSlot->SetPadding(FMargin(10.f, 0.f));
		HintSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}

	MessageText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("MessageText"));
	MessageText->SetFont(MakeFont(8, TEXT("Bold")));
	if (UHorizontalBoxSlot* MessageSlot = Row->AddChildToHorizontalBox(MessageText))
	{
		MessageSlot->SetVerticalAlignment(VAlign_Center);
		MessageSlot->SetHorizontalAlignment(HAlign_Right);
	}
}

void UStageStatusBarWidget::NativeConstruct()
{
	Super::NativeConstruct();
	BindSources();
	RefreshMode();
	ClearMessage();
}

void UStageStatusBarWidget::NativeDestruct()
{
	UnbindSources();
	if (const UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(MessageTimer);
	}
	Super::NativeDestruct();
}

void UStageStatusBarWidget::BindSources()
{
	AModularPlayerController* OwningController = Cast<AModularPlayerController>(GetOwningPlayer());
	Controller = OwningController;
	if (!OwningController)
	{
		return;
	}

	OwningController->OnRequestRejected.AddUniqueDynamic(this, &ThisClass::HandleRequestRejected);
	OwningController->OnPossessedPawnChanged.AddUniqueDynamic(this, &ThisClass::HandlePossessedPawnChanged);
	BindCameraPawn(OwningController->GetPawn());

	if (UStagePlacementToolComponent* Tool = OwningController->GetPlacementTool())
	{
		PlacementTool = Tool;
		Tool->OnEditModeChanged.AddUniqueDynamic(this, &ThisClass::HandleEditModeChanged);
		Tool->OnArmedItemChanged.AddUniqueDynamic(this, &ThisClass::HandleArmedItemChanged);
	}

	const UGameInstance* GameInstance = GetGameInstance();
	if (UStageEconomySubsystem* EconomySubsystem = GameInstance ? GameInstance->GetSubsystem<UStageEconomySubsystem>() : nullptr)
	{
		Economy = EconomySubsystem;
		EconomySubsystem->OnPurchaseCompleted.AddUniqueDynamic(this, &ThisClass::HandlePurchaseCompleted);
	}

	if (UStageEditHistorySubsystem* HistorySubsystem = UWorld::GetSubsystem<UStageEditHistorySubsystem>(GetWorld()))
	{
		History = HistorySubsystem;
		HistorySubsystem->OnHistoryChanged.AddUniqueDynamic(this, &ThisClass::HandleHistoryChanged);
	}

	if (USelectionComponent* SelectionComponent = OwningController->GetSelection())
	{
		Selection = SelectionComponent;
		SelectionComponent->OnSelectionSetChanged.AddUniqueDynamic(this, &ThisClass::HandleSelectionSetChanged);
	}

	if (UStageSceneComponent* Scenes = OwningController->GetSceneFiles())
	{
		SceneFiles = Scenes;
		Scenes->OnSceneOperationFinished.AddUniqueDynamic(this, &ThisClass::HandleSceneOperationFinished);
	}

	if (UStageRenderSubsystem* RenderSubsystem = UWorld::GetSubsystem<UStageRenderSubsystem>(GetWorld()))
	{
		Render = RenderSubsystem;
		RenderSubsystem->OnRenderStateChanged.AddUniqueDynamic(this, &ThisClass::HandleRenderStateChanged);
		RenderSubsystem->OnRenderFinished.AddUniqueDynamic(this, &ThisClass::HandleRenderFinished);
	}

	if (UStageSpanToolComponent* Tool = OwningController->GetSpanTool())
	{
		SpanTool = Tool;
		Tool->OnStatusChanged.AddUniqueDynamic(this, &ThisClass::HandleSpanStatusChanged);
	}

	if (UStageBillOfMaterialsSubsystem* Report = UWorld::GetSubsystem<UStageBillOfMaterialsSubsystem>(GetWorld()))
	{
		BillOfMaterials = Report;
		Report->OnExportFinished.AddUniqueDynamic(this, &ThisClass::HandleReportExportFinished);
	}
	if (UStagePatchSheetSubsystem* PatchSheetSubsystem = UWorld::GetSubsystem<UStagePatchSheetSubsystem>(GetWorld()))
	{
		PatchSheet = PatchSheetSubsystem;
		PatchSheetSubsystem->OnExportFinished.AddUniqueDynamic(this, &ThisClass::HandleReportExportFinished);
	}
	if (UStageCableScheduleSubsystem* CableScheduleSubsystem = UWorld::GetSubsystem<UStageCableScheduleSubsystem>(GetWorld()))
	{
		CableSchedule = CableScheduleSubsystem;
		CableScheduleSubsystem->OnExportFinished.AddUniqueDynamic(this, &ThisClass::HandleReportExportFinished);
	}

	if (UStageMeshImportSubsystem* ImportSubsystem = GameInstance ? GameInstance->GetSubsystem<UStageMeshImportSubsystem>() : nullptr)
	{
		Importer = ImportSubsystem;
		ImportSubsystem->OnImportStarted.AddUniqueDynamic(this, &ThisClass::HandleModelImportStarted);
		ImportSubsystem->OnImportFinished.AddUniqueDynamic(this, &ThisClass::HandleModelImportFinished);
	}

	// The hints name the user's keys.
	if (UStageInputPreferencesSubsystem* Preferences = ULocalPlayer::GetSubsystem<UStageInputPreferencesSubsystem>(GetOwningLocalPlayer()))
	{
		InputPreferences = Preferences;
		Preferences->OnInputPreferencesChanged.AddUniqueDynamic(this, &ThisClass::HandleInputPreferencesChanged);
	}
}

void UStageStatusBarWidget::UnbindSources()
{
	if (AModularPlayerController* OwningController = Controller.Get())
	{
		OwningController->OnRequestRejected.RemoveDynamic(this, &ThisClass::HandleRequestRejected);
		OwningController->OnPossessedPawnChanged.RemoveDynamic(this, &ThisClass::HandlePossessedPawnChanged);
	}
	if (UStagePlacementToolComponent* Tool = PlacementTool.Get())
	{
		Tool->OnEditModeChanged.RemoveDynamic(this, &ThisClass::HandleEditModeChanged);
		Tool->OnArmedItemChanged.RemoveDynamic(this, &ThisClass::HandleArmedItemChanged);
	}
	if (UStageEconomySubsystem* EconomySubsystem = Economy.Get())
	{
		EconomySubsystem->OnPurchaseCompleted.RemoveDynamic(this, &ThisClass::HandlePurchaseCompleted);
	}
	if (UStageEditHistorySubsystem* HistorySubsystem = History.Get())
	{
		HistorySubsystem->OnHistoryChanged.RemoveDynamic(this, &ThisClass::HandleHistoryChanged);
	}
	if (USelectionComponent* SelectionComponent = Selection.Get())
	{
		SelectionComponent->OnSelectionSetChanged.RemoveDynamic(this, &ThisClass::HandleSelectionSetChanged);
	}
	if (UStageSceneComponent* Scenes = SceneFiles.Get())
	{
		Scenes->OnSceneOperationFinished.RemoveDynamic(this, &ThisClass::HandleSceneOperationFinished);
	}
	if (UStageRenderSubsystem* RenderSubsystem = Render.Get())
	{
		RenderSubsystem->OnRenderStateChanged.RemoveDynamic(this, &ThisClass::HandleRenderStateChanged);
		RenderSubsystem->OnRenderFinished.RemoveDynamic(this, &ThisClass::HandleRenderFinished);
	}
	if (UStageSpanToolComponent* Tool = SpanTool.Get())
	{
		Tool->OnStatusChanged.RemoveDynamic(this, &ThisClass::HandleSpanStatusChanged);
	}
	if (UStageBillOfMaterialsSubsystem* Report = BillOfMaterials.Get())
	{
		Report->OnExportFinished.RemoveDynamic(this, &ThisClass::HandleReportExportFinished);
	}
	if (UStagePatchSheetSubsystem* PatchSheetSubsystem = PatchSheet.Get())
	{
		PatchSheetSubsystem->OnExportFinished.RemoveDynamic(this, &ThisClass::HandleReportExportFinished);
	}
	if (UStageCableScheduleSubsystem* CableScheduleSubsystem = CableSchedule.Get())
	{
		CableScheduleSubsystem->OnExportFinished.RemoveDynamic(this, &ThisClass::HandleReportExportFinished);
	}
	if (UStageMeshImportSubsystem* ImportSubsystem = Importer.Get())
	{
		ImportSubsystem->OnImportStarted.RemoveDynamic(this, &ThisClass::HandleModelImportStarted);
		ImportSubsystem->OnImportFinished.RemoveDynamic(this, &ThisClass::HandleModelImportFinished);
	}
	if (UStageInputPreferencesSubsystem* Preferences = InputPreferences.Get())
	{
		Preferences->OnInputPreferencesChanged.RemoveDynamic(this, &ThisClass::HandleInputPreferencesChanged);
	}
	InputPreferences.Reset();
	BindCameraPawn(nullptr);

	Controller.Reset();
	PlacementTool.Reset();
	Economy.Reset();
	History.Reset();
	Selection.Reset();
	SceneFiles.Reset();
	Render.Reset();
	SpanTool.Reset();
	BillOfMaterials.Reset();
	PatchSheet.Reset();
	CableSchedule.Reset();
	Importer.Reset();
}

void UStageStatusBarWidget::BindCameraPawn(APawn* Pawn)
{
	if (AStageCameraPawn* Previous = CameraPawn.Get())
	{
		Previous->OnCameraSpeedChanged.RemoveDynamic(this, &ThisClass::HandleCameraSpeedChanged);
	}
	AStageCameraPawn* NewCamera = Cast<AStageCameraPawn>(Pawn);
	CameraPawn = NewCamera;
	if (NewCamera)
	{
		NewCamera->OnCameraSpeedChanged.AddUniqueDynamic(this, &ThisClass::HandleCameraSpeedChanged);
	}
}

void UStageStatusBarWidget::RefreshMode()
{
	const UStagePlacementToolComponent* Tool = PlacementTool.Get();
	const EStageEditMode Mode = Tool ? Tool->GetEditMode() : EStageEditMode::Select;
	const UBaseItemData* Armed = Tool ? Tool->GetArmedItem() : nullptr;
	const FText ArmedName = !Armed ? FText::GetEmpty()
		: (Armed->DisplayName.IsEmpty() ? FText::FromName(Armed->GetFName()) : Armed->DisplayName);

	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
	const bool bPlacing = Mode == EStageEditMode::Place;
	if (ModeChip)
	{
		ModeChip->SetBrush(bPlacing ? MakeBox(Palette.AccentColor, CornerRadius)
			: MakeBox(Palette.PanelHeader, CornerRadius, Palette.Divider, 1.f));
	}
	if (ModeText)
	{
		ModeText->SetText(bPlacing ? LOCTEXT("PlaceChip", "PLACE") : LOCTEXT("SelectChip", "SELECT"));
		ModeText->SetColorAndOpacity(FSlateColor(bPlacing ? Palette.WindowBackground : Palette.TextSecondary));
	}
	if (HintText)
	{
		const USelectionComponent* SelectionComponent = Selection.Get();
		const UStageSpanToolComponent* Span = SpanTool.Get();
		const UStageInputPreferencesSubsystem* Preferences = InputPreferences.Get();
		const StageStatusBar::FHintKeys Keys = Preferences ? StageStatusBar::FHintKeys::FromBindings(Preferences->GetKeyBindings()) : StageStatusBar::FHintKeys::Defaults();
		HintText->SetText(Span && Span->IsToolActive()
			? StageStatusBar::MakeSpanHint(Span->GetStatus(), Keys)
			: StageStatusBar::MakeHint(Mode, ArmedName, SelectionComponent ? SelectionComponent->GetSelectionCount() : 0, Keys));
	}
}

void UStageStatusBarWidget::HandleSpanStatusChanged(const FStageSpanToolStatus& Status)
{
	RefreshMode();
}

void UStageStatusBarWidget::HandleInputPreferencesChanged()
{
	RefreshMode();
}

void UStageStatusBarWidget::HandleReportExportFinished(const FStageReportExportResult& Result)
{
	ShowMessage(Result.Message, Result.bSucceeded ? EMessageSeverity::Info : EMessageSeverity::Warning);
}

void UStageStatusBarWidget::HandleModelImportStarted(const FString& SourceFile)
{
	const UStageMeshImportSubsystem* ImportSubsystem = Importer.Get();
	const int32 Waiting = ImportSubsystem ? ImportSubsystem->GetPendingCount() - 1 : 0;
	const FText File = FText::FromString(FPaths::GetCleanFilename(SourceFile));
	ShowMessage(Waiting > 0
		? FText::Format(LOCTEXT("ImportingQueued", "Importing {0}... ({1} more waiting)"), File, FText::AsNumber(Waiting))
		: FText::Format(LOCTEXT("Importing", "Importing {0}..."), File), EMessageSeverity::Info);
}

void UStageStatusBarWidget::HandleModelImportFinished(const FStageMeshImportOutcome& Outcome)
{
	// Models restored at startup are expected to come back; only a failure to restore one is news.
	if (Outcome.Origin == EStageMeshImportOrigin::Restore && Outcome.IsSuccess())
	{
		return;
	}
	ShowMessage(Outcome.IsSuccess()
		? FText::Format(LOCTEXT("ImportedHint", "{0} Click it in the Library to place it."), Outcome.Message)
		: Outcome.Message, Outcome.IsSuccess() ? EMessageSeverity::Info : EMessageSeverity::Warning);
}

void UStageStatusBarWidget::ShowMessage(const FText& Message, EMessageSeverity Severity)
{
	if (!MessageText)
	{
		return;
	}

	const UStageCraftUITheme& Palette = GetTheme();
	MessageText->SetText(Message);
	MessageText->SetColorAndOpacity(FSlateColor(Severity == EMessageSeverity::Warning ? Palette.WarningColor : Palette.TextPrimary));
	MessageText->SetVisibility(ESlateVisibility::HitTestInvisible);

	// One timer: a newer message replaces the older one and restarts the countdown.
	if (const UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(MessageTimer, this, &ThisClass::ClearMessage, MessageDuration);
	}
}

void UStageStatusBarWidget::ClearMessage()
{
	if (MessageText)
	{
		MessageText->SetText(FText::GetEmpty());
		MessageText->SetVisibility(ESlateVisibility::Collapsed);
	}
}

void UStageStatusBarWidget::HandleEditModeChanged(EStageEditMode NewMode, EStageEditMode PreviousMode)
{
	RefreshMode();
}

void UStageStatusBarWidget::HandleArmedItemChanged(UBaseItemData* ArmedItem)
{
	RefreshMode();
}

void UStageStatusBarWidget::HandleRequestRejected(const FStageEconomyResultInfo& Result)
{
	ShowMessage(Result.Message, EMessageSeverity::Warning);
}

void UStageStatusBarWidget::HandlePurchaseCompleted(UStageProductData* Product, FStageEconomyResultInfo Result)
{
	// Failures arrive through the controller's OnRequestRejected; only successes are announced here.
	if (Result.IsSuccess() && Product)
	{
		ShowMessage(FText::Format(LOCTEXT("Purchased", "Purchased {0}"), Product->DisplayName), EMessageSeverity::Info);
	}
}

void UStageStatusBarWidget::HandlePossessedPawnChanged(APawn* PreviousPawn, APawn* NewPawn)
{
	BindCameraPawn(NewPawn);
}

void UStageStatusBarWidget::HandleHistoryChanged(EStageHistoryChange Change, const FText& Description)
{
	// Recording is silent (it happens on every edit); undo and redo say what they reverted, like the editor does.
	if (Change == EStageHistoryChange::Undone)
	{
		ShowMessage(FText::Format(LOCTEXT("Undone", "Undo: {0}"), Description), EMessageSeverity::Info);
	}
	else if (Change == EStageHistoryChange::Redone)
	{
		ShowMessage(FText::Format(LOCTEXT("Redone", "Redo: {0}"), Description), EMessageSeverity::Info);
	}
}

void UStageStatusBarWidget::HandleSelectionSetChanged(int32 Count)
{
	RefreshMode();
}

void UStageStatusBarWidget::HandleSceneOperationFinished(const FStageSceneOperationResult& Result)
{
	// Failures arrive through the controller's OnRequestRejected (with the error cue); successes are announced here.
	if (Result.IsSuccess())
	{
		ShowMessage(Result.Message, Result.SkippedCount > 0 ? EMessageSeverity::Warning : EMessageSeverity::Info);
	}
}

void UStageStatusBarWidget::HandleRenderStateChanged(EStageRenderState NewState)
{
	const UStageRenderSubsystem* RenderSubsystem = Render.Get();
	if (NewState == EStageRenderState::Capturing && RenderSubsystem)
	{
		const FIntPoint Size = StageRender::GetOutputSize(RenderSubsystem->GetSettings().Resolution);
		ShowMessage(FText::Format(LOCTEXT("Rendering", "Rendering {0} x {1}..."), FText::AsNumber(Size.X), FText::AsNumber(Size.Y)), EMessageSeverity::Info);
	}
}

void UStageStatusBarWidget::HandleRenderFinished(const FStageRenderResult& Result)
{
	FNumberFormattingOptions OneDecimal;
	OneDecimal.SetMinimumFractionalDigits(1).SetMaximumFractionalDigits(1);
	ShowMessage(Result.bSucceeded
			? FText::Format(LOCTEXT("Rendered", "{0}  ({1} s)"), Result.Message, FText::AsNumber(Result.Seconds, &OneDecimal))
			: Result.Message,
		Result.bSucceeded ? EMessageSeverity::Info : EMessageSeverity::Warning);
}

void UStageStatusBarWidget::HandleCameraSpeedChanged(float NewCameraSpeed)
{
	const AStageCameraPawn* Camera = CameraPawn.Get();
	FNumberFormattingOptions TwoDecimals;
	TwoDecimals.SetMinimumFractionalDigits(2).SetMaximumFractionalDigits(2);
	FNumberFormattingOptions OneDecimal;
	OneDecimal.SetMinimumFractionalDigits(1).SetMaximumFractionalDigits(1);
	// The editor's own number first (as on its camera speed slider), then what it means for flight.
	ShowMessage(FText::Format(LOCTEXT("CameraSpeed", "Camera speed {0}   ·   flying {1} m/s"),
		FText::AsNumber(NewCameraSpeed, &TwoDecimals),
		FText::AsNumber(Camera ? Camera->GetFlySpeed() / 100.f : NewCameraSpeed * 20.f, &OneDecimal)), EMessageSeverity::Info);
}

#undef LOCTEXT_NAMESPACE
