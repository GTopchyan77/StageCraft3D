// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageViewportToolbar.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/ButtonSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Placement/StageSnappingComponent.h"
#include "Player/ModularPlayerController.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageThemedComboBox.h"
#include "UI/StageToolButton.h"
#include "Viewport/StageViewMath.h"
#include "Viewport/StageViewportComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageViewportToolbar)

#define LOCTEXT_NAMESPACE "StageViewportToolbar"

namespace StageViewportToolbar
{
	constexpr float BarHeight = 22.f;
	constexpr float GroupGap = 10.f;
	constexpr float ChoiceGap = 2.f;

	/** Order of the projection dropdown; its index is the enum value. */
	const EStageViewProjection Projections[] = { EStageViewProjection::Perspective, EStageViewProjection::Top, EStageViewProjection::Front,
		EStageViewProjection::Back, EStageViewProjection::Left, EStageViewProjection::Right };

	const EGizmoMode GizmoModes[] = { EGizmoMode::Translate, EGizmoMode::Rotate, EGizmoMode::Scale };

	/** Index of Value in a preset list, or INDEX_NONE. */
	int32 FindPreset(TConstArrayView<float> Presets, float Value)
	{
		return Presets.IndexOfByPredicate([Value](float Preset) { return FMath::IsNearlyEqual(Preset, Value, 1.e-3f); });
	}

	FText DescribePreset(float Value, const FText& Units)
	{
		return Value > 0.f
			? FText::Format(LOCTEXT("PresetValue", "{0}{1}"), FText::AsNumber(FMath::RoundToInt(Value)), Units)
			: LOCTEXT("PresetOff", "Off");
	}
}

const UStageCraftUITheme& UStageViewportToolbar::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

AModularPlayerController* UStageViewportToolbar::GetStageController() const
{
	return Cast<AModularPlayerController>(GetOwningPlayer());
}

void UStageViewportToolbar::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
	if (ProjectionCombo)
	{
		ProjectionCombo->OnSelectionChanged.AddUniqueDynamic(this, &ThisClass::HandleProjectionSelected);
	}
	if (GridSnapCombo)
	{
		GridSnapCombo->OnSelectionChanged.AddUniqueDynamic(this, &ThisClass::HandleGridSnapSelected);
	}
	if (AngleSnapCombo)
	{
		AngleSnapCombo->OnSelectionChanged.AddUniqueDynamic(this, &ThisClass::HandleAngleSnapSelected);
	}
}

UTextBlock* UStageViewportToolbar::MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color) const
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
	Block->SetColorAndOpacity(FSlateColor(Color));
	return Block;
}

UWidget* UStageViewportToolbar::MakeLabel(const FText& Text) const
{
	return MakeText(Text, 7, TEXT("Bold"), GetTheme().TextSecondary);
}

UWidget* UStageViewportToolbar::MakeSeparator() const
{
	UImage* Separator = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
	Separator->SetBrush(StageCraftWidgetStyle::MakeBox(GetTheme().Divider));
	Separator->SetDesiredSizeOverride(FVector2D(1.f, 16.f));
	return Separator;
}

UStageThemedComboBox* UStageViewportToolbar::MakeCombo(FName Name, TConstArrayView<FText> Options, const FText& Tip)
{
	UStageThemedComboBox* Combo = WidgetTree->ConstructWidget<UStageThemedComboBox>(UStageThemedComboBox::StaticClass(), Name);
	// Before the combo builds its Slate widget: its font and colours can only be set then.
	Combo->ApplyTheme(GetTheme());
	for (const FText& Option : Options)
	{
		Combo->AddOption(Option.ToString());
	}
	Combo->SetToolTipText(Tip);
	return Combo;
}

UWidget* UStageViewportToolbar::MakeSegment(TConstArrayView<FText> Labels, TConstArrayView<FText> Tips, TArray<TObjectPtr<UStageChoiceButton>>& OutButtons,
	void (UStageViewportToolbar::*Handler)(int32))
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	OutButtons.Reset(Labels.Num());
	for (int32 Index = 0; Index < Labels.Num(); ++Index)
	{
		UStageChoiceButton* Button = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
		Button->SetChoiceIndex(Index);
		Button->OnPicked.BindUObject(this, Handler);
		if (Tips.IsValidIndex(Index))
		{
			Button->SetToolTipText(Tips[Index]);
		}
		if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Button->AddChild(MakeText(Labels[Index], 8, TEXT("Bold"), GetTheme().TextSecondary))))
		{
			LabelSlot->SetPadding(FMargin(8.f, 2.f));
			LabelSlot->SetHorizontalAlignment(HAlign_Center);
			LabelSlot->SetVerticalAlignment(VAlign_Center);
		}
		if (UHorizontalBoxSlot* ButtonSlot = Row->AddChildToHorizontalBox(Button))
		{
			ButtonSlot->SetPadding(FMargin(Index == 0 ? 0.f : StageViewportToolbar::ChoiceGap, 0.f, 0.f, 0.f));
			ButtonSlot->SetVerticalAlignment(VAlign_Fill);
		}
		OutButtons.Add(Button);
	}
	return Row;
}

