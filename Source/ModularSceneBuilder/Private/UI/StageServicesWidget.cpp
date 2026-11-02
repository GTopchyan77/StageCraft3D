// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageServicesWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Bridge/StageBridgeProtocol.h"
#include "Bridge/StageBridgeSubsystem.h"
#include "Components/Border.h"
#include "Components/ButtonSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Player/ModularPlayerController.h"
#include "Services/StageAIToolSubsystem.h"
#include "Services/StageCredentialVault.h"
#include "TimerManager.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageToolButton.h"
#include "Workspace/StageWorkspaceSubsystem.h"
#include "Workspace/StageWorkspaceTypes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageServicesWidget)

#define LOCTEXT_NAMESPACE "StageServicesWidget"

namespace StageServicesPanel
{
	constexpr float GroupGap = 14.f;
	constexpr int32 ActionCount = 3;
}

// --- Construction ---

const UStageCraftUITheme& UStageServicesWidget::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

AModularPlayerController* UStageServicesWidget::GetStageController() const
{
	return Cast<AModularPlayerController>(GetOwningPlayer());
}

void UStageServicesWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
}

UTextBlock* UStageServicesWidget::MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
	Block->SetColorAndOpacity(FSlateColor(Color));
	return Block;
}

UStageChoiceButton* UStageServicesWidget::MakeButton(const FText& Label, const FText& Tip, int32 Index, void (UStageServicesWidget::*Handler)(int32))
{
	UStageChoiceButton* Button = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
	Button->SetChoiceIndex(Index);
	Button->SetToolTipText(Tip);
	Button->OnPicked.BindUObject(this, Handler);
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Button->AddChild(MakeText(Label, 8, TEXT("Bold"), GetTheme().TextSecondary))))
	{
		LabelSlot->SetPadding(FMargin(8.f, 3.f));
	}
	StyleChoice(*Button);
	return Button;
}

void UStageServicesWidget::StyleChoice(UStageChoiceButton& Button) const
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
	FButtonStyle Style = MakeFlatButton(FLinearColor::Transparent, Palette.RowHover, Palette.Divider);
	Style.SetNormal(MakeBox(FLinearColor::Transparent, CornerRadius, Palette.Divider, 1.f));
	Button.SetStyle(Style);
	if (UTextBlock* Label = Cast<UTextBlock>(Button.GetContent()))
	{
		Label->SetColorAndOpacity(FSlateColor(Palette.TextSecondary));
	}
}

void UStageServicesWidget::BuildTree()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Root = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Root"));
	Root->SetBrush(MakeBox(Palette.PanelBackground));
	Root->SetPadding(FMargin(0.f));
	WidgetTree->RootWidget = Root;

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Root->SetContent(Column);

	UBorder* Header = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Header->SetBrush(MakeBox(Palette.PanelHeader));
	Header->SetPadding(FMargin(10.f, 6.f));
	Header->SetContent(MakeText(LOCTEXT("Title", "SERVICES & KEYS"), 10, TEXT("Bold"), Palette.AccentColor));
	Column->AddChildToVerticalBox(Header);

	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	Scroll->SetWidgetBarStyle(MakeScrollBar(Palette));
	if (UVerticalBoxSlot* ScrollSlot = Column->AddChildToVerticalBox(Scroll))
	{
		ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		ScrollSlot->SetPadding(FMargin(12.f, 10.f));
	}
	UVerticalBox* Body = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Scroll->AddChild(Body);
	const auto Add = [Body](UWidget* Widget, float Top = 4.f) { Body->AddChildToVerticalBox(Widget)->SetPadding(FMargin(0.f, Top, 0.f, 0.f)); };

	Add(MakeText(LOCTEXT("ToolsHeading", "AI TOOLS"), 8, TEXT("Bold"), Palette.TextSecondary), 0.f);
	UTextBlock* ToolsIntro = MakeText(LOCTEXT("ToolsIntro", "A key is stored encrypted for your Windows user and is never shown again. It is sent only to its own tool, when you use that tool."),
		8, TEXT("Regular"), Palette.TextSecondary);
	ToolsIntro->SetAutoWrapText(true);
	Add(ToolsIntro);
	ToolList = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("ToolList"));
	Add(ToolList, 6.f);

	Add(MakeText(LOCTEXT("AppsHeading", "3D APPS"), 8, TEXT("Bold"), Palette.TextSecondary), StageServicesPanel::GroupGap);
	UTextBlock* AppsIntro = MakeText(LOCTEXT("AppsIntro", "Maya, 3ds Max, Cinema 4D and Blender licences are managed by those apps. StageCraft lists an app when its StageCraft add-on connects over the bridge."),
		8, TEXT("Regular"), Palette.TextSecondary);
	AppsIntro->SetAutoWrapText(true);
	Add(AppsIntro);
	AppList = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("AppList"));
	Add(AppList, 6.f);
	Add(MakeButton(LOCTEXT("OpenSendTo", "SEND TO 3D APP..."), LOCTEXT("OpenSendToTip", "Switch the bridge on or off and send your own stages."), 0, &ThisClass::HandleOpenSendTo), 6.f);

	StatusText = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextSecondary);
	StatusText->SetAutoWrapText(true);
	Add(StatusText, StageServicesPanel::GroupGap);
}

