// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageSketchCanvasWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/ButtonSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Data/BaseItemData.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Player/ModularPlayerController.h"
#include "Sketch/StageSketchGenerator.h"
#include "Sketch/StageSketchMath.h"
#include "Sketch/StageSketchSubsystem.h"
#include "Subsystems/StageItemSubsystem.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageSketchSurfaceWidget.h"
#include "UI/StageToolButton.h"
#include "UI/StageValueSlider.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageSketchCanvasWidget)

#define LOCTEXT_NAMESPACE "StageSketchCanvasWidget"

namespace StageSketchCanvas
{
	constexpr float RowGap = 4.f;
	constexpr float ChoiceGap = 3.f;
	constexpr float GroupGap = 14.f;
	constexpr float SliderWidth = 110.f;
	constexpr float ValueWidth = 46.f;

	/** Most plan warnings listed; the rest are counted. */
	constexpr int32 MaxWarningsShown = 3;

	/** Slider steps, in cm. */
	constexpr float DeckHeightStep = 10.f;
	constexpr float TrussHeightStep = 50.f;

	/** Grid presets (cm) and smoothing presets, in button order. */
	constexpr float GridPresets[] = { 25.f, 50.f, 100.f };
	constexpr float SmoothingPresets[] = { 0.f, 0.5f, 0.8f };

	/** Undo stroke, Clear, Frame. */
	enum class EAction : int32 { UndoStroke, Clear, Frame };

	constexpr EStageSketchTool Tools[] = { EStageSketchTool::Freehand, EStageSketchTool::Shape, EStageSketchTool::Truss, EStageSketchTool::Fixture, EStageSketchTool::Erase };

	FText ToolHint(EStageSketchTool Tool)
	{
		switch (Tool)
		{
		case EStageSketchTool::Freehand:	return LOCTEXT("HintFreehand", "Drag to draw. End near the start for a stage shape; an open line becomes a curved truss run.");
		case EStageSketchTool::Shape:		return LOCTEXT("HintShape", "Click the corners of a deck. Click the first corner, double-click or press Enter to close. Backspace removes a corner, Esc cancels.");
		case EStageSketchTool::Truss:		return LOCTEXT("HintTruss", "Click the points of a truss run. Double-click or Enter finishes; a click on the first point closes a loop.");
		case EStageSketchTool::Fixture:		return LOCTEXT("HintFixture", "Click to place a fixture. Near a truss line it hangs from the truss; on a deck it stands on top.");
		default:							return LOCTEXT("HintErase", "Click a stroke to remove it.");
		}
	}

	int32 FindPreset(TConstArrayView<float> Presets, float Value)
	{
		for (int32 Index = 0; Index < Presets.Num(); ++Index)
		{
			if (FMath::IsNearlyEqual(Presets[Index], Value, 0.01f))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}
}

const UStageCraftUITheme& UStageSketchCanvasWidget::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

UStageSketchSubsystem* UStageSketchCanvasWidget::GetSketch() const
{
	return BoundSketch.Get();
}

AModularPlayerController* UStageSketchCanvasWidget::GetStageController() const
{
	return Cast<AModularPlayerController>(GetOwningPlayer());
}

void UStageSketchCanvasWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
}

UTextBlock* UStageSketchCanvasWidget::MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
	Block->SetColorAndOpacity(FSlateColor(Color));
	return Block;
}

UStageChoiceButton* UStageSketchCanvasWidget::MakeButton(const FText& Label, const FText& Tip, int32 Index, void (UStageSketchCanvasWidget::*Handler)(int32))
{
	UStageChoiceButton* Button = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
	Button->SetChoiceIndex(Index);
	Button->SetToolTipText(Tip);
	Button->OnPicked.BindUObject(this, Handler);
	UTextBlock* Text = MakeText(Label, 8, TEXT("Bold"), GetTheme().TextSecondary);
	Text->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Button->AddChild(Text)))
	{
		LabelSlot->SetPadding(FMargin(7.f, 3.f));
		LabelSlot->SetHorizontalAlignment(HAlign_Center);
		LabelSlot->SetVerticalAlignment(VAlign_Center);
	}
	StyleChoice(*Button, false);
	return Button;
}