void UStageViewportToolbar::BuildTree()
{
	using namespace StageCraftWidgetStyle;
	using namespace StageViewportToolbar;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Root = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Root"));
	Root->SetBrush(MakeBox(Palette.PanelHeader));
	Root->SetPadding(FMargin(6.f, 3.f));
	WidgetTree->RootWidget = Root;

	USizeBox* Height = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("BarHeight"));
	Height->SetHeightOverride(BarHeight);
	Root->SetContent(Height);

	UHorizontalBox* Bar = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("Bar"));
	Height->SetContent(Bar);

	const auto Add = [Bar](UWidget* Widget, float LeftGap)
	{
		if (UHorizontalBoxSlot* BarSlot = Bar->AddChildToHorizontalBox(Widget))
		{
			BarSlot->SetPadding(FMargin(LeftGap, 0.f, 0.f, 0.f));
			BarSlot->SetVerticalAlignment(VAlign_Center);
		}
	};

	// Projection.
	ProjectionCombo = MakeCombo(TEXT("ProjectionCombo"),
		{ LOCTEXT("Perspective", "Perspective"), LOCTEXT("Top", "Top"), LOCTEXT("Front", "Front"), LOCTEXT("Back", "Back"),
		  LOCTEXT("Left", "Left"), LOCTEXT("Right", "Right") },
		LOCTEXT("ProjectionTip", "Projection. Top looks down (XY); Front / Back look along Y (XZ); Left / Right look along X (YZ).\nIn orthographic views: right-drag pans, the wheel zooms at the cursor, W/A/S/D pan."));
	USizeBox* ProjectionBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("ProjectionBox"));
	ProjectionBox->SetWidthOverride(104.f);
	ProjectionBox->SetHeightOverride(BarHeight);
	ProjectionBox->SetContent(ProjectionCombo);
	Add(ProjectionBox, 0.f);

	// View mode.
	Add(MakeSegment({ LOCTEXT("Lit", "LIT"), LOCTEXT("Unlit", "UNLIT"), LOCTEXT("Lines", "LINES") },
		{ LOCTEXT("LitTip", "Lit: full photometric lighting, as rendered."), LOCTEXT("UnlitTip", "Unlit: base colours without lights, shadows or fog, for clean placement."),
		  LOCTEXT("LinesTip", "Lines: CAD line view. Outlines and edges on a flat background, no lights.") },
		ViewModeButtons, &ThisClass::HandleViewModePicked), GroupGap);

	// Grid visibility.
	Add(MakeSegment({ LOCTEXT("Grid", "GRID") }, { LOCTEXT("GridTip", "Show or hide the reference grid (it follows the grid snap size).") },
		GridToggleButtons, &ThisClass::HandleGridTogglePicked), ChoiceGap * 3.f);

	Add(MakeSeparator(), GroupGap);

	// Gizmo.
	Add(MakeSegment({ LOCTEXT("Move", "MOVE"), LOCTEXT("Rotate", "ROTATE"), LOCTEXT("Scale", "SCALE") },
		{ LOCTEXT("MoveTip", "Move the selection (Space cycles the tools)."), LOCTEXT("RotateTip", "Rotate the selection."), LOCTEXT("ScaleTip", "Scale the selection.") },
		GizmoModeButtons, &ThisClass::HandleGizmoModePicked), GroupGap);

	Add(MakeSeparator(), GroupGap);

	// Snaps.
	TArray<FText> GridOptions;
	for (float Preset : StageViewMath::GetGridSnapPresets())
	{
		GridOptions.Add(DescribePreset(Preset, LOCTEXT("Cm", " cm")));
	}
	TArray<FText> AngleOptions;
	for (float Preset : StageViewMath::GetAngleSnapPresets())
	{
		AngleOptions.Add(DescribePreset(Preset, LOCTEXT("Degrees", "°")));
	}

	Add(MakeLabel(LOCTEXT("GridSnapLabel", "GRID")), GroupGap);
	GridSnapCombo = MakeCombo(TEXT("GridSnapCombo"), GridOptions,
		LOCTEXT("GridSnapTip", "Grid snap for placing and moving items; positions land on the grid. Off places and moves freely."));
	USizeBox* GridBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("GridSnapBox"));
	GridBox->SetWidthOverride(72.f);
	GridBox->SetHeightOverride(BarHeight);
	GridBox->SetContent(GridSnapCombo);
	Add(GridBox, 4.f);

	Add(MakeLabel(LOCTEXT("AngleSnapLabel", "ANGLE")), GroupGap);
	AngleSnapCombo = MakeCombo(TEXT("AngleSnapCombo"), AngleOptions, LOCTEXT("AngleSnapTip", "Rotation snap for the rotate tool. Off rotates freely."));
	USizeBox* AngleBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("AngleSnapBox"));
	AngleBox->SetWidthOverride(64.f);
	AngleBox->SetHeightOverride(BarHeight);
	AngleBox->SetContent(AngleSnapCombo);
	Add(AngleBox, 4.f);
}

