// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StagePreferencesPanel.h"

#include "Audio/StageAudioSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/ButtonSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Input/StageInputPreferencesSubsystem.h"
#include "Placement/StageSnappingComponent.h"
#include "Player/ModularPlayerController.h"
#include "Settings/StageCraftUserSettings.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageToolButton.h"
#include "UI/StageValueSlider.h"
#include "Viewport/StageViewportComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StagePreferencesPanel)

#define LOCTEXT_NAMESPACE "StagePreferencesPanel"

namespace StagePreferencesPanel
{
	constexpr float SectionGap = 12.f;
	constexpr float ChoiceGap = 3.f;
	constexpr float RowGap = 3.f;
	constexpr float LabelWidth = 112.f;
	constexpr float ValueWidth = 44.f;
	constexpr float CommandLabelWidth = 150.f;
	constexpr float BindingWidth = 104.f;

	/** The panel's sliders, in order; each one's index in UStagePreferencesPanel::Sliders. */
	enum class ESlider : int32
	{
		LookSensitivity,
		MasterVolume,
		EffectsVolume,
		Count
	};

	int32 BindingIndex(EStageInputCommand Command, EStageKeySlot Slot)
	{
		return static_cast<int32>(Command) * 2 + static_cast<int32>(Slot);
	}

	FText CategoryTitle(EStageInputCategory Category)
	{
		switch (Category)
		{
		case EStageInputCategory::Editing:		return LOCTEXT("CategoryEditing", "EDITING");
		case EStageInputCategory::Tools:		return LOCTEXT("CategoryTools", "TOOLS");
		case EStageInputCategory::View:			return LOCTEXT("CategoryView", "VIEW");
		case EStageInputCategory::Navigation:	return LOCTEXT("CategoryNavigation", "FLYING  (while the right mouse button is held)");
		default:								return FText::GetEmpty();
		}
	}
}

const UStageCraftUITheme& UStagePreferencesPanel::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

void UStagePreferencesPanel::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	// Only so a shortcut can be captured: the panel takes keyboard focus while a binding button is armed, never otherwise.
	SetIsFocusable(true);
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
}

UTextBlock* UStagePreferencesPanel::MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
	Block->SetColorAndOpacity(FSlateColor(Color));
	return Block;
}

UStageChoiceButton* UStagePreferencesPanel::MakeButton(const FText& Label, const FText& Tip, int32 Index)
{
	const UStageCraftUITheme& Palette = GetTheme();
	UStageChoiceButton* Button = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
	Button->SetChoiceIndex(Index);
	Button->SetToolTipText(Tip);
	UTextBlock* Text = MakeText(Label, 8, TEXT("Bold"), Palette.TextSecondary);
	Text->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Button->AddChild(Text)))
	{
		LabelSlot->SetPadding(FMargin(4.f, 3.f));
		LabelSlot->SetHorizontalAlignment(HAlign_Center);
	}
	StyleChoice(*Button, false);
	return Button;
}

