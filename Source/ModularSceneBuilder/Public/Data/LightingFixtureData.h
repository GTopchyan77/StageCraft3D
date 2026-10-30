// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/BaseItemData.h"
#include "LightingFixtureData.generated.h"

/** Fixture type, for the catalog, the assistant ("all the washes") and reports. New types are appended: assets store the name. */
UENUM(BlueprintType)
enum class EStageFixtureKind : uint8
{
	MovingHeadSpot		UMETA(DisplayName = "Moving Head (Spot)"),
	MovingHeadBeam		UMETA(DisplayName = "Moving Head (Beam)"),
	MovingHeadWash		UMETA(DisplayName = "Moving Head (Wash)"),
	LEDPar				UMETA(DisplayName = "LED Par"),
	Strobe				UMETA(DisplayName = "Strobe"),
	Laser				UMETA(DisplayName = "Laser"),
	MovingHeadHybrid	UMETA(DisplayName = "Moving Head (Hybrid)"),
	Profile				UMETA(DisplayName = "Profile (Ellipsoidal)"),
	Fresnel				UMETA(DisplayName = "Fresnel"),
	Blinder				UMETA(DisplayName = "Blinder"),
	PixelBar			UMETA(DisplayName = "Pixel Bar"),
};

/**
 * How the fixture makes colour (Docs/ADR/0020-professional-fixture-suite.md §2). Additive systems (RGB...) mix emitters; CMY
 * filters the lamp's white. Colour is stored as R/G/B(/W/A) attributes for every system: on CMY they are the filters'
 * transmissions (C = 1 - R), converted only at the DMX boundary.
 */
UENUM(BlueprintType)
enum class EStageFixtureColorSystem : uint8
{
	None	UMETA(DisplayName = "None (fixed white)"),
	RGB		UMETA(DisplayName = "RGB"),
	RGBW	UMETA(DisplayName = "RGBW"),
	CMY		UMETA(DisplayName = "CMY"),
	RGBA	UMETA(DisplayName = "RGBA"),
	RGBWA	UMETA(DisplayName = "RGBWA"),
};

/** What frames the beam's edge: a profile's hard framing shutters, or a fresnel's or par's soft barndoors. */
UENUM(BlueprintType)
enum class EStageBeamShaper : uint8
{
	None,
	FramingShutters	UMETA(DisplayName = "Framing Shutters"),
	Barndoors,
};

/** What each cell of a multi-cell fixture controls. */
UENUM(BlueprintType)
enum class EStageCellControl : uint8
{
	/** Colour per cell (a pixel bar): the fixture's colour system, per cell. */
	Color,
	/** Intensity per cell (a blinder's lamps). */
	Dimmer,
	/** Both. */
	DimmerAndColor	UMETA(DisplayName = "Dimmer and Color"),
};

/** Which gobo wheel a slot query is about. */
enum class EStageGoboWheel : uint8
{
	/** The rotating wheel (Attribute_Gobo, indexable and spinning). */
	Rotating,
	/** The static wheel (Attribute_Gobo2). */
	Static,
};

/** One gobo on a gobo wheel. Slot 0 of a wheel is always "Open" and is not listed. */
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

