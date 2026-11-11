// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageGdtfSharePanel.h"

#include "Blueprint/WidgetTree.h"
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
#include "Fixtures/StageFixtureLibrarySubsystem.h"
#include "Fixtures/StageGdtfShareProtocol.h"
#include "Fixtures/StageGdtfShareSubsystem.h"
#include "Player/ModularPlayerController.h"
#include "TimerManager.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageToolButton.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageGdtfSharePanel)

#define LOCTEXT_NAMESPACE "StageGdtfSharePanel"

namespace StageGdtfSharePanelText
{
	FText Uploader(EStageGdtfShareUploader Uploader)
	{
		switch (Uploader)
		{
		case EStageGdtfShareUploader::Manufacturer: return LOCTEXT("UploaderManufacturer", "MANUFACTURER FILE");
		case EStageGdtfShareUploader::User: return LOCTEXT("UploaderUser", "USER FILE");
		default: return LOCTEXT("UploaderOther", "OTHER UPLOADER");
		}
	}

	FText Size(int64 Bytes)
	{
		return Bytes > 0 ? FText::AsMemory(Bytes) : FText::GetEmpty();
	}
}

const UStageCraftUITheme& UStageGdtfSharePanel::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

UStageGdtfShareSubsystem* UStageGdtfSharePanel::GetShare() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UStageGdtfShareSubsystem>() : nullptr;
}

UStageFixtureLibrarySubsystem* UStageGdtfSharePanel::GetLibrary() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UStageFixtureLibrarySubsystem>() : nullptr;
}

AModularPlayerController* UStageGdtfSharePanel::GetStageController() const
{
	return Cast<AModularPlayerController>(GetOwningPlayer());
}

UTextBlock* UStageGdtfSharePanel::MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color, bool bWrap)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
	Block->SetColorAndOpacity(FSlateColor(Color));
	Block->SetAutoWrapText(bWrap);
	return Block;
}

UEditableTextBox* UStageGdtfSharePanel::MakeField(const FText& Hint)
{
	UEditableTextBox* Field = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass());
	Field->SetWidgetStyle(StageCraftWidgetStyle::MakeInputField(GetTheme()));
	Field->SetHintText(Hint);
	return Field;
}

void UStageGdtfSharePanel::StyleButton(UStageChoiceButton& Button, bool bPrimary, bool bEnabled) const
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
	Style.SetDisabled(MakeBox(FLinearColor::Transparent, CornerRadius, Palette.Divider, 1.f));
	Button.SetStyle(Style);
	Button.SetIsEnabled(bEnabled);
	if (UTextBlock* Label = Cast<UTextBlock>(Button.GetContent()))
	{
		Label->SetColorAndOpacity(FSlateColor(bPrimary && bEnabled ? Palette.WindowBackground : (bEnabled ? Palette.TextSecondary : Palette.TextDisabled)));
	}
}

UStageChoiceButton* UStageGdtfSharePanel::MakeButton(const FText& Label, const FText& Tip, int32 Index, bool bPrimary)
{
	UStageChoiceButton* Button = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
	Button->SetChoiceIndex(Index);
	Button->SetToolTipText(Tip);
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Button->AddChild(MakeText(Label, 7, TEXT("Bold"), GetTheme().TextSecondary))))
	{
		LabelSlot->SetPadding(FMargin(8.f, 3.f));
	}
	StyleButton(*Button, bPrimary, true);
	return Button;
}

void UStageGdtfSharePanel::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
}

