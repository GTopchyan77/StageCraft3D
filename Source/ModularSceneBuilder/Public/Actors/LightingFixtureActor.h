// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Actors/ModularBaseActor.h"
#include "Show/ShowControlTypes.h"
#include "Show/StageColorScience.h"
#include "Show/StagePhotometry.h"
#include "LightingFixtureActor.generated.h"

/**
 * Placed lighting fixture. Holds the live, per-instance state (fixture ID, DMX patch and
 * attribute values) and turns it into visuals: pan/tilt pivots, a spot light for the beam.
 *
 * Attributes are the single source of truth for what the fixture does. Every writer (inspector,
 * cue playback, incoming DMX) goes through SetAttribute/SetAttributes, which clamp to the
 * fixture's capabilities, update components once and broadcast OnParameterChanged.
 *
 * Beam shaping (gobos, iris, framing shutters or barndoors, prisms, frost) is projected by a light-function material on the
 * beam light (Docs/ADR/0007 §5.2, Docs/ADR/0020 §2.4). The dynamic instance is created only for fixtures whose data
 * has beam shaping, and its parameters change only when an attribute does; spinning gobos and prisms turn on the
 * material's own clock.
 *
 * Colour (Docs/ADR/0020 §2.3): the colour attributes (emitter levels, CMY filters, a gel frame, cells) become one beam mix
 * (GetBeamMix) that sets the light's colour and, with the dimmer, its intensity, and the lens or cell glow.
 *
 * Strobe: while Shutter is above 0 the fixture is registered with UShowControlSubsystem's strobe
 * clock, which gates its output (SetStrobeOpen). Photometrics (intensity, throw, illuminance,
 * beam diameter) are read-only parameters derived from the attributes and one line trace for the
 * throw, made lazily after pan, tilt or the transform changed (StagePhotometry).
 *
 * Registers on BeginPlay with UShowControlSubsystem, which assigns a free fixture ID, then with UStageDMXPatchSubsystem,
 * which auto-patches it if needed and is told of every later patch change. Like every stage item it never ticks.
 */
UCLASS(Blueprintable, Config = Game)
class MODULARSCENEBUILDER_API ALightingFixtureActor : public AModularBaseActor
{
	GENERATED_BODY()

public:
	ALightingFixtureActor();

	UFUNCTION(BlueprintPure, Category = "StageCraft|Fixture")
	class ULightingFixtureData* GetFixtureData() const;

	// --- Attributes ---

	/** Sets one attribute (clamped). Returns true if the value changed. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Fixture")
	bool SetAttribute(FGameplayTag Attribute, float Value);

	/** Sets several attributes with one visual update, for cue fades and DMX frames. Returns true if anything changed. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Fixture")
	bool SetAttributes(const TMap<FGameplayTag, float>& Values);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Fixture")
	float GetAttribute(FGameplayTag Attribute) const;

	UFUNCTION(BlueprintPure, Category = "StageCraft|Fixture")
	const TMap<FGameplayTag, float>& GetAttributes() const { return Attributes; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Fixture")
	FLinearColor GetColorRGB() const;

	/**
	 * The light leaving the fixture (Docs/ADR/0020 §2): colour after emitters or filters, CTO, colour wheel and cells, and
	 * the share of the rated flux it carries (before the dimmer). Drives the beam, the lens glow and photometrics.
	 */
	StageColorScience::FMix GetBeamMix() const;

	/** One cell's light, with the cell's own dimmer (master dimmer excluded). Black for fixtures without cells. */
	StageColorScience::FMix GetCellMix(int32 CellIndex) const;

	/**
	 * The colour attribute values (cells included) that make this fixture's light its own full white seen through Filter: emitter
	 * levels on an additive fixture (as bright as its emitters allow), the filter itself on CMY and a gel frame. Empty for a
	 * fixture that cannot colour its light. Used by the gel picker and the assistant; the values still go through the normal writes.
	 */
	TMap<FGameplayTag, float> MatchColor(const FLinearColor& Filter) const;

	/** Code of the gel last applied from the gel library ("L106"), or None once the colour was changed otherwise. */
	FName GetAppliedGel() const { return AppliedGel; }

