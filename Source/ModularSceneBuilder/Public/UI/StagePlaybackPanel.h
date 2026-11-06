// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Engine/TimerHandle.h"
#include "Show/ShowControlTypes.h"
#include "Styling/SlateTypes.h"
#include "StagePlaybackPanel.generated.h"

/**
 * The playback desk (StageCraft.Panel.Playback, Docs/ADR/0026-cue-timing-tracking-playbacks.md §6), laid out like a console's
 * playback wing:
 *  - ten playback strips: name, current cue, a vertical fader, level, GO, FLASH (held), REC (record the programmer as the next
 *    cue), REL (release) and CLR (delete its cues);
 *  - the masters: the Grand Master fader and BLACKOUT; the AUDIO master and KILL for effects that follow the music (ADR 0029 §7).
 *
 * A view in the project's binding pattern: reads UShowControlSubsystem and owns no state. The strips are built once (the desk
 * always has ten); OnPlaybacksChanged and OnDeskStateChanged refresh their texts and fader positions on the next tick, never
 * inside the fader's own callback. Every write goes through AModularPlayerController (RequestSetPlaybackLevel, RequestPlaybackFlash,
 * ...), which asks the GameMode; a refusal is reported on OnRequestRejected and the refresh shows the real state. Bindings are made
 * in NativeConstruct and removed in NativeDestruct. No tick.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStagePlaybackPanel : public UUserWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Playbacks")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	/** The widgets of one playback strip, refreshed in place. */
	struct FStrip
	{
		TWeakObjectPtr<class UBorder> Frame;
		TWeakObjectPtr<class UTextBlock> Name;
		TWeakObjectPtr<class UTextBlock> Cue;
		TWeakObjectPtr<class UStageValueSlider> Fader;
		TWeakObjectPtr<class UTextBlock> Level;
		TWeakObjectPtr<class UStageFlashButton> Flash;
	};

	void BuildTree();
	class UWidget* BuildStrip(int32 Index);
	class UWidget* BuildMasters();
	/** One master: title, fader (reporting FaderIndex), level and a button under it. */
	class UWidget* BuildMasterColumn(const FText& Title, const FText& TitleTip, int32 FaderIndex, const FText& FaderTip, const FText& ButtonLabel,
		const FText& ButtonTip, TObjectPtr<class UStageValueSlider>& OutFader, TObjectPtr<class UTextBlock>& OutLevel, TObjectPtr<class UStageChoiceButton>& OutButton);
	class UStageChoiceButton* MakeButton(const FText& Label, const FText& ToolTip, int32 ChoiceIndex, bool bLit);
	FButtonStyle MakeButtonStyle(bool bLit, const FLinearColor& LitColor) const;
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color);
	class UStageValueSlider* MakeFader(int32 Index, const FText& ToolTip);
	const class UStageCraftUITheme& GetTheme() const;
	class AModularPlayerController* GetStageController() const;

	void RequestRefresh();
	void Refresh();

	UFUNCTION()
	void HandleDeskChanged();

	UFUNCTION()
	void HandleDeskChangedBySource(EStageControlSource NewSource);

	void HandleFaderMoved(int32 FaderIndex, float Value);
	void HandleFlash(int32 Index, bool bDown);
	void HandleGoPicked(int32 Index);
	void HandleRecordPicked(int32 Index);
	void HandleReleasePicked(int32 Index);
	void HandleClearPicked(int32 Index);
	void HandleBlackoutPicked(int32 ChoiceIndex);
	void HandleAudioKillPicked(int32 ChoiceIndex);

	/** Built once in BuildTree; the widget tree owns the widgets (weak references here). */
	TArray<FStrip> Strips;

	UPROPERTY(Transient)
	TObjectPtr<class UStageValueSlider> GrandMasterFader = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> GrandMasterText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageChoiceButton> BlackoutButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageValueSlider> AudioMasterFader = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> AudioMasterText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageChoiceButton> AudioKillButton = nullptr;

	TWeakObjectPtr<class UShowControlSubsystem> ShowControl;
	FTimerHandle RefreshTimer;
};
