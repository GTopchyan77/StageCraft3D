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
#include "Engine/LocalPlayer.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Input/StageInputPreferencesSubsystem.h"
#include "Placement/StageSnappingComponent.h"
#include "Player/ModularPlayerController.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageMenuButton.h"
#include "UI/StageThemedComboBox.h"
#include "UI/StageToolButton.h"
#include "Viewport/StageViewMath.h"
#include "Viewport/StageViewportComponent.h"
#include "Workspace/StageWorkspaceSubsystem.h"
#include "Workspace/StageWorkspaceTypes.h"
#include "Engine/GameInstance.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageViewportToolbar)

#define LOCTEXT_NAMESPACE "StageViewportToolbar"

namespace StageViewportToolbar
{
	constexpr float BarHeight = 22.f;
	/** Height of the strip when collapsed: just the expand button. */
	constexpr float CollapsedBarHeight = 14.f;
	/** Space on each side of a section divider. */
	constexpr float SectionGap = 8.f;
	constexpr float DividerHeight = 14.f;
	constexpr float ChoiceGap = 2.f;
	/** Widths of the menu buttons: room for their longest label ("Detail Lighting", "Snap 100 cm · 45°"). */
	constexpr float ViewMenuWidth = 120.f;
	constexpr float SnapMenuWidth = 136.f;
	constexpr float TransformMenuWidth = 84.f;
	constexpr float ToolsMenuWidth = 72.f;

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

	FText DescribePreset(float Value, const FText& Units)
	{
		return Value > 0.f
			? FText::Format(LOCTEXT("PresetValue", "{0}{1}"), FText::AsNumber(FMath::RoundToInt(Value)), Units)
			: LOCTEXT("PresetOff", "Off");
	}

	FText GetGizmoModeName(EGizmoMode Mode)
	{
		switch (Mode)
		{
		case EGizmoMode::Rotate:	return LOCTEXT("Rotate", "Rotate");
		case EGizmoMode::Scale:		return LOCTEXT("Scale", "Scale");
		case EGizmoMode::Translate:
		default:					return LOCTEXT("Move", "Move");
		}
	}

	FText GetGizmoModeTip(EGizmoMode Mode)
	{
		switch (Mode)
		{
		case EGizmoMode::Rotate:	return LOCTEXT("RotateTip", "Rotate the selection (in angle snap steps).");
		case EGizmoMode::Scale:		return LOCTEXT("ScaleTip", "Scale the selection.");
		case EGizmoMode::Translate:
		default:					return LOCTEXT("MoveTip", "Move the selection (in grid snap steps).");
		}
	}

	/** "Snap 10 cm · 15°", "Snap Off · 15°". */
	FText DescribeSnap(float GridCm, float AngleDegrees)
	{
		return FText::Format(LOCTEXT("SnapLabel", "Snap {0} · {1}"),
			DescribePreset(GridCm, LOCTEXT("Cm", " cm")), DescribePreset(AngleDegrees, LOCTEXT("Degrees", "°")));
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

UWidget* UStageViewportToolbar::MakeSeparator() const
{
	using namespace StageCraftWidgetStyle;
	// Half-strength disabled text: one step brighter than the button outlines (Divider), which a 1 px line would vanish into.
	UImage* Separator = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
	Separator->SetBrush(MakeLine(WithAlpha(GetTheme().TextDisabled, 0.5f), FVector2D(1.f, StageViewportToolbar::DividerHeight)));
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

	BarHeightBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("BarHeight"));
	BarHeightBox->SetHeightOverride(BarHeight);
	Root->SetContent(BarHeightBox);

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("Row"));
	BarHeightBox->SetContent(Row);

	// 0. The collapse toggle stays when everything else folds away, so the bar can always be brought back with the mouse.
	if (UHorizontalBoxSlot* ToggleSlot = Row->AddChildToHorizontalBox(MakeSegment({ LOCTEXT("Collapse", "«") }, {}, CollapseButtons, &ThisClass::HandleCollapsePicked)))
	{
		ToggleSlot->SetVerticalAlignment(VAlign_Center);
	}

