// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageSendToWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Bridge/StageBridgeProtocol.h"
#include "Bridge/StageBridgeSubsystem.h"
#include "Bridge/StageSceneExporter.h"
#include "Components/Border.h"
#include "Components/ButtonSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Import/StageFileDialog.h"
#include "Player/ModularPlayerController.h"
#include "Scene/StageSceneComponent.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageThemedComboBox.h"
#include "UI/StageToolButton.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageSendToWidget)

#define LOCTEXT_NAMESPACE "StageSendToWidget"

namespace StageSendToPanel
{
	constexpr float GroupGap = 12.f;
}

const UStageCraftUITheme& UStageSendToWidget::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

AModularPlayerController* UStageSendToWidget::GetStageController() const
{
	return Cast<AModularPlayerController>(GetOwningPlayer());
}

UStageBridgeSubsystem* UStageSendToWidget::GetBridge() const
{
	return GetGameInstance() ? GetGameInstance()->GetSubsystem<UStageBridgeSubsystem>() : nullptr;
}

void UStageSendToWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
}

// --- Construction ---

UTextBlock* UStageSendToWidget::MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
	Block->SetColorAndOpacity(FSlateColor(Color));
	return Block;
}

UStageChoiceButton* UStageSendToWidget::MakeButton(const FText& Label, const FText& Tip, int32 Index, void (UStageSendToWidget::*Handler)(int32))
{
	UStageChoiceButton* Button = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
	Button->SetChoiceIndex(Index);
	Button->SetToolTipText(Tip);
	Button->OnPicked.BindUObject(this, Handler);
	UTextBlock* Text = MakeText(Label, 9, TEXT("Bold"), GetTheme().TextSecondary);
	Text->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Button->AddChild(Text)))
	{
		LabelSlot->SetPadding(FMargin(10.f, 5.f));
		LabelSlot->SetHorizontalAlignment(HAlign_Left);
	}
	StyleChoice(*Button, false);
	return Button;
}

void UStageSendToWidget::StyleChoice(UStageChoiceButton& Button, bool bSelected) const
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
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
		Label->SetColorAndOpacity(FSlateColor(bSelected ? Palette.WindowBackground : (Button.GetIsEnabled() ? Palette.TextSecondary : Palette.TextDisabled)));
	}
}

void UStageSendToWidget::BuildTree()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Root = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Root"));
	Root->SetBrush(MakeBox(Palette.PanelBackground));
	Root->SetPadding(FMargin(0.f));
	WidgetTree->RootWidget = Root;

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Column"));
	Root->SetContent(Column);

	UBorder* Header = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Header"));
	Header->SetBrush(MakeBox(Palette.PanelHeader));
	Header->SetPadding(FMargin(10.f, 6.f));
	Header->SetContent(MakeText(LOCTEXT("Title", "SEND TO 3D APP"), 10, TEXT("Bold"), Palette.AccentColor));
	Column->AddChildToVerticalBox(Header);

	UVerticalBox* Body = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Body"));
	if (UVerticalBoxSlot* BodySlot = Column->AddChildToVerticalBox(Body))
	{
		BodySlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		BodySlot->SetPadding(FMargin(12.f, 10.f));
	}
	const auto AddGap = [Body](UWidget* Widget, float Top) { Body->AddChildToVerticalBox(Widget)->SetPadding(FMargin(0.f, Top, 0.f, 0.f)); };
	const auto Caption = [this, &Palette](const FText& Text) { return MakeText(Text, 7, TEXT("Bold"), Palette.TextSecondary); };

	// The switch: off by default, and every connection ends when it goes off (ADR 0022 §6).
	UHorizontalBox* ToggleRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	ToggleButton = MakeButton(LOCTEXT("Allow", "ALLOW 3D APPS TO CONNECT"),
		LOCTEXT("AllowTip", "While on, the StageCraft add-ons for Blender, 3ds Max, Cinema 4D and Maya on this computer can connect. Nothing outside this computer can."), 0,
		&ThisClass::HandleTogglePicked);
	ToggleRow->AddChildToHorizontalBox(ToggleButton)->SetVerticalAlignment(VAlign_Center);
	StateText = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextSecondary);
	StateText->SetAutoWrapText(true);
	if (UHorizontalBoxSlot* StateSlot = ToggleRow->AddChildToHorizontalBox(StateText))
	{
		StateSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		StateSlot->SetVerticalAlignment(VAlign_Center);
		StateSlot->SetPadding(FMargin(10.f, 0.f, 0.f, 0.f));
	}
	AddGap(ToggleRow, 0.f);

	AddGap(Caption(LOCTEXT("Scene", "SAVED SCENE")), StageSendToPanel::GroupGap);
	SceneCombo = WidgetTree->ConstructWidget<UStageThemedComboBox>(UStageThemedComboBox::StaticClass(), TEXT("SceneCombo"));
	SceneCombo->ApplyTheme(Palette);
	AddGap(SceneCombo, 2.f);
	SceneNote = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextSecondary);
	SceneNote->SetAutoWrapText(true);
	AddGap(SceneNote, 3.f);

	AddGap(Caption(LOCTEXT("SendTo", "SEND TO")), StageSendToPanel::GroupGap);
	TargetList = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Targets"));
	AddGap(TargetList, 2.f);

	StatusText = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextSecondary);
	StatusText->SetAutoWrapText(true);
	AddGap(StatusText, StageSendToPanel::GroupGap);
}