// --- Lifetime ---

void UStageServicesWidget::NativeConstruct()
{
	Super::NativeConstruct();
	UGameInstance* GameInstance = GetGameInstance();
	if (UStageAIToolSubsystem* Tools = GameInstance ? GameInstance->GetSubsystem<UStageAIToolSubsystem>() : nullptr)
	{
		BoundTools = Tools;
		Tools->OnToolsChanged.AddUniqueDynamic(this, &ThisClass::HandleToolsChanged);
		Tools->OnKeyVerified.AddUniqueDynamic(this, &ThisClass::HandleKeyVerified);
		Tools->OnJobCompleted.AddUniqueDynamic(this, &ThisClass::HandleJobCompleted);
	}
	if (UStageBridgeSubsystem* Bridge = GameInstance ? GameInstance->GetSubsystem<UStageBridgeSubsystem>() : nullptr)
	{
		BoundBridge = Bridge;
		Bridge->OnStateChanged.AddUniqueDynamic(this, &ThisClass::HandleBridgeStateChanged);
		Bridge->OnClientsChanged.AddUniqueDynamic(this, &ThisClass::HandleToolsChanged);
	}
	RefreshTools();
	RebuildApps();
}

void UStageServicesWidget::NativeDestruct()
{
	if (UStageAIToolSubsystem* Tools = BoundTools.Get())
	{
		Tools->OnToolsChanged.RemoveDynamic(this, &ThisClass::HandleToolsChanged);
		Tools->OnKeyVerified.RemoveDynamic(this, &ThisClass::HandleKeyVerified);
		Tools->OnJobCompleted.RemoveDynamic(this, &ThisClass::HandleJobCompleted);
	}
	if (UStageBridgeSubsystem* Bridge = BoundBridge.Get())
	{
		Bridge->OnStateChanged.RemoveDynamic(this, &ThisClass::HandleBridgeStateChanged);
		Bridge->OnClientsChanged.RemoveDynamic(this, &ThisClass::HandleToolsChanged);
	}
	if (const UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RebuildTimer);
	}
	// Whatever is still typed in a field is dropped with the panel.
	for (UEditableTextBox* Field : KeyFields)
	{
		if (Field)
		{
			Field->SetText(FText::GetEmpty());
		}
	}
	BoundTools.Reset();
	BoundBridge.Reset();
	Super::NativeDestruct();
}

// --- Rows ---

void UStageServicesWidget::RequestRebuild()
{
	UWorld* World = GetWorld();
	if (World && !RebuildTimer.IsValid())
	{
		RebuildTimer = World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
		{
			RebuildTimer.Invalidate();
			RefreshTools();
			RebuildApps();
		}));
	}
}

