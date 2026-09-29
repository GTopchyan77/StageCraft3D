// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "Data/StageItemTypes.h"
#include "InteractableInterface.generated.h"

/**
 * Snapshot of what an interactable wants the UI to know about it. Returned by value so callers
 * never hold on to actor internals; the details panel and color picker (Phase 5) read only this.
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageItemInteractionDetails
{
	GENERATED_BODY()

	/** Catalog entry the object was spawned from. Null for interactables that are not catalog items. */
	UPROPERTY(BlueprintReadOnly, Category = "Interaction")
	TObjectPtr<class UBaseItemData> ItemData = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "Interaction")
	FText DisplayName;

	UPROPERTY(BlueprintReadOnly, Category = "Interaction")
	EStageItemType ItemType = EStageItemType::Prop;

	/** When true the UI offers the color picker, and Color holds the current value. */
	UPROPERTY(BlueprintReadOnly, Category = "Interaction")
	bool bSupportsColorEditing = false;

	UPROPERTY(BlueprintReadOnly, Category = "Interaction")
	FLinearColor Color = FLinearColor::White;
};

UINTERFACE(MinimalAPI, BlueprintType)
class UInteractableInterface : public UInterface
{
	GENERATED_BODY()
};

/**
 * Contract between input code (player controller, spawn/selection components) and anything the
 * user can point at. Callers only ever see this interface, never concrete actor types.
 *
 * All functions are BlueprintNativeEvents so Blueprint-only actors can implement it too.
 * From C++, always call through the static wrappers, e.g.
 * IInteractableInterface::Execute_OnHoverBegin(Target), which work for both C++ and Blueprint implementers.
 *
 * Implementations must be idempotent: the caller may repeat a call without an intermediate
 * End/Deselect (e.g. after focus loss), and that must not stack effects.
 */
class MODULARSCENEBUILDER_API IInteractableInterface
{
	GENERATED_BODY()

public:
	/** The cursor started pointing at this object. Use it for highlight feedback only; do not change selection. */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "StageCraft|Interaction")
	void OnHoverBegin();

	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "StageCraft|Interaction")
	void OnHoverEnd();

	/** This object became the active selection (gizmo target, details panel subject). */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "StageCraft|Interaction")
	void OnSelect();

	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "StageCraft|Interaction")
	void OnDeselect();

	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "StageCraft|Interaction")
	FStageItemInteractionDetails GetInteractionDetails() const;
};