	// --- Strobe ---

	/** Strobe rate in flashes per second for the current Shutter value; 0 when the shutter is open (not strobing). */
	float GetStrobeRateHz() const;

	/** Called by UShowControlSubsystem's strobe clock while the fixture strobes: opens or closes the light output. */
	void SetStrobeOpen(bool bOpen);

	// --- Shadow budget (Docs/ADR/0018 §4) ---

	/** Set by UShowControlSubsystem's shadow budget only; a change rebuilds the light's shadows once. */
	void SetBeamShadowsEnabled(bool bEnabled);
	bool AreBeamShadowsEnabled() const;

	/** Luminous flux at the current dimmer, ignoring the strobe gate, so a strobing beam keeps its rank between flashes. */
	float GetShadowRankingFlux() const;

	// --- Photometrics ---

	/** Approximate beam photometrics for the current attributes and position. Traces for the throw once after a move. */
	StagePhotometry::FResult ComputePhotometrics() const;

	// --- Patch ---

	UFUNCTION(BlueprintPure, Category = "StageCraft|Patch")
	int32 GetFixtureId() const { return FixtureId; }

	/** Fails if another registered fixture already uses the ID. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Patch")
	bool SetFixtureId(int32 NewFixtureId);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Patch")
	FStageDMXPatch GetPatch() const { return Patch; }

	/** Overlapping patches are allowed (as on a real console) and reported by UShowControlSubsystem::FindPatchConflicts. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Patch")
	void SetPatch(const FStageDMXPatch& NewPatch);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Patch")
	int32 GetDMXFootprint() const;

	// --- DMX codec ---

	/** Writes this fixture's channels into a 512-slot universe buffer (index 0 = address 1). No-op if unpatched. */
	void WriteDMX(TArrayView<uint8> UniverseData) const;

	/** Reads this fixture's channels from a 512-slot universe buffer. Returns true if any attribute changed. */
	bool ReadDMX(TConstArrayView<uint8> UniverseData);

	//~ Begin IInteractableInterface
	virtual FStageItemInteractionDetails GetInteractionDetails_Implementation() const override;
	//~ End IInteractableInterface

protected:
	//~ Begin AActor Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End AActor Interface

	//~ Begin AModularBaseActor Interface
	virtual void ApplyItemData(const class UBaseItemData& Data) override;
	virtual void UpdateHighlight() override;
	virtual void GatherParameterSections(TArray<FStageParameterSection>& OutSections) const override;
	virtual bool ReadParameter(const FGameplayTag& ParameterId, FStageParameterValue& OutValue) const override;
	virtual bool WriteParameter(const FGameplayTag& ParameterId, const FStageParameterValue& Value) override;
	virtual void OnStageTransformChanged() override;
	//~ End AModularBaseActor Interface

	/** Pushes all Attributes onto pivots, the beam light and the beam-shaping projection (new data, BeginPlay). */
	virtual void ApplyAttributesToComponents();

	/**
	 * Light-function material that projects gobo, iris and blades (parameters listed on ApplyBeamShaping). Set in
	 * DefaultGame.ini; fixtures without beam shaping never load it.
	 */
	UPROPERTY(Config, EditDefaultsOnly, Category = "StageCraft|Fixture")
	TSoftObjectPtr<class UMaterialInterface> BeamShapingMaterial;

	/** Unlit material of the lens and the cell strip (EmitterColor, CellTexture, CellCount, Gate). Set in DefaultGame.ini. */
	UPROPERTY(Config, EditDefaultsOnly, Category = "StageCraft|Fixture")
	TSoftObjectPtr<class UMaterialInterface> EmitterMaterial;

	/** Disc drawn as the lens (a cylinder flattened to 1 cm); scaled to Body.LensDiameterCm. */
	UPROPERTY(Config, EditDefaultsOnly, Category = "StageCraft|Fixture")
	TSoftObjectPtr<class UStaticMesh> LensMesh;