UWidget* UStageSketchCanvasWidget::BuildRow(TConstArrayView<UWidget*> Children)
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	for (int32 Index = 0; Index < Children.Num(); ++Index)
	{
		if (UHorizontalBoxSlot* ChildSlot = Row->AddChildToHorizontalBox(Children[Index]))
		{
			ChildSlot->SetPadding(FMargin(Index == 0 ? 0.f : StageSketchCanvas::GroupGap, 0.f, 0.f, 0.f));
			ChildSlot->SetVerticalAlignment(VAlign_Center);
		}
	}
	return Row;
}

UWidget* UStageSketchCanvasWidget::BuildSegment(TConstArrayView<FText> Labels, TConstArrayView<FText> Tips, TArray<TObjectPtr<UStageChoiceButton>>& OutButtons,
	void (UStageSketchCanvasWidget::*Handler)(int32))
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	OutButtons.Reset(Labels.Num());
	for (int32 Index = 0; Index < Labels.Num(); ++Index)
	{
		UStageChoiceButton* Button = MakeButton(Labels[Index], Tips.IsValidIndex(Index) ? Tips[Index] : FText::GetEmpty(), Index, Handler);
		if (UHorizontalBoxSlot* ButtonSlot = Row->AddChildToHorizontalBox(Button))
		{
			ButtonSlot->SetPadding(FMargin(Index == 0 ? 0.f : StageSketchCanvas::ChoiceGap, 0.f, 0.f, 0.f));
		}
		OutButtons.Add(Button);
	}
	return Row;
}

UWidget* UStageSketchCanvasWidget::BuildSlider(ESlider Slider, const FText& Label, const FText& Tip, float Min, float Max)
{
	using namespace StageSketchCanvas;
	const UStageCraftUITheme& Palette = GetTheme();
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Row->SetToolTipText(Tip);
	if (UHorizontalBoxSlot* LabelSlot = Row->AddChildToHorizontalBox(MakeText(Label, 7, TEXT("Bold"), Palette.TextSecondary)))
	{
		LabelSlot->SetVerticalAlignment(VAlign_Center);
		LabelSlot->SetPadding(FMargin(0.f, 0.f, 6.f, 0.f));
	}

	UStageValueSlider* Control = WidgetTree->ConstructWidget<UStageValueSlider>(UStageValueSlider::StaticClass());
	Control->SetSliderIndex(int32(Slider));
	Control->SetMinValue(Min);
	Control->SetMaxValue(Max);
	Control->SetWidgetStyle(StageCraftWidgetStyle::MakeSlider(Palette));
	Control->OnMoved.BindUObject(this, &ThisClass::HandleSliderMoved);
	USizeBox* SliderBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	SliderBox->SetWidthOverride(SliderWidth);
	SliderBox->SetHeightOverride(StageCraftWidgetStyle::FieldHeight);
	SliderBox->SetContent(Control);
	if (UHorizontalBoxSlot* SliderSlot = Row->AddChildToHorizontalBox(SliderBox))
	{
		SliderSlot->SetVerticalAlignment(VAlign_Center);
	}
	Sliders[int32(Slider)] = Control;

	USizeBox* ValueBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	ValueBox->SetWidthOverride(ValueWidth);
	UTextBlock* Value = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextPrimary);
	Value->SetJustification(ETextJustify::Right);
	ValueBox->SetContent(Value);
	if (UHorizontalBoxSlot* ValueSlot = Row->AddChildToHorizontalBox(ValueBox))
	{
		ValueSlot->SetVerticalAlignment(VAlign_Center);
	}
	SliderValues[int32(Slider)] = Value;
	return Row;
}

UWidget* UStageSketchCanvasWidget::BuildHeader()
{
	const UStageCraftUITheme& Palette = GetTheme();
	UBorder* Header = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Header"));
	Header->SetBrush(StageCraftWidgetStyle::MakeBox(Palette.PanelHeader));
	Header->SetPadding(FMargin(10.f, 8.f));
	UTextBlock* Title = MakeText(LOCTEXT("Title", "SKETCH TO STAGE"), 9, TEXT("Bold"), Palette.TextPrimary);
	Title->SetToolTipText(LOCTEXT("TitleTip", "Draw the stage from above, then generate decks, truss and fixtures in 3D. Ctrl+Z removes a whole generation."));
	Header->SetContent(Title);
	return Header;
}

