// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Actors/ModularBaseActor.h"
#include "Show/ShowControlTypes.h"
#include "LightingFixtureActor.generated.h"

/**
 * Placed lighting fixture. Holds the live, per-instance state (fixture ID, DMX patch and
 * attribute values) and turns it into visuals: pan/tilt pivots, a spot light for the beam.
 *
 * Attributes are the single source of truth for what the fixture does. Every writer (inspector,
 * cue playback, incoming DMX) goes through SetAttribute/SetAttributes, which clamp to the
 * fixture's capabilities, update components once and broadcast OnParameterChanged.
 *
 * Registers with UShowControlSubsystem on BeginPlay, which assigns a free fixture ID and
 * auto-patches it if needed. Like every stage item it never ticks.
 */
UCLASS(Blueprintable)
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
	//~ End AModularBaseActor Interface

	/** Pushes Attributes onto pivots and the beam light. */
	virtual void ApplyAttributesToComponents();

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
	/** Clamps and stores without side effects. Returns true if the stored value changed. */
	bool StoreAttribute(const FGameplayTag& Attribute, float Value);

	void NotifyAttributeChanged(const FGameplayTag& Attribute);

	class UShowControlSubsystem* GetShowControl() const;
};
