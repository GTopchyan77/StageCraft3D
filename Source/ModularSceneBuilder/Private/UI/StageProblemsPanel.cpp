// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageProblemsPanel.h"

#include "Actors/LightingFixtureActor.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/ButtonSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Player/ModularPlayerController.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageShowDiagnosticsSubsystem.h"
#include "TimerManager.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageToolButton.h"
#include "Workspace/StageWorkspaceSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageProblemsPanel)

#define LOCTEXT_NAMESPACE "StageProblemsPanel"

TConstArrayView<FStageUIAnchor> UStageProblemsPanel::GetAnchors()
{
	static const FStageUIAnchor Anchors[] = {
		{ TEXT("ProblemList"), LOCTEXT("ProblemListAnchor", "Problems list"), LOCTEXT("ProblemListTip", "What may keep the show from looking or working as intended: patch conflicts, DMX in and out, Blackout and the Grand Master, cues naming missing fixtures or palettes, the music source. Worst first; click a problem to go to where it is fixed.") },
		{ TEXT("Refresh"), LOCTEXT("Refresh", "REFRESH"), LOCTEXT("RefreshTip", "Check the show again now. The list also updates by itself when the show changes.") },
	};
	return Anchors;
}

namespace StageProblemsPanelAnchors
{
	const FStageUIAnchor& Get(FName ControlId)
	{
		return StageUIAnchors::Get(UStageProblemsPanel::GetAnchors(), ControlId);
	}
}

namespace StageProblemsPanel
{
	FText SeverityLabel(EStageDiagnosticSeverity Severity)
	{
		switch (Severity)
		{
		case EStageDiagnosticSeverity::Error:
			return LOCTEXT("Error", "ERROR");
		case EStageDiagnosticSeverity::Warning:
			return LOCTEXT("Warning", "WARNING");
		default:
			return LOCTEXT("Info", "INFO");
		}
	}
}

const UStageCraftUITheme& UStageProblemsPanel::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

AModularPlayerController* UStageProblemsPanel::GetStageController() const
{
	return Cast<AModularPlayerController>(GetOwningPlayer());
}

UStageShowDiagnosticsSubsystem* UStageProblemsPanel::GetDiagnostics() const
{
	return UWorld::GetSubsystem<UStageShowDiagnosticsSubsystem>(GetWorld());
}

FLinearColor UStageProblemsPanel::GetSeverityColor(EStageDiagnosticSeverity Severity) const
{
	const UStageCraftUITheme& Palette = GetTheme();
	switch (Severity)
	{
	case EStageDiagnosticSeverity::Error:
		return Palette.AxisXColor;
	case EStageDiagnosticSeverity::Warning:
		return Palette.WarningColor;
	default:
		return Palette.TextSecondary;
	}
}

UTextBlock* UStageProblemsPanel::MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
	Block->SetColorAndOpacity(FSlateColor(Color));
	return Block;
}

void UStageProblemsPanel::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
}