void UStagePreferencesPanel::BuildTree()
{
	using namespace StageCraftWidgetStyle;
	using namespace StagePreferencesPanel;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Root = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Root"));
	Root->SetBrush(MakeBox(Palette.PanelBackground));
	Root->SetPadding(FMargin(0.f));
	WidgetTree->RootWidget = Root;

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Column"));
	Root->SetContent(Column);
	Column->AddChildToVerticalBox(BuildHeader());

	UImage* Divider = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("HeaderDivider"));
	Divider->SetBrush(MakeLine(Palette.Divider, FVector2D(1.f, 1.f)));
	Column->AddChildToVerticalBox(Divider);

	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("Scroll"));
	Scroll->SetWidgetBarStyle(MakeScrollBar(Palette));
	if (UVerticalBoxSlot* ScrollSlot = Column->AddChildToVerticalBox(Scroll))
	{
		ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}

	UVerticalBox* Body = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Body"));
	UBorder* BodyPadding = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("BodyPadding"));
	BodyPadding->SetBrush(MakeBox(FLinearColor::Transparent));
	BodyPadding->SetPadding(FMargin(10.f, 8.f));
	BodyPadding->SetContent(Body);
	Scroll->AddChild(BodyPadding);

	const auto Add = [Body](UWidget* Widget, float Gap = RowGap)
	{
		if (UVerticalBoxSlot* WidgetSlot = Body->AddChildToVerticalBox(Widget))
		{
			WidgetSlot->SetPadding(FMargin(0.f, 0.f, 0.f, Gap));
		}
	};

	Sliders.SetNum(int32(ESlider::Count));
	SliderValues.SetNum(int32(ESlider::Count));

	Add(BuildSectionTitle(LOCTEXT("Camera", "CAMERA")));
	Add(BuildSliderRow(LOCTEXT("LookSpeed", "Look speed"), LOCTEXT("LookSpeedTip", "How fast the view turns while the right mouse button is held."),
		int32(ESlider::LookSensitivity), StageInputPreferences::MinLookSensitivity, StageInputPreferences::MaxLookSensitivity));
	Add(BuildChoiceRow(LOCTEXT("VerticalLook", "Vertical look"), LOCTEXT("VerticalLookTip", "Inverted: moving the mouse up looks down, like a flight stick."),
		{ LOCTEXT("LookNormal", "Normal"), LOCTEXT("LookInverted", "Inverted") }, LookInvertButtons, &ThisClass::HandleLookInvertPicked), SectionGap);

	Add(BuildSectionTitle(LOCTEXT("Viewport", "VIEWPORT")));
	Add(BuildChoiceRow(LOCTEXT("SnapToItems", "Snap to items"), LOCTEXT("SnapToItemsTip", "While placing or moving, lock onto nearby items: side by side, stacked, or aligned."),
		{ LOCTEXT("Off", "Off"), LOCTEXT("On", "On") }, SnapItemsButtons, &ThisClass::HandleSnapItemsPicked));
	Add(BuildChoiceRow(LOCTEXT("Grid", "Grid"), LOCTEXT("GridTip", "The viewport's reference grid."),
		{ LOCTEXT("GridHidden", "Hidden"), LOCTEXT("GridShown", "Shown") }, GridButtons, &ThisClass::HandleGridPicked), SectionGap);

	Add(BuildSectionTitle(LOCTEXT("Audio", "AUDIO")));
	Add(BuildSliderRow(LOCTEXT("Master", "Master"), LOCTEXT("MasterTip", "Volume of every sound."), int32(ESlider::MasterVolume), 0.f, 1.f));
	Add(BuildSliderRow(LOCTEXT("Effects", "Effects"), LOCTEXT("EffectsTip", "Volume of the editor's feedback sounds."), int32(ESlider::EffectsVolume), 0.f, 1.f));
	Add(BuildChoiceRow(LOCTEXT("Mute", "Mute all"), LOCTEXT("MuteTip", "Silence every sound; the volumes are kept for when you unmute."),
		{ LOCTEXT("Off", "Off"), LOCTEXT("On", "On") }, MuteButtons, &ThisClass::HandleMutePicked), SectionGap);

	Add(BuildShortcutSection());
}

UWidget* UStagePreferencesPanel::BuildHeader()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Header = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Header"));
	Header->SetBrush(MakeBox(Palette.PanelHeader));
	Header->SetPadding(FMargin(10.f, 8.f));
	UTextBlock* Title = MakeText(LOCTEXT("Title", "PREFERENCES"), 9, TEXT("Bold"), Palette.TextPrimary);
	Title->SetToolTipText(LOCTEXT("TitleTip", "Saved for this computer and applied at once."));
	Header->SetContent(Title);
	return Header;
}

UWidget* UStagePreferencesPanel::BuildSectionTitle(const FText& Title)
{
	UTextBlock* Text = MakeText(Title, 7, TEXT("Bold"), GetTheme().TextSecondary);
	USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Box->SetContent(Text);
	return Box;
}