void UStageGdtfSharePanel::BuildTree()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
	ActionButtons.SetNum(int32(EAction::Count));
	const auto Action = [this](const FText& Label, const FText& Tip, EAction Which, bool bPrimary)
	{
		UStageChoiceButton* Button = MakeButton(Label, Tip, int32(Which), bPrimary);
		Button->OnPicked.BindUObject(this, &ThisClass::HandleAction);
		ActionButtons[int32(Which)] = Button;
		return Button;
	};

	UBorder* Root = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Root"));
	Root->SetBrush(MakeBox(Palette.PanelBackground));
	WidgetTree->RootWidget = Root;
	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	Scroll->SetWidgetBarStyle(MakeScrollBar(Palette));
	Root->SetContent(Scroll);
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Scroll->AddChild(Column);
	const auto Add = [](UVerticalBox* Box, UWidget* Widget, float Top) { Box->AddChildToVerticalBox(Widget)->SetPadding(FMargin(12.f, Top, 12.f, 0.f)); };

	Add(Column, MakeText(LOCTEXT("Title", "GDTF SHARE"), 9, TEXT("Bold"), Palette.TextPrimary), 10.f);
	Add(Column, MakeText(LOCTEXT("Intro", "Get fixture files from gdtf-share.com. Every file is checked and shown in the import report before it is added to your Library."),
		8, TEXT("Regular"), Palette.TextSecondary, true), 4.f);

	SignInBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	UserField = MakeField(LOCTEXT("UserHint", "GDTF Share user name"));
	PasswordField = MakeField(LOCTEXT("PasswordHint", "Password"));
	PasswordField->SetIsPassword(true);
	PasswordField->OnTextCommitted.AddUniqueDynamic(this, &ThisClass::HandlePasswordCommitted);
	SignInBox->AddChildToVerticalBox(UserField);
	SignInBox->AddChildToVerticalBox(PasswordField)->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
	SignInBox->AddChildToVerticalBox(Action(LOCTEXT("SignIn", "SIGN IN"), LOCTEXT("SignInTip", "Sign in with your own GDTF Share account (free at gdtf-share.com)."), EAction::SignIn, true))
		->SetPadding(FMargin(0.f, 6.f, 0.f, 0.f));
	SignInBox->AddChildToVerticalBox(MakeText(LOCTEXT("PasswordNote", "Your password goes only to GDTF Share, over a secure connection. StageCraft never saves it; you sign in again after a restart."),
		7, TEXT("Regular"), Palette.TextSecondary, true))->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
	Add(Column, SignInBox, 10.f);

	AccountBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	UHorizontalBox* AccountRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	AccountText = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextPrimary);
	AccountRow->AddChildToHorizontalBox(AccountText)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	AccountRow->AddChildToHorizontalBox(Action(LOCTEXT("Refresh", "REFRESH"), LOCTEXT("RefreshTip", "Load the GDTF Share list again now."), EAction::Refresh, false));
	AccountRow->AddChildToHorizontalBox(Action(LOCTEXT("SignOut", "SIGN OUT"), LOCTEXT("SignOutTip", "End the GDTF Share session and stop running downloads."), EAction::SignOut, false))
		->SetPadding(FMargin(6.f, 0.f, 0.f, 0.f));
	AccountBox->AddChildToVerticalBox(AccountRow);
	SearchField = MakeField(LOCTEXT("SearchHint", "Search manufacturer, fixture, revision or uploader"));
	SearchField->OnTextChanged.AddUniqueDynamic(this, &ThisClass::HandleSearchChanged);
	AccountBox->AddChildToVerticalBox(SearchField)->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));
	UHorizontalBox* Filters = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Filters->AddChildToHorizontalBox(Action(LOCTEXT("FilterAll", "ALL"), LOCTEXT("FilterAllTip", "Show files from manufacturers and users."), EAction::FilterAll, true));
	Filters->AddChildToHorizontalBox(Action(LOCTEXT("FilterManufacturer", "MANUFACTURER FILES"), LOCTEXT("FilterManufacturerTip", "Only files the manufacturer uploaded."), EAction::FilterManufacturer, false))
		->SetPadding(FMargin(6.f, 0.f, 0.f, 0.f));
	Filters->AddChildToHorizontalBox(Action(LOCTEXT("FilterUser", "USER FILES"), LOCTEXT("FilterUserTip", "Only files other users uploaded."), EAction::FilterUser, false))
		->SetPadding(FMargin(6.f, 0.f, 0.f, 0.f));
	AccountBox->AddChildToVerticalBox(Filters)->SetPadding(FMargin(0.f, 6.f, 0.f, 0.f));
	ResultInfoText = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), Palette.TextSecondary, true);
	AccountBox->AddChildToVerticalBox(ResultInfoText)->SetPadding(FMargin(0.f, 6.f, 0.f, 0.f));
	ResultsBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	AccountBox->AddChildToVerticalBox(ResultsBox)->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
	Add(Column, AccountBox, 10.f);

	UHorizontalBox* DownloadsHeader = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	DownloadsHeader->AddChildToHorizontalBox(MakeText(LOCTEXT("Downloads", "DOWNLOADS"), 8, TEXT("Bold"), Palette.TextPrimary))->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	DownloadsHeader->AddChildToHorizontalBox(Action(LOCTEXT("ClearFinished", "CLEAR FINISHED"), LOCTEXT("ClearFinishedTip", "Remove finished and failed downloads from this list."), EAction::ClearFinished, false));
	Add(Column, DownloadsHeader, 14.f);
	DownloadsBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Add(Column, DownloadsBox, 4.f);

	StatusText = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextSecondary, true);
	Add(Column, StatusText, 10.f);
	UTextBlock* Attribution = MakeText(UStageGdtfShareSubsystem::GetAttribution(), 8, TEXT("Bold"), Palette.TextSecondary);
	Attribution->SetToolTipText(LOCTEXT("AttributionTip", "Fixture files from GDTF Share (gdtf-share.com), the open General Device Type Format library."));
	Column->AddChildToVerticalBox(Attribution)->SetPadding(FMargin(12.f, 10.f, 12.f, 12.f));
}

