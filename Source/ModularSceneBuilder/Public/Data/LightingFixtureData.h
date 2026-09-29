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

#if WITH_EDITOR
	/** Replaces DMXChannels with a typical layout for the current capabilities (16-bit pan/tilt first, then dimmer, shutter, color, zoom). */
	UFUNCTION(CallInEditor, Category = "DMX")
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

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fixture")
	bool bHasShutter = true;

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