UWidget* UStagePreferencesPanel::BuildChoiceRow(const FText& Label, const FText& Tip, TConstArrayView<FText> Choices,
	TArray<TObjectPtr<UStageChoiceButton>>& OutButtons, void (UStagePreferencesPanel::*Handler)(int32))
{
	using namespace StagePreferencesPanel;
	const UStageCraftUITheme& Palette = GetTheme();

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Row->SetToolTipText(Tip);
	USizeBox* LabelBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	LabelBox->SetWidthOverride(LabelWidth);
	LabelBox->SetContent(MakeText(Label, 8, TEXT("Regular"), Palette.TextPrimary));
	if (UHorizontalBoxSlot* LabelSlot = Row->AddChildToHorizontalBox(LabelBox))
	{
		LabelSlot->SetVerticalAlignment(VAlign_Center);
	}

	OutButtons.Reset(Choices.Num());
	for (int32 Index = 0; Index < Choices.Num(); ++Index)
	{
		UStageChoiceButton* Button = MakeButton(Choices[Index], Tip, Index);
		Button->OnPicked.BindUObject(this, Handler);
		if (UHorizontalBoxSlot* ButtonSlot = Row->AddChildToHorizontalBox(Button))
		{
			ButtonSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			ButtonSlot->SetPadding(FMargin(Index == 0 ? 0.f : ChoiceGap, 0.f, 0.f, 0.f));
		}
		OutButtons.Add(Button);
	}

	USizeBox* SizedRow = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	SizedRow->SetHeightOverride(StageCraftWidgetStyle::FieldHeight);
	SizedRow->SetContent(Row);
	return SizedRow;
}

UWidget* UStagePreferencesPanel::BuildSliderRow(const FText& Label, const FText& Tip, int32 SliderIndex, float Min, float Max)
{
	using namespace StagePreferencesPanel;
	const UStageCraftUITheme& Palette = GetTheme();

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Row->SetToolTipText(Tip);
	USizeBox* LabelBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	LabelBox->SetWidthOverride(LabelWidth);
	LabelBox->SetContent(MakeText(Label, 8, TEXT("Regular"), Palette.TextPrimary));
	if (UHorizontalBoxSlot* LabelSlot = Row->AddChildToHorizontalBox(LabelBox))
	{
		LabelSlot->SetVerticalAlignment(VAlign_Center);
	}

	UStageValueSlider* Slider = WidgetTree->ConstructWidget<UStageValueSlider>(UStageValueSlider::StaticClass());
	Slider->SetSliderIndex(SliderIndex);
	Slider->SetMinValue(Min);
	Slider->SetMaxValue(Max);
	Slider->SetWidgetStyle(StageCraftWidgetStyle::MakeSlider(Palette));
	Slider->OnMoved.BindUObject(this, &ThisClass::HandleSliderMoved);
	if (UHorizontalBoxSlot* SliderSlot = Row->AddChildToHorizontalBox(Slider))
	{
		SliderSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		SliderSlot->SetVerticalAlignment(VAlign_Center);
		SliderSlot->SetPadding(FMargin(0.f, 0.f, 4.f, 0.f));
	}
	Sliders[SliderIndex] = Slider;

	USizeBox* ValueBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	ValueBox->SetWidthOverride(ValueWidth);
	UTextBlock* Value = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextSecondary);
	Value->SetJustification(ETextJustify::Right);
	ValueBox->SetContent(Value);
	if (UHorizontalBoxSlot* ValueSlot = Row->AddChildToHorizontalBox(ValueBox))
	{
		ValueSlot->SetVerticalAlignment(VAlign_Center);
	}
	SliderValues[SliderIndex] = Value;

	USizeBox* SizedRow = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	SizedRow->SetHeightOverride(StageCraftWidgetStyle::FieldHeight);
	SizedRow->SetContent(Row);
	return SizedRow;
}