FText UStageServicesWidget::DescribeKeyState(EStageAIToolKeyState State) const
{
	switch (State)
	{
	case EStageAIToolKeyState::NotNeeded:	return LOCTEXT("KeyNotNeeded", "No key needed");
	case EStageAIToolKeyState::Saved:		return LOCTEXT("KeySaved", "Key saved, not checked yet");
	case EStageAIToolKeyState::Verified:	return LOCTEXT("KeyVerified", "Key accepted by the service");
	case EStageAIToolKeyState::Rejected:	return LOCTEXT("KeyRejected", "Key refused by the service: save a new one");
	case EStageAIToolKeyState::Missing:
	default:								return LOCTEXT("KeyMissing", "No key saved");
	}
}

void UStageServicesWidget::RefreshTools()
{
	const UStageAIToolSubsystem* Tools = BoundTools.Get();
	const TArray<FStageAIToolInfo> Infos = Tools ? Tools->GetTools() : TArray<FStageAIToolInfo>();

	TArray<FName> Ids;
	Ids.Reserve(Infos.Num());
	for (const FStageAIToolInfo& Info : Infos)
	{
		Ids.Add(Info.ProviderId);
	}
	// Same tools as shown: update in place. Rebuilding would destroy the fields, and with them a key the user is typing; copying
	// that text across a rebuild would put the key in more unwiped memory (FText cannot be wiped).
	if (Ids == RowIds && !Ids.IsEmpty())
	{
		for (int32 Row = 0; Row < Infos.Num(); ++Row)
		{
			UpdateToolCard(Row, Infos[Row]);
		}
		return;
	}
	RebuildToolCards(Infos);
}

void UStageServicesWidget::UpdateToolCard(int32 Row, const FStageAIToolInfo& Info)
{
	if (!StateLabels.IsValidIndex(Row) || !SaveButtons.IsValidIndex(Row) || !CheckButtons.IsValidIndex(Row) || !RemoveButtons.IsValidIndex(Row))
	{
		return;
	}
	const UStageCraftUITheme& Palette = GetTheme();
	const UStageAIToolSubsystem* Tools = BoundTools.Get();
	const bool bProblem = Info.KeyState == EStageAIToolKeyState::Missing || Info.KeyState == EStageAIToolKeyState::Rejected;
	StateLabels[Row]->SetText(DescribeKeyState(Info.KeyState));
	StateLabels[Row]->SetColorAndOpacity(FSlateColor(bProblem ? Palette.WarningColor : Palette.AccentColor));

	const bool bHasKey = Info.KeyState != EStageAIToolKeyState::Missing && Info.KeyState != EStageAIToolKeyState::NotNeeded;
	SaveButtons[Row]->SetIsEnabled(Info.KeyState != EStageAIToolKeyState::NotNeeded && Tools && Tools->IsKeyStorageAvailable());
	CheckButtons[Row]->SetIsEnabled(bHasKey && Tools && !Tools->IsBusy());
	RemoveButtons[Row]->SetIsEnabled(bHasKey);
}

