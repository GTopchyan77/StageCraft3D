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
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Placement/StageSnappingComponent.h"
#include "Player/ModularPlayerController.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageMenuButton.h"
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

	/** Order of the View menu: the everyday modes, then the engine lighting diagnostics. */
	const EStageViewMode ViewModes[] = { EStageViewMode::Lit, EStageViewMode::Unlit, EStageViewMode::Lines,
		EStageViewMode::LightingOnly, EStageViewMode::DetailLighting, EStageViewMode::Reflections };

	/** Name in the View menu. */
	FText GetViewModeName(EStageViewMode Mode)
	{
		switch (Mode)
		{
		case EStageViewMode::Unlit:				return LOCTEXT("Unlit", "Unlit");
		case EStageViewMode::Lines:				return LOCTEXT("Lines", "Wireframe / CAD Lines");
		case EStageViewMode::LightingOnly:		return LOCTEXT("LightingOnly", "Lighting Only");
		case EStageViewMode::DetailLighting:	return LOCTEXT("DetailLighting", "Detail Lighting");
		case EStageViewMode::Reflections:		return LOCTEXT("Reflections", "Reflections");
		case EStageViewMode::Lit:
		default:								return LOCTEXT("Lit", "Lit");
		}
	}

	/** Name on the menu button, which has room for a short one. */
	FText GetViewModeButtonLabel(EStageViewMode Mode)
	{
		return Mode == EStageViewMode::Lines ? LOCTEXT("LinesShort", "CAD Lines") : GetViewModeName(Mode);
	}

	FText GetViewModeTip(EStageViewMode Mode)
	{
		switch (Mode)
		{
		case EStageViewMode::Unlit:				return LOCTEXT("UnlitTip", "Base colours without lights, shadows or fog, for clean placement.");
		case EStageViewMode::Lines:				return LOCTEXT("LinesTip", "CAD line view: outlines and edges on a flat background, no lights.");
		case EStageViewMode::LightingOnly:		return LOCTEXT("LightingOnlyTip", "Lighting on neutral grey surfaces: fixture coverage without colours or textures.");
		case EStageViewMode::DetailLighting:	return LOCTEXT("DetailLightingTip", "Lighting on neutral surfaces with their normal detail.");
		case EStageViewMode::Reflections:		return LOCTEXT("ReflectionsTip", "Every surface as a smooth mirror, to check reflections.");
		case EStageViewMode::Lit:
		default:								return LOCTEXT("LitTip", "Full photometric lighting, as rendered.");
		}
	}

	/** Index of Value in a preset list, or INDEX_NONE. */
	int32 FindPreset(TConstArrayView<float> Presets, float Value)
	{
		return Presets.IndexOfByPredicate([Value](float Preset) { return FMath::IsNearlyEqual(Preset, Value, 1.e-3f); });
	}

	FText DescribePreset(float Value, const FText& Units)
	{
		return Value > 0.f
			? FText::Format(LOCTEXT("PresetValue", "{0}{1}"), FText::AsNumber(FMath::RoundToInt(Value)), Units)
			: LOCTEXT("PresetOff", "OFF");
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

	// View menu: view modes and the grid toggle.
	ViewMenuButton = WidgetTree->ConstructWidget<UStageMenuButton>(UStageMenuButton::StaticClass(), TEXT("ViewMenuButton"));
	ViewMenuButton->ApplyTheme(Palette);
	ViewMenuButton->SetLabel(GetViewModeButtonLabel(EStageViewMode::Lit));
	ViewMenuButton->SetToolTipText(LOCTEXT("ViewMenuTip", "View mode and the reference grid."));
	ViewMenuButton->OnGetMenuContent.BindUObject(this, &ThisClass::BuildViewMenu);
	USizeBox* ViewMenuBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("ViewMenuBox"));
	ViewMenuBox->SetWidthOverride(120.f);
	ViewMenuBox->SetHeightOverride(BarHeight);
	ViewMenuBox->SetContent(ViewMenuButton);
	Add(ViewMenuBox, ChoiceGap * 3.f);

	// Snaps. Labels come from the presets, so the buttons and StageViewMath can never disagree.
	TArray<FText> GridOptions;
	for (float Preset : StageViewMath::GetGridSnapPresets())
	{
		GridOptions.Add(DescribePreset(Preset, LOCTEXT("Cm", " cm")));
	}
	const FText GridSnapTip = LOCTEXT("GridSnapTip", "Grid snap for placing and moving items; positions land on the grid, which the reference grid shows.\nClick the active step to turn snapping off; click OFF again to restore the last step.");
	TArray<FText> GridTips;
	GridTips.Init(GridSnapTip, GridOptions.Num());

	TArray<FText> AngleOptions;
	for (float Preset : StageViewMath::GetAngleSnapPresets())
	{
		AngleOptions.Add(DescribePreset(Preset, LOCTEXT("Degrees", "°")));
	}
	const FText AngleSnapTip = LOCTEXT("AngleSnapTip", "Rotation snap for the rotate tool.\nClick the active step to turn snapping off; click OFF again to restore the last step.");
	TArray<FText> AngleTips;
	AngleTips.Init(AngleSnapTip, AngleOptions.Num());

	Add(MakeSeparator(), GroupGap);
	Add(MakeLabel(LOCTEXT("GridSnapLabel", "SNAP")), GroupGap);
	Add(MakeSegment(GridOptions, GridTips, GridSnapButtons, &ThisClass::HandleGridSnapPicked), 4.f);

	Add(MakeLabel(LOCTEXT("AngleSnapLabel", "ANGLE")), GroupGap);
	Add(MakeSegment(AngleOptions, AngleTips, AngleSnapButtons, &ThisClass::HandleAngleSnapPicked), 4.f);

	Add(MakeSeparator(), GroupGap);

	// Gizmo.
	Add(MakeSegment({ LOCTEXT("Move", "MOVE"), LOCTEXT("Rotate", "ROTATE"), LOCTEXT("Scale", "SCALE") },
		{ LOCTEXT("MoveTip", "Move the selection (Space cycles the tools)."), LOCTEXT("RotateTip", "Rotate the selection."), LOCTEXT("ScaleTip", "Scale the selection.") },
		GizmoModeButtons, &ThisClass::HandleGizmoModePicked), GroupGap);
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
	if (ViewMenuButton)
	{
		ViewMenuButton->SetLabel(GetViewModeButtonLabel(Viewport ? Viewport->GetViewMode() : EStageViewMode::Lit));
		ViewMenuButton->SetIsEnabled(Viewport != nullptr);
	}
	StyleGroup(GizmoModeButtons, Gizmo ? int32(Gizmo->GetMode()) : INDEX_NONE, Gizmo != nullptr);
	StyleGroup(GridSnapButtons, Snapping ? FindPreset(StageViewMath::GetGridSnapPresets(), Snapping->GetGridSnap()) : INDEX_NONE, Snapping != nullptr);
	StyleGroup(AngleSnapButtons, Snapping ? FindPreset(StageViewMath::GetAngleSnapPresets(), Snapping->GetAngleSnap()) : INDEX_NONE, Snapping != nullptr);

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