// --- Lifetime ---

void UStageSendToWidget::NativeConstruct()
{
	Super::NativeConstruct();
	if (UStageBridgeSubsystem* Bridge = GetBridge())
	{
		BoundBridge = Bridge;
		Bridge->OnStateChanged.AddUniqueDynamic(this, &ThisClass::HandleBridgeStateChanged);
		Bridge->OnClientsChanged.AddUniqueDynamic(this, &ThisClass::HandleClientsChanged);
		Bridge->OnSendCompleted.AddUniqueDynamic(this, &ThisClass::HandleSendCompleted);
	}
	if (const AModularPlayerController* Controller = GetStageController())
	{
		if (UStageSceneComponent* Scenes = Controller->GetSceneFiles())
		{
			BoundScenes = Scenes;
			Scenes->OnSceneOperationFinished.AddUniqueDynamic(this, &ThisClass::HandleSceneOperationFinished);
		}
	}
	if (SceneCombo)
	{
		SceneCombo->OnSelectionChanged.AddUniqueDynamic(this, &ThisClass::HandleSceneSelected);
	}
	RefreshScenes();
	RefreshState();
	RebuildTargets();
}

void UStageSendToWidget::NativeDestruct()
{
	if (UStageBridgeSubsystem* Bridge = BoundBridge.Get())
	{
		Bridge->OnStateChanged.RemoveDynamic(this, &ThisClass::HandleBridgeStateChanged);
		Bridge->OnClientsChanged.RemoveDynamic(this, &ThisClass::HandleClientsChanged);
		Bridge->OnSendCompleted.RemoveDynamic(this, &ThisClass::HandleSendCompleted);
	}
	BoundBridge.Reset();
	if (UStageSceneComponent* Scenes = BoundScenes.Get())
	{
		Scenes->OnSceneOperationFinished.RemoveDynamic(this, &ThisClass::HandleSceneOperationFinished);
	}
	BoundScenes.Reset();
	if (SceneCombo)
	{
		SceneCombo->OnSelectionChanged.RemoveDynamic(this, &ThisClass::HandleSceneSelected);
	}
	Super::NativeDestruct();
}

// --- Refresh ---

FString UStageSendToWidget::GetSelectedScene() const
{
	return SceneCombo ? SceneCombo->GetSelectedOption() : FString();
}

