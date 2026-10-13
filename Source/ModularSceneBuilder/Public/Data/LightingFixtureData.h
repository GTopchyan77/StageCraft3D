// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/BaseItemData.h"
#include "LightingFixtureData.generated.h"

UENUM(BlueprintType)
enum class EStageFixtureKind : uint8
{
	MovingHeadSpot	UMETA(DisplayName = "Moving Head (Spot)"),
	MovingHeadBeam	UMETA(DisplayName = "Moving Head (Beam)"),
	MovingHeadWash	UMETA(DisplayName = "Moving Head (Wash)"),
	LEDPar			UMETA(DisplayName = "LED Par"),
	Strobe			UMETA(DisplayName = "Strobe / Blinder"),
	Laser			UMETA(DisplayName = "Laser"),
};

/** How the fixture mixes color. Internally every fixture is edited in RGB(W); CMY is converted only at the DMX boundary. */
UENUM(BlueprintType)
enum class EStageFixtureColorSystem : uint8
{
	None	UMETA(DisplayName = "None (fixed white)"),
	RGB		UMETA(DisplayName = "RGB"),
	RGBW	UMETA(DisplayName = "RGBW"),
	CMY		UMETA(DisplayName = "CMY"),
};

/** One gobo on the fixture's gobo wheel. Slot 0 of the wheel is always "Open" and is not listed. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageGoboSlot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gobo")
	FText Name;

	/**
	 * Greyscale projection mask, white = light passes. Projected across the full beam cone (the texture's edges touch the beam
	 * edge), so the texture should be clamped with a black border.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gobo", meta = (AssetBundles = "Game"))
	TSoftObjectPtr<class UTexture2D> Texture;

	/** Fraction of the beam's light the gobo passes (its open area), for photometrics. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gobo", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Transmission = 0.4f;
};

/** One slot of the fixture's DMX mode (personality). */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageFixtureDMXChannel
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DMX", meta = (Categories = "StageCraft.Attribute"))
	FGameplayTag Attribute;

	/** 1-based channel within the mode, as printed in the fixture manual. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DMX", meta = (ClampMin = "1", ClampMax = "512"))
	int32 Channel = 1;

	/** 16-bit attribute: Channel is coarse, Channel + 1 is fine. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DMX")
	bool b16Bit = false;

	int32 GetWidth() const { return b16Bit ? 2 : 1; }
};

/**
 * Catalog entry for a lighting fixture: its technical capabilities and DMX mode. Placed instances
 * (ALightingFixtureActor) hold the live state: patch and attribute values.
 *
 * Mesh (inherited) is the static base; YokeMesh rotates with pan and HeadMesh with tilt. Static
 * fixtures (pars, strobes) leave YokeMesh/HeadMesh empty and set bHasPanTilt = false.
 */
UCLASS(BlueprintType, Const)
class MODULARSCENEBUILDER_API ULightingFixtureData : public UBaseItemData
{
	GENERATED_BODY()

public:
	ULightingFixtureData();

	//~ Begin UObject Interface
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
	//~ End UObject Interface

	/** True if the fixture can be controlled on this attribute (derived from capabilities, not the DMX layout). */
	UFUNCTION(BlueprintPure, Category = "Fixture")
	bool SupportsAttribute(FGameplayTag Attribute) const;

	/** Physical range of an attribute. Also the normalization range for DMX. Returns false if unsupported. */
	UFUNCTION(BlueprintPure, Category = "Fixture")
	bool GetAttributeRange(FGameplayTag Attribute, float& OutMin, float& OutMax) const;

	/** Value a freshly placed fixture starts with: open white at full, pan/tilt home, widest zoom. */
	UFUNCTION(BlueprintPure, Category = "Fixture")
	float GetAttributeDefault(FGameplayTag Attribute) const;

	/** All attributes this fixture exposes, in inspector order. */
	UFUNCTION(BlueprintPure, Category = "Fixture")
	TArray<FGameplayTag> GetSupportedAttributes() const;

	/** Number of DMX channels the mode occupies. */
	UFUNCTION(BlueprintPure, Category = "DMX")
	int32 GetDMXFootprint() const;

	bool HasZoom() const { return BeamAngleMax > BeamAngleMin; }

	bool HasColorTemperatureControl() const { return bHasColorTemperatureControl && ColorSystem != EStageFixtureColorSystem::None; }

	/** Gobo slots including Open (slot 0); 1 when the fixture has no gobo wheel. */
	int32 GetGoboSlotCount() const { return GoboWheel.Num() + 1; }

	/** Display name of a gobo slot ("Open" for slot 0). */
	FText GetGoboSlotName(int32 Slot) const;

	/** Projection texture of a gobo slot; null for Open, out-of-range slots and unset textures. Loads the texture if needed. */
	class UTexture2D* LoadGoboTexture(int32 Slot) const;

	/** Light a gobo slot passes, 0..1 (1 for Open). */
	float GetGoboTransmission(int32 Slot) const;

	/** True when the beam needs a projected mask (gobo, iris or framing shutters): the fixture then carries a light function. */
	bool HasBeamShaping() const { return !GoboWheel.IsEmpty() || bHasIris || bHasFramingShutters; }

	/**
	 * Attributes that hold a slot index rather than a continuous value (the gobo wheel). They are stored rounded, a cue fade
	 * steps through them like a console snaps a wheel, and DMX maps each slot to an equal range.
	 */
	static bool IsSteppedAttribute(const FGameplayTag& Attribute);

