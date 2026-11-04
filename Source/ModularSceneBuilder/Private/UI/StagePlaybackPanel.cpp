// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StagePlaybackPanel.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/ButtonSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/World.h"
#include "Player/ModularPlayerController.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "TimerManager.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageToolButton.h"
#include "UI/StageValueSlider.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StagePlaybackPanel)

#define LOCTEXT_NAMESPACE "StagePlaybackPanel"

namespace StagePlaybackPanel
{
	constexpr float StripWidth = 62.f;
	constexpr float FaderHeight = 150.f;
	/** The Grand Master fader reports this index; playback faders report their playback's index. */
	constexpr int32 GrandMasterIndex = 100;

	FText FormatLevel(float Level)
	{
		return FText::Format(LOCTEXT("Percent", "{0}%"), FText::AsNumber(FMath::RoundToInt(Level * 100.f)));
	}
}

const UStageCraftUITheme& UStagePlaybackPanel::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

AModularPlayerController* UStagePlaybackPanel::GetStageController() const
{
	return Cast<AModularPlayerController>(GetOwningPlayer());
}

UTextBlock* UStagePlaybackPanel::MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
	Block->SetColorAndOpacity(FSlateColor(Color));
	Block->SetJustification(ETextJustify::Center);
	return Block;
}

FButtonStyle UStagePlaybackPanel::MakeButtonStyle(bool bLit, const FLinearColor& LitColor) const
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& UITheme = GetTheme();
	const FLinearColor Fill = bLit ? WithAlpha(LitColor, 0.45f) : FLinearColor::Transparent;
	FButtonStyle Style = MakeFlatButton(Fill, bLit ? WithAlpha(LitColor, 0.6f) : UITheme.RowHover, UITheme.Divider);
	Style.SetNormal(MakeBox(Fill, CornerRadius, UITheme.Divider, 1.f));
	return Style;
}

UStageChoiceButton* UStagePlaybackPanel::MakeButton(const FText& Label, const FText& ToolTip, int32 ChoiceIndex, bool bLit)
{
	const UStageCraftUITheme& UITheme = GetTheme();
	UStageChoiceButton* Button = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
	Button->SetStyle(MakeButtonStyle(bLit, UITheme.AccentColor));
	Button->SetToolTipText(ToolTip);
	Button->SetChoiceIndex(ChoiceIndex);
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Button->AddChild(MakeText(Label, 7, TEXT("Bold"), bLit ? UITheme.TextPrimary : UITheme.TextSecondary))))
	{
		LabelSlot->SetPadding(FMargin(2.f, 3.f));
		LabelSlot->SetHorizontalAlignment(HAlign_Center);
	}
	return Button;
}

UStageValueSlider* UStagePlaybackPanel::MakeFader(int32 Index, const FText& ToolTip)
{
	UStageValueSlider* Fader = WidgetTree->ConstructWidget<UStageValueSlider>(UStageValueSlider::StaticClass());
	Fader->SetOrientation(Orient_Vertical);
	Fader->SetWidgetStyle(StageCraftWidgetStyle::MakeSlider(GetTheme()));
	Fader->SetSliderHandleColor(FLinearColor::White);
	Fader->SetMinValue(0.f);
	Fader->SetMaxValue(1.f);
	Fader->SetStepSize(0.01f);
	Fader->SetSliderIndex(Index);
	Fader->SetToolTipText(ToolTip);
	Fader->OnMoved.BindUObject(this, &ThisClass::HandleFaderMoved);
	return Fader;
}

void UStagePlaybackPanel::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
}

