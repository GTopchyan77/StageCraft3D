// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Audio/StageAudioReactiveTypes.h"
#include "Blueprint/UserWidget.h"
#include "Engine/TimerHandle.h"
#include "Styling/SlateTypes.h"
#include "Types/SlateEnums.h"
#include "StageAudioPanel.generated.h"

/**
 * The Audio panel (StageCraft.Panel.Audio, Docs/ADR/0029-audio-analysis-and-audio-reactive-effects.md §5): the music audio effects
 * follow.
 *  - the source: OFF, FILE... (a song from disk, played here), INPUT (a line or microphone, analysed only), SYSTEM (whatever this PC
 *    plays, such as YouTube in a browser) or TEST (a built-in beat);
 *  - for a file: play / pause, stop, loop and a seek bar with the position; for the input: the device and its gain;
 *  - meters for the four bands and the level, the tempo with its confidence and a lamp on the beat;
 *  - "keep playing in the background", so music and analysis go on while another window has focus;
 *  - the latency offset of the source playing (the lights earlier or later than the sound), the flash limit of this PC with a
 *    warning above 3 per second, and LISTENING while a microphone, line input or the PC's own output is being analysed (Phase G3,
 *    ADR 0029 §6, §7).
 *
 * A view in the project's binding pattern: reads UStageAudioReactiveSubsystem and owns no audio state. OnAudioStatusChanged refreshes
 * it on the next tick, never inside the callback. The meters move from a timer that runs only while the panel is shown and a source
 * is running: one copy of the analyser's newest frame per update, no world access. Writes go through AModularPlayerController's audio
 * requests (judged by the GameMode); a refused change snaps back on the next refresh. None is undoable: they are live device actions.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageAudioPanel : public UUserWidget
{
	GENERATED_BODY()

public:
	/** The meters update this often while a source runs. */
	static constexpr float MeterIntervalSeconds = 1.f / 30.f;

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Audio")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	/** The band meters, in EStageAudioBand order. */
	static constexpr int32 MeterCount = 5;

	void BuildTree();
	class UWidget* BuildSourceRow();
	class UWidget* BuildFileRow();
	class UWidget* BuildInputRow();
	class UWidget* BuildMeters();
	class UWidget* BuildSafetyRows();
	/** A slider with its value text, reporting to HandleSliderMoved as SliderIndex and released through HandleSliderReleased. */
	class UWidget* MakeSliderLine(int32 SliderIndex, float StepSize, TWeakObjectPtr<class UStageValueSlider>& OutSlider, TWeakObjectPtr<class UTextBlock>& OutValue);
	class UWidget* BuildFooter();
	class UWidget* MakeLabeledRow(const FText& Label, class UWidget* Content);

	class UStageChoiceButton* MakeButton(const FText& Label, const FText& ToolTip, int32 ChoiceIndex);
	void SetButtonLit(class UStageChoiceButton* Button, bool bLit) const;
	FButtonStyle MakeButtonStyle(bool bLit) const;
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color);
	const class UStageCraftUITheme& GetTheme() const;
	class AModularPlayerController* GetStageController() const;
	class UStageAudioReactiveSubsystem* GetAudio() const;

	void RequestRefresh();
	void Refresh();
	void RefreshDevices();
	/** Starts the meter timer while a source runs; clears it otherwise. */
	void UpdateMeterTimer(bool bRunning);
	void UpdateMeters();

	void HandleStatusChanged();
	void HandleSourcePicked(int32 SourceIndex);
	void HandleTransportPicked(int32 CommandIndex);
	void HandleBackgroundPicked(int32 ChoiceIndex);
	void HandleSliderMoved(int32 SliderIndex, float Value);

	UFUNCTION()
	void HandleSliderReleased();

	UFUNCTION()
	void HandleDeviceChanged(FString SelectedItem, ESelectInfo::Type SelectionType);

	/** Built once in BuildTree; the widget tree owns the widgets (weak references here). */
	TArray<TWeakObjectPtr<class UStageChoiceButton>> SourceButtons;
	TWeakObjectPtr<class UWidget> FileRow;
	TWeakObjectPtr<class UWidget> InputRow;
	TWeakObjectPtr<class UTextBlock> FileName;
	TWeakObjectPtr<class UStageChoiceButton> PlayButton;
	TWeakObjectPtr<class UStageChoiceButton> LoopButton;
	TWeakObjectPtr<class UStageValueSlider> SeekSlider;
	TWeakObjectPtr<class UTextBlock> PositionText;
	TWeakObjectPtr<class UStageThemedComboBox> DeviceCombo;
	TWeakObjectPtr<class UStageValueSlider> GainSlider;
	TWeakObjectPtr<class UTextBlock> GainText;
	TWeakObjectPtr<class UWidget> LatencyRow;
	TWeakObjectPtr<class UStageValueSlider> LatencySlider;
	TWeakObjectPtr<class UTextBlock> LatencyText;
	TWeakObjectPtr<class UStageValueSlider> FlashSlider;
	TWeakObjectPtr<class UTextBlock> FlashText;
	TWeakObjectPtr<class UTextBlock> FlashWarning;
	TWeakObjectPtr<class UTextBlock> ListeningText;
	TWeakObjectPtr<class UProgressBar> Meters[MeterCount];
	TWeakObjectPtr<class UTextBlock> TempoText;
	TWeakObjectPtr<class UBorder> BeatLamp;
	TWeakObjectPtr<class UStageChoiceButton> BackgroundButton;
	TWeakObjectPtr<class UTextBlock> DetailText;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> StatusText = nullptr;

	/** The input devices in the combo's order, as ids; index 0 is the system default (empty id). */
	TArray<FString> DeviceIds;
	/** View state while the seek bar is dragged: the position it shows, not yet sent. */
	TOptional<double> SeekPreviewSeconds;
	/** The status as of the last refresh. */
	FStageAudioStatus Status;
	EStageAudioSource LitSource = EStageAudioSource::Off;
	/** The combo is being filled from the settings, not picked by the user. */
	bool bFillingDevices = false;
	/** The setting slider (gain, latency, flash limit) being dragged: its release applies the new value. INDEX_NONE when none. */
	int32 DraggedSetting = INDEX_NONE;
	/** Where the song dialog opens: the last folder a song came from, this session. */
	FString LastFileDirectory;

	TWeakObjectPtr<class UStageAudioReactiveSubsystem> Audio;
	FDelegateHandle StatusHandle;
	FTimerHandle RefreshTimer;
	FTimerHandle MeterTimer;
};