	/** Flat strip the cells glow on (a plane facing +Z, UV.x along the cells); scaled to Body.CellStripSizeCm. */
	UPROPERTY(Config, EditDefaultsOnly, Category = "StageCraft|Fixture")
	TSoftObjectPtr<class UStaticMesh> CellStripMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class USceneComponent> PanPivot = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class UStaticMeshComponent> YokeMeshComponent = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class USceneComponent> TiltPivot = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class UStaticMeshComponent> HeadMeshComponent = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class USpotLightComponent> BeamLight = nullptr;

	/** The glowing lens, or the cell strip of a multi-cell fixture, at the beam origin. Never collides or casts shadows. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class UStaticMeshComponent> LensMeshComponent = nullptr;

	/** Console fixture ID. 0 = let the show control subsystem assign one. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, SaveGame, Category = "StageCraft|Patch", meta = (ClampMin = "0"))
	int32 FixtureId = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, SaveGame, Category = "StageCraft|Patch")
	FStageDMXPatch Patch;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, SaveGame, Category = "StageCraft|Fixture", meta = (Categories = "StageCraft.Attribute"))
	TMap<FGameplayTag, float> Attributes;

private:
	/**
	 * The parts of the fixture an attribute drives. An edit or a fade frame re-applies only the parts its changed attributes
	 * drive: moving the pivots updates every component under them, and the beam-shaping projection has eight material
	 * parameters, so re-applying everything for a dimmer change made each crossfading fixture several times more expensive
	 * (Docs/ADR/0012-production-reports-and-fixture-benchmark.md §3).
	 */
	enum class EFixtureOutputs : uint8
	{
		None = 0,
		Pivots = 1 << 0,
		Intensity = 1 << 1,
		Cone = 1 << 2,
		Color = 1 << 3,
		Temperature = 1 << 4,
		BeamShaping = 1 << 5,
		All = Pivots | Intensity | Cone | Color | Temperature | BeamShaping
	};

	/**
	 * A continuously turning wheel (gobo or prism spin). The material turns it with its own clock (the Time node, world game
	 * time), so nothing ticks; a speed change restarts the clock from the angle reached, so the projection does not jump.
	 */
	struct FSpinClock
	{
		double PhaseDegrees = 0.0;
		double StartSeconds = 0.0;
		float Rpm = 0.f;
	};

	/** The beam's angles for the current zoom, frost and prism: the cone of the spot light and how the projection maps onto it. */
	struct FConeAngles
	{
		double BeamDegrees = 0.0;
		double FieldDegrees = 0.0;
		double OuterHalfDegrees = 0.0;
		double InnerHalfDegrees = 0.0;
		/** tan(outer half angle) / tan(beam half angle): the projection's beam edge sits at 1 / ConeToBeam of the cone. */
		double ConeToBeam = 1.0;
	};
	FRIEND_ENUM_CLASS_FLAGS(EFixtureOutputs);

	static EFixtureOutputs GetDrivenOutputs(const FGameplayTag& Attribute);

	/** Pushes the attributes behind Outputs onto the components. */
	void ApplyOutputs(EFixtureOutputs Outputs);

	/** Clamps (and rounds stepped attributes) and stores without side effects. Returns true if the stored value changed. */
	bool StoreAttribute(const FGameplayTag& Attribute, float Value);

	/**
	 * Brings restored values within the limits every live write has: a non-finite attribute takes the fixture's default,
	 * others are clamped and stepped ones rounded; the patch is clamped as SetPatch does; a negative ID becomes 0 (automatic).
	 * Each repair is reported (ReportStateRepair). Runs from ApplyItemData, after unsupported attributes were dropped.
	 */
	void SanitizeRestoredState(const class ULightingFixtureData& Fixture);

	static float SanitizeAttributeValue(const FGameplayTag& Attribute, float Value, float Min, float Max);
	static FStageDMXPatch ClampPatch(const FStageDMXPatch& InPatch);

	/** Broadcasts an attribute change plus the composite and derived rows that follow from it. */
	void NotifyAttributeChanged(const FGameplayTag& Attribute);
	void NotifyPhotometricsChanged();

	/** Creates the beam-shaping instance on first need and pushes the gobo / iris / blade / prism attributes into it. */
	void ApplyBeamShaping(const class ULightingFixtureData& Fixture);