void UStageSketchCanvasWidget::BuildTree()
{
	using namespace StageCraftWidgetStyle;
	using namespace StageSketchCanvas;
	const UStageCraftUITheme& Palette = GetTheme();
	Sliders.SetNum(int32(ESlider::Count));
	SliderValues.SetNum(int32(ESlider::Count));

	UBorder* Root = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Root"));
	Root->SetBrush(MakeBox(Palette.PanelBackground));
	Root->SetPadding(FMargin(0.f));
	WidgetTree->RootWidget = Root;

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Column"));
	Root->SetContent(Column);
	Column->AddChildToVerticalBox(BuildHeader());

	UVerticalBox* Body = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Body"));
	UBorder* BodyPadding = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("BodyPadding"));
	BodyPadding->SetBrush(MakeBox(FLinearColor::Transparent));
	BodyPadding->SetPadding(FMargin(10.f, 8.f));
	BodyPadding->SetContent(Body);
	if (UVerticalBoxSlot* BodySlot = Column->AddChildToVerticalBox(BodyPadding))
	{
		BodySlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	const auto Add = [Body](UWidget* Widget, bool bFill = false)
	{
		if (UVerticalBoxSlot* RowSlot = Body->AddChildToVerticalBox(Widget))
		{
			RowSlot->SetPadding(FMargin(0.f, RowGap));
			if (bFill)
			{
				RowSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			}
		}
	};

	// Tools and sketch actions.
	UWidget* ToolSegment = BuildSegment(
		{ LOCTEXT("ToolFreehand", "FREEHAND"), LOCTEXT("ToolShape", "STAGE SHAPE"), LOCTEXT("ToolTruss", "TRUSS LINE"), LOCTEXT("ToolFixture", "FIXTURE"), LOCTEXT("ToolErase", "ERASE") },
		{ ToolHint(EStageSketchTool::Freehand), ToolHint(EStageSketchTool::Shape), ToolHint(EStageSketchTool::Truss), ToolHint(EStageSketchTool::Fixture), ToolHint(EStageSketchTool::Erase) },
		ToolButtons, &ThisClass::HandleToolPicked);
	UWidget* ActionSegment = BuildSegment({ LOCTEXT("UndoStroke", "UNDO STROKE"), LOCTEXT("Clear", "CLEAR"), LOCTEXT("Frame", "FRAME") },
		{ LOCTEXT("UndoStrokeTip", "Remove the last stroke from the sketch (the stage is not changed)."),
		  LOCTEXT("ClearTip", "Remove every stroke from the sketch (the stage is not changed)."),
		  LOCTEXT("FrameTip", "Fit the view to the sketch. The wheel zooms, the right or middle button pans.") },
		ActionButtons, &ThisClass::HandleActionPicked);
	Add(BuildRow({ ToolSegment, ActionSegment }));

	HintText = MakeText(ToolHint(EStageSketchTool::Shape), 7, TEXT("Italic"), Palette.TextSecondary);
	HintText->SetAutoWrapText(true);
	Add(HintText);

	// Heights, grid, snapping and smoothing.
	UWidget* DeckSlider = BuildSlider(ESlider::DeckHeight, LOCTEXT("DeckHeight", "DECK"),
		LOCTEXT("DeckHeightTip", "Height of the next stage shapes you draw (shapes already drawn keep theirs)."), StageSketchMath::MinDeckHeight, 300.f);
	UWidget* TrussSlider = BuildSlider(ESlider::TrussHeight, LOCTEXT("TrussHeight", "TRUSS"),
		LOCTEXT("TrussHeightTip", "Height of the generated truss; fixtures near a truss line hang from it."), 200.f, 1500.f);
	Add(BuildRow({ DeckSlider, TrussSlider }));

	UHorizontalBox* GridGroup = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	GridGroup->AddChildToHorizontalBox(MakeText(LOCTEXT("Grid", "GRID  "), 7, TEXT("Bold"), Palette.TextSecondary))->SetVerticalAlignment(VAlign_Center);
	GridGroup->AddChildToHorizontalBox(BuildSegment({ LOCTEXT("Grid25", "25 cm"), LOCTEXT("Grid50", "50 cm"), LOCTEXT("Grid100", "1 m") },
		{ LOCTEXT("GridTip", "Grid step: deck corners and fixtures snap to it.") }, GridButtons, &ThisClass::HandleGridPicked));
	UHorizontalBox* SnapGroup = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	SnapGroup->AddChildToHorizontalBox(MakeText(LOCTEXT("Snap", "SNAP  "), 7, TEXT("Bold"), Palette.TextSecondary))->SetVerticalAlignment(VAlign_Center);
	SnapGroup->AddChildToHorizontalBox(BuildSegment({ LOCTEXT("Off", "OFF"), LOCTEXT("On", "ON") },
		{ LOCTEXT("SnapTip", "Snap clicked points to the grid. Truss lines always snap to whole sections of their truss.") }, SnapButtons, &ThisClass::HandleSnapPicked));
	UHorizontalBox* SmoothGroup = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	SmoothGroup->AddChildToHorizontalBox(MakeText(LOCTEXT("Smooth", "SMOOTH  "), 7, TEXT("Bold"), Palette.TextSecondary))->SetVerticalAlignment(VAlign_Center);
	SmoothGroup->AddChildToHorizontalBox(BuildSegment({ LOCTEXT("SmoothOff", "OFF"), LOCTEXT("SmoothLow", "LOW"), LOCTEXT("SmoothHigh", "HIGH") },
		{ LOCTEXT("SmoothTip", "Freehand stabiliser: higher values trail the cursor for smoother strokes.") }, SmoothingButtons, &ThisClass::HandleSmoothingPicked));
	Add(BuildRow({ GridGroup, SnapGroup, SmoothGroup }));

	// The catalog item per element.
	ItemButtons.Reset();
	TArray<UWidget*> ItemGroups;
	const FText ItemLabels[] = { LOCTEXT("ItemDeck", "DECK ITEM  "), LOCTEXT("ItemTruss", "TRUSS ITEM  "), LOCTEXT("ItemFixture", "FIXTURE ITEM  ") };
	for (int32 Element = 0; Element < int32(EStageSketchElement::Count); ++Element)
	{
		UHorizontalBox* Group = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		Group->AddChildToHorizontalBox(MakeText(ItemLabels[Element], 7, TEXT("Bold"), Palette.TextSecondary))->SetVerticalAlignment(VAlign_Center);
		UStageChoiceButton* Button = MakeButton(FText::GetEmpty(), LOCTEXT("ItemTip", "The catalog item this element is built from. Click for the next one."), Element, &ThisClass::HandleItemPicked);
		USizeBox* ButtonBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		ButtonBox->SetWidthOverride(170.f);
		ButtonBox->SetClipping(EWidgetClipping::ClipToBounds);
		ButtonBox->SetContent(Button);
		// A long item name ends in an ellipsis inside its button instead of running into the next label.
		if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Button->GetContentSlot()))
		{
			LabelSlot->SetHorizontalAlignment(HAlign_Fill);
		}
		Group->AddChildToHorizontalBox(ButtonBox);
		ItemButtons.Add(Button);
		ItemGroups.Add(Group);
	}
	Add(BuildRow(ItemGroups));

	// The canvas.
	UBorder* CanvasFrame = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("CanvasFrame"));
	CanvasFrame->SetBrush(MakeBox(Palette.WindowBackground, 0.f, Palette.Divider, 1.f));
	CanvasFrame->SetPadding(FMargin(1.f));
	Surface = WidgetTree->ConstructWidget<UStageSketchSurfaceWidget>(UStageSketchSurfaceWidget::StaticClass(), TEXT("Surface"));
	Surface->SetTheme(Theme);
	CanvasFrame->SetContent(Surface);
	USizeBox* CanvasMin = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("CanvasMinimum"));
	CanvasMin->SetMinDesiredHeight(260.f);
	CanvasMin->SetMinDesiredWidth(360.f);
	CanvasMin->SetContent(CanvasFrame);
	Add(CanvasMin, /*bFill*/ true);

	// What will be built, warnings, the last result, and GENERATE.
	PlanText = MakeText(FText::GetEmpty(), 8, TEXT("Bold"), Palette.TextPrimary);
	Add(PlanText);
	WarningText = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), Palette.WarningColor);
	WarningText->SetAutoWrapText(true);
	Add(WarningText);

	ResultText = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextSecondary);
	ResultText->SetAutoWrapText(true);
	GenerateButton = MakeButton(LOCTEXT("Generate", "GENERATE 3D"),
		LOCTEXT("GenerateTip", "Build the sketch on the stage: decks, truss runs and fixtures, checked by the stage's rules. All or nothing, and one Ctrl+Z removes it."),
		0, &ThisClass::HandleGeneratePicked);
	UHorizontalBox* Footer = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	if (UHorizontalBoxSlot* ResultSlot = Footer->AddChildToHorizontalBox(ResultText))
	{
		ResultSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		ResultSlot->SetVerticalAlignment(VAlign_Center);
	}
	if (UHorizontalBoxSlot* GenerateSlot = Footer->AddChildToHorizontalBox(GenerateButton))
	{
		GenerateSlot->SetPadding(FMargin(GroupGap, 0.f, 0.f, 0.f));
		GenerateSlot->SetVerticalAlignment(VAlign_Center);
	}
	Add(Footer);
}

