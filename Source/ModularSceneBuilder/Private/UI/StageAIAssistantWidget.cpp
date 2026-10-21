// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageAIAssistantWidget.h"

#include "AI/StageAICommandParser.h"
#include "AI/StageAICommandSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/ButtonSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/World.h"
#include "Player/ModularPlayerController.h"
#include "TimerManager.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageToolButton.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageAIAssistantWidget)

#define LOCTEXT_NAMESPACE "StageAIAssistantWidget"

namespace StageAIAssistantWidget
{
	constexpr float RowGap = 4.f;
	constexpr float ChipGap = 3.f;
	constexpr float BubbleRadius = 6.f;
	/** Space kept on the far side of a line, so user and assistant lines read as two columns. */
	constexpr float BubbleInset = 48.f;

	struct FChip
	{
		FText Label;
		const TCHAR* Prompt;
	};

	TArray<FChip> GetChips()
	{
		return {
			{ LOCTEXT("ChipSummary", "STAGE SUMMARY"), TEXT("status") },
			{ LOCTEXT("ChipPower", "POWER DRAW"), TEXT("power draw") },
			{ LOCTEXT("ChipWeight", "WEIGHT"), TEXT("total weight") },
			{ LOCTEXT("ChipFixtures", "FIXTURES"), TEXT("how many fixtures") },
			{ LOCTEXT("ChipCues", "CUES"), TEXT("list cues") },
			{ LOCTEXT("ChipHelp", "HELP"), TEXT("help") },
		};
	}
}

const UStageCraftUITheme& UStageAIAssistantWidget::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

void UStageAIAssistantWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
}

UTextBlock* UStageAIAssistantWidget::MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color) const
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
	Block->SetColorAndOpacity(FSlateColor(Color));
	return Block;
}

UStageChoiceButton* UStageAIAssistantWidget::MakeButton(const FText& Label, const FText& Tip, int32 Index, void (UStageAIAssistantWidget::*Handler)(int32), bool bPrimary)
{
	UStageChoiceButton* Button = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
	Button->SetChoiceIndex(Index);
	Button->SetToolTipText(Tip);
	Button->OnPicked.BindUObject(this, Handler);
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Button->AddChild(MakeText(Label, 8, TEXT("Bold"), GetTheme().TextSecondary))))
	{
		LabelSlot->SetPadding(FMargin(8.f, 3.f));
		LabelSlot->SetHorizontalAlignment(HAlign_Center);
		LabelSlot->SetVerticalAlignment(VAlign_Center);
	}
	StyleButton(*Button, bPrimary, true);
	return Button;
}

void UStageAIAssistantWidget::StyleButton(UStageChoiceButton& Button, bool bPrimary, bool bEnabled) const
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
	FButtonStyle Style = bPrimary
		? MakeFlatButton(Palette.AccentColor, Palette.AccentColor * 1.1f, Palette.AccentColor * 0.85f)
		: MakeFlatButton(FLinearColor::Transparent, Palette.RowHover, Palette.Divider);
	if (!bPrimary)
	{
		Style.SetNormal(MakeBox(FLinearColor::Transparent, CornerRadius, Palette.Divider, 1.f));
	}
	Button.SetStyle(Style);
	Button.SetIsEnabled(bEnabled);
	if (UTextBlock* Label = Cast<UTextBlock>(Button.GetContent()))
	{
		Label->SetColorAndOpacity(FSlateColor(!bEnabled ? Palette.TextDisabled : bPrimary ? Palette.WindowBackground : Palette.TextSecondary));
	}
}

UWidget* UStageAIAssistantWidget::BuildHeader()
{
	const UStageCraftUITheme& Palette = GetTheme();
	UBorder* Header = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Header"));
	Header->SetBrush(StageCraftWidgetStyle::MakeBox(Palette.PanelHeader));
	Header->SetPadding(FMargin(10.f, 6.f));
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	UTextBlock* Title = MakeText(LOCTEXT("Title", "AI ASSISTANT"), 9, TEXT("Bold"), Palette.TextPrimary);
	Title->SetToolTipText(LOCTEXT("TitleTip", "Ask about the stage or ask for a change. Every change goes through the stage's rules and Ctrl+Z undoes it."));
	if (UHorizontalBoxSlot* TitleSlot = Row->AddChildToHorizontalBox(Title))
	{
		TitleSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		TitleSlot->SetVerticalAlignment(VAlign_Center);
	}
	ClearButton = MakeButton(LOCTEXT("Clear", "CLEAR"), LOCTEXT("ClearTip", "Clear the conversation (the stage is not changed)."), 0, &ThisClass::HandleClearPicked, false);
	Row->AddChildToHorizontalBox(ClearButton)->SetVerticalAlignment(VAlign_Center);
	Header->SetContent(Row);
	return Header;
}