void UStageViewportToolbar::NativeConstruct()
{
	Super::NativeConstruct();

	if (AModularPlayerController* Controller = GetStageController())
	{
		if (UStageViewportComponent* Viewport = Controller->GetViewportView())
		{
			BoundViewport = Viewport;
			Viewport->OnViewChanged.AddUniqueDynamic(this, &ThisClass::HandleViewChanged);
		}
		if (UStageSnappingComponent* Snapping = Controller->GetSnapping())
		{
			BoundSnapping = Snapping;
			Snapping->OnSnapSettingsChanged.AddUniqueDynamic(this, &ThisClass::HandleSnapSettingsChanged);
		}
		if (AModularTransformGizmo* Gizmo = Controller->GetGizmo())
		{
			BoundGizmo = Gizmo;
			Gizmo->OnModeChanged.AddUniqueDynamic(this, &ThisClass::HandleGizmoModeChanged);
		}
	}
	Refresh();
}

void UStageViewportToolbar::NativeDestruct()
{
	if (UStageViewportComponent* Viewport = BoundViewport.Get())
	{
		Viewport->OnViewChanged.RemoveDynamic(this, &ThisClass::HandleViewChanged);
	}
	if (UStageSnappingComponent* Snapping = BoundSnapping.Get())
	{
		Snapping->OnSnapSettingsChanged.RemoveDynamic(this, &ThisClass::HandleSnapSettingsChanged);
	}
	if (AModularTransformGizmo* Gizmo = BoundGizmo.Get())
	{
		Gizmo->OnModeChanged.RemoveDynamic(this, &ThisClass::HandleGizmoModeChanged);
	}
	BoundViewport.Reset();
	BoundSnapping.Reset();
	BoundGizmo.Reset();
	Super::NativeDestruct();
}

void UStageViewportToolbar::StyleChoice(UStageChoiceButton& Button, bool bSelected) const
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	// Selected: solid accent like a lit console key; the others outlined, so they still read as buttons (as in the Render panel).
	FButtonStyle Style = bSelected
		? MakeFlatButton(Palette.AccentColor, Palette.AccentColor * 1.1f, Palette.AccentColor * 0.85f)
		: MakeFlatButton(FLinearColor::Transparent, Palette.RowHover, Palette.Divider);
	if (!bSelected)
	{
		Style.SetNormal(MakeBox(FLinearColor::Transparent, CornerRadius, Palette.Divider, 1.f));
	}
	Button.SetStyle(Style);
	if (UTextBlock* Label = Cast<UTextBlock>(Button.GetContent()))
	{
		Label->SetColorAndOpacity(FSlateColor(bSelected ? Palette.WindowBackground : Palette.TextSecondary));
	}
}