void UStageSendToWidget::RefreshScenes()
{
	const UStageSceneComponent* Scenes = BoundScenes.Get();
	if (!SceneCombo || !Scenes)
	{
		return;
	}
	const FString Previous = GetSelectedScene();
	const UStageSessionSubsystem* Session = GetWorld() ? GetWorld()->GetSubsystem<UStageSessionSubsystem>() : nullptr;
	const FString Current = Session ? Session->GetSceneName() : FString();

	SceneCombo->ClearOptions();
	for (const FString& Name : Scenes->GetSceneNames())
	{
		SceneCombo->AddOption(Name);
	}
	// Keep the user's pick; otherwise the open stage; otherwise the first scene.
	const FString Pick = SceneCombo->FindOptionIndex(Previous) != INDEX_NONE ? Previous : (SceneCombo->FindOptionIndex(Current) != INDEX_NONE ? Current : FString());
	if (!Pick.IsEmpty())
	{
		SceneCombo->SetSelectedOption(Pick);
	}
	else if (SceneCombo->GetOptionCount() > 0)
	{
		SceneCombo->SetSelectedIndex(0);
	}
	HandleSceneSelected(GetSelectedScene(), ESelectInfo::Direct);
}

void UStageSendToWidget::RefreshState()
{
	const UStageBridgeSubsystem* Bridge = BoundBridge.Get();
	const bool bRunning = Bridge && Bridge->IsRunning();
	if (ToggleButton)
	{
		StyleChoice(*ToggleButton, bRunning);
	}
	if (StateText)
	{
		StateText->SetText(bRunning
			? FText::Format(LOCTEXT("On", "On: the add-ons can connect (port {0}). Install them from Tools/DCCBridge."), FText::AsNumber(Bridge->GetPort(), &FNumberFormattingOptions::DefaultNoGrouping()))
			: LOCTEXT("OffState", "Off: no 3D app can connect. You can still save a .glb file."));
	}
}

void UStageSendToWidget::RebuildTargets()
{
	if (!TargetList)
	{
		return;
	}
	TargetList->ClearChildren();
	TargetClients.Reset();

	const UStageBridgeSubsystem* Bridge = BoundBridge.Get();
	const bool bIdle = !Bridge || !Bridge->IsSending();
	if (Bridge)
	{
		for (const FStageBridgeClientInfo& Client : Bridge->GetClients())
		{
			const FText AppName = FText::Format(LOCTEXT("AppName", "{0} {1}"), StageBridgeProtocol::GetAppDisplayName(Client.App), FText::FromString(Client.AppVersion));
			UStageChoiceButton* Button = MakeButton(Client.bImportsGlb ? AppName : FText::Format(LOCTEXT("AppNeedsUsd", "{0}: needs USD export (coming)"), AppName),
				FText::Format(LOCTEXT("AppTip", "Send the saved scene to {0}; it is imported into the open file."), AppName), TargetClients.Num(), &ThisClass::HandleTargetPicked);
			Button->SetIsEnabled(Client.bImportsGlb && bIdle);
			StyleChoice(*Button, false);
			TargetList->AddChildToVerticalBox(Button)->SetPadding(FMargin(0.f, 0.f, 0.f, 3.f));
			TargetClients.Add(Client.ClientId);
		}
	}
	if (TargetClients.IsEmpty())
	{
		UTextBlock* Hint = MakeText(Bridge && Bridge->IsRunning()
			? LOCTEXT("NoApps", "No app connected yet: in Blender, 3ds Max or Cinema 4D, use the StageCraft Bridge add-on's Connect.")
			: LOCTEXT("SwitchOn", "Switch on \"Allow 3D apps to connect\" to send to an app."), 8, TEXT("Regular"), GetTheme().TextSecondary);
		Hint->SetAutoWrapText(true);
		TargetList->AddChildToVerticalBox(Hint)->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
	}

	UStageChoiceButton* Disk = MakeButton(LOCTEXT("SaveGlb", "Save as .glb file..."),
		LOCTEXT("SaveGlbTip", "Save the saved scene as a glTF binary file, which Blender, 3ds Max, Cinema 4D, Unity and most 3D apps open."), TargetClients.Num(), &ThisClass::HandleTargetPicked);
	Disk->SetIsEnabled(StageFileDialog::IsAvailable());
	StyleChoice(*Disk, false);
	TargetList->AddChildToVerticalBox(Disk);
}