void UStagePlaybackPanel::BuildTree()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& UITheme = GetTheme();

	UBorder* Root = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Root"));
	Root->SetBrush(MakeBox(UITheme.PanelBackground));
	Root->SetPadding(FMargin(0.f));
	WidgetTree->RootWidget = Root;
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Column"));
	Root->SetContent(Column);

	UBorder* Header = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Header"));
	Header->SetBrush(MakeBox(UITheme.PanelHeader));
	Header->SetPadding(FMargin(10.f, 6.f));
	UHorizontalBox* HeaderLine = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Header->SetContent(HeaderLine);
	UTextBlock* Title = MakeText(LOCTEXT("Title", "PLAYBACKS"), 9, TEXT("Bold"), UITheme.TextPrimary);
	Title->SetJustification(ETextJustify::Left);
	HeaderLine->AddChildToHorizontalBox(Title);
	UTextBlock* Hint = MakeText(LOCTEXT("Hint", "Intensity: highest fader wins (HTP). Everything else: the playback raised or stepped last wins (LTP)."),
		7, TEXT("Regular"), UITheme.TextSecondary);
	Hint->SetJustification(ETextJustify::Left);
	if (UHorizontalBoxSlot* HintSlot = HeaderLine->AddChildToHorizontalBox(Hint))
	{
		HintSlot->SetPadding(FMargin(12.f, 0.f, 0.f, 0.f));
		HintSlot->SetVerticalAlignment(VAlign_Center);
	}
	Column->AddChildToVerticalBox(Header);

	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("DeskScroll"));
	Scroll->SetOrientation(Orient_Horizontal);
	Scroll->SetWidgetBarStyle(MakeScrollBar(UITheme));
	Scroll->SetScrollbarThickness(FVector2D(6.f, 6.f));
	if (UVerticalBoxSlot* ScrollSlot = Column->AddChildToVerticalBox(Scroll))
	{
		ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		ScrollSlot->SetPadding(FMargin(8.f, 6.f));
	}
	UHorizontalBox* Desk = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("Desk"));
	Scroll->AddChild(Desk);

	Strips.Reset();
	for (int32 Index = 0; Index < StagePlaybackRules::Count; ++Index)
	{
		if (UHorizontalBoxSlot* StripSlot = Desk->AddChildToHorizontalBox(BuildStrip(Index)))
		{
			StripSlot->SetPadding(FMargin(0.f, 0.f, 4.f, 0.f));
		}
	}
	if (UHorizontalBoxSlot* MasterSlot = Desk->AddChildToHorizontalBox(BuildMasters()))
	{
		MasterSlot->SetPadding(FMargin(12.f, 0.f, 0.f, 0.f));
	}
}