void UStageGdtfSharePanel::NativeConstruct()
{
	Super::NativeConstruct();
	if (UStageGdtfShareSubsystem* Share = GetShare())
	{
		Share->OnChanged.AddUniqueDynamic(this, &ThisClass::HandleShareChanged);
		Share->OnRequestFinished.AddUniqueDynamic(this, &ThisClass::HandleRequestFinished);
	}
	if (UStageFixtureLibrarySubsystem* Library = GetLibrary())
	{
		Library->OnLibraryChanged.AddUniqueDynamic(this, &ThisClass::HandleLibraryChanged);
	}
	Rebuild();
}

void UStageGdtfSharePanel::NativeDestruct()
{
	if (UStageGdtfShareSubsystem* Share = GetShare())
	{
		Share->OnChanged.RemoveDynamic(this, &ThisClass::HandleShareChanged);
		Share->OnRequestFinished.RemoveDynamic(this, &ThisClass::HandleRequestFinished);
	}
	if (UStageFixtureLibrarySubsystem* Library = GetLibrary())
	{
		Library->OnLibraryChanged.RemoveDynamic(this, &ThisClass::HandleLibraryChanged);
	}
	if (const UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RebuildTimer);
	}
	if (PasswordField)
	{
		PasswordField->SetText(FText::GetEmpty());
	}
	Super::NativeDestruct();
}

void UStageGdtfSharePanel::RequestRebuild()
{
	UWorld* World = GetWorld();
	if (!World || RebuildTimer.IsValid())
	{
		return;
	}
	RebuildTimer = World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ThisClass::Rebuild));
}