void UStageSendToWidget::SetStatus(const FText& Text, bool bWarning)
{
	if (StatusText)
	{
		StatusText->SetText(Text);
		StatusText->SetColorAndOpacity(FSlateColor(bWarning ? GetTheme().WarningColor : GetTheme().TextSecondary));
	}
}

// --- Handlers ---

void UStageSendToWidget::HandleTogglePicked(int32 /*Index*/)
{
	const UStageBridgeSubsystem* Bridge = BoundBridge.Get();
	if (AModularPlayerController* Controller = GetStageController())
	{
		Controller->RequestSetBridgeEnabled(!(Bridge && Bridge->IsRunning()));
	}
}

void UStageSendToWidget::HandleTargetPicked(int32 Index)
{
	AModularPlayerController* Controller = GetStageController();
	const FString Scene = GetSelectedScene();
	if (!Controller || Scene.IsEmpty())
	{
		SetStatus(LOCTEXT("NoScene", "Save the stage first (File › Save Scene); a saved scene is what gets sent."), true);
		return;
	}

	if (TargetClients.IsValidIndex(Index))
	{
		if (Controller->RequestSendScene(Scene, TargetClients[Index]))
		{
			SetStatus(FText::Format(LOCTEXT("Sending", "Sending \"{0}\"..."), FText::FromString(Scene)), false);
			RebuildTargets();
		}
		else
		{
			SetStatus(LOCTEXT("SendRefused", "Not sent; the status bar says why."), true);
		}
		return;
	}

	FString Path;
	const FString Exports = FPaths::Combine(FPlatformProcess::UserDir(), TEXT("StageCraft Exports"));
	if (!StageFileDialog::SaveFile(LOCTEXT("SaveTitle", "Save stage as glTF"), Exports, Scene + FStageSceneExporter::FileExtension,
		TEXT("glTF binary (*.glb)|*.glb"), Path))
	{
		return;
	}
	if (!Path.EndsWith(FStageSceneExporter::FileExtension, ESearchCase::IgnoreCase))
	{
		Path += FStageSceneExporter::FileExtension;
	}
	SetStatus(FText::Format(LOCTEXT("Exporting", "Exporting \"{0}\"..."), FText::FromString(Scene)), false);
	Controller->RequestExportScene(Scene, Path);
}

void UStageSendToWidget::HandleBridgeStateChanged(bool /*bRunning*/)
{
	RefreshState();
	RebuildTargets();
}

void UStageSendToWidget::HandleClientsChanged()
{
	RebuildTargets();
}

void UStageSendToWidget::HandleSendCompleted(const FStageBridgeResultInfo& Result)
{
	SetStatus(Result.FilePath.IsEmpty() || Result.ClientId != 0 ? Result.Message
		: FText::Format(LOCTEXT("SavedTo", "{0} Saved to {1}"), Result.Message, FText::FromString(Result.FilePath)), !Result.IsSuccess());
	RebuildTargets();
}

void UStageSendToWidget::HandleSceneSelected(FString SelectedItem, ESelectInfo::Type /*SelectionType*/)
{
	if (!SceneNote)
	{
		return;
	}
	const UStageSessionSubsystem* Session = GetWorld() ? GetWorld()->GetSubsystem<UStageSessionSubsystem>() : nullptr;
	if (SelectedItem.IsEmpty())
	{
		SceneNote->SetText(LOCTEXT("NoScenes", "There are no saved scenes yet. Save the stage first (File › Save Scene)."));
	}
	else if (Session && Session->IsDirty() && Session->GetSceneName().Equals(SelectedItem, ESearchCase::IgnoreCase))
	{
		SceneNote->SetText(LOCTEXT("Unsaved", "The open stage has unsaved changes: the saved file is what gets sent. Save first to include them."));
	}
	else
	{
		SceneNote->SetText(LOCTEXT("SceneHint", "Your own stages only: stages from the Community Marketplace stay in StageCraft. Items you do not own are left out."));
	}
}

void UStageSendToWidget::HandleSceneOperationFinished(const FStageSceneOperationResult& Result)
{
	if (Result.IsSuccess())
	{
		RefreshScenes();
	}
}

#undef LOCTEXT_NAMESPACE