	UHorizontalBox* Bar = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("Bar"));
	Sections = Bar;
	if (UHorizontalBoxSlot* SectionsSlot = Row->AddChildToHorizontalBox(Bar))
	{
		SectionsSlot->SetPadding(FMargin(SectionGap, 0.f, 0.f, 0.f));
		SectionsSlot->SetVerticalAlignment(VAlign_Center);
	}

	const auto Add = [Bar](UWidget* Widget, float LeftGap)
	{
		if (UHorizontalBoxSlot* BarSlot = Bar->AddChildToHorizontalBox(Widget))
		{
			BarSlot->SetPadding(FMargin(LeftGap, 0.f, 0.f, 0.f));
			BarSlot->SetVerticalAlignment(VAlign_Center);
		}
	};
	// Six sections, each separated by a divider with SectionGap on both sides: projection | view | grid snap | angle snap | gizmo | sketch.
	// The first widget of every section after the first is added with SectionGap, so all dividers sit centred in equal gaps.
	const auto AddDivider = [this, &Add]()
	{
		Add(MakeSeparator(), SectionGap);
	};

	// 1. Projection.
	ProjectionCombo = MakeCombo(TEXT("ProjectionCombo"),
		{ LOCTEXT("Perspective", "Perspective"), LOCTEXT("Top", "Top"), LOCTEXT("Front", "Front"), LOCTEXT("Back", "Back"),
		  LOCTEXT("Left", "Left"), LOCTEXT("Right", "Right") },
		LOCTEXT("ProjectionTip", "Projection. Top looks down (XY); Front / Back look along Y (XZ); Left / Right look along X (YZ).\nIn orthographic views: right-drag pans, the wheel zooms at the cursor, W/A/S/D pan."));
	USizeBox* ProjectionBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("ProjectionBox"));
	ProjectionBox->SetWidthOverride(104.f);
	ProjectionBox->SetHeightOverride(BarHeight);
	ProjectionBox->SetContent(ProjectionCombo);
	Add(ProjectionBox, 0.f);

	// Each menu button sits in a fixed-width box, so the bar does not jump when a label changes (Lit -> Detail Lighting).
	const auto AddMenu = [this, &Add, &AddDivider](UStageMenuButton* Button, FName BoxName, float Width)
	{
		USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), BoxName);
		Box->SetWidthOverride(Width);
		Box->SetHeightOverride(BarHeight);
		Box->SetContent(Button);
		AddDivider();
		Add(Box, SectionGap);
	};

	// 2. View: view modes and the grid toggle.
	ViewMenuButton = MakeMenuButton(TEXT("ViewMenuButton"), LOCTEXT("ViewMenuTip", "View mode and the reference grid."), &ThisClass::BuildViewMenu);
	AddMenu(ViewMenuButton, TEXT("ViewMenuBox"), ViewMenuWidth);

	// 3. Snap: grid and angle steps and Snap to Items, in one menu whose label shows both steps.
	SnapMenuButton = MakeMenuButton(TEXT("SnapMenuButton"),
		LOCTEXT("SnapMenuTip", "Snapping: the grid step for placing and moving, the angle step for rotating, and snapping to other items."), &ThisClass::BuildSnapMenu);
	AddMenu(SnapMenuButton, TEXT("SnapMenuBox"), SnapMenuWidth);

	// 4. Transform: the gizmo tool.
	TransformMenuButton = MakeMenuButton(TEXT("TransformMenuButton"),
		LOCTEXT("TransformMenuTip", "The transform tool for the selection: Move, Rotate or Scale (Space cycles them)."), &ThisClass::BuildTransformMenu);
	AddMenu(TransformMenuButton, TEXT("TransformMenuBox"), TransformMenuWidth);

	// 5. Tools: open (or bring forward) Sketch to Stage and the AI Assistant.
	ToolsMenuButton = MakeMenuButton(TEXT("ToolsMenuButton"), LOCTEXT("ToolsMenuTip", "Sketch to Stage and the AI Assistant."), &ThisClass::BuildToolsMenu);
	ToolsMenuButton->SetLabel(LOCTEXT("ToolsLabel", "Tools"));
	AddMenu(ToolsMenuButton, TEXT("ToolsMenuBox"), ToolsMenuWidth);
}

UStageMenuButton* UStageViewportToolbar::MakeMenuButton(FName Name, const FText& Tip, TSharedRef<SWidget> (UStageViewportToolbar::*Builder)())
{
	UStageMenuButton* Button = WidgetTree->ConstructWidget<UStageMenuButton>(UStageMenuButton::StaticClass(), Name);
	// Before the button builds its Slate widget: its style is copied then.
	Button->ApplyTheme(GetTheme());
	Button->SetToolTipText(Tip);
	Button->OnGetMenuContent.BindUObject(this, Builder);
	return Button;
}

