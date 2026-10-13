// Copyright DamenTools internal. Spline Route Tool plugin.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DolphinCompanion.generated.h"

class USkeletalMeshComponent;
class UAnimSequenceBase;

/**
 * Decorative dolphin that swims alongside a vessel. Spawned, attached and destroyed by
 * ASplineFollowerVessel from its catalog entry's FDolphinCompanionSettings — purely cosmetic,
 * so it is deliberately absent from the scenario save format.
 *
 * Blueprintable so BP_DolphinCompanion can carry the skeletal mesh + swim animation defaults
 * without hardcoding asset paths in C++.
 */
UCLASS(Blueprintable)
class SPLINEROUTETOOL_API ADolphinCompanion : public AActor
{
	GENERATED_BODY()

public:
	ADolphinCompanion();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dolphin")
	TObjectPtr<USkeletalMeshComponent> MeshComponent;

	/**
	 * Looping swim cycle started in BeginPlay. Driven through the single-node player rather than an
	 * AnimBP — the v1 companion only ever plays this one clip, so a graph would add no behaviour.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dolphin")
	TObjectPtr<UAnimSequenceBase> SwimAnimation;

	/** Station-keeping offset from the attach parent that the bob oscillates around. Set by Configure(). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dolphin")
	FVector BaseRelativeLocation = FVector::ZeroVector;

	/** Peak vertical displacement of the swim bob, in cm. Zero disables the bob (and its tick work). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dolphin|Bob", meta = (ClampMin = "0.0"))
	float BobAmplitude = 15.0f;

	/** Bob frequency in Hz, measured at time-scale 1.0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dolphin|Bob", meta = (ClampMin = "0.0"))
	float BobSpeed = 0.5f;

	/** Corrective mesh rotation applied to MeshComponent, not the actor root — mirrors
	 *  FVesselCatalogEntry::RotationOffset's DCC axis fixup. Set by Configure(). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dolphin")
	FRotator MeshRotationOffset = FRotator(0.0f, 90.0f, 0.0f);

	/**
	 * Apply attach offset + scale + bob tuning + mesh rotation in one shot.
	 * Call between SpawnActorDeferred and FinishSpawning so BeginPlay sees the final values.
	 */
	UFUNCTION(BlueprintCallable, Category = "Dolphin")
	void Configure(FVector InRelativeOffset, float InScale = 1.0f, float InBobAmplitude = 15.0f, float InBobSpeed = 0.5f, FRotator InMeshRotationOffset = FRotator(0.0f, 90.0f, 0.0f));

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	/** Global playback multiplier from USplineRouteTimeSubsystem; 1.0 when the subsystem is unavailable. */
	float GetEffectiveTimeScale() const;

	/** Time-scaled accumulator driving the bob sine — deliberately not real elapsed time. */
	float BobTime = 0.0f;

	/** False for level-placed instances, which keep their authored relative location as the bob origin. */
	bool bConfigured = false;
};
