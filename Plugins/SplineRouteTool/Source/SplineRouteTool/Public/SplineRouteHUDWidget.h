// Copyright DamenTools internal. Spline Route Tool plugin.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "SplineRouteHUDWidget.generated.h"

class USlider;
class UTextBlock;
class UComboBoxString;
class UButton;
class UWidget;
class ASplineRouteActor;

/**
 * HUD widget for the spline-route demo.
 *
 * BindWidget contract:
 *   - TimeSlider      USlider     0.0 .. MaxTimeScale (subsystem-driven)
 *   - TimeScaleLabel  UTextBlock  optional, displays the current multiplier
 */
UCLASS()
class SPLINEROUTETOOL_API USplineRouteHUDWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

protected:
	/** Inner panel of the HUD — used for cursor-over-HUD hit-test so we don't block the whole viewport. */
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UWidget> Panel;

	UPROPERTY(meta = (BindWidget))
	TObjectPtr<USlider> TimeSlider;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> TimeScaleLabel;

	/** Stretch — vessel picker. */
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UComboBoxString> VesselPicker;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UButton> SpawnVesselButton;

	/**
	 * Time of Day / Weather dropdowns — moved in from the standalone SkyWeatherControls
	 * plugin's own top-right viewport panel (USkyWeatherControlsWidget), which is no longer
	 * spawned. Drives Ultra Dynamic Sky / Ultra Dynamic Weather via UUltraDynamicSkyBridge,
	 * same as before.
	 */
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UComboBoxString> TimeOfDayPicker;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UComboBoxString> WeatherPicker;

	/** Stretch — per-segment speed entry. */
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<USlider> SegmentSpeedSlider;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> SelectedSegmentLabel;

	/** Waypoint cycle menu — select previous / next control point without having to click the handle. */
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UButton> CyclePrevButton;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UButton> CycleNextButton;

	/** Shows "Waypoint i / N" for the currently selected point. */
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> SelectedPointLabel;

	/** Global turn-rate (deg/sec) slider — drives USplineRouteAuthoringSubsystem::SetTurnRateForAllVessels. */
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<USlider> TurnRateSlider;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> TurnRateLabel;

	/** Toggles the selected waypoint as a reverse (stern-first) node. */
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UButton> ReverseToggleButton;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> ReverseLabel;

	// --- Bottom control bar: playback orchestrator, scenario, route medium, obstructions ---

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UButton> PlayButton;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UButton> PauseButton;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UButton> StopButton;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UButton> SaveButton;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UButton> LoadButton;

	/** Destroys every live vessel (both kinds), leaving routes/waypoints and obstructions untouched. */
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UButton> DeleteAllVesselsButton;

	/** Clears every control point on every route, leaving vessels and obstructions untouched. */
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UButton> DeleteAllWaypointsButton;

	/** Flips the active route between water (vessel) and land (ground vehicle). */
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UButton> RouteTypeButton;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> RouteTypeLabel;

	/** Toggles obstruction-placement mode (F drops an obstruction instead of a waypoint). */
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UButton> ObstructionModeButton;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UButton> ObsPrevButton;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UButton> ObsNextButton;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> ObstructionLabel;

	/** Live waypoint-size slider — drives ASplineRouteActor::SetHandleSize on the active route. */
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<USlider> HandleSizeSlider;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> HandleSizeLabel;

	/** Live ship-size slider — drives USplineRouteAuthoringSubsystem::SetVesselSizeMultiplier. */
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<USlider> VesselSizeSlider;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> VesselSizeLabel;

	/** Min-spawn-gap slider — drives USplineRouteAuthoringSubsystem::SetMinSpawnGap. */
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<USlider> MinSpawnGapSlider;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> MinSpawnGapLabel;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Spline Route|HUD", meta = (ClampMin = "0.05"))
	float MinVesselSize = 0.25f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Spline Route|HUD", meta = (ClampMin = "0.05"))
	float MaxVesselSize = 10.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Spline Route|HUD", meta = (ClampMin = "0.05"))
	float DefaultVesselSize = 1.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Spline Route|HUD", meta = (ClampMin = "1.0"))
	float MinHandleSize = 20.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Spline Route|HUD", meta = (ClampMin = "1.0"))
	float MaxHandleSize = 400.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Spline Route|HUD", meta = (ClampMin = "1.0"))
	float DefaultHandleSize = 300.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Spline Route|HUD", meta = (ClampMin = "0.0"))
	float MinTurnRate = 0.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Spline Route|HUD", meta = (ClampMin = "0.0"))
	float MaxTurnRate = 180.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Spline Route|HUD", meta = (ClampMin = "0.0"))
	float DefaultTurnRate = 90.0f;

	/** Max value used for the segment-speed slider. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Spline Route|HUD")
	float MaxSegmentSpeed = 500.0f;

	/** Min value for the segment-speed slider. Kept non-zero so mid-drag intermediates never freeze the vessel. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Spline Route|HUD")
	float MinSegmentSpeed = 50.0f;

	/** Slider position used when no override is set on the selected segment (visual default). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Spline Route|HUD")
	float DefaultSegmentSpeedDisplay = 200.0f;

private:
	UFUNCTION()
	void HandleSliderChanged(float NewValue);

	UFUNCTION()
	void HandleTimeScaleChanged(float NewScale);

	UFUNCTION()
	void HandleVesselPicked(FString SelectedItem, ESelectInfo::Type SelectionType);

	UFUNCTION()
	void HandleSpawnVesselClicked();

	UFUNCTION()
	void HandleTimeOfDayChanged(FString SelectedItem, ESelectInfo::Type SelectionType);

	UFUNCTION()
	void HandleWeatherChanged(FString SelectedItem, ESelectInfo::Type SelectionType);

	/** Re-enables SpawnVesselButton after the brief post-spawn disable window. */
	void ReenableSpawnButton();

	UFUNCTION()
	void HandleSegmentSpeedChanged(float NewValue);

	UFUNCTION()
	void HandleHandleSizeChanged(float NewValue);

	UFUNCTION()
	void HandleVesselSizeChanged(float NewValue);

	UFUNCTION()
	void HandleMinSpawnGapChanged(float NewValue);

	UFUNCTION()
	void HandleTurnRateChanged(float NewValue);

	UFUNCTION()
	void HandleReverseToggleClicked();

	UFUNCTION() void HandlePlayClicked();
	UFUNCTION() void HandlePauseClicked();
	UFUNCTION() void HandleStopClicked();
	UFUNCTION() void HandleSaveClicked();
	UFUNCTION() void HandleLoadClicked();
	UFUNCTION() void HandleDeleteAllVesselsClicked();
	UFUNCTION() void HandleDeleteAllWaypointsClicked();
	UFUNCTION() void HandleRouteTypeClicked();
	UFUNCTION() void HandleObstructionModeClicked();
	UFUNCTION() void HandleObsPrevClicked();
	UFUNCTION() void HandleObsNextClicked();

	/** Refresh the route-medium label + obstruction mode/type labels from subsystem state. */
	void RefreshControlBar();

	UFUNCTION()
	void HandleActiveRouteChanged(ASplineRouteActor* NewRoute);

	UFUNCTION()
	void HandleSelectedControlPointChanged(int32 NewIndex);

	UFUNCTION()
	void HandleCyclePrevClicked();

	UFUNCTION()
	void HandleCycleNextClicked();

	/**
	 * Hides the entire HUD (`this` — every direct child of the root canvas, no exceptions: the recording
	 * control widget lives entirely outside this widget tree, in NGGVideoRecorder's own separate Slate
	 * panel, so there's nothing here to exclude) the instant a recording is requested, before
	 * NGGVideoRecorder's capture hook goes live one tick later — so the hide is guaranteed to have
	 * already rendered before the first frame can be captured.
	 */
	UFUNCTION()
	void HandleRecordingWillStart(const FString& FilePath);

	/** Restores the HUD's pre-recording visibility once NGGVideoRecorder confirms the capture has actually stopped. */
	UFUNCTION()
	void HandleRecordingFinished(bool bSuccess, const FString& FilePath, const FString& Error);

	/** Update SelectedPointLabel ("Waypoint i / N") from the active route's selection. */
	void RefreshWaypointLabel() const;

	void RefreshLabel(float Scale) const;
	void PopulateVesselPicker();
	/** Fills TimeOfDayPicker/WeatherPicker and seeds their initial selection from live UDS state. */
	void PopulateSkyWeatherControls();
	void RefreshSegmentSpeedUI();
	void RefreshHandleSizeLabel(float Size) const;
	void ApplyHandleSizeToActiveRoute(float NewSize);
	void RefreshVesselSizeLabel(float Size) const;
	void RefreshMinSpawnGapLabel(float Value) const;
	void RefreshTurnRateLabel(float Rate) const;
	/** Sync the reverse checkbox + label to the selected point (and disable when nothing is selected). */
	void RefreshReverseUI();
	void BindRouteSelection(ASplineRouteActor* Route);
	void UnbindRouteSelection(ASplineRouteActor* Route);

	/**
	 * Reconfigure the four sliders from the catalog's global slider-range fields.
	 * Any range with Max <= Min falls back to the HUD's built-in Min/Max/Default* UPROPERTYs.
	 * If bResetToDefault is true (first-time setup), each slider's value is set to the catalog's
	 * Default. Otherwise current values are preserved (Slate auto-clamps into the new range).
	 */
	void ApplyCatalogSliderRanges(bool bResetToDefault);

	UPROPERTY(Transient)
	TObjectPtr<ASplineRouteActor> BoundRoute;

	/** Timer that re-enables SpawnVesselButton 0.5s after a successful spawn. */
	FTimerHandle SpawnButtonReenableTimer;

	/** Visibility captured right before HandleRecordingWillStart hides the whole HUD, so it's restored exactly (not assumed). */
	ESlateVisibility CachedVisibilityBeforeRecording = ESlateVisibility::Visible;
};