UWidget* UStagePlaybackPanel::BuildStrip(int32 Index)
{
	using namespace StageCraftWidgetStyle;
	using namespace StagePlaybackPanel;
	const UStageCraftUITheme& UITheme = GetTheme();

	UBorder* Frame = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Frame->SetBrush(MakeBox(UITheme.RowBackground, CornerRadius, UITheme.Divider, 1.f));
	Frame->SetPadding(FMargin(4.f));
	USizeBox* Width = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Width->SetWidthOverride(StripWidth);
	Width->SetContent(Frame);
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Frame->SetContent(Column);
	const auto Add = [Column](UWidget* Widget, float TopGap)
	{
		if (UVerticalBoxSlot* RowSlot = Column->AddChildToVerticalBox(Widget))
		{
			RowSlot->SetPadding(FMargin(0.f, TopGap, 0.f, 0.f));
			RowSlot->SetHorizontalAlignment(HAlign_Fill);
		}
	};

	FStrip Strip;
	UTextBlock* Name = MakeText(FText::AsNumber(Index + 1), 8, TEXT("Bold"), UITheme.TextPrimary);
	Name->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	Add(Name, 0.f);
	UTextBlock* Cue = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), UITheme.TextSecondary);
	Add(Cue, 1.f);

	USizeBox* FaderBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	FaderBox->SetHeightOverride(FaderHeight);
	UStageValueSlider* Fader = MakeFader(Index, LOCTEXT("FaderTip", "This playback's level. Raising it from 0 starts the playback at its first cue."));
	FaderBox->SetContent(Fader);
	if (UVerticalBoxSlot* FaderSlot = Column->AddChildToVerticalBox(FaderBox))
	{
		FaderSlot->SetPadding(FMargin(0.f, 4.f));
		FaderSlot->SetHorizontalAlignment(HAlign_Center);
	}
	UTextBlock* Level = MakeText(FText::GetEmpty(), 7, TEXT("Bold"), UITheme.TextSecondary);
	Add(Level, 0.f);

	UStageChoiceButton* Go = MakeButton(LOCTEXT("Go", "GO"), LOCTEXT("GoTip", "Step this playback to its next cue (its first when released)."), Index, false);
	Go->OnPicked.BindUObject(this, &ThisClass::HandleGoPicked);
	Add(Go, 4.f);

	UStageFlashButton* Flash = WidgetTree->ConstructWidget<UStageFlashButton>(UStageFlashButton::StaticClass());
	Flash->SetStyle(MakeButtonStyle(false, UITheme.AccentColor));
	Flash->SetToolTipText(LOCTEXT("FlashTip", "Hold to bring this playback to full; let go to return to its fader."));
	Flash->SetFlashIndex(Index);
	Flash->OnFlash.BindUObject(this, &ThisClass::HandleFlash);
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Flash->AddChild(MakeText(LOCTEXT("Flash", "FLASH"), 7, TEXT("Bold"), UITheme.TextSecondary))))
	{
		LabelSlot->SetPadding(FMargin(2.f, 3.f));
		LabelSlot->SetHorizontalAlignment(HAlign_Center);
	}
	Add(Flash, 2.f);

	UStageChoiceButton* Record = MakeButton(LOCTEXT("Record", "REC"), LOCTEXT("RecordTip", "Record what the programmer holds as this playback's next cue, then clear the programmer (undoable)."), Index, false);
	Record->OnPicked.BindUObject(this, &ThisClass::HandleRecordPicked);
	Add(Record, 6.f);
	UStageChoiceButton* Release = MakeButton(LOCTEXT("Release", "REL"), LOCTEXT("ReleaseTip", "Release this playback: fader to 0, back before its first cue."), Index, false);
	Release->OnPicked.BindUObject(this, &ThisClass::HandleReleasePicked);
	Add(Release, 2.f);
	UStageChoiceButton* Clear = MakeButton(LOCTEXT("Clear", "CLR"), LOCTEXT("ClearTip", "Delete this playback's cues (undoable)."), Index, false);
	Clear->OnPicked.BindUObject(this, &ThisClass::HandleClearPicked);
	Add(Clear, 2.f);

	Strip.Frame = Frame;
	Strip.Name = Name;
	Strip.Cue = Cue;
	Strip.Fader = Fader;
	Strip.Level = Level;
	Strip.Flash = Flash;
	Strips.Add(Strip);
	return Width;
}

UWidget* UStagePlaybackPanel::BuildMasters()
{
	using namespace StageCraftWidgetStyle;
	using namespace StagePlaybackPanel;
	const UStageCraftUITheme& UITheme = GetTheme();

	UBorder* Frame = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Frame->SetBrush(MakeBox(UITheme.PanelHeader, CornerRadius, UITheme.Divider, 1.f));
	Frame->SetPadding(FMargin(4.f));
	USizeBox* Width = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Width->SetWidthOverride(StripWidth + 12.f);
	Width->SetContent(Frame);
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Frame->SetContent(Column);

	UTextBlock* Title = MakeText(LOCTEXT("GrandMaster", "GM"), 8, TEXT("Bold"), UITheme.TextPrimary);
	Title->SetToolTipText(LOCTEXT("GrandMasterTitleTip", "Grand Master: scales every light's intensity last, whatever plays or is held."));
	Column->AddChildToVerticalBox(Title);
	Column->AddChildToVerticalBox(MakeText(LOCTEXT("Masters", "MASTER"), 7, TEXT("Regular"), UITheme.TextSecondary));
	USizeBox* FaderBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	FaderBox->SetHeightOverride(FaderHeight);
	GrandMasterFader = MakeFader(GrandMasterIndex, LOCTEXT("GrandMasterTip", "Grand Master. A stage always opens at full."));
	FaderBox->SetContent(GrandMasterFader);
	if (UVerticalBoxSlot* FaderSlot = Column->AddChildToVerticalBox(FaderBox))
	{
		FaderSlot->SetPadding(FMargin(0.f, 4.f));
		FaderSlot->SetHorizontalAlignment(HAlign_Center);
	}
	GrandMasterText = MakeText(FText::GetEmpty(), 7, TEXT("Bold"), UITheme.TextSecondary);
	Column->AddChildToVerticalBox(GrandMasterText);
	BlackoutButton = MakeButton(LOCTEXT("Blackout", "BLACKOUT"), LOCTEXT("BlackoutTip", "Take every light to zero at once (Ctrl+Shift+B). Click again to bring them back."), 0, false);
	BlackoutButton->OnPicked.BindUObject(this, &ThisClass::HandleBlackoutPicked);
	if (UVerticalBoxSlot* BlackoutSlot = Column->AddChildToVerticalBox(BlackoutButton))
	{
		BlackoutSlot->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
	}
	return Width;
}

