// Copyright DamenTools internal. Sky & Weather Controls plugin.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "SkyWeatherControlsWidget.generated.h"

class UComboBoxString;
class UPanelWidget;
class UWidget;

/**
 * Viewport overlay holding two dropdowns -- Time of Day and Weather -- wired to Ultra
 * Dynamic Sky / Ultra Dynamic Weather via UUltraDynamicSkyBridge.
 *
 * The widget tree is built in C++ so the panel needs no companion Widget Blueprint and
 * works in any level that has UDS in it. Deriving a Widget Blueprint from this class is
 * still supported for restyling: the tree is only built when the class does not already
 * supply one.
 */
UCLASS(Blueprintable, BlueprintType)
class SKYWEATHERCONTROLS_API USkyWeatherControlsWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Corner of the viewport the panel docks to. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky & Weather Controls")
	FVector2D ScreenAnchor = FVector2D(1.0f, 0.0f);

	/** Inset from the anchored corner, in slate units. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky & Weather Controls")
	FVector2D ScreenPadding = FVector2D(28.0f, 28.0f);

	/** Applies the currently selected dropdown entries to UDS/UDW. */
	UFUNCTION(BlueprintCallable, Category = "Sky & Weather Controls")
	void ApplyCurrentSelection();

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

	UFUNCTION()
	void HandleTimeOfDayChanged(FString SelectedItem, ESelectInfo::Type SelectionType);

	UFUNCTION()
	void HandleWeatherChanged(FString SelectedItem, ESelectInfo::Type SelectionType);

private:
	void BuildControlPanel();
	UWidget* BuildLabelledDropdown(const TCHAR* Label, TObjectPtr<UComboBoxString>& OutComboBox);
	void PopulateOptions();

	UPROPERTY(Transient)
	TObjectPtr<UComboBoxString> TimeOfDayBox;

	UPROPERTY(Transient)
	TObjectPtr<UComboBoxString> WeatherBox;

	/** Set while seeding initial selections, so priming the boxes does not fire UDS calls. */
	bool bSuppressSelectionCallbacks = false;
};