UWidget* UStagePreferencesPanel::BuildShortcutSection()
{
	using namespace StagePreferencesPanel;
	const UStageCraftUITheme& Palette = GetTheme();

	UVerticalBox* Section = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	const auto Add = [Section](UWidget* Widget, float Gap)
	{
		if (UVerticalBoxSlot* WidgetSlot = Section->AddChildToVerticalBox(Widget))
		{
			WidgetSlot->SetPadding(FMargin(0.f, 0.f, 0.f, Gap));
		}
	};

	// Title, then the actions that act on the whole map.
	UHorizontalBox* TitleRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	if (UHorizontalBoxSlot* TitleSlot = TitleRow->AddChildToHorizontalBox(BuildSectionTitle(LOCTEXT("Shortcuts", "KEYBOARD SHORTCUTS"))))
	{
		TitleSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		TitleSlot->SetVerticalAlignment(VAlign_Center);
	}
	ClearButton = MakeButton(LOCTEXT("Clear", "CLEAR"), LOCTEXT("ClearTip", "Remove the shortcut being changed."), 0);
	ClearButton->OnPicked.BindUObject(this, &ThisClass::HandleClearPicked);
	if (UHorizontalBoxSlot* ClearSlot = TitleRow->AddChildToHorizontalBox(ClearButton))
	{
		ClearSlot->SetPadding(FMargin(0.f, 0.f, ChoiceGap, 0.f));
	}
	UStageChoiceButton* ResetAll = MakeButton(LOCTEXT("ResetAll", "RESET ALL"), LOCTEXT("ResetAllTip", "Every shortcut back to its default key."), 0);
	ResetAll->OnPicked.BindUObject(this, &ThisClass::HandleResetAllPicked);
	TitleRow->AddChildToHorizontalBox(ResetAll);
	Add(TitleRow, RowGap);

	NoticeText = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextSecondary);
	NoticeText->SetAutoWrapText(true);
	Add(NoticeText, RowGap * 2.f);

	BindingButtons.SetNum(static_cast<int32>(EStageInputCommand::Count) * 2);
	ResetButtons.SetNum(static_cast<int32>(EStageInputCommand::Count));
	TOptional<EStageInputCategory> Category;
	for (const FStageInputCommandInfo& Info : StageKeyBindings::GetCommands())
	{
		if (!Category.IsSet() || Category.GetValue() != Info.Category)
		{
			if (Category.IsSet())
			{
				Add(WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass()), RowGap * 2.f);
			}
			Category = Info.Category;
			Add(MakeText(CategoryTitle(Info.Category), 7, TEXT("Bold"), Palette.TextDisabled), RowGap);
		}
		Add(BuildCommandRow(Info), RowGap);
	}

	// The mouse is not remappable: the viewport's cursor capture and the gizmo are built around these buttons (ADR 0013 §2.3).
	UTextBlock* Mouse = MakeText(LOCTEXT("MouseFixed",
		"Mouse (fixed): left click selects or places   ·   Ctrl+Click adds or removes   ·   hold the right button to look and fly   ·   wheel: fly speed"),
		8, TEXT("Regular"), Palette.TextSecondary);
	Mouse->SetAutoWrapText(true);
	Add(WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass()), RowGap * 2.f);
	Add(Mouse, 0.f);
	return Section;
}

UWidget* UStagePreferencesPanel::BuildCommandRow(const FStageInputCommandInfo& Info)
{
	using namespace StagePreferencesPanel;
	const UStageCraftUITheme& Palette = GetTheme();

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	USizeBox* LabelBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	LabelBox->SetWidthOverride(CommandLabelWidth);
	UTextBlock* Label = MakeText(Info.Label, 8, TEXT("Regular"), Palette.TextPrimary);
	Label->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	Label->SetToolTipText(Info.Description);
	LabelBox->SetContent(Label);
	if (UHorizontalBoxSlot* LabelSlot = Row->AddChildToHorizontalBox(LabelBox))
	{
		LabelSlot->SetVerticalAlignment(VAlign_Center);
	}

	for (EStageKeySlot KeySlot : { EStageKeySlot::Primary, EStageKeySlot::Secondary })
	{
		const int32 Index = BindingIndex(Info.Command, KeySlot);
		UStageChoiceButton* Button = MakeButton(FText::GetEmpty(), KeySlot == EStageKeySlot::Primary
			? FText::Format(LOCTEXT("PrimaryTip", "{0}: click, then press the new shortcut."), Info.Label)
			: FText::Format(LOCTEXT("SecondaryTip", "{0}, second shortcut: click, then press it."), Info.Label), Index);
		Button->OnPicked.BindUObject(this, &ThisClass::HandleBindingPicked);
		USizeBox* ButtonBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		ButtonBox->SetWidthOverride(BindingWidth);
		ButtonBox->SetContent(Button);
		if (UHorizontalBoxSlot* ButtonSlot = Row->AddChildToHorizontalBox(ButtonBox))
		{
			ButtonSlot->SetPadding(FMargin(ChoiceGap, 0.f, 0.f, 0.f));
		}
		BindingButtons[Index] = Button;
	}

	UStageChoiceButton* Reset = MakeButton(LOCTEXT("Reset", "RESET"), FText::Format(LOCTEXT("ResetTip", "{0}: back to the default keys."), Info.Label), static_cast<int32>(Info.Command));
	Reset->OnPicked.BindUObject(this, &ThisClass::HandleResetPicked);
	if (UHorizontalBoxSlot* ResetSlot = Row->AddChildToHorizontalBox(Reset))
	{
		ResetSlot->SetPadding(FMargin(ChoiceGap, 0.f, 0.f, 0.f));
	}
	ResetButtons[static_cast<int32>(Info.Command)] = Reset;

	USizeBox* SizedRow = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	SizedRow->SetHeightOverride(StageCraftWidgetStyle::FieldHeight);
	SizedRow->SetContent(Row);
	return SizedRow;
}