void UStageGdtfSharePanel::Rebuild()
{
	RebuildTimer.Invalidate();
	const UStageGdtfShareSubsystem* Share = GetShare();
	if (!Share || !SignInBox || !AccountBox)
	{
		return;
	}
	const EStageGdtfShareState State = Share->GetState();
	SignInBox->SetVisibility(State == EStageGdtfShareState::SignedIn ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
	AccountBox->SetVisibility(State == EStageGdtfShareState::SignedIn ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	if (UStageChoiceButton* SignIn = ActionButtons[int32(EAction::SignIn)])
	{
		StyleButton(*SignIn, true, State == EStageGdtfShareState::SignedOut);
	}
	if (UStageChoiceButton* Refresh = ActionButtons[int32(EAction::Refresh)])
	{
		StyleButton(*Refresh, false, !Share->IsListLoading());
	}
	StyleButton(*ActionButtons[int32(EAction::FilterAll)], !UploaderFilter.IsSet(), true);
	StyleButton(*ActionButtons[int32(EAction::FilterManufacturer)], UploaderFilter == EStageGdtfShareUploader::Manufacturer, true);
	StyleButton(*ActionButtons[int32(EAction::FilterUser)], UploaderFilter == EStageGdtfShareUploader::User, true);
	AccountText->SetText(Share->IsListLoading()
		? FText::Format(LOCTEXT("AccountLoading", "Signed in as {0}. Loading the fixture list..."), FText::FromString(Share->GetUserName()))
		: FText::Format(LOCTEXT("Account", "Signed in as {0}."), FText::FromString(Share->GetUserName())));
	RebuildResults();
	RebuildDownloads();
}

void UStageGdtfSharePanel::RebuildResults()
{
	const UStageGdtfShareSubsystem* Share = GetShare();
	const UStageFixtureLibrarySubsystem* Library = GetLibrary();
	const UStageCraftUITheme& Palette = GetTheme();
	ResultsBox->ClearChildren();
	ResultRevisions.Reset();
	if (!Share || Share->GetState() != EStageGdtfShareState::SignedIn)
	{
		return;
	}
	const TArray<FStageGdtfShareEntry>& Entries = Share->GetEntries();
	const TArray<int32> Matches = StageGdtfShare::Search(Entries, SearchField ? SearchField->GetText().ToString() : FString(), UploaderFilter, -1);
	ResultInfoText->SetText(Matches.Num() > MaxShownResults
		? FText::Format(LOCTEXT("ResultsCapped", "{0} of {1} files match; showing the first {2}. Type more to narrow the search."), FText::AsNumber(Matches.Num()), FText::AsNumber(Entries.Num()), FText::AsNumber(MaxShownResults))
		: FText::Format(LOCTEXT("Results", "{0} of {1} files match."), FText::AsNumber(Matches.Num()), FText::AsNumber(Entries.Num())));

	for (int32 Shown = 0; Shown < Matches.Num() && Shown < MaxShownResults; ++Shown)
	{
		const FStageGdtfShareEntry& Entry = Entries[Matches[Shown]];
		const bool bInLibrary = Library && Entry.FixtureTypeId.IsValid() && Library->FindType(Entry.FixtureTypeId);
		const FStageGdtfShareDownload* Running = Share->GetDownloads().FindByPredicate([&Entry](const FStageGdtfShareDownload& Row) { return Row.RevisionId == Entry.RevisionId; });
		const bool bDownloading = Running && Running->State == EStageGdtfShareDownloadState::Downloading;

		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		UVerticalBox* Text = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
		Text->AddChildToVerticalBox(MakeText(FText::FromString(StageGdtfShare::MakeLabel(Entry)), 8, TEXT("Bold"), Palette.TextPrimary, true));
		const FText Modes = FText::Format(LOCTEXT("Modes", "{0} {0}|plural(one=mode,other=modes)"), FText::AsNumber(Entry.Modes.Num()));
		FText Details = FText::Format(LOCTEXT("Details", "{0} · {1} · GDTF {2} {3}"), StageGdtfSharePanelText::Uploader(Entry.Uploader), Modes,
			FText::FromString(Entry.GdtfVersion), StageGdtfSharePanelText::Size(Entry.FileSizeBytes));
		if (bInLibrary)
		{
			Details = FText::Format(LOCTEXT("DetailsInLibrary", "{0} · IN YOUR LIBRARY"), Details);
		}
		Text->AddChildToVerticalBox(MakeText(Details, 7, TEXT("Regular"), Entry.Uploader == EStageGdtfShareUploader::Manufacturer ? Palette.AccentColor : Palette.TextSecondary, true));
		Row->AddChildToHorizontalBox(Text)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

		const int32 RowIndex = ResultRevisions.Add(Entry.RevisionId);
		UStageChoiceButton* Get = MakeButton(bInLibrary ? LOCTEXT("GetAgain", "GET AGAIN") : LOCTEXT("Get", "GET"),
			bInLibrary ? LOCTEXT("GetAgainTip", "Download this file again; the import report offers to replace the type in your Library.")
				: LOCTEXT("GetTip", "Download this file; its import report opens before anything is added."), RowIndex, !bInLibrary);
		Get->OnPicked.BindUObject(this, &ThisClass::HandleGet);
		StyleButton(*Get, !bInLibrary, !bDownloading);
		UHorizontalBoxSlot* GetSlot = Row->AddChildToHorizontalBox(Get);
		GetSlot->SetVerticalAlignment(VAlign_Center);
		GetSlot->SetPadding(FMargin(6.f, 0.f, 0.f, 0.f));
		ResultsBox->AddChildToVerticalBox(Row)->SetPadding(FMargin(0.f, 3.f, 0.f, 3.f));
	}
}

void UStageGdtfSharePanel::RebuildDownloads()
{
	const UStageGdtfShareSubsystem* Share = GetShare();
	const UStageCraftUITheme& Palette = GetTheme();
	DownloadsBox->ClearChildren();
	DownloadRevisions.Reset();
	if (!Share)
	{
		return;
	}
	if (Share->GetDownloads().IsEmpty())
	{
		DownloadsBox->AddChildToVerticalBox(MakeText(LOCTEXT("NoDownloads", "Nothing downloaded yet."), 7, TEXT("Regular"), Palette.TextSecondary));
	}
	for (const FStageGdtfShareDownload& Download : Share->GetDownloads())
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		UVerticalBox* Text = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
		Text->AddChildToVerticalBox(MakeText(FText::FromString(Download.Label), 8, TEXT("Bold"), Palette.TextPrimary, true));
		FText Progress = Download.Message;
		if (Download.State == EStageGdtfShareDownloadState::Downloading && Download.TotalBytes > 0)
		{
			Progress = FText::Format(LOCTEXT("Progress", "Downloading... {0} of {1}"), FText::AsMemory(Download.ReceivedBytes), FText::AsMemory(Download.TotalBytes));
		}
		const FLinearColor Color = Download.State == EStageGdtfShareDownloadState::Failed ? Palette.WarningColor : Palette.TextSecondary;
		Text->AddChildToVerticalBox(MakeText(Progress, 7, TEXT("Regular"), Color, true));
		Row->AddChildToHorizontalBox(Text)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		if (Download.State == EStageGdtfShareDownloadState::Downloading)
		{
			const int32 RowIndex = DownloadRevisions.Add(Download.RevisionId);
			UStageChoiceButton* Cancel = MakeButton(LOCTEXT("Cancel", "CANCEL"), LOCTEXT("CancelTip", "Stop this download."), RowIndex, false);
			Cancel->OnPicked.BindUObject(this, &ThisClass::HandleCancel);
			UHorizontalBoxSlot* CancelSlot = Row->AddChildToHorizontalBox(Cancel);
			CancelSlot->SetVerticalAlignment(VAlign_Center);
			CancelSlot->SetPadding(FMargin(6.f, 0.f, 0.f, 0.f));
		}
		DownloadsBox->AddChildToVerticalBox(Row)->SetPadding(FMargin(0.f, 3.f, 0.f, 3.f));
	}
}

void UStageGdtfSharePanel::HandleAction(int32 Index)
{
	AModularPlayerController* Controller = GetStageController();
	UStageGdtfShareSubsystem* Share = GetShare();
	if (!Controller || !Share)
	{
		return;
	}
	switch (static_cast<EAction>(Index))
	{
	case EAction::SignIn:
	{
		const FString Password = PasswordField ? PasswordField->GetText().ToString() : FString();
		if (PasswordField)
		{
			PasswordField->SetText(FText::GetEmpty());
		}
		StatusText->SetText(FText::GetEmpty());
		Controller->RequestGdtfShareSignIn(UserField ? UserField->GetText().ToString() : FString(), Password);
		break;
	}
	case EAction::SignOut:
		Controller->RequestGdtfShareSignOut();
		break;
	case EAction::Refresh:
		Controller->RequestGdtfShareRefresh(true);
		break;
	case EAction::FilterAll:
		UploaderFilter.Reset();
		break;
	case EAction::FilterManufacturer:
		UploaderFilter = EStageGdtfShareUploader::Manufacturer;
		break;
	case EAction::FilterUser:
		UploaderFilter = EStageGdtfShareUploader::User;
		break;
	case EAction::ClearFinished:
		Share->ClearFinishedDownloads();
		break;
	default:
		return;
	}
	RequestRebuild();
}

void UStageGdtfSharePanel::HandleGet(int32 RowIndex)
{
	AModularPlayerController* Controller = GetStageController();
	if (Controller && ResultRevisions.IsValidIndex(RowIndex))
	{
		Controller->RequestGdtfShareDownload(ResultRevisions[RowIndex]);
	}
}

void UStageGdtfSharePanel::HandleCancel(int32 RowIndex)
{
	AModularPlayerController* Controller = GetStageController();
	if (Controller && DownloadRevisions.IsValidIndex(RowIndex))
	{
		Controller->RequestGdtfShareCancelDownload(DownloadRevisions[RowIndex]);
	}
}

void UStageGdtfSharePanel::HandleShareChanged()
{
	RequestRebuild();
}

void UStageGdtfSharePanel::HandleRequestFinished(const FStageGdtfShareResultInfo& Result)
{
	if (StatusText)
	{
		StatusText->SetText(Result.Message);
		StatusText->SetColorAndOpacity(FSlateColor(Result.IsSuccess() ? GetTheme().TextSecondary : GetTheme().WarningColor));
	}
}

void UStageGdtfSharePanel::HandleLibraryChanged()
{
	RequestRebuild();
}

void UStageGdtfSharePanel::HandleSearchChanged(const FText& Text)
{
	RequestRebuild();
}

void UStageGdtfSharePanel::HandlePasswordCommitted(const FText& Text, ETextCommit::Type CommitMethod)
{
	if (CommitMethod == ETextCommit::OnEnter)
	{
		HandleAction(int32(EAction::SignIn));
	}
}

#undef LOCTEXT_NAMESPACE