void UStageServicesWidget::RebuildToolCards(const TArray<FStageAIToolInfo>& Infos)
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
	const UStageAIToolSubsystem* Tools = BoundTools.Get();

	ToolList->ClearChildren();
	KeyFields.Reset();
	StateLabels.Reset();
	SaveButtons.Reset();
	CheckButtons.Reset();
	RemoveButtons.Reset();
	RowIds.Reset();
	if (Infos.IsEmpty())
	{
		UTextBlock* None = MakeText(LOCTEXT("NoTools", "No AI tools are set up in this build. A tool appears here once it is configured (ADR 0023)."), 8, TEXT("Italic"), Palette.TextSecondary);
		None->SetAutoWrapText(true);
		ToolList->AddChildToVerticalBox(None);
		return;
	}
	if (Tools && !Tools->IsKeyStorageAvailable())
	{
		UTextBlock* NoVault = MakeText(LOCTEXT("NoVault", "Keys cannot be stored safely on this system, so none can be saved."), 8, TEXT("Bold"), Palette.WarningColor);
		NoVault->SetAutoWrapText(true);
		ToolList->AddChildToVerticalBox(NoVault);
	}

	for (int32 Row = 0; Row < Infos.Num(); ++Row)
	{
		const FStageAIToolInfo& Info = Infos[Row];
		RowIds.Add(Info.ProviderId);

		UBorder* Card = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Card->SetBrush(MakeBox(Palette.RowBackground, CornerRadius, Palette.Divider, 1.f));
		Card->SetPadding(FMargin(8.f, 6.f));
		UVerticalBox* Content = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
		Card->SetContent(Content);

		UHorizontalBox* TitleRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		TitleRow->AddChildToHorizontalBox(MakeText(Info.DisplayName, 9, TEXT("Bold"), Palette.TextPrimary))->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		UTextBlock* StateLabel = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.AccentColor);
		TitleRow->AddChildToHorizontalBox(StateLabel)->SetVerticalAlignment(VAlign_Center);
		Content->AddChildToVerticalBox(TitleRow);
		StateLabels.Add(StateLabel);

		UEditableTextBox* Field = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass());
		Field->SetWidgetStyle(MakeInputField(Palette));
		Field->SetIsPassword(true);
		Field->SetHintText(Info.KeyState == EStageAIToolKeyState::NotNeeded ? LOCTEXT("NoKeyHint", "This tool needs no key")
			: LOCTEXT("KeyHint", "Paste a new API key, then SAVE"));
		Field->SetIsEnabled(Info.KeyState != EStageAIToolKeyState::NotNeeded);
		KeyFields.Add(Field);

		const int32 Base = Row * StageServicesPanel::ActionCount;
		UStageChoiceButton* Save = MakeButton(LOCTEXT("Save", "SAVE"), LOCTEXT("SaveTip", "Encrypt and store the key for your Windows user."), Base + int32(EToolAction::Save), &ThisClass::HandleToolAction);
		UStageChoiceButton* Check = MakeButton(LOCTEXT("Check", "CHECK"), LOCTEXT("CheckTip", "Ask the service whether it accepts the saved key."), Base + int32(EToolAction::Check), &ThisClass::HandleToolAction);
		UStageChoiceButton* Remove = MakeButton(LOCTEXT("Remove", "REMOVE"), LOCTEXT("RemoveTip", "Delete the saved key from this computer."), Base + int32(EToolAction::Remove), &ThisClass::HandleToolAction);
		SaveButtons.Add(Save);
		CheckButtons.Add(Check);
		RemoveButtons.Add(Remove);
		UpdateToolCard(Row, Info);

		UHorizontalBox* KeyRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		KeyRow->AddChildToHorizontalBox(Field)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		for (UStageChoiceButton* Button : { Save, Check, Remove })
		{
			KeyRow->AddChildToHorizontalBox(Button)->SetPadding(FMargin(4.f, 0.f, 0.f, 0.f));
		}
		Content->AddChildToVerticalBox(KeyRow)->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
		ToolList->AddChildToVerticalBox(Card)->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
	}
}

void UStageServicesWidget::RebuildApps()
{
	const UStageCraftUITheme& Palette = GetTheme();
	const UStageBridgeSubsystem* Bridge = BoundBridge.Get();
	AppList->ClearChildren();
	if (!Bridge || !Bridge->IsRunning())
	{
		AppList->AddChildToVerticalBox(MakeText(LOCTEXT("BridgeOff", "The bridge is off: no 3D app can connect."), 8, TEXT("Italic"), Palette.TextSecondary));
		return;
	}
	if (Bridge->GetClients().IsEmpty())
	{
		AppList->AddChildToVerticalBox(MakeText(FText::Format(LOCTEXT("BridgeWaiting", "The bridge is on (port {0}); no app has connected yet."), Bridge->GetPort()),
			8, TEXT("Italic"), Palette.TextSecondary));
		return;
	}
	for (const FStageBridgeClientInfo& Client : Bridge->GetClients())
	{
		AppList->AddChildToVerticalBox(MakeText(FText::Format(LOCTEXT("AppRow", "{0} {1}  ·  {2}"), StageBridgeProtocol::GetAppDisplayName(Client.App),
			FText::FromString(Client.AppVersion), Client.bImportsGlb ? LOCTEXT("AppGlb", "receives glTF") : LOCTEXT("AppUsd", "needs USD (coming)")),
			8, TEXT("Regular"), Client.bImportsGlb ? Palette.TextPrimary : Palette.TextSecondary));
	}
}

