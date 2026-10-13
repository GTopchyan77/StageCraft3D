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
#include "Import/StageMeshImportSubsystem.h"
#include "Misc/Paths.h"
#include "Placement/StagePlacementToolComponent.h"
#include "Player/ModularPlayerController.h"
#include "Player/StageCameraPawn.h"
#include "Render/StageRenderSubsystem.h"
#include "Scene/StageSceneComponent.h"
#include "Subsystems/StageEconomySubsystem.h"
#include "TimerManager.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageStatusBarWidget)

#define LOCTEXT_NAMESPACE "StageStatusBar"

namespace StageStatusBar
{
	FText MakeHint(EStageEditMode Mode, const FText& ArmedItemName, int32 SelectedCount)
	{
		if (Mode == EStageEditMode::Place)
		{
			return ArmedItemName.IsEmpty()
				? LOCTEXT("PlaceNoItem", "Pick an item in the Library to place it   ·   Esc or P: back to Select")
				: FText::Format(LOCTEXT("PlaceItem", "Placing {0}: each click places a copy   ·   Esc or P: finish"), ArmedItemName);
		}
		if (SelectedCount > 1)
		{
			return FText::Format(LOCTEXT("SelectGroup", "{0} items selected: the gizmo moves them together   ·   Delete: remove all   ·   Ctrl+Click: add / remove"),
				FText::AsNumber(SelectedCount));
		}
		return ArmedItemName.IsEmpty()
			? LOCTEXT("SelectNoItem", "Click an item to select it   ·   Ctrl+Click: select several   ·   Space: Move / Rotate / Scale   ·   Right mouse + WASD: fly")
			: FText::Format(LOCTEXT("SelectItem", "Click an item to select it   ·   Ctrl+Click: select several   ·   Space: Move / Rotate / Scale   ·   P: place {0}"), ArmedItemName);
	}

	FText MakeSpanHint(const FStageSpanToolStatus& Status)
	{
		if (Status.Phase == EStageSpanToolPhase::PickStart)
		{
			return FText::Format(LOCTEXT("SpanStart", "Drawing {0}: click where the run starts   ·   Esc or P: finish"), Status.ItemName);
		}
		if (!Status.bHasRun)
		{
			return FText::Format(LOCTEXT("SpanEndNoRun", "Drawing {0}: click where the run ends   ·   Esc: new start point"), Status.ItemName);
		}

		FNumberFormattingOptions TwoDecimals;
		TwoDecimals.SetMinimumFractionalDigits(2).SetMaximumFractionalDigits(2);
		const FText Run = FText::Format(LOCTEXT("SpanRun", "{0} m, {1} {1}|plural(one=segment,other=segments)"),
			FText::AsNumber(Status.Length * 0.01, &TwoDecimals), Status.SegmentCount);
		return Status.bRefused
			? FText::Format(LOCTEXT("SpanRefused", "{0}: {1}, not allowed by this stage's limits   ·   Esc: new start point"), Status.ItemName, Run)
			: FText::Format(LOCTEXT("SpanEnd", "{0}: {1}   ·   click to place; the next run starts at its end   ·   Esc: new start point"), Status.ItemName, Run);
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

	if (UStageMeshImportSubsystem* ImportSubsystem = GameInstance ? GameInstance->GetSubsystem<UStageMeshImportSubsystem>() : nullptr)
	{
		Importer = ImportSubsystem;
		ImportSubsystem->OnImportStarted.AddUniqueDynamic(this, &ThisClass::HandleModelImportStarted);
		ImportSubsystem->OnImportFinished.AddUniqueDynamic(this, &ThisClass::HandleModelImportFinished);
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
	if (UStageMeshImportSubsystem* ImportSubsystem = Importer.Get())
	{
		ImportSubsystem->OnImportStarted.RemoveDynamic(this, &ThisClass::HandleModelImportStarted);
		ImportSubsystem->OnImportFinished.RemoveDynamic(this, &ThisClass::HandleModelImportFinished);
	}
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
	Importer.Reset();
}

void UStageStatusBarWidget::BindCameraPawn(APawn* Pawn)
{
	if (AStageCameraPawn* Previous = CameraPawn.Get())
	{
		Previous->OnFlySpeedChanged.RemoveDynamic(this, &ThisClass::HandleFlySpeedChanged);
	}
	AStageCameraPawn* NewCamera = Cast<AStageCameraPawn>(Pawn);
	CameraPawn = NewCamera;
	if (NewCamera)
	{
		NewCamera->OnFlySpeedChanged.AddUniqueDynamic(this, &ThisClass::HandleFlySpeedChanged);
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
		HintText->SetText(Span && Span->IsToolActive()
			? StageStatusBar::MakeSpanHint(Span->GetStatus())
			: StageStatusBar::MakeHint(Mode, ArmedName, SelectionComponent ? SelectionComponent->GetSelectionCount() : 0));
	}
}

void UStageStatusBarWidget::HandleSpanStatusChanged(const FStageSpanToolStatus& Status)
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

void UStageStatusBarWidget::HandleFlySpeedChanged(float NewFlySpeed)
{
	const AStageCameraPawn* Camera = CameraPawn.Get();
	FNumberFormattingOptions TwoDecimals;
	TwoDecimals.SetMinimumFractionalDigits(2).SetMaximumFractionalDigits(2);
	ShowMessage(FText::Format(LOCTEXT("FlySpeed", "Camera speed {0} m/s   ·   look x{1}"),
		FText::AsNumber(NewFlySpeed / 100.f, &TwoDecimals),
		FText::AsNumber(Camera ? Camera->GetLookSpeedScale() : 1.f, &TwoDecimals)), EMessageSeverity::Info);
}

#undef LOCTEXT_NAMESPACE