	/**
	 * DMX encoding of an attribute value, 0..1 of the channel's range. Continuous attributes map their range linearly; stepped
	 * ones (Gobo) map each slot to the centre of an equal share of the range, so a console sending any value inside a slot's
	 * share selects that slot. Returns 0 for unsupported attributes.
	 */
	float NormalizeForDMX(const FGameplayTag& Attribute, float Value) const;

	/** Inverse of NormalizeForDMX: the attribute value for a received 0..1 channel value (stepped: the slot whose share holds it). */
	float DenormalizeFromDMX(const FGameplayTag& Attribute, float Normalized) const;

#if WITH_EDITOR
	/** Replaces DMXChannels with a typical layout for the current capabilities (16-bit pan/tilt first, then dimmer, shutter, color, zoom). */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "DMX")
	void GenerateDefaultDMXLayout();
#endif

	// --- Fixture ---

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fixture")
	EStageFixtureKind FixtureKind = EStageFixtureKind::MovingHeadSpot;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fixture")
	EStageFixtureColorSystem ColorSystem = EStageFixtureColorSystem::RGBW;

	/** Correlated color temperature of the white source; used for fixed-white fixtures. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fixture", meta = (ClampMin = "1700.0", ClampMax = "12000.0", Units = "K"))
	float ColorTemperatureK = 6500.f;

	/** Colour temperature correction (CTC) on a colour-mixing fixture: a Kelvin attribute that tints the beam. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fixture", meta = (EditCondition = "ColorSystem != EStageFixtureColorSystem::None"))
	bool bHasColorTemperatureControl = false;

	/** CTC range in Kelvin (X = warmest, Y = coolest). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fixture", meta = (EditCondition = "bHasColorTemperatureControl"))
	FVector2D ColorTemperatureRangeK = FVector2D(2700.0, 8000.0);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fixture")
	bool bHasShutter = true;

	/** Strobe rate at the lowest and highest Shutter value above 0, in flashes per second. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fixture", meta = (EditCondition = "bHasShutter", ClampMin = "0.1", ClampMax = "50.0", Units = "Hz"))
	FVector2D StrobeRateHz = FVector2D(1.0, 20.0);

	// --- Optics ---

	/** Output at 100 % dimmer. Drives the spot light intensity in lumens. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Optics", meta = (ClampMin = "0.0", Units = "lm"))
	float LuminousFlux = 20000.f;

	/** Narrowest full beam angle. Equal to BeamAngleMax for fixtures without zoom. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Optics", meta = (ClampMin = "0.5", ClampMax = "170.0", Units = "deg"))
	float BeamAngleMin = 4.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Optics", meta = (ClampMin = "0.5", ClampMax = "170.0", Units = "deg"))
	float BeamAngleMax = 40.f;

	/** Distance the beam light reaches. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Optics", meta = (ClampMin = "100.0", Units = "cm"))
	float BeamRange = 4000.f;

	/** Rotating gobo wheel, in wheel order. Empty: the fixture has no gobos. Slot 0 (Open) is implicit. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Optics", meta = (TitleProperty = "Name"))
	TArray<FStageGoboSlot> GoboWheel;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Optics")
	bool bHasIris = false;

	/** Beam radius left by a fully closed iris, as a fraction of the open beam. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Optics", meta = (EditCondition = "bHasIris", ClampMin = "0.02", ClampMax = "0.9"))
	float IrisMinFraction = 0.1f;

	/** Four framing blades (top, right, bottom, left) and a rotating shutter assembly, as on a profile fixture. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Optics")
	bool bHasFramingShutters = false;

	// --- Motion ---

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Motion")
	bool bHasPanTilt = true;

	/** Pan range in degrees around home (X = min, Y = max). Typical moving heads: 540 degrees. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Motion", meta = (EditCondition = "bHasPanTilt"))
	FVector2D PanLimits = FVector2D(-270.0, 270.0);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Motion", meta = (EditCondition = "bHasPanTilt"))
	FVector2D TiltLimits = FVector2D(-135.0, 135.0);

	/** Part that rotates with pan, attached at PanPivotOffset. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Motion", meta = (AssetBundles = "Game", EditCondition = "bHasPanTilt"))
	TSoftObjectPtr<class UStaticMesh> YokeMesh;

	/** Part that rotates with tilt and emits the beam, attached at TiltPivotOffset (relative to the yoke). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Motion", meta = (AssetBundles = "Game"))
	TSoftObjectPtr<class UStaticMesh> HeadMesh;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Motion", meta = (Units = "cm"))
	FVector PanPivotOffset = FVector(0.0, 0.0, 10.0);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Motion", meta = (Units = "cm"))
	FVector TiltPivotOffset = FVector(0.0, 0.0, 25.0);

	/** Beam origin relative to the tilt pivot. The beam leaves along the head's local +Z (straight up when standing). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Motion", meta = (Units = "cm"))
	FVector BeamOriginOffset = FVector(0.0, 0.0, 15.0);

	// --- DMX ---

	/** Mode name as the manufacturer calls it, e.g. "Extended 24ch". */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "DMX")
	FName DMXModeName = TEXT("Standard");

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "DMX", meta = (TitleProperty = "Attribute"))
	TArray<FStageFixtureDMXChannel> DMXChannels;
};
