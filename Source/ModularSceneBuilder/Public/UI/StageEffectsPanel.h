// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Engine/TimerHandle.h"
#include "Show/ShowControlTypes.h"
#include "Show/StageEffectRunner.h"
#include "Styling/SlateTypes.h"
#include "StageEffectsPanel.generated.h"

/**
 * The Effects panel (StageCraft.Panel.Effects, Docs/ADR/0027-effects-engine.md §6):
 *  - quick effects (Circle, Dimmer Chase, Rainbow, ...) that start on the selected fixtures, in selection order, with one click;
 *  - an editor for the chosen running effect: waveform, direction, size fan, axis, colours or beam attribute, and sliders for rate
 *    (Hz and BPM, with TAP), size, low/high, phase, spread, duty, wings and blocks, over a live preview of the wave and its fixtures;
 *  - the AUDIO rows (Docs/ADR/0029 §4): mode (Off, Sync, Modulate, Trigger), band, what it modulates, beats, response and amount,
 *    and six audio quick effects (§5); running effects that follow the music are marked ♪;
 *  - the running effects, each with STOP, and STOP ALL.
 *
 * A view in the project's binding pattern: reads UStageProgrammerSubsystem (the effects) and UShowControlSubsystem (their clocks,
 * the control source) and owns no show state; which effect the editor shows is view state. Built once; OnValuesChanged and
 * OnControlSourceChanged refresh it in place on the next tick, never inside the callback that caused them, so a slider being
 * dragged is never rebuilt. Writes go through AModularPlayerController: a drag previews (RequestPreviewEffect) and its release
 * commits one undo step (RequestUpdateEffect); buttons commit at once. Bindings are made in NativeConstruct, removed in NativeDestruct.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageEffectsPanel : public UUserWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Effects")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	/** The editor's sliders; the value is the slider's index. */
	enum class ESetting : int32
	{
		Rate,
		Size,
		Low,
		High,
		Phase,
		Spread,
		Duty,
		Wings,
		Blocks,
		/** Modulate: the depth (Docs/ADR/0029 §4). */
		Amount,
		Count,
	};

	/** The editor's rows of choices; a button's index is Row * ChoiceStride + option. */
	enum class EChoiceRow : int32
	{
		Waveform,
		Direction,
		Fan,
		Axis,
		ColorMode,
		ColorA,
		ColorB,
		BeamAttribute,
		// How the effect follows the music (Docs/ADR/0029 §4).
		AudioMode,
		AudioBand,
		AudioTarget,
		AudioBeats,
		AudioResponse,
		Count,
	};

	struct FSettingRow
	{
		TWeakObjectPtr<class UWidget> Row;
		TWeakObjectPtr<class UStageValueSlider> Slider;
		TWeakObjectPtr<class UTextBlock> Value;
	};

	struct FChoiceRow
	{
		TWeakObjectPtr<class UWidget> Row;
		TArray<TWeakObjectPtr<class UStageChoiceButton>> Buttons;
	};

	/** One running effect's line in the list, refreshed in place while the set of effects stays the same. */
	struct FRunningRow
	{
		TWeakObjectPtr<class UBorder> Frame;
		TWeakObjectPtr<class UTextBlock> Label;
		/** EDIT: shown for an effect a cue or playback runs. */
		TWeakObjectPtr<class UWidget> Edit;
	};

	void BuildTree();
	class UWidget* BuildQuickColumn();
	/** A wrap box of quick-effect buttons for QuickEffects[First, First + Count). */
	class UWidget* BuildQuickGrid(int32 First, int32 Count);
	class UWidget* BuildEditor();
	class UWidget* BuildRunningColumn();
	class UWidget* BuildSettingRow(ESetting Setting, const FText& Label, const FText& ToolTip);
	class UWidget* BuildChoiceRow(EChoiceRow Row, const FText& Label, TConstArrayView<FText> Options, const FText& ToolTip);
	void RebuildRunningList();

	class UStageChoiceButton* MakeButton(const FText& Label, const FText& ToolTip, int32 ChoiceIndex, bool bLit);
	FButtonStyle MakeButtonStyle(bool bLit, const FLinearColor& LitColor) const;
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color);
	const class UStageCraftUITheme& GetTheme() const;
	class AModularPlayerController* GetStageController() const;

	/** The effect the editor shows, or null (none running, or it stopped). */
	const FStageEffectEntry* FindSelected() const;
	/** Whether the selected effect is the programmer's (editable); a cue's or playback's is shown read-only until EDIT takes it. */
	bool IsSelectedEditable() const;
	bool GetPreviewState(FStageEffect& OutEffect, int32& OutCount, double& OutCycles) const;

	void RequestRefresh();
	void Refresh();
	void RefreshEditor(const FStageEffectEntry* Selected, bool bEnabled);
	void RefreshRunningList(bool bEnabled);

	static float ToSliderValue(ESetting Setting, const FStageEffect& Effect);
	static void FromSliderValue(ESetting Setting, float SliderValue, FStageEffect& InOutEffect);
	static FText FormatSetting(ESetting Setting, const FStageEffect& Effect);
	/** Which option of Row Effect has lit; INDEX_NONE for none (a colour that is not one of the buttons'). */
	static int32 GetLitOption(EChoiceRow Row, const FStageEffect& Effect);
	/** Applies option Option of Row to Effect; false when it changes nothing. */
	static bool ApplyOption(EChoiceRow Row, int32 Option, FStageEffect& InOutEffect);

	/** Commits Effect as the selected effect's new settings (one undo step). */
	void CommitSelected(const FStageEffect& Effect);

	UFUNCTION()
	void HandleProgrammerChanged();

	UFUNCTION()
	void HandleControlSourceChanged(EStageControlSource NewSource);

	UFUNCTION()
	void HandleSliderReleased();

	void HandleQuickPicked(int32 Index);
	void HandleChoicePicked(int32 ChoiceIndex);
	void HandleSliderMoved(int32 SliderIndex, float Value);
	void HandleRunningPicked(int32 Index);
	void HandleStopPicked(int32 Index);
	void HandleEditPicked(int32 Index);

	UFUNCTION()
	void HandleShowChanged();

	UFUNCTION()
	void HandleCueEvent(float CueNumber);
	void HandleStopAllPicked(int32 ChoiceIndex);
	void HandleTapPicked(int32 ChoiceIndex);

	/** Built once in BuildTree; the widget tree owns the widgets (weak references here). */
	TArray<FSettingRow> SettingRows;
	TArray<FChoiceRow> ChoiceRows;
	TArray<FRunningRow> RunningRows;
	/** The effects the running list shows (every source, not those fading out), as of the last refresh; a different set rebuilds it. */
	TArray<FStageRunningEffect> Listed;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> EditorTitle = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> EditorHint = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UWidget> EditorBody = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageEffectPreview> Preview = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UVerticalBox> RunningList = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> StatusText = nullptr;

	/** The plain quick effects, then the audio ones (Docs/ADR/0029 §5) from AudioQuickStart on. */
	TArray<FStageEffect> QuickEffects;
	int32 AudioQuickStart = 0;
	/** View state: which running effect the editor shows. */
	FGuid SelectedEffectId;
	/** TAP: the last taps (FPlatformTime seconds); the tempo is their average spacing. */
	TArray<double> TapTimes;

	TWeakObjectPtr<class UStageProgrammerSubsystem> Programmer;
	TWeakObjectPtr<class UShowControlSubsystem> ShowControl;
	FTimerHandle RefreshTimer;
};
