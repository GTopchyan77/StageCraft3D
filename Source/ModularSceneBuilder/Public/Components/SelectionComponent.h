// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SelectionComponent.generated.h"

/** The primary selection (gizmo target, inspector subject) changed. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnStageSelectionChanged, AActor*, NewSelection, AActor*, PreviousSelection);

/** Anything about the selected set changed: items added or removed, or a new primary. Count is the new set size. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageSelectionSetChanged, int32, Count);

/**
 * Owns "which stage items are selected": an ordered set whose last entry is the primary selection.
 * The gizmo attaches to the primary and the inspector shows it; batch operations (delete, group moves)
 * act on the whole set (Docs/ADR/0004-selection-scenes-and-rendering.md §3.1).
 *
 * Works purely through IInteractableInterface, so any actor type (C++ or Blueprint) can be selected;
 * visual feedback is the target's own job (AModularBaseActor swaps its overlay material in OnSelect /
 * OnDeselect). Every selected item gets OnSelect exactly once while it is in the set.
 *
 * Consumers bind OnSelectionChanged (primary) or OnSelectionSetChanged (set) instead of polling. An item
 * leaves the set automatically when it is destroyed. All changes go through one private path, so the
 * highlight, destroy bindings and notifications can never disagree. Game thread only. No tick.
 */
UCLASS(ClassGroup = (StageCraft), meta = (BlueprintSpawnableComponent))
class MODULARSCENEBUILDER_API USelectionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USelectionComponent();

	/** Replaces the selection with Target alone. Ignores actors that are not interactable. Returns true if Target is now selected. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Selection")
	bool SelectActor(AActor* Target);

	/**
	 * Ctrl+Click: adds Target to the selection (it becomes the primary), or removes it if it was already selected.
	 * Returns true if Target is selected afterwards. Non-interactable actors are ignored (returns false).
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Selection")
	bool ToggleActorSelection(AActor* Target);

	/** Removes Target from the selection if it is in it. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Selection")
	void DeselectActor(AActor* Target);

	/** Replaces the selection with every selectable actor in Targets, keeping their order (the last becomes the primary). */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Selection")
	void SelectActors(const TArray<AActor*>& Targets);

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Selection")
	void ClearSelection();

	/** The primary selection: the most recently selected item, or null. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Selection")
	AActor* GetSelectedActor() const { return SelectedActors.IsEmpty() ? nullptr : SelectedActors.Last().Get(); }

	/** Every selected item, oldest first; the last is the primary. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Selection")
	TArray<AActor*> GetSelectedActors() const;

	UFUNCTION(BlueprintPure, Category = "StageCraft|Selection")
	int32 GetSelectionCount() const { return SelectedActors.Num(); }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Selection")
	bool HasSelection() const { return !SelectedActors.IsEmpty(); }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Selection")
	bool IsActorSelected(const AActor* Actor) const;

	/** The primary selection changed (including to or from null). Fires before OnSelectionSetChanged. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Selection")
	FOnStageSelectionChanged OnSelectionChanged;

	/** The set changed in any way. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Selection")
	FOnStageSelectionSetChanged OnSelectionSetChanged;

protected:
	//~ Begin UActorComponent Interface
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End UActorComponent Interface

private:
	static bool IsSelectable(const AActor* Actor);

	/** The single mutation path: deselects what left, selects what joined, then notifies once. */
	void ApplySelection(const TArray<AActor*>& NewSelection);

	UFUNCTION()
	void HandleSelectedActorDestroyed(AActor* DestroyedActor);

	/** Ordered: the last entry is the primary selection. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class AActor>> SelectedActors;
};
