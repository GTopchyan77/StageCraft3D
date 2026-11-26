// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Input/StageKeyBindings.h"
#include "StagePreferencesPanel.generated.h"

/**
 * The Preferences panel (StageCraft.Panel.Preferences, Docs/ADR/0013-preferences-key-bindings-and-audit-fixes.md §2.5): camera
 * look, viewport and audio preferences and the keyboard shortcuts. Opens from Edit › Preferences… or the Window menu.
 *
 * A view in the project's binding pattern:
 *  - Every value is read from its owner and written back through that owner's single write path: the key map and look
 *    preferences through UStageInputPreferencesSubsystem, snapping through UStageSnappingComponent, the grid through
 *    UStageViewportComponent, audio through UStageAudioSubsystem, graphics through UStageGraphicsSubsystem. The panel re-reads after each owner's change event, so a
 *    refused or clamped change shows the stored value. It never writes UStageCraftUserSettings.
 *  - Bound in NativeConstruct, unbound in NativeDestruct. No tick.
 *
 * Capturing a shortcut: clicking a binding button arms that slot and gives the panel keyboard focus, so the keys never reach
 * the viewport. The next key that is not a modifier, with the modifiers held, becomes the binding. CLEAR unbinds the slot;
 * losing focus (a click elsewhere) cancels. A refusal (conflict, unsupported key) is shown and changes nothing.
 *
 * The widget tree is built in code from the theme, like the Environment panel; there is no Widget Blueprint.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStagePreferencesPanel : public UUserWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	virtual void NativeOnFocusLost(const FFocusEvent& InFocusEvent) override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Preferences")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	/** The slot being captured. */
	struct FCaptureTarget
	{
		EStageInputCommand Command = EStageInputCommand::Count;
		EStageKeySlot Slot = EStageKeySlot::Primary;
	};

	void BuildTree();
	class UWidget* BuildHeader();
	class UWidget* BuildSectionTitle(const FText& Title);
	class UWidget* BuildChoiceRow(const FText& Label, const FText& Tip, TConstArrayView<FText> Choices,
		TArray<TObjectPtr<class UStageChoiceButton>>& OutButtons, void (UStagePreferencesPanel::*Handler)(int32));
	class UWidget* BuildSliderRow(const FText& Label, const FText& Tip, int32 SliderIndex, float Min, float Max);
	class UWidget* BuildShortcutSection();
	class UWidget* BuildCommandRow(const struct FStageInputCommandInfo& Info);
	class UStageChoiceButton* MakeButton(const FText& Label, const FText& Tip, int32 Index);
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color);
	const class UStageCraftUITheme& GetTheme() const;

	void Refresh();
	void StyleChoice(class UStageChoiceButton& Button, bool bSelected) const;
	void SetNotice(const FText& Text, bool bWarning);

	void HandleLookInvertPicked(int32 Index);
	void HandleLookLinkPicked(int32 Index);
	void HandleSnapItemsPicked(int32 Index);
	void HandleGridPicked(int32 Index);
	void HandleMutePicked(int32 Index);
	void HandleSliderMoved(int32 SliderIndex, float Value);

	/** GRAPHICS (STATE.md #111): quality, render resolution, frame cap and VSync, written through UStageGraphicsSubsystem. */
	void HandleQualityPicked(int32 Index);
	void HandleResolutionPicked(int32 Index);
	void HandleFrameCapPicked(int32 Index);
	void HandleVSyncPicked(int32 Index);
	/** Sends the stored choices with one row changed to the owner; the panel re-reads on its change event. */
	void ApplyGraphics(TFunctionRef<void(struct FStageGraphicsChoices&)> Change);
	void RefreshGraphics();

	/**
	 * REMOTE CONTROL (Docs/ADR/0035): OSC scope, MIDI input and output, the MIDI MAP editor (§11) and the status line. Written through
	 * the controller's requests. The map editor's rows are built once and only relabelled: LEARN and CLEAR change the map from inside
	 * their own click, so nothing may be rebuilt then (Rules.md Safe data binding).
	 */
	class UWidget* BuildRemoteSection();
	/** One MIDI MAP row: the target's name, its controls, LEARN and CLEAR. Index is into StagePreferencesPanel::MidiTargets. */
	class UWidget* BuildMidiTargetRow(int32 Index);
	void HandleOscPicked(int32 Index);
	void HandleMidiPicked(int32 Index);
	void HandleMidiOutputPicked(int32 Index);
	/** RESET TO DEFAULT, CLEAR ALL. */
	void HandleMidiMapPicked(int32 Index);
	/** The playback (1–10) the playback rows of MIDI MAP edit. */
	void HandleMidiPlaybackPicked(int32 Index);
	/** LEARN on target row Index: waits for a control, or cancels when that row is already waiting. */
	void HandleMidiLearnPicked(int32 Index);
	void HandleMidiClearPicked(int32 Index);
	void ApplyRemoteSettings(const struct FStageRemoteSettings& Settings);
	void RefreshRemote();
	void RefreshMidiMap(const struct FStageRemoteSettings& Settings, const struct FStageRemoteStatus* Status);
	/** The playback target row Index edits: MidiMapPlayback for the playback rows, 1 (ignored) for the others. */
	int32 MidiTargetPlayback(int32 Index) const;

	/**
	 * WEB REMOTE (Docs/ADR/0036): scope, remembering devices, NEW PIN and UNPAIR ALL, a QR code that opens the page and pairs, the paired
	 * devices with FORGET, and a status line with the addresses and the PIN. Written through the controller's requests.
	 */
	class UWidget* BuildWebRemoteSection();
	void HandleWebScopePicked(int32 Index);
	void HandleWebRememberPicked(int32 Index);
	void HandleWebPairingPicked(int32 Index);
	/** OTHER ADDRESS: the QR code moves to the next of this PC's addresses (the first is a guess when there are several cards). */
	void HandleWebQrPicked(int32 Index);
	/** FORGET on device row Index. */
	void HandleWebForgetPicked(int32 Index);
	void RefreshWebRemote();
	/** Redraws the QR texture only when its text changes, so the status line's counters do not rebuild it four times a second. */
	void RefreshWebQr(const struct FStageWebRemoteStatus* Status);
	void RefreshWebDevices(const struct FStageWebRemoteStatus* Status);

	/** STAGECRAFT PRO (Docs/ADR/0034 §17): the licence's status and the subscription actions, all through the controller. */
	class UWidget* BuildProSection();
	void HandleProSubscribePicked(int32 Index);
	void HandleProLicencePicked(int32 Index);
	void RefreshPro();

	/** A binding button: Index = command * 2 + slot. Arms capture for that slot (or disarms it when it is already armed). */
	void HandleBindingPicked(int32 Index);
	void HandleResetPicked(int32 CommandIndex);
	void HandleClearPicked(int32 Unused);
	void HandleResetAllPicked(int32 Unused);

	void BeginCapture(const FCaptureTarget& Target);
	void EndCapture();
	/** The only writer of Capture: keeps the input preferences' key-capture flag in step with it. */
	void SetCapture(const TOptional<FCaptureTarget>& Target);

	UFUNCTION()
	void HandlePreferencesChanged();

	class UStageInputPreferencesSubsystem* GetInputPreferences() const;
	class UStageAudioSubsystem* GetAudio() const;
	class UStageSnappingComponent* GetSnapping() const;
	class UStageViewportComponent* GetViewport() const;

	TOptional<FCaptureTarget> Capture;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> LookInvertButtons;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> LookLinkButtons;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> SnapItemsButtons;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> GridButtons;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> MuteButtons;

	/** Low, Medium, High, Epic; none selected when the saved levels are Custom. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> QualityButtons;

	/** One per StageGraphics::GetResolutionPresets entry; none selected for a hand-edited value. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> ResolutionButtons;

	/** One per StageGraphics::GetFrameCapPresets entry; none selected for a hand-edited value. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> FrameCapButtons;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> VSyncButtons;

	TWeakObjectPtr<class UStageGraphicsSubsystem> BoundGraphics;
	FDelegateHandle GraphicsHandle;

	/** Look sensitivity, master volume, effects volume (StagePreferencesPanel::ESlider order). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageValueSlider>> Sliders;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UTextBlock>> SliderValues;

	/** Two per command, indexed command * 2 + slot. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> BindingButtons;

	/** One per command. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> ResetButtons;

	UPROPERTY(Transient)
	TObjectPtr<class UStageChoiceButton> ClearButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> NoticeText = nullptr;

	TWeakObjectPtr<class UStageInputPreferencesSubsystem> BoundPreferences;
	TWeakObjectPtr<class UStageAudioSubsystem> BoundAudio;
	TWeakObjectPtr<class UStageSnappingComponent> BoundSnapping;
	TWeakObjectPtr<class UStageViewportComponent> BoundViewport;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> OscButtons;

	/** Off, then one per MIDI input found when the panel was built (MidiInputNames). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> MidiButtons;

	TArray<FString> MidiInputNames;

	/** Off, then one per MIDI output found when the panel was built (MidiOutputNames). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> MidiOutputButtons;

	TArray<FString> MidiOutputNames;

	/** RESET TO DEFAULT, CLEAR ALL: actions, never shown as selected. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> MidiMapButtons;

	/** Playbacks 1–10 for the playback rows of MIDI MAP. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> MidiPlaybackButtons;

	/** One per MIDI MAP target row: the controls that drive it, LEARN, CLEAR. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UTextBlock>> MidiTargetTexts;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> MidiLearnButtons;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> MidiClearButtons;

	/** The playback (1–10) the playback rows of MIDI MAP show; view state only. */
	int32 MidiMapPlayback = 1;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> RemoteStatusText = nullptr;

	TWeakObjectPtr<class UStageRemoteControlSubsystem> BoundRemote;
	FDelegateHandle RemoteStatusHandle;

	/** Off, This PC, Network. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> WebScopeButtons;

	/** NEW PIN, UNPAIR ALL: actions, never shown as selected. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> WebPairingButtons;

	/** Off, On (bRememberDevices). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> WebRememberButtons;

	/** OTHER ADDRESS: an action, never shown as selected. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> WebQrButtons;

	/** The OTHER ADDRESS row, shown only when this PC has more than one address to offer. */
	UPROPERTY(Transient)
	TObjectPtr<class UWidget> WebQrRow = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UImage> WebQrImage = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> WebQrCaption = nullptr;

	/** The QR code drawn for WebQrText; transient, owned by this panel. */
	UPROPERTY(Transient)
	TObjectPtr<class UTexture2D> WebQrTexture = nullptr;

	FString WebQrText;
	int32 WebQrAddressIndex = 0;

	/**
	 * One row per possible device (StageWebRemote::MaxPairedDevices), built once and shown or hidden: FORGET changes the list from
	 * inside its own click, so the rows must not be rebuilt then (Rules.md Safe data binding).
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UWidget>> WebDeviceRows;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UTextBlock>> WebDeviceTexts;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> WebForgetButtons;

	/** The device Id each row's FORGET forgets. */
	TArray<FString> WebDeviceIds;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> WebStatusText = nullptr;

	TWeakObjectPtr<class UStageWebRemoteSubsystem> BoundWebRemote;
	FDelegateHandle WebStatusHandle;

	/** SUBSCRIBE MONTHLY, SUBSCRIBE YEARLY: actions, never shown as selected. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> ProSubscribeButtons;

	/** REFRESH, MANAGE, RELEASE THIS PC: actions, never shown as selected. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> ProLicenceButtons;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> ProStatusText = nullptr;

	TWeakObjectPtr<class UStageLicenceSubsystem> BoundLicence;
	FDelegateHandle LicenceHandle;

	/** The cloud's sign-in state decides which Pro actions can run; bound with the licence. */
	TWeakObjectPtr<class UStageCloudSubsystem> BoundCloud;
	FDelegateHandle CloudHandle;
};