void UStageViewportToolbar::HandleCollapsePicked(int32 Index)
{
	// A layout preference of this machine: the viewport component stores it and broadcasts OnViewChanged, which redraws here.
	if (UStageViewportComponent* Viewport = BoundViewport.Get())
	{
		Viewport->SetToolbarCollapsed(!Viewport->IsToolbarCollapsed());
	}
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
	if (SnapMenuButton)
	{
		SnapMenuButton->SetLabel(Snapping ? DescribeSnap(Snapping->GetGridSnap(), Snapping->GetAngleSnap()) : LOCTEXT("SnapUnavailable", "Snap"));
		SnapMenuButton->SetIsEnabled(Snapping != nullptr);
	}
	if (TransformMenuButton)
	{
		TransformMenuButton->SetLabel(GetGizmoModeName(Gizmo ? Gizmo->GetMode() : EGizmoMode::Translate));
		TransformMenuButton->SetIsEnabled(Gizmo != nullptr);
	}

	// Collapsed: only the toggle remains, on a thinner strip; the sections keep their state and come back as they were.
	const bool bCollapsed = Viewport && Viewport->IsToolbarCollapsed();
	StyleGroup(CollapseButtons, INDEX_NONE, Viewport != nullptr);
	if (Sections)
	{
		Sections->SetVisibility(bCollapsed ? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible);
	}
	if (BarHeightBox)
	{
		BarHeightBox->SetHeightOverride(bCollapsed ? StageViewportToolbar::CollapsedBarHeight : StageViewportToolbar::BarHeight);
	}
	if (UStageChoiceButton* Toggle = CollapseButtons.IsEmpty() ? nullptr : CollapseButtons[0].Get())
	{
		if (UTextBlock* Label = Cast<UTextBlock>(Toggle->GetContent()))
		{
			Label->SetText(bCollapsed ? LOCTEXT("Expand", "»  VIEWPORT TOOLS") : LOCTEXT("Collapse", "«"));
		}
		const AModularPlayerController* Controller = GetStageController();
		const UStageInputPreferencesSubsystem* Preferences = Controller ? ULocalPlayer::GetSubsystem<UStageInputPreferencesSubsystem>(Controller->GetLocalPlayer()) : nullptr;
		const FText Keys = Preferences ? Preferences->DescribeCommand(EStageInputCommand::ToggleViewportToolbar) : FText::GetEmpty();
		const FText Tip = bCollapsed ? LOCTEXT("ExpandTip", "Show the viewport toolbar.") : LOCTEXT("CollapseTip", "Hide the viewport toolbar to this button.");
		Toggle->SetToolTipText(Keys.IsEmpty() ? Tip : FText::Format(LOCTEXT("ToggleTipKeys", "{0} ({1})"), Tip, Keys));
	}

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

TSharedRef<SWidget> UStageViewportToolbar::BuildSnapMenu()
{
	using namespace StageViewportToolbar;

	// As BuildViewMenu: built from the state now, the lambdas hold the component weakly. The entries are radio buttons that
	// set a step directly (SetGridSnap), not the old segment's toggle semantics: in a menu, picking the checked entry keeps it.
	// The component broadcasts OnSnapSettingsChanged, which redraws the button label.
	const TWeakObjectPtr<UStageSnappingComponent> WeakSnapping = BoundSnapping;
	const UStageSnappingComponent* Snapping = WeakSnapping.Get();

	FMenuBuilder Menu(/*bInShouldCloseWindowAfterMenuSelection*/ true, /*InCommandList*/ nullptr);
	const auto AddPresets = [&Menu, &WeakSnapping, Snapping](TConstArrayView<float> Presets, float Current, const FText& Units, const FText& Tip, bool bGrid)
	{
		for (const float Preset : Presets)
		{
			const bool bChecked = Snapping && FMath::IsNearlyEqual(Preset, Current, 1.e-3f);
			Menu.AddMenuEntry(DescribePreset(Preset, Units), Tip, FSlateIcon(),
				FUIAction(
					FExecuteAction::CreateLambda([WeakSnapping, Preset, bGrid]()
					{
						if (UStageSnappingComponent* Target = WeakSnapping.Get())
						{
							bGrid ? Target->SetGridSnap(Preset) : Target->SetAngleSnap(Preset);
						}
					}),
					FCanExecuteAction::CreateLambda([bCanPick = Snapping != nullptr]() { return bCanPick; }),
					FIsActionChecked::CreateLambda([bChecked]() { return bChecked; })),
				NAME_None, EUserInterfaceActionType::RadioButton);
		}
	};

	Menu.BeginSection(TEXT("GridSnap"), LOCTEXT("GridSnapSection", "Grid Snap"));
	AddPresets(StageViewMath::GetGridSnapPresets(), Snapping ? Snapping->GetGridSnap() : 0.f, LOCTEXT("Cm", " cm"),
		LOCTEXT("GridSnapTip", "Grid step for placing and moving items; positions land on the grid, which the reference grid shows."), true);
	Menu.EndSection();

	Menu.BeginSection(TEXT("AngleSnap"), LOCTEXT("AngleSnapSection", "Angle Snap"));
	AddPresets(StageViewMath::GetAngleSnapPresets(), Snapping ? Snapping->GetAngleSnap() : 0.f, LOCTEXT("Degrees", "°"),
		LOCTEXT("AngleSnapTip", "Rotation step for the rotate tool."), false);
	Menu.EndSection();

	const bool bSnapToItems = Snapping && Snapping->IsSnapToItemsEnabled();
	Menu.BeginSection(TEXT("Items"), LOCTEXT("ItemsSection", "Items"));
	Menu.AddMenuEntry(LOCTEXT("SnapToItems", "Snap to Items"),
		LOCTEXT("SnapToItemsTip", "Placed and moved items snap to the edges and faces of nearby items."), FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateLambda([WeakSnapping]()
			{
				if (UStageSnappingComponent* Target = WeakSnapping.Get())
				{
					Target->SetSnapToItemsEnabled(!Target->IsSnapToItemsEnabled());
				}
			}),
			FCanExecuteAction::CreateLambda([bCanToggle = Snapping != nullptr]() { return bCanToggle; }),
			FIsActionChecked::CreateLambda([bSnapToItems]() { return bSnapToItems; })),
		NAME_None, EUserInterfaceActionType::ToggleButton);
	Menu.EndSection();

	return Menu.MakeWidget();
}

