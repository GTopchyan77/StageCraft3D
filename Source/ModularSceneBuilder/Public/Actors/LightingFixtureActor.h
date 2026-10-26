// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Actors/ModularBaseActor.h"
#include "Show/ShowControlTypes.h"
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
 * Beam shaping (gobo, iris, framing shutters) is projected by a light-function material on the
 * beam light (Docs/ADR/0007 §5.2). The dynamic instance is created only for fixtures whose data
 * has beam shaping, and its parameters change only when an attribute does.
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

	/** Creates the beam-shaping instance on first need and pushes the gobo / iris / blade attributes into it. */
	void ApplyBeamShaping(const class ULightingFixtureData& Fixture);

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

	/** The beam-shaping material could not be loaded; not retried, so the warning is logged once. */
	bool bBeamShapingUnavailable = false;

	/** False while the strobe clock has the output closed. Always true while not strobing. */
	bool bStrobeOpen = true;

	/** Throw distance in cm from the last trace; unset after pan, tilt or the transform changed. */
	mutable TOptional<double> CachedThrowCm;
};

ENUM_CLASS_FLAGS(ALightingFixtureActor::EFixtureOutputs);