void UStageProblemsPanel::BuildTree()
{
	AnchorWidgets.Reset();
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
	UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Header->SetContent(Line);
	Column->AddChildToVerticalBox(Header);

	UTextBlock* Title = MakeText(LOCTEXT("Title", "PROBLEMS"), 9, TEXT("Bold"), Palette.TextPrimary);
	Title->SetToolTipText(StageProblemsPanelAnchors::Get(TEXT("ProblemList")).Tooltip);
	AnchorWidgets.Add(TEXT("ProblemList"), Title);
	if (UHorizontalBoxSlot* TitleSlot = Line->AddChildToHorizontalBox(Title))
	{
		TitleSlot->SetVerticalAlignment(VAlign_Center);
	}
	SummaryText = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), Palette.TextSecondary);
	SummaryText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	if (UHorizontalBoxSlot* SummarySlot = Line->AddChildToHorizontalBox(SummaryText))
	{
		SummarySlot->SetVerticalAlignment(VAlign_Center);
		SummarySlot->SetPadding(FMargin(12.f, 0.f, 8.f, 0.f));
		SummarySlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	UStageChoiceButton* Refresh = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
	FButtonStyle RefreshStyle = MakeFlatButton(FLinearColor::Transparent, Palette.RowHover, Palette.Divider);
	RefreshStyle.SetNormal(MakeBox(FLinearColor::Transparent, CornerRadius, Palette.Divider, 1.f));
	Refresh->SetStyle(RefreshStyle);
	Refresh->SetToolTipText(StageProblemsPanelAnchors::Get(TEXT("Refresh")).Tooltip);
	Refresh->OnPicked.BindUObject(this, &ThisClass::HandleRefreshPicked);
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Refresh->AddChild(MakeText(StageProblemsPanelAnchors::Get(TEXT("Refresh")).Label, 7, TEXT("Bold"), Palette.TextSecondary))))
	{
		LabelSlot->SetPadding(FMargin(8.f, 4.f));
	}
	AnchorWidgets.Add(TEXT("Refresh"), Refresh);
	if (UHorizontalBoxSlot* RefreshSlot = Line->AddChildToHorizontalBox(Refresh))
	{
		RefreshSlot->SetVerticalAlignment(VAlign_Center);
	}

	EmptyText = MakeText(LOCTEXT("Empty", "No problems found."), 8, TEXT("Italic"), Palette.TextSecondary);
	EmptyText->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* EmptySlot = Column->AddChildToVerticalBox(EmptyText))
	{
		EmptySlot->SetPadding(FMargin(12.f, 16.f));
	}

	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("ListScroll"));
	Scroll->SetWidgetBarStyle(MakeScrollBar(Palette));
	Scroll->SetScrollbarThickness(FVector2D(6.f, 6.f));
	if (UVerticalBoxSlot* ScrollSlot = Column->AddChildToVerticalBox(Scroll))
	{
		ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		ScrollSlot->SetPadding(FMargin(6.f, 4.f));
	}
	List = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("List"));
	Scroll->AddChild(List);
}

void UStageProblemsPanel::NativeConstruct()
{
	Super::NativeConstruct();
	if (UStageShowDiagnosticsSubsystem* Diagnostics = GetDiagnostics())
	{
		ProblemsHandle = Diagnostics->OnProblemsChanged.AddUObject(this, &ThisClass::HandleProblemsChanged);
		BoundDiagnostics = Diagnostics;
	}
	Rebuild();
}

void UStageProblemsPanel::NativeDestruct()
{
	if (UStageShowDiagnosticsSubsystem* Diagnostics = BoundDiagnostics.Get())
	{
		Diagnostics->OnProblemsChanged.Remove(ProblemsHandle);
	}
	ProblemsHandle.Reset();
	BoundDiagnostics.Reset();
	if (const UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RebuildTimer);
	}
	Super::NativeDestruct();
}

void UStageProblemsPanel::HandleProblemsChanged()
{
	RequestRebuild();
}

void UStageProblemsPanel::RequestRebuild()
{
	UWorld* World = GetWorld();
	if (!World || RebuildTimer.IsValid())
	{
		return;
	}
	// Next tick: never inside the broadcast or the click that caused it.
	RebuildTimer = World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ThisClass::Rebuild));
}