void UStageSketchCanvasWidget::NativeConstruct()
{
	Super::NativeConstruct();
	if (UStageSketchSubsystem* Sketch = UWorld::GetSubsystem<UStageSketchSubsystem>(GetWorld()))
	{
		BoundSketch = Sketch;
		Sketch->OnSketchChanged.AddUniqueDynamic(this, &ThisClass::HandleSketchChanged);
		Sketch->OnGenerationFinished.AddUniqueDynamic(this, &ThisClass::HandleGenerationFinished);
	}
	if (UStageItemSubsystem* Catalog = GetGameInstance() ? GetGameInstance()->GetSubsystem<UStageItemSubsystem>() : nullptr)
	{
		BoundCatalog = Catalog;
		Catalog->OnCatalogLoaded.AddUniqueDynamic(this, &ThisClass::HandleCatalogChanged);
		Catalog->OnCatalogChanged.AddUniqueDynamic(this, &ThisClass::HandleCatalogChanged);
	}
	Refresh();
}

void UStageSketchCanvasWidget::NativeDestruct()
{
	if (UStageSketchSubsystem* Sketch = BoundSketch.Get())
	{
		Sketch->OnSketchChanged.RemoveDynamic(this, &ThisClass::HandleSketchChanged);
		Sketch->OnGenerationFinished.RemoveDynamic(this, &ThisClass::HandleGenerationFinished);
	}
	if (UStageItemSubsystem* Catalog = BoundCatalog.Get())
	{
		Catalog->OnCatalogLoaded.RemoveDynamic(this, &ThisClass::HandleCatalogChanged);
		Catalog->OnCatalogChanged.RemoveDynamic(this, &ThisClass::HandleCatalogChanged);
	}
	BoundSketch.Reset();
	BoundCatalog.Reset();
	Super::NativeDestruct();
}