TSharedRef<SWidget> UStageViewportToolbar::BuildViewMenu()
{
	using namespace StageViewportToolbar;

	// Built from the state at this moment; picking an entry closes the menu, so nothing in it needs to stay live. The
	// lambdas hold the viewport weakly: the popup is Slate-owned and must not keep it alive or touch it after teardown.
	const TWeakObjectPtr<UStageViewportComponent> WeakViewport = BoundViewport;
	const UStageViewportComponent* Viewport = WeakViewport.Get();
	const EStageViewMode CurrentMode = Viewport ? Viewport->GetViewMode() : EStageViewMode::Lit;

	FMenuBuilder Menu(/*bInShouldCloseWindowAfterMenuSelection*/ true, /*InCommandList*/ nullptr);

	Menu.BeginSection(TEXT("ViewMode"), LOCTEXT("ViewModeSection", "View Mode"));
	for (const EStageViewMode Mode : ViewModes)
	{
		const bool bAvailable = StageViewMath::IsViewModeAvailable(Mode);
		const bool bCanPick = Viewport && bAvailable;
		const bool bChecked = Viewport && Mode == CurrentMode;
		const FText Tip = bAvailable ? GetViewModeTip(Mode)
			: FText::Format(LOCTEXT("UnavailableTip", "{0}\nNot available in this build: Shipping compiles out this engine view mode."), GetViewModeTip(Mode));

		Menu.AddMenuEntry(GetViewModeName(Mode), Tip, FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([WeakViewport, Mode]()
				{
					if (UStageViewportComponent* Target = WeakViewport.Get())
					{
						Target->SetViewMode(Mode); // Refused modes are logged by the component; the read-back keeps the label true.
					}
				}),
				FCanExecuteAction::CreateLambda([bCanPick]() { return bCanPick; }),
				FIsActionChecked::CreateLambda([bChecked]() { return bChecked; })),
			NAME_None, EUserInterfaceActionType::RadioButton);
	}
	Menu.EndSection();

	const bool bGridVisible = Viewport && Viewport->IsGridVisible();
	Menu.BeginSection(TEXT("Show"), LOCTEXT("ShowSection", "Show"));
	Menu.AddMenuEntry(LOCTEXT("Grid", "Grid"), LOCTEXT("GridTip", "Show or hide the reference grid (it follows the grid snap size)."), FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateLambda([WeakViewport]()
			{
				if (UStageViewportComponent* Target = WeakViewport.Get())
				{
					Target->SetGridVisible(!Target->IsGridVisible());
				}
			}),
			FCanExecuteAction::CreateLambda([bCanToggle = Viewport != nullptr]() { return bCanToggle; }),
			FIsActionChecked::CreateLambda([bGridVisible]() { return bGridVisible; })),
		NAME_None, EUserInterfaceActionType::ToggleButton);
	Menu.EndSection();

	return Menu.MakeWidget();
}

void UStageViewportToolbar::HandleGizmoModePicked(int32 Index)
{
	if (AModularTransformGizmo* Gizmo = BoundGizmo.Get(); Gizmo && Index >= 0 && Index < UE_ARRAY_COUNT(StageViewportToolbar::GizmoModes))
	{
		Gizmo->SetMode(StageViewportToolbar::GizmoModes[Index]);
	}
	Refresh();
}

void UStageViewportToolbar::HandleGridSnapPicked(int32 Index)
{
	const TConstArrayView<float> Presets = StageViewMath::GetGridSnapPresets();
	if (UStageSnappingComponent* Snapping = BoundSnapping.Get(); Snapping && Presets.IsValidIndex(Index))
	{
		Snapping->PickGridSnap(Presets[Index]);
	}
	Refresh();
}

void UStageViewportToolbar::HandleAngleSnapPicked(int32 Index)
{
	const TConstArrayView<float> Presets = StageViewMath::GetAngleSnapPresets();
	if (UStageSnappingComponent* Snapping = BoundSnapping.Get(); Snapping && Presets.IsValidIndex(Index))
	{
		Snapping->PickAngleSnap(Presets[Index]);
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
