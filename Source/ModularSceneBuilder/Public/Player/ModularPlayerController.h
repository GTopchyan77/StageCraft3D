// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "ModularPlayerController.generated.h"

class UInputAction;
class UInputMappingContext;
class USpawnSystemComponent;

/**
 * Stage editing controller. Its only jobs are Enhanced Input and cursor tracing: it converts
 * button presses into FHitResults and hands them to focused components (USpawnSystemComponent
 * now, selection/gizmo in Phase 4). Gameplay decisions live in those components, not here.
 *
 * Input assets are designer-assignable in a Blueprint subclass. If any slot is empty, an
 * equivalent mapping (LMB place, RMB delete) is built in code, so the project works with no content.
 */
UCLASS()
class MODULARSCENEBUILDER_API AModularPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AModularPlayerController();

	UFUNCTION(BlueprintPure, Category = "StageCraft")
	USpawnSystemComponent* GetSpawnSystem() const { return SpawnSystem; }

	/** Cursor trace against surfaces items can be placed on (floor, level geometry, other items). */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Trace")
	bool GetPlacementHitUnderCursor(FHitResult& OutHit) const;

	/** Cursor trace that only hits placed stage items. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Trace")
	bool GetStageItemHitUnderCursor(FHitResult& OutHit) const;

protected:
	//~ Begin APlayerController Interface
	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;
	//~ End APlayerController Interface

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USpawnSystemComponent> SpawnSystem;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	TObjectPtr<UInputMappingContext> EditorMappingContext;

	/** Hold to place. Continuous items paint the grid while held; single items spawn once per press. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	TObjectPtr<UInputAction> PlaceAction;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	TObjectPtr<UInputAction> DeleteAction;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Input")
	int32 MappingContextPriority = 0;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Trace")
	TEnumAsByte<ECollisionChannel> PlacementTraceChannel;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Trace")
	TEnumAsByte<ECollisionChannel> StageItemTraceChannel;

private:
	void BuildDefaultInputMapping();

	void HandlePlaceStarted();
	void HandlePlaceTriggered();
	void HandlePlaceCompleted();
	void HandleDeleteStarted();
};
