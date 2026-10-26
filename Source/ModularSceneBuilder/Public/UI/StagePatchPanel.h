// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/SpinBox.h"
#include "GameplayTagContainer.h"
#include "StagePatchPanel.generated.h"

/** Display helpers of the patch sheet, shared by the panel and its tests. */
namespace StagePatchFormat
{
	/** Console notation "U.AAA" (e.g. "1.001"), or "—" when unpatched. */
	MODULARSCENEBUILDER_API FText FormatPatch(int32 Universe, int32 Address);

	/** The DMX value of a channel as the console shows it: 0..255, or 0..65535 for a 16-bit pair. */
	MODULARSCENEBUILDER_API int32 ReadChannelValue(TConstArrayView<uint8> Universe, int32 Address, bool b16Bit);
}

DECLARE_DELEGATE_TwoParams(FOnStagePatchFieldCommitted, class UStagePatchField& /*Field*/, int32 /*Value*/);

/**
 * An integer field of the patch sheet (fixture ID, universe or address) that knows which fixture and parameter it edits, so
 * the panel can route its commit. Commits only when the user finishes (Enter, focus loss, end of a drag), never while
 * dragging: intermediate addresses would flash false patch conflicts.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStagePatchField : public USpinBox
{
	GENERATED_BODY()

public:
	void Configure(class ALightingFixtureActor& InFixture, const FGameplayTag& InParameterId);

	class ALightingFixtureActor* GetFixture() const { return Fixture.Get(); }
	const FGameplayTag& GetParameterId() const { return ParameterId; }

	/** Bound by the owning panel. */
	FOnStagePatchFieldCommitted OnFieldCommitted;

private:
	UFUNCTION()
	void HandleValueCommitted(float InValue, ETextCommit::Type CommitMethod);

	UFUNCTION()
	void HandleEndSliderMovement(float InValue);

	TWeakObjectPtr<class ALightingFixtureActor> Fixture;
	FGameplayTag ParameterId;
};