void UStagePlaybackPanel::NativeConstruct()
{
	Super::NativeConstruct();
	UShowControlSubsystem* Subsystem = UWorld::GetSubsystem<UShowControlSubsystem>(GetWorld());
	ShowControl = Subsystem;
	if (Subsystem)
	{
		Subsystem->OnPlaybacksChanged.AddUniqueDynamic(this, &ThisClass::HandleDeskChanged);
		Subsystem->OnDeskStateChanged.AddUniqueDynamic(this, &ThisClass::HandleDeskChanged);
		Subsystem->OnControlSourceChanged.AddUniqueDynamic(this, &ThisClass::HandleDeskChangedBySource);
	}
	Refresh();
}

void UStagePlaybackPanel::NativeDestruct()
{
	if (UShowControlSubsystem* Subsystem = ShowControl.Get())
	{
		Subsystem->OnPlaybacksChanged.RemoveDynamic(this, &ThisClass::HandleDeskChanged);
		Subsystem->OnDeskStateChanged.RemoveDynamic(this, &ThisClass::HandleDeskChanged);
		Subsystem->OnControlSourceChanged.RemoveDynamic(this, &ThisClass::HandleDeskChangedBySource);
	}
	if (const UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RefreshTimer);
	}
	ShowControl.Reset();
	Super::NativeDestruct();
}

void UStagePlaybackPanel::HandleDeskChanged()
{
	RequestRefresh();
}

void UStagePlaybackPanel::HandleDeskChangedBySource(EStageControlSource NewSource)
{
	RequestRefresh();
}

void UStagePlaybackPanel::RequestRefresh()
{
	UWorld* World = GetWorld();
	if (!World || RefreshTimer.IsValid())
	{
		return;
	}
	// Next tick: never inside the fader callback that caused it, and a drag's burst of changes is one refresh a frame.
	RefreshTimer = World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ThisClass::Refresh));
}

