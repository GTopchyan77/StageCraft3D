// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SelectionComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnStageSelectionChanged, AActor*, NewSelection, AActor*, PreviousSelection);

/**
 * Owns "which stage item is selected". Works purely through IInteractableInterface, so any
 * actor type (C++ or Blueprint) can be selected; visual feedback is the target's own job
 * (AModularBaseActor swaps its overlay material in OnSelect/OnDeselect).
 *
 * Consumers (gizmo, details panel) bind to OnSelectionChanged instead of polling. Selection is
 * released automatically when the selected actor is destroyed.
 */
UCLASS(ClassGroup = (StageCraft), meta = (BlueprintSpawnableComponent))
class MODULARSCENEBUILDER_API USelectionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USelectionComponent();

	/** Selects the target, deselecting the previous one. Ignores actors that are not interactable. Returns true if Target is now selected. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Selection")
	bool SelectActor(AActor* Target);

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Selection")
	void ClearSelection();

	UFUNCTION(BlueprintPure, Category = "StageCraft|Selection")
	AActor* GetSelectedActor() const { return SelectedActor; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Selection")
	bool HasSelection() const { return SelectedActor != nullptr; }

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Selection")
	FOnStageSelectionChanged OnSelectionChanged;

protected:
	//~ Begin UActorComponent Interface
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End UActorComponent Interface

private:
	void SetSelection(AActor* NewSelection);

	UFUNCTION()
	void HandleSelectedActorDestroyed(AActor* DestroyedActor);

	UPROPERTY(Transient)
	TObjectPtr<class AActor> SelectedActor = nullptr;
};