void UStageServicesWidget::SetStatus(const FText& Message, bool bWarning)
{
	StatusText->SetText(Message);
	StatusText->SetColorAndOpacity(FSlateColor(bWarning ? GetTheme().WarningColor : GetTheme().TextSecondary));
}

// --- Actions ---

void UStageServicesWidget::HandleToolAction(int32 Index)
{
	AModularPlayerController* Controller = GetStageController();
	const int32 Row = Index / StageServicesPanel::ActionCount;
	if (!Controller || !RowIds.IsValidIndex(Row) || !KeyFields.IsValidIndex(Row))
	{
		return;
	}
	const FName Id = RowIds[Row];
	TArray<FStageEconomyResultInfo> Refusals;
	switch (static_cast<EToolAction>(Index % StageServicesPanel::ActionCount))
	{
	case EToolAction::Save:
	{
		FString Key = KeyFields[Row]->GetText().ToString();
		// The field is cleared whatever happens: a key is typed once.
		KeyFields[Row]->SetText(FText::GetEmpty());
		Refusals = Controller->RunCapturingRejections([&Controller, Id, &Key]() { Controller->RequestStoreProviderKey(Id, Key); });
		FStageCredentialVault::WipeString(Key);
		SetStatus(Refusals.IsEmpty() ? LOCTEXT("Saved", "Key saved. Press CHECK to test it with the service.") : Refusals[0].Message, !Refusals.IsEmpty());
		break;
	}
	case EToolAction::Check:
		// Shown first: a tool may answer inside the request, and its answer (HandleKeyVerified) must not be overwritten.
		SetStatus(LOCTEXT("Checking", "Checking the key with the service..."), false);
		Refusals = Controller->RunCapturingRejections([&Controller, Id]() { Controller->RequestVerifyProviderKey(Id); });
		if (!Refusals.IsEmpty())
		{
			SetStatus(Refusals[0].Message, true);
		}
		break;
	case EToolAction::Remove:
		Refusals = Controller->RunCapturingRejections([&Controller, Id]() { Controller->RequestRemoveProviderKey(Id); });
		SetStatus(Refusals.IsEmpty() ? LOCTEXT("Removed", "Key removed from this computer.") : Refusals[0].Message, !Refusals.IsEmpty());
		break;
	default:
		break;
	}
	RequestRebuild();
}

void UStageServicesWidget::HandleOpenSendTo(int32 Unused)
{
	if (UStageWorkspaceSubsystem* Workspace = GetGameInstance() ? GetGameInstance()->GetSubsystem<UStageWorkspaceSubsystem>() : nullptr)
	{
		Workspace->OpenPanel(FGameplayTag(StageCraftTags::Panel_SendTo));
	}
}

void UStageServicesWidget::HandleToolsChanged()
{
	RequestRebuild();
}

void UStageServicesWidget::HandleBridgeStateChanged(bool bRunning)
{
	RequestRebuild();
}

void UStageServicesWidget::HandleKeyVerified(const FStageAIToolResultInfo& Result)
{
	SetStatus(Result.Message, !Result.IsSuccess());
	RequestRebuild();
}

void UStageServicesWidget::HandleJobCompleted(const FStageAIToolResultInfo& Result)
{
	RequestRebuild();
}

#undef LOCTEXT_NAMESPACE