	/** True when the current attributes project something (a gobo, iris, blades or a prism): the light function is on. */
	bool IsProjectingShape(const class ULightingFixtureData& Fixture) const;

	FConeAngles ComputeConeAngles(const class ULightingFixtureData& Fixture) const;

	/** Sizes the yoke and head to the data's Body and places the lens or cell strip. */
	void ApplyBody(const class ULightingFixtureData& Fixture);

	/** Pushes the dimmer and the strobe gate into the emitter material (cheap: two scalars). */
	void ApplyEmitterLevel();

	/** Pushes the lens colour or the cell colours (a texture upload) into the emitter material. Only after colour changes. */
	void ApplyEmitterColor(const class ULightingFixtureData& Fixture);

	/** Light output for the spot light, in lumens: rated flux x dimmer x mix fraction, corrected for the light colour's luminance. */
	float ComputeBeamIntensity(const class ULightingFixtureData& Fixture) const;

	/** Restarts a spin clock at a new speed from the angle it has reached. */
	void UpdateSpinClock(FSpinClock& Clock, float NewRpm) const;

	/** Gel picker: sets the colour attributes that reproduce gel Index of the library on this fixture. */
	bool ApplyGel(int32 GelIndex);

	/** One cell's mix for emitter outputs computed once by the caller. */
	StageColorScience::FMix ComputeCellMix(const class ULightingFixtureData& Fixture, TConstArrayView<FLinearColor> Outputs, int32 CellIndex) const;

	/** Clears the gel label after a colour edit that did not come from the gel picker. */
	void ClearAppliedGel();

	/**
	 * MatchColor at Color's own brightness (a colour picker's dark red is a dimmer red), for every cell or only OnlyCell
	 * (INDEX_NONE: the fixture's colour, all cells on a multi-cell fixture).
	 */
	TMap<FGameplayTag, float> MatchColorValues(const FLinearColor& Color, int32 OnlyCell) const;

	/**
	 * The colour a colour row shows, which written back gives the same light: the emitters' mix at the brightest emitter's
	 * level on additive fixtures, the filters on CMY and gel frames, the bar's mix for a multi-cell fixture (Cell INDEX_NONE).
	 */
	FLinearColor GetDisplayColor(int32 Cell) const;

	/** Joins or leaves the subsystem's strobe clock to match the Shutter attribute. */
	void UpdateStrobeRegistration();

	/** Distance in cm to the first surface along the beam (0 = none within BeamRange). */
	double TraceThrow() const;

	class UShowControlSubsystem* GetShowControl() const;
	class UStageDMXPatchSubsystem* GetPatchSubsystem() const;

	/** Tells the patch subsystem the addressing changed (patch or footprint). No-op before BeginPlay. */
	void NotifyPatchSubsystem();

	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> BeamShapingInstance = nullptr;

	/** Gobo slot whose texture the beam-shaping instance holds; INDEX_NONE until first applied. */
	int32 AppliedGoboSlot = INDEX_NONE;
	int32 AppliedStaticGoboSlot = INDEX_NONE;

	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> EmitterInstance = nullptr;

	/** One texel per cell, rewritten when a cell's colour or level changes. Multi-cell fixtures only. */
	UPROPERTY(Transient)
	TObjectPtr<class UTexture2D> CellTexture = nullptr;

	/** See GetAppliedGel. Saved, so a reloaded show still names its gels. */
	UPROPERTY(SaveGame)
	FName AppliedGel;

	/** True while ApplyGel writes the colour, so the write does not clear the gel label it is setting. */
	bool bApplyingGel = false;

	FSpinClock GoboSpinClock;
	FSpinClock PrismSpinClock;

	/** The beam-shaping material could not be loaded; not retried, so the warning is logged once. */
	bool bBeamShapingUnavailable = false;

	/** False while the strobe clock has the output closed. Always true while not strobing. */
	bool bStrobeOpen = true;

	/** Throw distance in cm from the last trace; unset after pan, tilt or the transform changed. */
	mutable TOptional<double> CachedThrowCm;
};

ENUM_CLASS_FLAGS(ALightingFixtureActor::EFixtureOutputs);