UWidget* UStageAIAssistantWidget::BuildChips()
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("Chips"));
	const TArray<StageAIAssistantWidget::FChip> Chips = StageAIAssistantWidget::GetChips();
	ChipButtons.Reset(Chips.Num());
	for (int32 Index = 0; Index < Chips.Num(); ++Index)
	{
		UStageChoiceButton* Chip = MakeButton(Chips[Index].Label, FText::Format(LOCTEXT("ChipTip", "Ask \"{0}\"."), FText::FromString(Chips[Index].Prompt)),
			Index, &ThisClass::HandleChipPicked, false);
		if (UHorizontalBoxSlot* ChipSlot = Row->AddChildToHorizontalBox(Chip))
		{
			ChipSlot->SetPadding(FMargin(Index == 0 ? 0.f : StageAIAssistantWidget::ChipGap, 0.f, 0.f, 0.f));
		}
		ChipButtons.Add(Chip);
	}
	return Row;
}

UWidget* UStageAIAssistantWidget::BuildInputRow()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("InputRow"));

	PromptField = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass(), TEXT("Prompt"));
	FEditableTextBoxStyle TextStyle = MakeInputField(Palette);
	TextStyle.SetPadding(FMargin(FieldTextInset, 4.f));
	PromptField->SetWidgetStyle(TextStyle);
	PromptField->SetHintText(LOCTEXT("PromptHint", "Ask about the stage, or say what to do: \"add 4 moving heads\", \"make it rain\", \"store cue 1\""));
	PromptField->SetClearKeyboardFocusOnCommit(false);
	PromptField->SetSelectAllTextWhenFocused(false);
	PromptField->OnTextCommitted.AddUniqueDynamic(this, &ThisClass::HandleTextCommitted);
	USizeBox* FieldBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	FieldBox->SetHeightOverride(FieldHeight + 6.f);
	FieldBox->SetContent(PromptField);
	if (UHorizontalBoxSlot* FieldSlot = Row->AddChildToHorizontalBox(FieldBox))
	{
		FieldSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		FieldSlot->SetVerticalAlignment(VAlign_Center);
	}

	SendButton = MakeButton(LOCTEXT("Send", "SEND"), LOCTEXT("SendTip", "Send the request (Enter)."), 0, &ThisClass::HandleSendPicked, true);
	if (UHorizontalBoxSlot* SendSlot = Row->AddChildToHorizontalBox(SendButton))
	{
		SendSlot->SetPadding(FMargin(6.f, 0.f, 0.f, 0.f));
		SendSlot->SetVerticalAlignment(VAlign_Fill);
	}
	return Row;
}

void UStageAIAssistantWidget::BuildTree()
{
	using namespace StageCraftWidgetStyle;
	using namespace StageAIAssistantWidget;
	const UStageCraftUITheme& Palette = GetTheme();

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

	// The conversation.
	UBorder* ConversationFrame = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("ConversationFrame"));
	ConversationFrame->SetBrush(MakeBox(Palette.WindowBackground, CornerRadius, Palette.Divider, 1.f));
	ConversationFrame->SetPadding(FMargin(8.f));
	MessageScroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("MessageScroll"));
	MessageScroll->SetWidgetBarStyle(MakeScrollBar(Palette));
	MessageList = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("MessageList"));
	MessageScroll->AddChild(MessageList);
	ConversationFrame->SetContent(MessageScroll);
	USizeBox* ConversationMin = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("ConversationMinimum"));
	ConversationMin->SetMinDesiredHeight(220.f);
	ConversationMin->SetMinDesiredWidth(320.f);
	ConversationMin->SetContent(ConversationFrame);
	Add(ConversationMin, /*bFill*/ true);

	Add(BuildChips());
	Add(BuildInputRow());

	FooterText = MakeText(FText::GetEmpty(), 7, TEXT("Italic"), Palette.TextSecondary);
	FooterText->SetAutoWrapText(true);
	Add(FooterText);
}