void UStagePlaybackPanel::Refresh()
{
	RefreshTimer.Invalidate();
	const UShowControlSubsystem* Subsystem = ShowControl.Get();
	if (!Subsystem)
	{
		return;
	}
	const UStageCraftUITheme& UITheme = GetTheme();
	const bool bExternal = Subsystem->GetControlSource() == EStageControlSource::External;
	for (int32 Index = 0; Index < Strips.Num() && Index < Subsystem->GetPlaybacks().Num(); ++Index)
	{
		const FStrip& Strip = Strips[Index];
		const FStagePlayback& Playback = Subsystem->GetPlaybacks()[Index];
		const bool bActive = Subsystem->IsPlaybackActive(Index);
		const int32 CueIndex = Subsystem->GetPlaybackCueIndex(Index);
		if (UBorder* Frame = Strip.Frame.Get())
		{
			Frame->SetBrush(StageCraftWidgetStyle::MakeBox(bActive ? StageCraftWidgetStyle::WithAlpha(UITheme.AccentColor, 0.18f) : UITheme.RowBackground,
				StageCraftWidgetStyle::CornerRadius, bActive ? UITheme.AccentColor : UITheme.Divider, 1.f));
		}
		if (UTextBlock* Name = Strip.Name.Get())
		{
			Name->SetText(Playback.Name.IsEmpty() ? FText::AsNumber(Index + 1) : FText::Format(LOCTEXT("NamedPlayback", "{0} {1}"), FText::AsNumber(Index + 1), Playback.Name));
			Name->SetToolTipText(Playback.Name);
		}
		if (UTextBlock* Cue = Strip.Cue.Get())
		{
			Cue->SetText(Playback.Cues.IsEmpty() ? LOCTEXT("EmptyPlayback", "empty")
				: FText::Format(LOCTEXT("CuePosition", "{0}/{1}"), CueIndex == INDEX_NONE ? FText::FromString(TEXT("-")) : FText::AsNumber(CueIndex + 1), FText::AsNumber(Playback.Cues.Num())));
		}
		if (UStageValueSlider* Fader = Strip.Fader.Get())
		{
			Fader->SetValue(Subsystem->GetPlaybackLevel(Index));
			Fader->SetIsEnabled(!bExternal);
		}
		if (UTextBlock* Level = Strip.Level.Get())
		{
			Level->SetText(StagePlaybackPanel::FormatLevel(Subsystem->GetPlaybackLevel(Index)));
			Level->SetColorAndOpacity(FSlateColor(bActive ? UITheme.AccentColor : UITheme.TextSecondary));
		}
		if (UStageFlashButton* Flash = Strip.Flash.Get())
		{
			Flash->SetStyle(MakeButtonStyle(Subsystem->IsPlaybackFlashing(Index), UITheme.AccentColor));
		}
	}
	GrandMasterFader->SetValue(Subsystem->GetGrandMaster());
	GrandMasterText->SetText(StagePlaybackPanel::FormatLevel(Subsystem->GetGrandMaster()));
	BlackoutButton->SetStyle(MakeButtonStyle(Subsystem->IsBlackout(), UITheme.WarningColor));
	if (UTextBlock* Label = Cast<UTextBlock>(BlackoutButton->GetChildAt(0)))
	{
		Label->SetColorAndOpacity(FSlateColor(Subsystem->IsBlackout() ? UITheme.TextPrimary : UITheme.TextSecondary));
	}
}

void UStagePlaybackPanel::HandleFaderMoved(int32 FaderIndex, float Value)
{
	AModularPlayerController* Controller = GetStageController();
	if (!Controller)
	{
		return;
	}
	if (FaderIndex == StagePlaybackPanel::GrandMasterIndex)
	{
		Controller->RequestSetGrandMaster(Value);
	}
	else
	{
		Controller->RequestSetPlaybackLevel(FaderIndex, Value);
	}
	RequestRefresh();
}

void UStagePlaybackPanel::HandleFlash(int32 Index, bool bDown)
{
	if (AModularPlayerController* Controller = GetStageController())
	{
		Controller->RequestPlaybackFlash(Index, bDown);
	}
	RequestRefresh();
}

void UStagePlaybackPanel::HandleGoPicked(int32 Index)
{
	if (AModularPlayerController* Controller = GetStageController())
	{
		Controller->RequestPlaybackGo(Index);
	}
	RequestRefresh();
}

void UStagePlaybackPanel::HandleRecordPicked(int32 Index)
{
	if (AModularPlayerController* Controller = GetStageController())
	{
		Controller->RequestRecordPlayback(Index);
	}
	RequestRefresh();
}

void UStagePlaybackPanel::HandleReleasePicked(int32 Index)
{
	if (AModularPlayerController* Controller = GetStageController())
	{
		Controller->RequestReleasePlayback(Index);
	}
	RequestRefresh();
}

void UStagePlaybackPanel::HandleClearPicked(int32 Index)
{
	if (AModularPlayerController* Controller = GetStageController())
	{
		Controller->RequestClearPlayback(Index);
	}
	RequestRefresh();
}

void UStagePlaybackPanel::HandleBlackoutPicked(int32 ChoiceIndex)
{
	AModularPlayerController* Controller = GetStageController();
	const UShowControlSubsystem* Subsystem = ShowControl.Get();
	if (Controller && Subsystem)
	{
		Controller->RequestSetBlackout(!Subsystem->IsBlackout());
	}
	RequestRefresh();
}

#undef LOCTEXT_NAMESPACE