// --- Lifetime ---

void UStagePreferencesPanel::NativeConstruct()
{
	Super::NativeConstruct();

	if (UStageInputPreferencesSubsystem* Preferences = GetInputPreferences())
	{
		BoundPreferences = Preferences;
		Preferences->OnInputPreferencesChanged.AddUniqueDynamic(this, &ThisClass::HandlePreferencesChanged);
	}
	if (UStageAudioSubsystem* Audio = GetAudio())
	{
		BoundAudio = Audio;
		Audio->OnAudioSettingsChanged.AddUniqueDynamic(this, &ThisClass::HandlePreferencesChanged);
	}
	if (UStageSnappingComponent* Snapping = GetSnapping())
	{
		BoundSnapping = Snapping;
		Snapping->OnSnapSettingsChanged.AddUniqueDynamic(this, &ThisClass::HandlePreferencesChanged);
	}
	if (UStageViewportComponent* Viewport = GetViewport())
	{
		BoundViewport = Viewport;
		Viewport->OnViewChanged.AddUniqueDynamic(this, &ThisClass::HandlePreferencesChanged);
	}
	SetNotice(LOCTEXT("Intro", "Click a shortcut to change it. Fly keys work only while the right mouse button is held, so they may share a key with a tool (W: Move tool and fly forward)."), false);
	Refresh();
}

void UStagePreferencesPanel::NativeDestruct()
{
	Capture.Reset();
	if (UStageInputPreferencesSubsystem* Preferences = BoundPreferences.Get())
	{
		Preferences->OnInputPreferencesChanged.RemoveDynamic(this, &ThisClass::HandlePreferencesChanged);
	}
	if (UStageAudioSubsystem* Audio = BoundAudio.Get())
	{
		Audio->OnAudioSettingsChanged.RemoveDynamic(this, &ThisClass::HandlePreferencesChanged);
	}
	if (UStageSnappingComponent* Snapping = BoundSnapping.Get())
	{
		Snapping->OnSnapSettingsChanged.RemoveDynamic(this, &ThisClass::HandlePreferencesChanged);
	}
	if (UStageViewportComponent* Viewport = BoundViewport.Get())
	{
		Viewport->OnViewChanged.RemoveDynamic(this, &ThisClass::HandlePreferencesChanged);
	}
	BoundPreferences.Reset();
	BoundAudio.Reset();
	BoundSnapping.Reset();
	BoundViewport.Reset();
	Super::NativeDestruct();
}

UStageInputPreferencesSubsystem* UStagePreferencesPanel::GetInputPreferences() const
{
	return ULocalPlayer::GetSubsystem<UStageInputPreferencesSubsystem>(GetOwningLocalPlayer());
}

UStageAudioSubsystem* UStagePreferencesPanel::GetAudio() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UStageAudioSubsystem>() : nullptr;
}

UStageSnappingComponent* UStagePreferencesPanel::GetSnapping() const
{
	const AModularPlayerController* Controller = Cast<AModularPlayerController>(GetOwningPlayer());
	return Controller ? Controller->GetSnapping() : nullptr;
}