UWidget* UStageAIAssistantWidget::BuildMessage(const FStageAIMessage& Message) const
{
	using namespace StageCraftWidgetStyle;
	using namespace StageAIAssistantWidget;
	const UStageCraftUITheme& Palette = GetTheme();
	const bool bUser = Message.Speaker == EStageAISpeaker::User;

	FText Caption = LOCTEXT("CaptionAssistant", "ASSISTANT");
	FLinearColor CaptionColor = Palette.TextSecondary;
	if (bUser)
	{
		Caption = LOCTEXT("CaptionUser", "YOU");
	}
	else if (Message.Outcome == EStageAIOutcome::Succeeded)
	{
		Caption = LOCTEXT("CaptionDone", "ASSISTANT · DONE");
		CaptionColor = Palette.AccentColor;
	}
	else if (Message.Outcome == EStageAIOutcome::Refused)
	{
		Caption = LOCTEXT("CaptionRefused", "ASSISTANT · NOT DONE");
		CaptionColor = Palette.WarningColor;
	}
	else if (Message.Outcome == EStageAIOutcome::Pending)
	{
		Caption = LOCTEXT("CaptionPending", "ASSISTANT · WORKING");
	}

	UVerticalBox* Content = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Content->AddChildToVerticalBox(MakeText(Caption, 7, TEXT("Bold"), CaptionColor));
	UTextBlock* Body = MakeText(Message.Text, 9, TEXT("Regular"), Palette.TextPrimary);
	Body->SetAutoWrapText(true);
	if (UVerticalBoxSlot* BodySlot = Content->AddChildToVerticalBox(Body))
	{
		BodySlot->SetPadding(FMargin(0.f, 2.f, 0.f, 0.f));
	}

	UBorder* Bubble = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	const FLinearColor Fill = bUser ? WithAlpha(Palette.AccentColor, 0.18f) : Palette.RowBackground;
	const FLinearColor Outline = !bUser && Message.Outcome == EStageAIOutcome::Refused ? WithAlpha(Palette.WarningColor, 0.6f) : Palette.Divider;
	Bubble->SetBrush(MakeBox(Fill, BubbleRadius, Outline, 1.f));
	Bubble->SetPadding(FMargin(9.f, 6.f));
	Bubble->SetContent(Content);

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	const auto AddInset = [this, Row]()
	{
		USpacer* Inset = WidgetTree->ConstructWidget<USpacer>(USpacer::StaticClass());
		Inset->SetSize(FVector2D(BubbleInset, 1.f));
		Row->AddChildToHorizontalBox(Inset);
	};
	if (bUser)
	{
		AddInset();
	}
	if (UHorizontalBoxSlot* BubbleSlot = Row->AddChildToHorizontalBox(Bubble))
	{
		BubbleSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		BubbleSlot->SetHorizontalAlignment(bUser ? HAlign_Right : HAlign_Left);
	}
	if (!bUser)
	{
		AddInset();
	}
	return Row;
}

void UStageAIAssistantWidget::NativeConstruct()
{
	Super::NativeConstruct();
	if (UStageAICommandSubsystem* Assistant = UWorld::GetSubsystem<UStageAICommandSubsystem>(GetWorld()))
	{
		BoundAssistant = Assistant;
		Assistant->OnConversationChanged.AddUniqueDynamic(this, &ThisClass::HandleConversationChanged);
	}
	Rebuild();
}

void UStageAIAssistantWidget::NativeDestruct()
{
	if (UStageAICommandSubsystem* Assistant = BoundAssistant.Get())
	{
		Assistant->OnConversationChanged.RemoveDynamic(this, &ThisClass::HandleConversationChanged);
	}
	BoundAssistant.Reset();
	bRebuildQueued = false;
	Super::NativeDestruct();
}

void UStageAIAssistantWidget::HandleConversationChanged()
{
	RequestRebuild();
}