/**
 * The DMX patch sheet dock panel (StageCraft.Panel.Patch, Docs/ADR/0007-lighting-patch-paths-and-pipeline-hardening.md §6,
 * Docs/ADR/0019-dmx-patch-and-network-input.md §5), laid out like a console's patch window:
 *  - the network input: DMX IN switch and its status (listening ports, live universes and sources, frames, errors);
 *  - patch tools: AUTO PATCH (the unpatched), FIX CONFLICTS, PATCH SELECTION AT U.AAA, and the usage of every universe;
 *  - a fixture table: fixture ID, name, type, mode, patch (U.AAA), footprint, with overlapping patches marked;
 *  - the channel map of the selected fixture: absolute address, attribute, 8/16-bit and the live DMX value.
 *
 * A view in the project's binding pattern:
 *  - Reads UShowControlSubsystem (registry), UStageDMXPatchSubsystem (conflicts, usage, plans), UStageDMXNetworkSubsystem
 *    (input status) and the fixtures; owns no patch state.
 *  - Push, never poll: rebuilds on the next tick after OnFixturesChanged, OnPatchChanged or a fixture's patch/label change;
 *    refreshes channel values only on the selected fixture's attribute changes; follows the selection through
 *    OnSelectionSetChanged; updates the input line on OnInputChanged (at most twice a second while frames arrive).
 *  - Writes go through the controller: field edits through RequestParameterChange (the fields read the value back, so a
 *    refused or clamped edit snaps back), the patch tools through RequestPatchFixtures (one undo step), DMX IN through
 *    RequestSetDMXInput.
 *  - Every binding is made in NativeConstruct / Rebuild and removed in Rebuild / NativeDestruct. No tick.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStagePatchPanel : public UUserWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Patch")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	/** One channel-map row's value cell. */
	struct FChannelRow
	{
		int32 Address = 0;
		bool b16Bit = false;
		TWeakObjectPtr<class UTextBlock> Value;
	};

	void BuildTree();
	class UWidget* BuildHeader();
	/** DMX IN switch and status; the patch tools and universe usage. */
	class UWidget* BuildToolsBar();
	/** A small outlined header button labelled Label. OutLabel (optional) receives the label, for buttons whose text changes. */
	class UButton* MakeHeaderButton(FName Name, const FText& Label, const FText& ToolTip, class UTextBlock** OutLabel = nullptr);
	/** A plain integer field (not tied to a fixture), styled like the patch fields. */
	class USpinBox* MakeNumberField(int32 Value, int32 Min, int32 Max, float Width, const FText& ToolTip);
	class UWidget* MakeFixtureRow(class ALightingFixtureActor& Fixture, bool bConflict, bool bSelected);
	class UWidget* MakeColumnTitles();
	class UStagePatchField* MakePatchField(class ALightingFixtureActor& Fixture, const FGameplayTag& ParameterId, int32 Value, int32 Min, int32 Max, const FText& ToolTip);
	class UWidget* MakeCell(class UWidget& Content, float Width);
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color);
	const class UStageCraftUITheme& GetTheme() const;
	class AModularPlayerController* GetStageController() const;
	class ALightingFixtureActor* GetSelectedFixture() const;

	void RequestRebuild();
	void Rebuild();
	void RebuildChannelMap();
	void RefreshChannelValues();
	void BindFixture(class ALightingFixtureActor& Fixture);
	void UnbindFixtures();

	/** Sends one patch edit through the controller and snaps the field back to the fixture's value. */
	void HandleFieldCommitted(class UStagePatchField& Field, int32 Value);

	UFUNCTION()
	void HandleFixturesChanged();

	UFUNCTION()
	void HandleFixtureParameterChanged(class AModularBaseActor* Actor, FGameplayTag ParameterId);

	UFUNCTION()
	void HandleSelectionSetChanged(int32 Count);

	/** A fixture row was clicked (UStageChoiceButton reports its row index): select that fixture. */
	void HandleRowPicked(int32 RowIndex);

	UFUNCTION()
	void HandlePatchUnpatchedClicked();

	UFUNCTION()
	void HandleFixConflictsClicked();

	/** Patches the selected fixtures one after another from the U.AAA in the fields. */
	UFUNCTION()
	void HandlePatchSelectionClicked();

	UFUNCTION()
	void HandleInputClicked();

	UFUNCTION()
	void HandlePatchChanged();

	/** The input line only: cheap, no rebuild. */
	void RefreshInputStatus();

	/** Sends a plan through the controller; an empty plan is reported with Empty. */
	void ApplyPlan(TConstArrayView<struct FStageDMXPatchAssignment> Plan, const FText& Empty);

	/** Exports the patch sheet as PDF + CSV through the controller (Docs/ADR/0011-cue-list-and-patch-sheet-export.md §3.2). */
	UFUNCTION()
	void HandleExportClicked();

	UPROPERTY(Transient)
	TObjectPtr<class UVerticalBox> TableBody = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UVerticalBox> ChannelBody = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> SubtitleText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> ChannelTitleText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> EmptyText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UButton> PatchUnpatchedButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UButton> ExportButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UButton> InputButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> InputButtonLabel = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> InputStatusText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UButton> FixConflictsButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UButton> PatchSelectionButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class USpinBox> StartUniverseField = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class USpinBox> StartAddressField = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> UsageText = nullptr;

	/** Fixtures in table order, matched to a clicked row by index. */
	TArray<TWeakObjectPtr<class ALightingFixtureActor>> RowFixtures;
	TArray<FChannelRow> ChannelRows;

	/** Fixtures whose OnParameterChanged this panel is bound to. */
	TArray<TWeakObjectPtr<class ALightingFixtureActor>> BoundFixtures;

	/** The fixture shown in the channel map. */
	TWeakObjectPtr<class ALightingFixtureActor> ChannelFixture;

	TWeakObjectPtr<class UShowControlSubsystem> ShowControl;
	TWeakObjectPtr<class UStageDMXPatchSubsystem> Patching;
	TWeakObjectPtr<class UStageDMXNetworkSubsystem> Network;
	TWeakObjectPtr<class USelectionComponent> Selection;
	FDelegateHandle InputChangedHandle;
	FTimerHandle RebuildTimer;
};