UStageViewportComponent* UStagePreferencesPanel::GetViewport() const
{
	const AModularPlayerController* Controller = Cast<AModularPlayerController>(GetOwningPlayer());
	return Controller ? Controller->GetViewportView() : nullptr;
}

// --- Read-back ---

void UStagePreferencesPanel::StyleChoice(UStageChoiceButton& Button, bool bSelected) const
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	// Selected: solid accent like a lit console key; the others outlined, so they still read as buttons.
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

void UStagePreferencesPanel::SetNotice(const FText& Text, bool bWarning)
{
	if (NoticeText)
	{
		const UStageCraftUITheme& Palette = GetTheme();
		NoticeText->SetText(Text);
		NoticeText->SetColorAndOpacity(FSlateColor(bWarning ? Palette.WarningColor : Palette.TextSecondary));
	}
}

void UStagePreferencesPanel::Refresh()
{
	using namespace StagePreferencesPanel;

	const auto StyleRow = [this](const TArray<TObjectPtr<UStageChoiceButton>>& Buttons, int32 Selected, bool bEnabled)
	{
		for (UStageChoiceButton* Button : Buttons)
		{
			if (Button)
			{
				StyleChoice(*Button, Button->GetChoiceIndex() == Selected);
				Button->SetIsEnabled(bEnabled);
			}
		}
	};
	const auto ShowSlider = [this](ESlider Slider, float Value, const FText& Text, bool bEnabled)
	{
		if (UStageValueSlider* Widget = Sliders.IsValidIndex(int32(Slider)) ? Sliders[int32(Slider)].Get() : nullptr)
		{
			Widget->SetValue(Value);
			Widget->SetIsEnabled(bEnabled);
		}
		if (UTextBlock* Widget = SliderValues.IsValidIndex(int32(Slider)) ? SliderValues[int32(Slider)].Get() : nullptr)
		{
			Widget->SetText(Text);
		}
	};

	const UStageInputPreferencesSubsystem* Preferences = BoundPreferences.Get();
	const float Look = Preferences ? Preferences->GetLookSensitivity() : StageInputPreferences::DefaultLookSensitivity;
	FNumberFormattingOptions OneDecimal;
	OneDecimal.SetMinimumFractionalDigits(1).SetMaximumFractionalDigits(1);
	ShowSlider(ESlider::LookSensitivity, Look, FText::Format(LOCTEXT("Times", "{0}×"), FText::AsNumber(Look, &OneDecimal)), Preferences != nullptr);
	StyleRow(LookInvertButtons, Preferences && Preferences->IsLookInverted() ? 1 : 0, Preferences != nullptr);

	const UStageSnappingComponent* Snapping = BoundSnapping.Get();
	StyleRow(SnapItemsButtons, Snapping && Snapping->IsSnapToItemsEnabled() ? 1 : 0, Snapping != nullptr);
	const UStageViewportComponent* Viewport = BoundViewport.Get();
	StyleRow(GridButtons, Viewport && Viewport->IsGridVisible() ? 1 : 0, Viewport != nullptr);

	const UStageAudioSubsystem* Audio = BoundAudio.Get();
	const float Master = Audio ? Audio->GetMasterVolume() : 0.f;
	const float Effects = Audio ? Audio->GetEffectsVolume() : 0.f;
	ShowSlider(ESlider::MasterVolume, Master, FText::AsPercent(Master), Audio != nullptr);
	ShowSlider(ESlider::EffectsVolume, Effects, FText::AsPercent(Effects), Audio != nullptr);
	StyleRow(MuteButtons, Audio && Audio->IsMuted() ? 1 : 0, Audio != nullptr);

	const FStageKeyBindings Bindings = Preferences ? Preferences->GetKeyBindings() : FStageKeyBindings::MakeDefaults();
	for (const FStageInputCommandInfo& Info : StageKeyBindings::GetCommands())
	{
		for (EStageKeySlot KeySlot : { EStageKeySlot::Primary, EStageKeySlot::Secondary })
		{
			UStageChoiceButton* Button = BindingButtons[BindingIndex(Info.Command, KeySlot)];
			if (!Button)
			{
				continue;
			}
			const bool bCapturing = Capture.IsSet() && Capture->Command == Info.Command && Capture->Slot == KeySlot;
			const FText Chord = Bindings.Get(Info.Command, KeySlot).ToText();
			StyleChoice(*Button, bCapturing);
			Button->SetIsEnabled(Preferences != nullptr);
			if (UTextBlock* Label = Cast<UTextBlock>(Button->GetContent()))
			{
				Label->SetText(bCapturing ? LOCTEXT("PressKeys", "Press keys...") : Chord.IsEmpty() ? LOCTEXT("Unbound", "-") : Chord);
			}
		}
		if (UStageChoiceButton* Reset = ResetButtons[static_cast<int32>(Info.Command)])
		{
			const bool bIsDefault = Bindings.Get(Info.Command, EStageKeySlot::Primary) == Info.DefaultPrimary
				&& Bindings.Get(Info.Command, EStageKeySlot::Secondary) == Info.DefaultSecondary;
			Reset->SetIsEnabled(Preferences != nullptr && !bIsDefault);
		}
	}
	if (ClearButton)
	{
		ClearButton->SetVisibility(Capture.IsSet() ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
}

void UStagePreferencesPanel::HandlePreferencesChanged()
{
	Refresh();
}

// --- Writes, each through the value's owner ---

void UStagePreferencesPanel::HandleLookInvertPicked(int32 Index)
{
	if (UStageInputPreferencesSubsystem* Preferences = BoundPreferences.Get())
	{
		Preferences->SetLookInverted(Index == 1);
	}
}

void UStagePreferencesPanel::HandleSnapItemsPicked(int32 Index)
{
	if (UStageSnappingComponent* Snapping = BoundSnapping.Get())
	{
		Snapping->SetSnapToItemsEnabled(Index == 1);
	}
}

void UStagePreferencesPanel::HandleGridPicked(int32 Index)
{
	if (UStageViewportComponent* Viewport = BoundViewport.Get())
	{
		Viewport->SetGridVisible(Index == 1);
	}
}

void UStagePreferencesPanel::HandleMutePicked(int32 Index)
{
	if (UStageAudioSubsystem* Audio = BoundAudio.Get())
	{
		Audio->SetMuted(Index == 1);
	}
}

void UStagePreferencesPanel::HandleSliderMoved(int32 SliderIndex, float Value)
{
	using StagePreferencesPanel::ESlider;
	switch (static_cast<ESlider>(SliderIndex))
	{
	case ESlider::LookSensitivity:
		if (UStageInputPreferencesSubsystem* Preferences = BoundPreferences.Get())
		{
			Preferences->SetLookSensitivity(Value);
		}
		break;
	case ESlider::MasterVolume:
		if (UStageAudioSubsystem* Audio = BoundAudio.Get())
		{
			Audio->SetMasterVolume(Value);
		}
		break;
	case ESlider::EffectsVolume:
		if (UStageAudioSubsystem* Audio = BoundAudio.Get())
		{
			Audio->SetEffectsVolume(Value);
		}
		break;
	default:
		break;
	}
}

// --- Shortcut capture ---

void UStagePreferencesPanel::HandleBindingPicked(int32 Index)
{
	FCaptureTarget Target;
	Target.Command = static_cast<EStageInputCommand>(Index / 2);
	Target.Slot = static_cast<EStageKeySlot>(Index % 2);
	if (Target.Command >= EStageInputCommand::Count)
	{
		return;
	}
	// A second click on the armed slot disarms it.
	if (Capture.IsSet() && Capture->Command == Target.Command && Capture->Slot == Target.Slot)
	{
		EndCapture();
		SetNotice(FText::GetEmpty(), false);
		return;
	}
	BeginCapture(Target);
}

void UStagePreferencesPanel::BeginCapture(const FCaptureTarget& Target)
{
	Capture = Target;
	const FStageInputCommandInfo& Info = StageKeyBindings::GetInfo(Target.Command);
	SetNotice(StageKeyBindings::IsNavigationCommand(Target.Command)
		? FText::Format(LOCTEXT("CaptureFly", "{0}: press a key (no Ctrl, Shift or Alt). CLEAR removes it; clicking elsewhere cancels."), Info.Label)
		: FText::Format(LOCTEXT("Capture", "{0}: press the new shortcut, with Ctrl, Shift or Alt if you like. CLEAR removes it; clicking elsewhere cancels."), Info.Label), false);
	// The keys must reach this panel, not the viewport, so the shortcut being typed is not also run.
	SetKeyboardFocus();
	Refresh();
}

void UStagePreferencesPanel::EndCapture()
{
	Capture.Reset();
	Refresh();
}

FReply UStagePreferencesPanel::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (!Capture.IsSet())
	{
		return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
	}
	const FKey Key = InKeyEvent.GetKey();
	if (Key.IsModifierKey())
	{
		return FReply::Handled(); // Waiting for the key the modifiers go with.
	}

	const FCaptureTarget Target = Capture.GetValue();
	const FStageKeyChord Chord(Key, InKeyEvent.IsControlDown(), InKeyEvent.IsShiftDown(), InKeyEvent.IsAltDown());
	EStageInputCommand Conflict = EStageInputCommand::Count;
	UStageInputPreferencesSubsystem* Preferences = BoundPreferences.Get();
	const EStageKeyBindingResult Result = Preferences ? Preferences->SetKeyBinding(Target.Command, Target.Slot, Chord, Conflict) : EStageKeyBindingResult::UnsupportedKey;
	Capture.Reset();

	const FText Label = StageKeyBindings::GetInfo(Target.Command).Label;
	if (Result == EStageKeyBindingResult::Success || Result == EStageKeyBindingResult::Unchanged)
	{
		SetNotice(FText::Format(LOCTEXT("Bound", "{0}: {1}"), Label, Chord.ToText()), false);
	}
	else
	{
		SetNotice(UStageInputPreferencesSubsystem::DescribeRefusal(Result, Chord, Conflict), true);
	}
	Refresh();
	return FReply::Handled();
}

void UStagePreferencesPanel::NativeOnFocusLost(const FFocusEvent& InFocusEvent)
{
	Super::NativeOnFocusLost(InFocusEvent);
	if (Capture.IsSet())
	{
		EndCapture();
		SetNotice(LOCTEXT("CaptureCancelled", "Not changed."), false);
	}
}

void UStagePreferencesPanel::HandleClearPicked(int32 Unused)
{
	if (!Capture.IsSet())
	{
		return;
	}
	const FCaptureTarget Target = Capture.GetValue();
	if (UStageInputPreferencesSubsystem* Preferences = BoundPreferences.Get())
	{
		Preferences->ClearKeyBinding(Target.Command, Target.Slot);
	}
	SetNotice(FText::Format(LOCTEXT("Cleared", "{0}: no shortcut."), StageKeyBindings::GetInfo(Target.Command).Label), false);
	EndCapture();
}

void UStagePreferencesPanel::HandleResetPicked(int32 CommandIndex)
{
	const EStageInputCommand Command = static_cast<EStageInputCommand>(CommandIndex);
	UStageInputPreferencesSubsystem* Preferences = BoundPreferences.Get();
	if (!Preferences || Command >= EStageInputCommand::Count)
	{
		return;
	}
	Capture.Reset();
	const FText Label = StageKeyBindings::GetInfo(Command).Label;
	SetNotice(Preferences->ResetKeyBinding(Command)
		? FText::Format(LOCTEXT("ResetDone", "{0}: default keys again."), Label)
		: FText::Format(LOCTEXT("ResetPartial", "{0}: a default key is used by another shortcut now, so that slot stays empty."), Label),
		false);
	Refresh();
}

void UStagePreferencesPanel::HandleResetAllPicked(int32 Unused)
{
	if (UStageInputPreferencesSubsystem* Preferences = BoundPreferences.Get())
	{
		Capture.Reset();
		Preferences->ResetAllKeyBindings();
		SetNotice(LOCTEXT("ResetAllDone", "Every shortcut uses its default key again."), false);
		Refresh();
	}
}

#undef LOCTEXT_NAMESPACE