void UStageViewportToolbar::Refresh()
{
	using namespace StageViewportToolbar;

	const UStageViewportComponent* Viewport = BoundViewport.Get();
	const UStageSnappingComponent* Snapping = BoundSnapping.Get();
	const AModularTransformGizmo* Gizmo = BoundGizmo.Get();

	const auto StyleGroup = [this](TArray<TObjectPtr<UStageChoiceButton>>& Buttons, int32 SelectedIndex, bool bEnabled)
	{
		for (UStageChoiceButton* Button : Buttons)
		{
			if (Button)
			{
				StyleChoice(*Button, Button->GetChoiceIndex() == SelectedIndex);
				Button->SetIsEnabled(bEnabled);
			}
		}
	};
	StyleGroup(ViewModeButtons, Viewport ? int32(Viewport->GetViewMode()) : INDEX_NONE, Viewport != nullptr);
	StyleGroup(GridToggleButtons, Viewport && Viewport->IsGridVisible() ? 0 : INDEX_NONE, Viewport != nullptr);
	StyleGroup(GizmoModeButtons, Gizmo ? int32(Gizmo->GetMode()) : INDEX_NONE, Gizmo != nullptr);

	// SetSelectedIndex reports ESelectInfo::Direct, which the handlers ignore: only user picks are edits.
	const auto Select = [](UStageThemedComboBox* Combo, int32 Index, bool bEnabled)
	{
		if (Combo)
		{
			if (!Combo->IsOpen() && Combo->GetSelectedIndex() != Index)
			{
				Combo->SetSelectedIndex(Index);
			}
			Combo->SetIsEnabled(bEnabled);
		}
	};
	Select(ProjectionCombo, Viewport ? int32(Viewport->GetProjection()) : INDEX_NONE, Viewport != nullptr);
	Select(GridSnapCombo, Snapping ? FindPreset(StageViewMath::GetGridSnapPresets(), Snapping->GetGridSnap()) : INDEX_NONE, Snapping != nullptr);
	Select(AngleSnapCombo, Snapping ? FindPreset(StageViewMath::GetAngleSnapPresets(), Snapping->GetAngleSnap()) : INDEX_NONE, Snapping != nullptr);
}

// --- User picks: write through the owner, then read back ---

void UStageViewportToolbar::HandleProjectionSelected(FString SelectedItem, ESelectInfo::Type SelectionType)
{
	if (SelectionType == ESelectInfo::Direct)
	{
		return; // Set by Refresh; only user picks are edits.
	}
	UStageViewportComponent* Viewport = BoundViewport.Get();
	const int32 Index = ProjectionCombo ? ProjectionCombo->GetSelectedIndex() : INDEX_NONE;
	if (Viewport && Index >= 0 && Index < UE_ARRAY_COUNT(StageViewportToolbar::Projections))
	{
		Viewport->SetProjection(StageViewportToolbar::Projections[Index]);
	}
	Refresh();
}

void UStageViewportToolbar::HandleViewModePicked(int32 Index)
{
	if (UStageViewportComponent* Viewport = BoundViewport.Get(); Viewport && Index >= 0 && Index <= int32(EStageViewMode::Lines))
	{
		Viewport->SetViewMode(static_cast<EStageViewMode>(Index));
	}
	Refresh();
}

void UStageViewportToolbar::HandleGridTogglePicked(int32 Index)
{
	if (UStageViewportComponent* Viewport = BoundViewport.Get())
	{
		Viewport->SetGridVisible(!Viewport->IsGridVisible());
	}
	Refresh();
}

void UStageViewportToolbar::HandleGizmoModePicked(int32 Index)
{
	if (AModularTransformGizmo* Gizmo = BoundGizmo.Get(); Gizmo && Index >= 0 && Index < UE_ARRAY_COUNT(StageViewportToolbar::GizmoModes))
	{
		Gizmo->SetMode(StageViewportToolbar::GizmoModes[Index]);
	}
	Refresh();
}

void UStageViewportToolbar::HandleGridSnapSelected(FString SelectedItem, ESelectInfo::Type SelectionType)
{
	if (SelectionType == ESelectInfo::Direct)
	{
		return;
	}
	const TConstArrayView<float> Presets = StageViewMath::GetGridSnapPresets();
	const int32 Index = GridSnapCombo ? GridSnapCombo->GetSelectedIndex() : INDEX_NONE;
	if (UStageSnappingComponent* Snapping = BoundSnapping.Get(); Snapping && Presets.IsValidIndex(Index))
	{
		Snapping->SetGridSnap(Presets[Index]);
	}
	Refresh();
}

void UStageViewportToolbar::HandleAngleSnapSelected(FString SelectedItem, ESelectInfo::Type SelectionType)
{
	if (SelectionType == ESelectInfo::Direct)
	{
		return;
	}
	const TConstArrayView<float> Presets = StageViewMath::GetAngleSnapPresets();
	const int32 Index = AngleSnapCombo ? AngleSnapCombo->GetSelectedIndex() : INDEX_NONE;
	if (UStageSnappingComponent* Snapping = BoundSnapping.Get(); Snapping && Presets.IsValidIndex(Index))
	{
		Snapping->SetAngleSnap(Presets[Index]);
	}
	Refresh();
}

// --- Pushed changes ---

void UStageViewportToolbar::HandleViewChanged()
{
	Refresh();
}

void UStageViewportToolbar::HandleSnapSettingsChanged()
{
	Refresh();
}

void UStageViewportToolbar::HandleGizmoModeChanged(EGizmoMode NewMode)
{
	Refresh();
}

#undef LOCTEXT_NAMESPACE