void UStageProblemsPanel::Rebuild()
{
	using namespace StageCraftWidgetStyle;
	RebuildTimer.Invalidate();
	if (!List)
	{
		return;
	}
	const UStageCraftUITheme& Palette = GetTheme();
	UStageShowDiagnosticsSubsystem* Diagnostics = BoundDiagnostics.Get();
	Shown = Diagnostics ? Diagnostics->GetProblems() : TArray<FStageDiagnostic>();

	int32 Counts[3] = { 0, 0, 0 };
	List->ClearChildren();
	for (int32 Index = 0; Index < Shown.Num(); ++Index)
	{
		const FStageDiagnostic& Problem = Shown[Index];
		++Counts[static_cast<int32>(Problem.Severity)];
		const FLinearColor Color = GetSeverityColor(Problem.Severity);

		UStageChoiceButton* Row = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
		FButtonStyle Style = MakeFlatButton(Palette.RowBackground, Palette.RowHover, Palette.Divider);
		Style.SetNormal(MakeBox(Palette.RowBackground, CornerRadius, WithAlpha(Color, 0.35f), 1.f));
		Row->SetStyle(Style);
		Row->SetChoiceIndex(Index);
		Row->SetToolTipText(LOCTEXT("RowTip", "Go to it: selects the fixtures concerned and shows the control that fixes it."));
		Row->OnPicked.BindUObject(this, &ThisClass::HandleRowPicked);

		UHorizontalBox* Face = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		UTextBlock* Tag = MakeText(StageProblemsPanel::SeverityLabel(Problem.Severity), 7, TEXT("Bold"), Color);
		if (UHorizontalBoxSlot* TagSlot = Face->AddChildToHorizontalBox(Tag))
		{
			TagSlot->SetVerticalAlignment(VAlign_Top);
			TagSlot->SetPadding(FMargin(0.f, 1.f, 8.f, 0.f));
		}
		UTextBlock* Message = MakeText(StageShowDiagnostics::Describe(Problem, EStageAILanguage::English), 8, TEXT("Regular"), Palette.TextPrimary);
		Message->SetAutoWrapText(true);
		if (UHorizontalBoxSlot* MessageSlot = Face->AddChildToHorizontalBox(Message))
		{
			MessageSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}
		if (UButtonSlot* FaceSlot = Cast<UButtonSlot>(Row->AddChild(Face)))
		{
			FaceSlot->SetPadding(FMargin(8.f, 5.f));
			FaceSlot->SetHorizontalAlignment(HAlign_Fill);
		}
		if (UVerticalBoxSlot* RowSlot = List->AddChildToVerticalBox(Row))
		{
			RowSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
		}
	}

	const int32 Errors = Counts[static_cast<int32>(EStageDiagnosticSeverity::Error)];
	const int32 Warnings = Counts[static_cast<int32>(EStageDiagnosticSeverity::Warning)];
	const int32 Notes = Counts[static_cast<int32>(EStageDiagnosticSeverity::Info)];
	SummaryText->SetText(Shown.IsEmpty() ? LOCTEXT("SummaryClean", "All clear")
		: FText::Format(LOCTEXT("Summary", "{0} {0}|plural(one=error,other=errors), {1} {1}|plural(one=warning,other=warnings), {2} {2}|plural(one=note,other=notes)"),
			FText::AsNumber(Errors), FText::AsNumber(Warnings), FText::AsNumber(Notes)));
	SummaryText->SetColorAndOpacity(FSlateColor(Errors > 0 ? GetSeverityColor(EStageDiagnosticSeverity::Error)
		: Warnings > 0 ? GetSeverityColor(EStageDiagnosticSeverity::Warning) : Palette.TextSecondary));
	EmptyText->SetVisibility(Shown.IsEmpty() ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
}

void UStageProblemsPanel::HandleRowPicked(int32 Index)
{
	GoToProblem(Index);
}

void UStageProblemsPanel::HandleRefreshPicked(int32 ChoiceIndex)
{
	if (UStageShowDiagnosticsSubsystem* Diagnostics = BoundDiagnostics.Get())
	{
		Diagnostics->RefreshProblems();
	}
	RequestRebuild();
}

bool UStageProblemsPanel::GoToProblem(int32 Index)
{
	if (!Shown.IsValidIndex(Index))
	{
		return false;
	}
	const FStageDiagnostic& Problem = Shown[Index];
	// Selecting is a request like any other selection; fixtures that are gone are skipped.
	if (!Problem.FixtureIds.IsEmpty())
	{
		AModularPlayerController* Controller = GetStageController();
		const UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(GetWorld());
		TArray<AModularBaseActor*> Fixtures;
		for (const int32 FixtureId : Problem.FixtureIds)
		{
			if (ALightingFixtureActor* Fixture = Show ? Show->FindFixture(FixtureId) : nullptr)
			{
				Fixtures.Add(Fixture);
			}
		}
		if (Controller && !Fixtures.IsEmpty())
		{
			Controller->RequestSelectItems(Fixtures, EStageSelectionClick::Replace);
		}
	}
	// Opening a panel and pointing at a control is navigation, not a stage change.
	FGameplayTag Panel;
	FName Control;
	UGameInstance* GameInstance = GetGameInstance();
	UStageWorkspaceSubsystem* Workspace = GameInstance ? GameInstance->GetSubsystem<UStageWorkspaceSubsystem>() : nullptr;
	if (Workspace && StageUIAnchors::GetPlaceTarget(Problem.Place, Panel, Control))
	{
		Workspace->ShowControl(Panel, Control);
	}
	return true;
}

#undef LOCTEXT_NAMESPACE
