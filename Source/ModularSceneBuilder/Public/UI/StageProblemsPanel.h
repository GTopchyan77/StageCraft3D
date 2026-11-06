// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Engine/TimerHandle.h"
#include "Show/StageShowDiagnostics.h"
#include "Styling/SlateTypes.h"
#include "UI/StageUIAnchors.h"
#include "StageProblemsPanel.generated.h"

/**
 * The Problems panel (StageCraft.Panel.Problems, Docs/ADR/0030 §5): the findings of UStageShowDiagnosticsSubsystem, worst first, one
 * row each. Clicking a row goes to the problem: it selects the fixtures concerned and opens the panel that fixes it with that
 * control pointed at (SHOW ME). Works without the assistant, which reads the same findings.
 *
 * A view in the project's binding pattern: owns no show state; rebuilds on the next tick after OnProblemsChanged; REFRESH runs the
 * checks again (the music's signal is not pushed). Selection goes through AModularPlayerController::RequestSelectItems; opening a
 * panel is navigation (UStageWorkspaceSubsystem::ShowControl). Bindings are made in NativeConstruct and removed in NativeDestruct.
 * No tick.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageProblemsPanel : public UUserWidget, public IStageAnchoredPanel
{
	GENERATED_BODY()

public:
	/** The controls the assistant can name and point at (Docs/ADR/0030 §3); their labels and tooltips are read from here. */
	static TConstArrayView<FStageUIAnchor> GetAnchors();

	//~ Begin IStageAnchoredPanel Interface
	virtual class UWidget* FindAnchorWidget(FName ControlId) const override { return AnchorWidgets.Find(ControlId); }
	//~ End IStageAnchoredPanel Interface

	/** The rows shown now, in order (tests read them). */
	const TArray<FStageDiagnostic>& GetShownProblems() const { return Shown; }

	/** Does what clicking row Index does. False for an index out of range. */
	bool GoToProblem(int32 Index);

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Problems")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	FStageAnchorWidgets AnchorWidgets;

	void BuildTree();
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color);
	const class UStageCraftUITheme& GetTheme() const;
	class AModularPlayerController* GetStageController() const;
	class UStageShowDiagnosticsSubsystem* GetDiagnostics() const;
	FLinearColor GetSeverityColor(EStageDiagnosticSeverity Severity) const;

	void RequestRebuild();
	void Rebuild();
	void HandleProblemsChanged();
	void HandleRowPicked(int32 Index);
	void HandleRefreshPicked(int32 ChoiceIndex);

	UPROPERTY(Transient)
	TObjectPtr<class UVerticalBox> List = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> SummaryText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> EmptyText = nullptr;

	TWeakObjectPtr<class UStageShowDiagnosticsSubsystem> BoundDiagnostics;
	FDelegateHandle ProblemsHandle;

	/** What the rows show, so a click finds its finding. */
	TArray<FStageDiagnostic> Shown;

	FTimerHandle RebuildTimer;
};