/** One emitter of an additive colour system, in attribute order (R, G, B, then W, then A). */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageColorEmitter
{
	GENERATED_BODY()

	/** Linear colour of the emitter's light (its chromaticity; brightness comes from RelativeFlux). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Color")
	FLinearColor Color = FLinearColor::White;

	/** Light output at full relative to the other emitters. The fixture's LuminousFlux is the output with every emitter at full. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Color", meta = (ClampMin = "0.0"))
	float RelativeFlux = 1.f;
};

/** One filter on a colour wheel. Slot 0 of the wheel is always "Open" and is not listed. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageColorWheelSlot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Color")
	FText Name;

	/** Linear transmission per channel (white = clear). The lamp's white times this is the beam colour. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Color")
	FLinearColor Filter = FLinearColor::White;
};

/** One prism. It splits the beam into Facets copies around the axis (circular) or along a line (linear). */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStagePrism
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prism")
	FText Name;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prism", meta = (ClampMin = "2", ClampMax = "16"))
	int32 Facets = 3;

	/** Copies in a row instead of a ring. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prism")
	bool bLinear = false;

	/** Distance of each copy's centre from the beam axis, in beam radii. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prism", meta = (ClampMin = "0.1", ClampMax = "2.0"))
	float Spread = 0.6f;

	/** Fraction of the light the prism passes. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Prism", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Transmission = 0.85f;
};

/**
 * The fixture's body, built from engine basic shapes scaled to these sizes (Docs/ADR/0020 §1): the base (Mesh), the yoke
 * (YokeMesh), the head (HeadMesh, a cylinder along the beam) and the lens or cell strip. Zero sizes leave a part at its mesh's
 * own size, as before #49.
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageFixtureBody
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Body", meta = (ClampMin = "0.0", Units = "cm"))
	FVector BaseSizeCm = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Body", meta = (ClampMin = "0.0", Units = "cm"))
	FVector YokeSizeCm = FVector::ZeroVector;

	/** Head as a box (width, depth, length along the beam); a pixel bar's or blinder's housing. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Body", meta = (ClampMin = "0.0", Units = "cm"))
	FVector HeadSizeCm = FVector::ZeroVector;

	/** Diameter of the lens, the glowing disc at the beam origin. Zero: no lens drawn. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Body", meta = (ClampMin = "0.0", Units = "cm"))
	float LensDiameterCm = 0.f;

	/** Length (X) and width (Y) of the glowing strip the cells sit on, for multi-cell fixtures. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Body", meta = (ClampMin = "0.0", Units = "cm"))
	FVector2D CellStripSizeCm = FVector2D::ZeroVector;
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
 * Catalog entry for a lighting fixture: its technical capabilities, body, photometrics and DMX mode. Placed instances
 * (ALightingFixtureActor) hold the live state: patch and attribute values.
 *
 * Mesh (inherited) is the static base; YokeMesh rotates with pan and HeadMesh with tilt. Static fixtures (pars, profiles,
 * strobes, bars) leave YokeMesh empty and set bHasPanTilt = false. Every capability turns into attributes
 * (GetSupportedAttributes), so cues, DMX, scenes and the inspector follow the data without fixture-specific code.
 */
UCLASS(BlueprintType, Const)
class MODULARSCENEBUILDER_API ULightingFixtureData : public UBaseItemData
{
	GENERATED_BODY()

public:
	ULightingFixtureData();

	//~ Begin UObject Interface
	virtual void PostLoad() override;
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

	/** All attributes this fixture exposes, in inspector order (cells last). */
	UFUNCTION(BlueprintPure, Category = "Fixture")
	TArray<FGameplayTag> GetSupportedAttributes() const;

	/** Number of DMX channels the mode occupies. */
	UFUNCTION(BlueprintPure, Category = "DMX")
	int32 GetDMXFootprint() const;

	bool HasZoom() const { return BeamAngleMax > BeamAngleMin; }

	bool HasColorTemperatureControl() const { return bHasColorTemperatureControl && IsColorMixing(); }

	/** Mixes colour by itself (additive emitters or CMY); a gel frame does not count. */
	bool IsColorMixing() const { return ColorSystem != EStageFixtureColorSystem::None; }

	bool IsAdditive() const { return IsColorMixing() && ColorSystem != EStageFixtureColorSystem::CMY; }

	/** A white-only fixture with a gel frame: R/G/B hold the gel's transmission, with no DMX channels. */
	bool HasGelFrame() const { return !IsColorMixing() && bAcceptsGel && !HasCells(); }

	/** Takes a gel from the library: a gel frame, or a colour mixer that matches the gel's colour. */
	bool AcceptsGelPreset() const { return IsColorMixing() || HasGelFrame(); }

	/** Several individually controlled cells; their colour or level replaces the fixture's master colour. */
	bool HasCells() const { return CellCount > 1; }

	bool HasCellColor() const { return HasCells() && CellControl != EStageCellControl::Dimmer && IsColorMixing(); }
	bool HasCellDimmer() const { return HasCells() && CellControl != EStageCellControl::Color; }

	bool HasBlades() const { return BeamShaper != EStageBeamShaper::None; }

	bool HasPrisms() const { return !Prisms.IsEmpty(); }

	/** Colour attributes of the colour system, in emitter order (R, G, B, W, A); empty for white-only fixtures. */
	TArray<FGameplayTag> GetColorAttributes() const;

	/**
	 * Each additive emitter's output at full (colour x relative flux), in GetColorAttributes order. Emitters left empty in the
	 * asset get the colour system's defaults: Rec. 709 primaries, a white emitter at WhiteEmitterKelvin, amber at about 590 nm.
	 */
	TArray<FLinearColor> GetEmitterOutputs() const;

	/** Gobo slots of a wheel including Open (slot 0); 1 when the fixture has no such wheel. */
	int32 GetGoboSlotCount(EStageGoboWheel Wheel = EStageGoboWheel::Rotating) const { return GetWheel(Wheel).Num() + 1; }

	/** Display name of a gobo slot ("Open" for slot 0). */
	FText GetGoboSlotName(int32 Slot, EStageGoboWheel Wheel = EStageGoboWheel::Rotating) const;

	/** Projection texture of a gobo slot; null for Open, out-of-range slots and unset textures. Loads the texture if needed. */
	class UTexture2D* LoadGoboTexture(int32 Slot, EStageGoboWheel Wheel = EStageGoboWheel::Rotating) const;

	/** Light a gobo slot passes, 0..1 (1 for Open). */
	float GetGoboTransmission(int32 Slot, EStageGoboWheel Wheel = EStageGoboWheel::Rotating) const;

	/** Display name of a colour wheel slot ("Open" for slot 0). */
	FText GetColorWheelSlotName(int32 Slot) const;

	/** Filter of a colour wheel slot (white for Open and out-of-range slots). */
	FLinearColor GetColorWheelFilter(int32 Slot) const;

	/** Display name of a prism slot ("Out" for slot 0). */
	FText GetPrismSlotName(int32 Slot) const;

	/** The prism in a slot; null for Out (0) and out-of-range slots. */
	const FStagePrism* GetPrism(int32 Slot) const;

	/** True when the beam needs a projected mask (gobos, iris, blades, barndoors, prisms): the fixture then carries a light function. */
	bool HasBeamShaping() const { return !GoboWheel.IsEmpty() || !StaticGoboWheel.IsEmpty() || bHasIris || HasBlades() || HasPrisms(); }

	/**
	 * Attributes that hold a slot index rather than a continuous value (gobo, colour and prism wheels). They are stored rounded,
	 * a cue fade steps through them like a console snaps a wheel, and DMX maps each slot to an equal range.
	 */
	static bool IsSteppedAttribute(const FGameplayTag& Attribute);

	/**
	 * DMX encoding of an attribute value, 0..1 of the channel's range. Continuous attributes map their range linearly; stepped
	 * ones map each slot to the centre of an equal share of the range, so a console sending any value inside a slot's share
	 * selects that slot. Returns 0 for unsupported attributes.
	 */
	float NormalizeForDMX(const FGameplayTag& Attribute, float Value) const;

	/** Inverse of NormalizeForDMX: the attribute value for a received 0..1 channel value (stepped: the slot whose share holds it). */
	float DenormalizeFromDMX(const FGameplayTag& Attribute, float Normalized) const;

#if WITH_EDITOR
	/**
	 * Replaces DMXChannels with a typical layout for the current capabilities, in console order: 16-bit pan/tilt, dimmer,
	 * shutter, colour (CMY + CTO), colour wheel, gobos, prism, frost, iris, zoom, blades, then one block per cell.
	 */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "DMX")
	void GenerateDefaultDMXLayout();
#endif

	// --- Fixture ---

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fixture")
	EStageFixtureKind FixtureKind = EStageFixtureKind::MovingHeadSpot;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fixture")
	EStageFixtureColorSystem ColorSystem = EStageFixtureColorSystem::RGBW;

	/** Correlated colour temperature of the white source: a white-only fixture's light, a CMY fixture's lamp. */
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

	// --- Color ---

	/** Additive emitters in attribute order (R, G, B, W, A). Empty: the colour system's defaults (GetEmitterOutputs). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Color")
	TArray<FStageColorEmitter> Emitters;

	/** Colour temperature of a default white emitter (RGBW, RGBWA). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Color", meta = (ClampMin = "1700.0", ClampMax = "12000.0", Units = "K"))
	float WhiteEmitterKelvin = 5600.f;

	/** A variable CTO filter (usually with CMY): warms the lamp by up to FullCTOMiredShift. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Color")
	bool bHasCTO = false;

	/** Mired shift of the CTO at full. +159 takes 6500 K to about 3200 K, like a full CTO gel. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Color", meta = (EditCondition = "bHasCTO", ClampMin = "0.0", ClampMax = "400.0"))
	float FullCTOMiredShift = 159.f;

	/** Colour wheel, in wheel order. Empty: no colour wheel. Slot 0 (Open) is implicit. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Color", meta = (TitleProperty = "Name"))
	TArray<FStageColorWheelSlot> ColorWheel;

	/** A white-only fixture's gel frame: the gel library colours it (stored as R/G/B filter attributes, not on DMX). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Color", meta = (EditCondition = "ColorSystem == EStageFixtureColorSystem::None"))
	bool bAcceptsGel = false;

	// --- Optics ---

	/** Output at 100 % dimmer with every emitter (or open white) at full. Drives the spot light intensity in lumens. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Optics", meta = (ClampMin = "0.0", Units = "lm"))
	float LuminousFlux = 20000.f;

	/** Narrowest full beam angle (50 % of peak). Equal to BeamAngleMax for fixtures without zoom. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Optics", meta = (ClampMin = "0.5", ClampMax = "170.0", Units = "deg"))
	float BeamAngleMin = 4.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Optics", meta = (ClampMin = "0.5", ClampMax = "170.0", Units = "deg"))
	float BeamAngleMax = 40.f;

	/** Field angle (10 % of peak) over beam angle. A hard-edged beam is near 1, a soft wash about 1.5-2. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Optics", meta = (ClampMin = "1.0", ClampMax = "3.0"))
	float FieldAngleRatio = 1.5f;

	/** Distance the beam light reaches. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Optics", meta = (ClampMin = "100.0", Units = "cm"))
	float BeamRange = 4000.f;

	/** Rotating gobo wheel, in wheel order. Empty: the fixture has no rotating gobos. Slot 0 (Open) is implicit. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Optics", meta = (TitleProperty = "Name"))
	TArray<FStageGoboSlot> GoboWheel;

	/** The rotating wheel's gobos can spin continuously (GoboSpin), not only be indexed (GoboRotation). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Optics")
	bool bGoboSpin = false;

	/** Static gobo wheel, in wheel order. Empty: none. Slot 0 (Open) is implicit. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Optics", meta = (TitleProperty = "Name"))
	TArray<FStageGoboSlot> StaticGoboWheel;

	/** Prisms, in wheel order. Empty: no prism. Slot 0 (Out) is implicit. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Optics", meta = (TitleProperty = "Name"))
	TArray<FStagePrism> Prisms;

	/** Fastest gobo and prism rotation either way, in revolutions per minute. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Optics", meta = (ClampMin = "1.0", ClampMax = "300.0"))
	float MaxRotationRpm = 60.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Optics")
	bool bHasFrost = false;

	/** Field angle at full frost, as a multiple of the clear field angle. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Optics", meta = (EditCondition = "bHasFrost", ClampMin = "1.0", ClampMax = "4.0"))
	float FrostFieldMultiplier = 1.8f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Optics")
	bool bHasIris = false;

	/** Beam radius left by a fully closed iris, as a fraction of the open beam. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Optics", meta = (EditCondition = "bHasIris", ClampMin = "0.02", ClampMax = "0.9"))
	float IrisMinFraction = 0.1f;

	/** Four blades (top, right, bottom, left) and their rotation: a profile's framing shutters or soft barndoors. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Optics")
	EStageBeamShaper BeamShaper = EStageBeamShaper::None;

	// --- Cells ---

	/** Individually controlled cells (pixels, blinder lamps), laid along the cell strip. 1: a single-source fixture. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Cells", meta = (ClampMin = "1", ClampMax = "32"))
	int32 CellCount = 1;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Cells", meta = (EditCondition = "CellCount > 1"))
	EStageCellControl CellControl = EStageCellControl::Color;

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

	// --- Body ---

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Body", meta = (ShowOnlyInnerProperties))
	FStageFixtureBody Body;

	// --- DMX ---

	/** Mode name as the manufacturer calls it, e.g. "Extended 24ch". */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "DMX")
	FName DMXModeName = TEXT("Standard");

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "DMX", meta = (TitleProperty = "Attribute"))
	TArray<FStageFixtureDMXChannel> DMXChannels;

private:
	const TArray<FStageGoboSlot>& GetWheel(EStageGoboWheel Wheel) const { return Wheel == EStageGoboWheel::Static ? StaticGoboWheel : GoboWheel; }

	/** Assets saved before #49 had a bool for framing shutters; PostLoad turns it into BeamShaper. */
	UPROPERTY()
	bool bHasFramingShutters_DEPRECATED = false;
};
