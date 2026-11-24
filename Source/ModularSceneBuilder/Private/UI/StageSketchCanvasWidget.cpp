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
#include "Data/LightingFixtureData.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Import/StageFileDialog.h"
#include "Misc/Paths.h"
#include "Player/ModularPlayerController.h"
#include "Sketch/StageSketchGenerator.h"
#include "Sketch/StageSketchImageImport.h"
#include "Sketch/StageSketchMath.h"
#include "Sketch/StageSketchSubsystem.h"
#include "Subsystems/StageItemSubsystem.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageMenuButton.h"
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
	constexpr float PhotoWidthStep = 100.f;

	/** The toolbar dropdowns: their height, the gap between them, and widths with room for the longest label ("Snap 1 m · Smooth High"). */
	constexpr float MenuHeight = 22.f;
	constexpr float MenuGap = 4.f;
	constexpr float EditMenuWidth = 64.f;
	constexpr float SnapMenuWidth = 168.f;
	constexpr float ItemsMenuWidth = 70.f;
	constexpr float PhotoMenuWidth = 92.f;

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

	// The toolbar: the drawing tools stay one click away (they change every few strokes); the less frequent settings sit in
	// View-menu style dropdowns next to them, so the window has one tool row instead of five rows of segments (STATE.md #100).
	UWidget* ToolSegment = BuildSegment(
		{ LOCTEXT("ToolFreehand", "FREEHAND"), LOCTEXT("ToolShape", "STAGE SHAPE"), LOCTEXT("ToolTruss", "TRUSS LINE"), LOCTEXT("ToolFixture", "FIXTURE"), LOCTEXT("ToolErase", "ERASE") },
		{ ToolHint(EStageSketchTool::Freehand), ToolHint(EStageSketchTool::Shape), ToolHint(EStageSketchTool::Truss), ToolHint(EStageSketchTool::Fixture), ToolHint(EStageSketchTool::Erase) },
		ToolButtons, &ThisClass::HandleToolPicked);
	UHorizontalBox* Menus = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("ToolbarMenus"));
	const auto AddMenu = [Menus](UWidget* Box)
	{
		if (UHorizontalBoxSlot* MenuSlot = Menus->AddChildToHorizontalBox(Box))
		{
			MenuSlot->SetPadding(FMargin(Menus->GetChildrenCount() == 1 ? 0.f : MenuGap, 0.f, 0.f, 0.f));
			MenuSlot->SetVerticalAlignment(VAlign_Center);
		}
	};
	AddMenu(MakeMenuButton(TEXT("SketchEditMenu"), LOCTEXT("EditMenuTip", "Undo the last stroke, clear the sketch, or fit the view to it."),
		EditMenuWidth, &ThisClass::BuildEditMenu, EditMenuButton));
	AddMenu(MakeMenuButton(TEXT("SketchSnapMenu"), LOCTEXT("SnapMenuTip", "The grid step, snapping clicked points to it, and the freehand smoothing."),
		SnapMenuWidth, &ThisClass::BuildSnapMenu, SnapMenuButton));
	AddMenu(MakeMenuButton(TEXT("SketchItemsMenu"), LOCTEXT("ItemsMenuTip", "The catalog items the decks, truss runs and fixtures are built from."),
		ItemsMenuWidth, &ThisClass::BuildItemsMenu, ItemsMenuButton));
	AddMenu(MakeMenuButton(TEXT("SketchPhotoMenu"), LOCTEXT("PhotoMenuTip", "Import a photo or scan of a drawing, show it under the sketch, and choose who reads it."),
		PhotoMenuWidth, &ThisClass::BuildPhotoMenu, PhotoMenuButton));
	EditMenuButton->SetLabel(LOCTEXT("EditLabel", "Edit"));
	ItemsMenuButton->SetLabel(LOCTEXT("ItemsLabel", "Items"));
	Add(BuildRow({ ToolSegment, Menus }));

	HintText = MakeText(ToolHint(EStageSketchTool::Shape), 7, TEXT("Italic"), Palette.TextSecondary);
	HintText->SetAutoWrapText(true);
	Add(HintText);

	// The heights of what is drawn next, and the real width of the next imported photo.
	UWidget* DeckSlider = BuildSlider(ESlider::DeckHeight, LOCTEXT("DeckHeight", "DECK"),
		LOCTEXT("DeckHeightTip", "Height of the next stage shapes you draw (shapes already drawn keep theirs)."), StageSketchMath::MinDeckHeight, 300.f);
	UWidget* TrussSlider = BuildSlider(ESlider::TrussHeight, LOCTEXT("TrussHeight", "TRUSS"),
		LOCTEXT("TrussHeightTip", "Height of the generated truss; fixtures near a truss line hang from it."), 200.f, 1500.f);
	UWidget* PhotoWidthSlider = BuildSlider(ESlider::PhotoWidth, LOCTEXT("PhotoWidth", "PHOTO WIDTH"),
		LOCTEXT("PhotoWidthTip", "The real width of the whole photo: it sets the drawing's scale. Applies to the next import (Photo > Import Photo...)."),
		StageSketchMath::MinPhotoWidth, StageSketchMath::MaxPhotoWidth);
	Add(BuildRow({ DeckSlider, TrussSlider, PhotoWidthSlider }));

	// The canvas.
	UBorder* CanvasFrame = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("CanvasFrame"));
	CanvasFrame->SetBrush(MakeBox(Palette.WindowBackground, 0.f, Palette.Divider, 1.f));
	CanvasFrame->SetPadding(FMargin(1.f));
	Surface = WidgetTree->ConstructWidget<UStageSketchSurfaceWidget>(UStageSketchSurfaceWidget::StaticClass(), TEXT("Surface"));
	Surface->SetTheme(Theme);
	// Strokes and the photo can extend past the visible plan; keep them inside the frame.
	Surface->SetClipping(EWidgetClipping::ClipToBounds);
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
	// The refusal message of an image that looks like a photograph names these two; they show only while it stands (Refresh).
	UWidget* RefusedSegment = BuildSegment({ LOCTEXT("TraceAnyway", "TRACE ANYWAY"), LOCTEXT("UseColours", "USE COLOURS") },
		{ LOCTEXT("TraceAnywayTip", "Trace the refused image on this computer after all: for a drawing on textured or graph paper. A real photograph gives random strokes."),
		  LOCTEXT("UseColoursTip", "Light the selected fixtures with the photo's main colours instead: one colour per fixture, brighter colours brighter, one undo step.") },
		RefusedButtons, &ThisClass::HandleRefusedPicked);
	if (UHorizontalBoxSlot* RefusedSlot = Footer->AddChildToHorizontalBox(RefusedSegment))
	{
		RefusedSlot->SetPadding(FMargin(GroupGap, 0.f, 0.f, 0.f));
		RefusedSlot->SetVerticalAlignment(VAlign_Center);
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
		Sketch->OnImportChanged.AddUniqueDynamic(this, &ThisClass::HandleImportChanged);
		// An import that finished before this window opened is not announced again.
		ShownImport = Sketch->GetImportsFinished();
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
		Sketch->OnImportChanged.RemoveDynamic(this, &ThisClass::HandleImportChanged);
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
	if (HintText)
	{
		HintText->SetText(ToolHint(Tool));
	}

	// The dropdowns describe their entries when they open; here only their labels and whether they open at all.
	for (UStageMenuButton* Button : { EditMenuButton.Get(), SnapMenuButton.Get(), ItemsMenuButton.Get() })
	{
		if (Button)
		{
			Button->SetIsEnabled(Sketch != nullptr);
		}
	}
	if (SnapMenuButton)
	{
		SnapMenuButton->SetLabel(StageSketchMenus::DescribeSnap(Settings));
	}

	FNumberFormattingOptions TwoDecimals = FNumberFormattingOptions::DefaultNoGrouping();
	TwoDecimals.SetMinimumFractionalDigits(2).SetMaximumFractionalDigits(2);
	FNumberFormattingOptions WholeMetres = FNumberFormattingOptions::DefaultNoGrouping();
	WholeMetres.SetMaximumFractionalDigits(0);
	const float SliderSettings[] = { Settings.DeckHeight, Settings.TrussHeight, Settings.PhotoWidth };
	static_assert(UE_ARRAY_COUNT(SliderSettings) == int32(ESlider::Count), "Every slider needs its setting.");
	for (int32 Index = 0; Index < int32(ESlider::Count); ++Index)
	{
		if (Sliders[Index])
		{
			Sliders[Index]->SetValue(SliderSettings[Index]);
			Sliders[Index]->SetIsEnabled(Sketch != nullptr);
		}
		if (SliderValues[Index])
		{
			SliderValues[Index]->SetText(FText::Format(LOCTEXT("Metres", "{0} m"),
				FText::AsNumber(SliderSettings[Index] * 0.01f, Index == int32(ESlider::PhotoWidth) ? &WholeMetres : &TwoDecimals)));
		}
	}

	// The photo: the Photo dropdown's label, and the refused image's two actions.
	const StageSketchMenus::FPhotoState Photo = GetPhotoState();
	if (!Photo.bOnline && PhotoReader == EStageSketchReader::Claude)
	{
		PhotoReader = EStageSketchReader::Local;
	}
	if (PhotoMenuButton)
	{
		PhotoMenuButton->SetLabel(StageSketchMenus::DescribePhoto(Photo));
		PhotoMenuButton->SetIsEnabled(Photo.bAvailable);
	}
	const bool bRefusedPhoto = Sketch && !Sketch->GetRefusedPhotoPath().IsEmpty();
	const AModularPlayerController* Controller = GetStageController();
	if (UWidget* Segment = RefusedButtons.IsEmpty() ? nullptr : RefusedButtons[0]->GetParent())
	{
		// The whole segment, so its gap goes with it.
		Segment->SetVisibility(bRefusedPhoto ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	for (UStageChoiceButton* Button : RefusedButtons)
	{
		const bool bTrace = Button->GetChoiceIndex() == 0;
		StyleChoice(*Button, false);
		Button->SetIsEnabled(bRefusedPhoto && !Photo.bImporting && (bTrace || (Controller && !Controller->IsColorLookPending())));
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
		const bool bPending = (Sketch && Sketch->IsGenerationPending()) || Photo.bImporting;
		ResultText->SetText(Photo.bImporting ? Sketch->GetImportMessage() : bPending ? LOCTEXT("Generating", "Generating...") : ResultMessage);
		ResultText->SetColorAndOpacity(FSlateColor(bResultSucceeded || bPending ? Palette.TextSecondary : Palette.WarningColor));
	}
	if (GenerateButton)
	{
		StyleChoice(*GenerateButton, false, /*bPrimary*/ !Plan.IsEmpty());
		GenerateButton->SetIsEnabled(!Plan.IsEmpty() && Sketch && !Sketch->IsGenerationPending() && GetStageController());
	}
}

UWidget* UStageSketchCanvasWidget::MakeMenuButton(FName Name, const FText& Tip, float Width, FStageMenu (UStageSketchCanvasWidget::*Builder)(),
	TObjectPtr<UStageMenuButton>& OutButton)
{
	UStageMenuButton* Button = WidgetTree->ConstructWidget<UStageMenuButton>(UStageMenuButton::StaticClass(), Name);
	// Before the button builds its Slate widget: its style is copied then.
	Button->ApplyTheme(GetTheme());
	Button->SetToolTipText(Tip);
	// This window owns the button through its widget tree, so the binding cannot outlive it.
	Button->OnGetMenu.BindUObject(this, Builder);
	OutButton = Button;
	USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Box->SetWidthOverride(Width);
	Box->SetHeightOverride(StageSketchCanvas::MenuHeight);
	Box->SetContent(Button);
	return Box;
}

// --- Dropdowns: described from the state when they open. The lambdas hold this window weakly (the popup is Slate-owned) and
// call its handlers, which write through the sketch's validated setters and read back, as a button click does. ---

FStageMenu UStageSketchCanvasWidget::BuildEditMenu()
{
	const UStageSketchSubsystem* Sketch = GetSketch();
	return StageSketchMenus::MakeEditMenu(Sketch != nullptr, Sketch && !Sketch->GetStrokes().IsEmpty(),
		[WeakThis = TWeakObjectPtr<UStageSketchCanvasWidget>(this)](StageSketchMenus::EEditCommand Command)
		{
			if (UStageSketchCanvasWidget* This = WeakThis.Get())
			{
				This->HandleEditCommand(Command);
			}
		});
}

FStageMenu UStageSketchCanvasWidget::BuildSnapMenu()
{
	const UStageSketchSubsystem* Sketch = GetSketch();
	return StageSketchMenus::MakeSnapMenu(Sketch ? Sketch->GetSettings() : FStageSketchSettings(), Sketch != nullptr,
		[WeakThis = TWeakObjectPtr<UStageSketchCanvasWidget>(this)](const StageSketchMenus::FSettingsEdit& Edit)
		{
			if (UStageSketchCanvasWidget* This = WeakThis.Get())
			{
				This->HandleSettingsPicked(Edit);
			}
		});
}

FStageMenu UStageSketchCanvasWidget::BuildItemsMenu()
{
	const UStageSketchSubsystem* Sketch = GetSketch();
	TArray<StageSketchMenus::FItemChoice> Choices;
	// The candidates as they are now, held weakly: a pick names the item itself, so a catalog change while the menu is open
	// cannot shift it onto another one (SetItem also refuses an item that is no longer a candidate).
	TArray<TArray<TWeakObjectPtr<UBaseItemData>>> Candidates;
	for (int32 Element = 0; Element < int32(EStageSketchElement::Count); ++Element)
	{
		StageSketchMenus::FItemChoice& Choice = Choices.AddDefaulted_GetRef();
		Choice.Element = EStageSketchElement(Element);
		TArray<TWeakObjectPtr<UBaseItemData>>& Items = Candidates.AddDefaulted_GetRef();
		const UBaseItemData* Chosen = Sketch ? Sketch->GetItem(Choice.Element) : nullptr;
		for (UBaseItemData* Item : Sketch ? Sketch->GetCandidateItems(Choice.Element) : TArray<UBaseItemData*>())
		{
			if (Item == Chosen)
			{
				Choice.Chosen = Items.Num();
			}
			Choice.Names.Add(Item->DisplayName);
			// The fixture type groups a long list (a large GDTF library). Other items have no kind: they all land in one group, and a
			// single group is never split, so decks and trusses stay one list.
			const ULightingFixtureData* Fixture = Cast<ULightingFixtureData>(Item);
			Choice.Groups.Add(Fixture ? StaticEnum<EStageFixtureKind>()->GetDisplayNameTextByValue(static_cast<int64>(Fixture->FixtureKind)) : FText::GetEmpty());
			Items.Add(Item);
		}
	}
	return StageSketchMenus::MakeItemsMenu(Choices,
		[WeakThis = TWeakObjectPtr<UStageSketchCanvasWidget>(this), Candidates](EStageSketchElement Element, int32 Index)
		{
			UStageSketchCanvasWidget* This = WeakThis.Get();
			if (This && Candidates.IsValidIndex(int32(Element)) && Candidates[int32(Element)].IsValidIndex(Index))
			{
				This->HandleItemPicked(Element, Candidates[int32(Element)][Index].Get());
			}
		});
}

StageSketchMenus::FPhotoState UStageSketchCanvasWidget::GetPhotoState() const
{
	const UStageSketchSubsystem* Sketch = GetSketch();
	StageSketchMenus::FPhotoState State;
	State.bAvailable = Sketch != nullptr;
	State.bCanBrowse = StageFileDialog::IsAvailable();
	State.bImporting = Sketch && Sketch->IsImportPending();
	State.bHasPhoto = Sketch && Sketch->GetReferenceImage() != nullptr;
	State.bShown = Sketch && Sketch->IsReferenceVisible();
	State.OnlineUnavailable = LOCTEXT("NoSketch", "The sketch is not available in this level.");
	State.bOnline = Sketch && Sketch->CanReadOnline(State.OnlineUnavailable);
	State.Reader = State.bOnline ? PhotoReader : EStageSketchReader::Local;
	return State;
}

FStageMenu UStageSketchCanvasWidget::BuildPhotoMenu()
{
	return StageSketchMenus::MakePhotoMenu(GetPhotoState(),
		[WeakThis = TWeakObjectPtr<UStageSketchCanvasWidget>(this)](StageSketchMenus::EPhotoCommand Command)
		{
			if (UStageSketchCanvasWidget* This = WeakThis.Get())
			{
				This->HandlePhotoCommand(Command);
			}
		});
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

void UStageSketchCanvasWidget::HandleEditCommand(StageSketchMenus::EEditCommand Command)
{
	using StageSketchMenus::EEditCommand;
	UStageSketchSubsystem* Sketch = GetSketch();
	switch (Command)
	{
	case EEditCommand::UndoStroke:
		if (Surface)
		{
			Surface->CancelStroke();
		}
		if (Sketch)
		{
			Sketch->RemoveLastStroke();
		}
		break;
	case EEditCommand::Clear:
		if (Surface)
		{
			Surface->CancelStroke();
		}
		if (Sketch)
		{
			Sketch->ClearStrokes();
		}
		break;
	case EEditCommand::Frame:
		if (Surface)
		{
			Surface->FrameSketch();
		}
		break;
	}
	Refresh();
}

void UStageSketchCanvasWidget::HandleSettingsPicked(const StageSketchMenus::FSettingsEdit& Edit)
{
	// The edit goes onto the settings as they are now; the subsystem sanitizes and broadcasts OnSketchChanged, and the label is read back.
	if (UStageSketchSubsystem* Sketch = GetSketch(); Sketch && Edit)
	{
		FStageSketchSettings Settings = Sketch->GetSettings();
		Edit(Settings);
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
		else if (SliderIndex == int32(ESlider::PhotoWidth))
		{
			Settings.PhotoWidth = FMath::GridSnap(Value, PhotoWidthStep);
		}
		Sketch->SetSettings(Settings);
	}
	Refresh();
}

void UStageSketchCanvasWidget::HandleItemPicked(EStageSketchElement Element, UBaseItemData* Item)
{
	// SetItem refuses an item that is not (or no longer) a candidate for Element.
	if (UStageSketchSubsystem* Sketch = GetSketch(); Sketch && Item)
	{
		Sketch->SetItem(Element, Item);
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

void UStageSketchCanvasWidget::HandlePhotoCommand(StageSketchMenus::EPhotoCommand Command)
{
	using StageSketchMenus::EPhotoCommand;
	UStageSketchSubsystem* Sketch = GetSketch();
	if (!Sketch)
	{
		return;
	}
	switch (Command)
	{
	case EPhotoCommand::Remove:
		Sketch->ClearReference();
		break;
	case EPhotoCommand::ToggleShown:
		Sketch->SetReferenceVisible(!Sketch->IsReferenceVisible());
		break;
	case EPhotoCommand::ReadHere:
		PhotoReader = EStageSketchReader::Local;
		break;
	case EPhotoCommand::ReadByClaude:
		// Refresh falls back to this PC when Claude cannot be reached.
		PhotoReader = EStageSketchReader::Claude;
		break;
	case EPhotoCommand::Import:
	{
		TArray<FString> Files;
		if (!StageFileDialog::OpenFiles(LOCTEXT("ImportPhotoTitle", "Import a Stage Drawing"), LastPhotoDirectory, StageSketchImageImport::GetFileDialogFilter(), Files) || Files.IsEmpty())
		{
			return;
		}
		LastPhotoDirectory = FPaths::GetPath(Files[0]);
		FText Error;
		if (!Sketch->ImportImage(Files[0], PhotoReader, Error))
		{
			ResultMessage = Error;
			bResultSucceeded = false;
		}
		break;
	}
	}
	Refresh();
}

void UStageSketchCanvasWidget::HandleRefusedPicked(int32 Index)
{
	UStageSketchSubsystem* Sketch = GetSketch();
	const FString Refused = Sketch ? Sketch->GetRefusedPhotoPath() : FString();
	if (Refused.IsEmpty())
	{
		return;
	}
	if (Index == 0)
	{
		FText Error;
		if (!Sketch->ImportImage(Refused, EStageSketchReader::Local, Error, EStageSketchPhotoCheck::TraceAnyway))
		{
			ResultMessage = Error;
			bResultSucceeded = false;
		}
	}
	else if (AModularPlayerController* Controller = GetStageController())
	{
		// A refusal (nothing selected) goes to the status bar; the message here says what was asked.
		if (Controller->RequestColorLookFromPhoto(Refused))
		{
			ResultMessage = FText::Format(LOCTEXT("ColoursRequested", "The colours of {0} go on the selected fixtures (one undo step)."),
				FText::FromString(FPaths::GetCleanFilename(Refused)));
			bResultSucceeded = true;
		}
	}
	Refresh();
}

void UStageSketchCanvasWidget::HandleImportChanged()
{
	const UStageSketchSubsystem* Sketch = GetSketch();
	if (Sketch && !Sketch->IsImportPending() && !Sketch->GetImportMessage().IsEmpty())
	{
		// The reference photo's SHOW / REMOVE fire this too: only a finished import's new message is taken, once.
		const uint32 Finished = Sketch->GetImportsFinished();
		if (Finished != ShownImport)
		{
			ShownImport = Finished;
			ResultMessage = Sketch->GetImportMessage();
			bResultSucceeded = Sketch->WasImportSuccessful();
			// Frame the new drawing and its photo.
			if (bResultSucceeded && Surface)
			{
				Surface->FrameSketch();
			}
		}
	}
	Refresh();
}

#undef LOCTEXT_NAMESPACE