void UStageSketchCanvasWidget::StyleChoice(UStageChoiceButton& Button, bool bSelected, bool bPrimary) const
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
	const bool bLit = bSelected || bPrimary;
	FButtonStyle Style = bLit
		? MakeFlatButton(Palette.AccentColor, Palette.AccentColor * 1.1f, Palette.AccentColor * 0.85f)
		: MakeFlatButton(FLinearColor::Transparent, Palette.RowHover, Palette.Divider);
	if (!bLit)
	{
		Style.SetNormal(MakeBox(FLinearColor::Transparent, CornerRadius, Palette.Divider, 1.f));
	}
	Button.SetStyle(Style);
	if (UTextBlock* Label = Cast<UTextBlock>(Button.GetContent()))
	{
		Label->SetColorAndOpacity(FSlateColor(bLit ? Palette.WindowBackground : Palette.TextSecondary));
	}
}

void UStageSketchCanvasWidget::Refresh()
{
	using namespace StageSketchCanvas;
	const UStageSketchSubsystem* Sketch = GetSketch();
	const UStageCraftUITheme& Palette = GetTheme();
	const FStageSketchSettings Settings = Sketch ? Sketch->GetSettings() : FStageSketchSettings();

	const EStageSketchTool Tool = Surface ? Surface->GetTool() : EStageSketchTool::Shape;
	for (UStageChoiceButton* Button : ToolButtons)
	{
		StyleChoice(*Button, Tools[Button->GetChoiceIndex()] == Tool);
	}
	for (UStageChoiceButton* Button : ActionButtons)
	{
		StyleChoice(*Button, false);
		Button->SetIsEnabled(Sketch && (Button->GetChoiceIndex() == int32(EAction::Frame) || !Sketch->GetStrokes().IsEmpty()));
	}
	if (HintText)
	{
		HintText->SetText(ToolHint(Tool));
	}

	const auto StyleGroup = [this, Sketch](TArray<TObjectPtr<UStageChoiceButton>>& Buttons, int32 Selected)
	{
		for (UStageChoiceButton* Button : Buttons)
		{
			StyleChoice(*Button, Button->GetChoiceIndex() == Selected);
			Button->SetIsEnabled(Sketch != nullptr);
		}
	};
	StyleGroup(GridButtons, FindPreset(GridPresets, Settings.GridStep));
	StyleGroup(SnapButtons, Settings.bSnapToGrid ? 1 : 0);
	StyleGroup(SmoothingButtons, FindPreset(SmoothingPresets, Settings.Smoothing));

	FNumberFormattingOptions TwoDecimals = FNumberFormattingOptions::DefaultNoGrouping();
	TwoDecimals.SetMinimumFractionalDigits(2).SetMaximumFractionalDigits(2);
	const float SliderSettings[] = { Settings.DeckHeight, Settings.TrussHeight };
	for (int32 Index = 0; Index < int32(ESlider::Count); ++Index)
	{
		if (Sliders[Index])
		{
			Sliders[Index]->SetValue(SliderSettings[Index]);
			Sliders[Index]->SetIsEnabled(Sketch != nullptr);
		}
		if (SliderValues[Index])
		{
			SliderValues[Index]->SetText(FText::Format(LOCTEXT("Metres", "{0} m"), FText::AsNumber(SliderSettings[Index] * 0.01f, &TwoDecimals)));
		}
	}

	for (UStageChoiceButton* Button : ItemButtons)
	{
		const EStageSketchElement Element = EStageSketchElement(Button->GetChoiceIndex());
		const UBaseItemData* Item = Sketch ? Sketch->GetItem(Element) : nullptr;
		if (UTextBlock* Label = Cast<UTextBlock>(Button->GetContent()))
		{
			Label->SetText(Item ? Item->DisplayName : LOCTEXT("NoItem", "none in catalog"));
		}
		// The full name, since a long one is cut with an ellipsis.
		Button->SetToolTipText(FText::Format(LOCTEXT("ItemButtonTip", "{0}\nThe catalog item this element is built from. Click for the next one."),
			Item ? Item->DisplayName : LOCTEXT("NoItemTip", "No catalog item can build this.")));
		StyleChoice(*Button, false);
		Button->SetIsEnabled(Sketch && Sketch->GetCandidateItems(Element).Num() > 1);
	}

	const FStageSketchBuildPlan Plan = Sketch ? Sketch->MakePlan() : FStageSketchBuildPlan();
	if (PlanText)
	{
		PlanText->SetText(Plan.IsEmpty()
			? (Sketch && !Sketch->GetStrokes().IsEmpty() ? LOCTEXT("PlanNothing", "Nothing in the sketch can be built yet.") : LOCTEXT("PlanEmpty", "Draw a stage shape, a truss line or fixtures to begin."))
			: FText::Format(LOCTEXT("PlanSummary", "Will build: {0}"), StageSketchGenerator::DescribePlan(Plan)));
	}
	if (WarningText)
	{
		TArray<FText> Shown;
		for (int32 Index = 0; Index < FMath::Min(Plan.Warnings.Num(), MaxWarningsShown); ++Index)
		{
			Shown.Add(Plan.Warnings[Index]);
		}
		if (Plan.Warnings.Num() > MaxWarningsShown)
		{
			Shown.Add(FText::Format(LOCTEXT("MoreWarnings", "and {0} more."), FText::AsNumber(Plan.Warnings.Num() - MaxWarningsShown)));
		}
		WarningText->SetText(FText::Join(FText::FromString(TEXT("\n")), Shown));
		WarningText->SetVisibility(Shown.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}
	if (ResultText)
	{
		const bool bPending = Sketch && Sketch->IsGenerationPending();
		ResultText->SetText(bPending ? LOCTEXT("Generating", "Generating...") : ResultMessage);
		ResultText->SetColorAndOpacity(FSlateColor(bResultSucceeded || bPending ? Palette.TextSecondary : Palette.WarningColor));
	}
	if (GenerateButton)
	{
		StyleChoice(*GenerateButton, false, /*bPrimary*/ !Plan.IsEmpty());
		GenerateButton->SetIsEnabled(!Plan.IsEmpty() && Sketch && !Sketch->IsGenerationPending() && GetStageController());
	}
}

// --- Handlers: write through the owner, then read back ---

void UStageSketchCanvasWidget::HandleToolPicked(int32 Index)
{
	if (Surface && Index >= 0 && Index < UE_ARRAY_COUNT(StageSketchCanvas::Tools))
	{
		Surface->SetTool(StageSketchCanvas::Tools[Index]);
	}
	Refresh();
}

void UStageSketchCanvasWidget::HandleActionPicked(int32 Index)
{
	using StageSketchCanvas::EAction;
	UStageSketchSubsystem* Sketch = GetSketch();
	switch (EAction(Index))
	{
	case EAction::UndoStroke:
		if (Surface)
		{
			Surface->CancelStroke();
		}
		if (Sketch)
		{
			Sketch->RemoveLastStroke();
		}
		break;
	case EAction::Clear:
		if (Surface)
		{
			Surface->CancelStroke();
		}
		if (Sketch)
		{
			Sketch->ClearStrokes();
		}
		break;
	case EAction::Frame:
		if (Surface)
		{
			Surface->FrameSketch();
		}
		break;
	}
	Refresh();
}

void UStageSketchCanvasWidget::HandleGridPicked(int32 Index)
{
	if (UStageSketchSubsystem* Sketch = GetSketch(); Sketch && Index >= 0 && Index < UE_ARRAY_COUNT(StageSketchCanvas::GridPresets))
	{
		FStageSketchSettings Settings = Sketch->GetSettings();
		Settings.GridStep = StageSketchCanvas::GridPresets[Index];
		Sketch->SetSettings(Settings);
	}
	Refresh();
}

void UStageSketchCanvasWidget::HandleSnapPicked(int32 Index)
{
	if (UStageSketchSubsystem* Sketch = GetSketch())
	{
		FStageSketchSettings Settings = Sketch->GetSettings();
		Settings.bSnapToGrid = Index == 1;
		Sketch->SetSettings(Settings);
	}
	Refresh();
}

void UStageSketchCanvasWidget::HandleSmoothingPicked(int32 Index)
{
	if (UStageSketchSubsystem* Sketch = GetSketch(); Sketch && Index >= 0 && Index < UE_ARRAY_COUNT(StageSketchCanvas::SmoothingPresets))
	{
		FStageSketchSettings Settings = Sketch->GetSettings();
		Settings.Smoothing = StageSketchCanvas::SmoothingPresets[Index];
		Sketch->SetSettings(Settings);
	}
	Refresh();
}

void UStageSketchCanvasWidget::HandleSliderMoved(int32 SliderIndex, float Value)
{
	using namespace StageSketchCanvas;
	if (UStageSketchSubsystem* Sketch = GetSketch())
	{
		FStageSketchSettings Settings = Sketch->GetSettings();
		if (SliderIndex == int32(ESlider::DeckHeight))
		{
			Settings.DeckHeight = FMath::GridSnap(Value, DeckHeightStep);
		}
		else if (SliderIndex == int32(ESlider::TrussHeight))
		{
			Settings.TrussHeight = FMath::GridSnap(Value, TrussHeightStep);
		}
		Sketch->SetSettings(Settings);
	}
	Refresh();
}

void UStageSketchCanvasWidget::HandleItemPicked(int32 Element)
{
	UStageSketchSubsystem* Sketch = GetSketch();
	if (Sketch && Element >= 0 && Element < int32(EStageSketchElement::Count))
	{
		const TArray<UBaseItemData*> Candidates = Sketch->GetCandidateItems(EStageSketchElement(Element));
		const int32 Current = Candidates.IndexOfByKey(Sketch->GetItem(EStageSketchElement(Element)));
		if (!Candidates.IsEmpty())
		{
			Sketch->SetItem(EStageSketchElement(Element), Candidates[(Current + 1) % Candidates.Num()]);
		}
	}
	Refresh();
}

void UStageSketchCanvasWidget::HandleGeneratePicked(int32 Unused)
{
	if (AModularPlayerController* Controller = GetStageController())
	{
		// A refusal is reported by the controller (status bar); the outcome arrives on OnGenerationFinished.
		Controller->RequestGenerateFromSketch();
	}
	Refresh();
}

void UStageSketchCanvasWidget::HandleSketchChanged()
{
	ResultMessage = FText::GetEmpty();
	bResultSucceeded = false;
	Refresh();
}

void UStageSketchCanvasWidget::HandleGenerationFinished(const FStageSketchGenerateResult& Result)
{
	ResultMessage = Result.Message;
	bResultSucceeded = Result.bSucceeded;
	Refresh();
}

void UStageSketchCanvasWidget::HandleCatalogChanged()
{
	Refresh();
}

#undef LOCTEXT_NAMESPACE