TSharedRef<SWidget> UStageViewportToolbar::BuildTransformMenu()
{
	using namespace StageViewportToolbar;

	// The gizmo broadcasts OnModeChanged, which redraws the button label.
	const TWeakObjectPtr<AModularTransformGizmo> WeakGizmo = BoundGizmo;
	const AModularTransformGizmo* Gizmo = WeakGizmo.Get();
	const EGizmoMode CurrentMode = Gizmo ? Gizmo->GetMode() : EGizmoMode::Translate;

	FMenuBuilder Menu(/*bInShouldCloseWindowAfterMenuSelection*/ true, /*InCommandList*/ nullptr);
	Menu.BeginSection(TEXT("Transform"), LOCTEXT("TransformSection", "Transform Tool (Space cycles)"));
	for (const EGizmoMode Mode : GizmoModes)
	{
		const bool bChecked = Gizmo && Mode == CurrentMode;
		Menu.AddMenuEntry(GetGizmoModeName(Mode), GetGizmoModeTip(Mode), FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([WeakGizmo, Mode]()
				{
					if (AModularTransformGizmo* Target = WeakGizmo.Get())
					{
						Target->SetMode(Mode);
					}
				}),
				FCanExecuteAction::CreateLambda([bCanPick = Gizmo != nullptr]() { return bCanPick; }),
				FIsActionChecked::CreateLambda([bChecked]() { return bChecked; })),
			NAME_None, EUserInterfaceActionType::RadioButton);
	}
	Menu.EndSection();
	return Menu.MakeWidget();
}

TSharedRef<SWidget> UStageViewportToolbar::BuildToolsMenu()
{
	// Opening a panel is workspace layout, not stage state. The workspace lives on the game instance; held weakly.
	const UGameInstance* GameInstance = GetGameInstance();
	const TWeakObjectPtr<UStageWorkspaceSubsystem> WeakWorkspace = GameInstance ? GameInstance->GetSubsystem<UStageWorkspaceSubsystem>() : nullptr;

	FMenuBuilder Menu(/*bInShouldCloseWindowAfterMenuSelection*/ true, /*InCommandList*/ nullptr);
	const auto AddPanel = [&Menu, &WeakWorkspace](const FText& Label, const FText& Tip, FGameplayTag Panel)
	{
		Menu.AddMenuEntry(Label, Tip, FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([WeakWorkspace, Panel]()
				{
					if (UStageWorkspaceSubsystem* Workspace = WeakWorkspace.Get())
					{
						Workspace->OpenPanel(Panel);
					}
				}),
				FCanExecuteAction::CreateLambda([bCanOpen = WeakWorkspace.IsValid()]() { return bCanOpen; })));
	};
	Menu.BeginSection(TEXT("Tools"), LOCTEXT("ToolsSection", "Tool Windows"));
	AddPanel(LOCTEXT("Sketch", "Sketch to Stage"),
		LOCTEXT("SketchTip", "Draw stage shapes, truss lines and fixtures from above, or import a plan, and generate them in 3D."), StageCraftTags::Panel_Sketch);
	AddPanel(LOCTEXT("Assist", "AI Assistant"),
		LOCTEXT("AssistTip", "Ask about the stage, or ask for fixtures, cues, weather and reports in plain language."), StageCraftTags::Panel_Assistant);
	Menu.EndSection();
	return Menu.MakeWidget();
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