void UStageAIAssistantWidget::RequestRebuild()
{
	UWorld* World = GetWorld();
	if (bRebuildQueued || !World)
	{
		return;
	}
	bRebuildQueued = true;
	World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
	{
		bRebuildQueued = false;
		Rebuild();
	}));
}

void UStageAIAssistantWidget::Rebuild()
{
	const UStageAICommandSubsystem* Assistant = GetAssistant();
	const UStageCraftUITheme& Palette = GetTheme();
	const bool bBusy = Assistant && Assistant->IsBusy();

	if (MessageList)
	{
		MessageList->ClearChildren();
		const auto AddRow = [this](UWidget* Widget)
		{
			if (UVerticalBoxSlot* RowSlot = MessageList->AddChildToVerticalBox(Widget))
			{
				RowSlot->SetPadding(FMargin(0.f, 3.f));
			}
		};
		if (!Assistant || Assistant->GetMessages().IsEmpty())
		{
			UTextBlock* Welcome = MakeText(Assistant
				? LOCTEXT("Welcome", "Ask me about the stage (\"how heavy is the stage?\", \"power draw\", \"list cues\") or tell me what to do (\"add 6 moving heads\", \"make it rain\", \"store cue 1 called Intro\"). Type \"help\" for everything I understand.")
				: LOCTEXT("NoAssistant", "The AI assistant is not available in this level."), 9, TEXT("Italic"), Palette.TextSecondary);
			Welcome->SetAutoWrapText(true);
			AddRow(Welcome);
		}
		else
		{
			for (const FStageAIMessage& Message : Assistant->GetMessages())
			{
				AddRow(BuildMessage(Message));
			}
		}
		if (Assistant && Assistant->GetState() == EStageAIAssistantState::Interpreting)
		{
			AddRow(MakeText(LOCTEXT("Thinking", "Thinking…"), 8, TEXT("Italic"), Palette.TextSecondary));
		}
	}
	if (MessageScroll)
	{
		MessageScroll->ScrollToEnd();
		// New rows have no size until Slate lays them out this frame, so the end moves once more: scroll again next tick.
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
			{
				if (MessageScroll)
				{
					MessageScroll->ScrollToEnd();
				}
			}));
		}
	}

	if (PromptField)
	{
		PromptField->SetIsReadOnly(bBusy || !Assistant);
	}
	if (SendButton)
	{
		StyleButton(*SendButton, true, !bBusy && Assistant);
	}
	if (ClearButton)
	{
		StyleButton(*ClearButton, false, !bBusy && Assistant && !Assistant->GetMessages().IsEmpty());
	}
	for (UStageChoiceButton* Chip : ChipButtons)
	{
		StyleButton(*Chip, false, !bBusy && Assistant);
	}
	if (FooterText)
	{
		FooterText->SetText(Assistant ? Assistant->GetInterpreterDescription() : FText::GetEmpty());
	}
}

bool UStageAIAssistantWidget::SendPrompt(const FString& Prompt)
{
	AModularPlayerController* Controller = Cast<AModularPlayerController>(GetOwningPlayer());
	return Controller && Controller->RequestAssistantPrompt(Prompt.Left(StageAICommandParser::MaxPromptLength));
}

void UStageAIAssistantWidget::HandleChipPicked(int32 Index)
{
	const TArray<StageAIAssistantWidget::FChip> Chips = StageAIAssistantWidget::GetChips();
	if (Chips.IsValidIndex(Index))
	{
		SendPrompt(Chips[Index].Prompt);
	}
}

void UStageAIAssistantWidget::HandleSendPicked(int32 Unused)
{
	if (PromptField && SendPrompt(PromptField->GetText().ToString()))
	{
		PromptField->SetText(FText::GetEmpty());
	}
}

void UStageAIAssistantWidget::HandleClearPicked(int32 Unused)
{
	if (UStageAICommandSubsystem* Assistant = GetAssistant())
	{
		Assistant->ClearConversation();
	}
}

void UStageAIAssistantWidget::HandleTextCommitted(const FText& Text, ETextCommit::Type CommitMethod)
{
	if (CommitMethod == ETextCommit::OnEnter && PromptField && SendPrompt(Text.ToString()))
	{
		PromptField->SetText(FText::GetEmpty());
	}
}

#undef LOCTEXT_NAMESPACE
