// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Data/StageParameterTypes.h"
#include "StageInspectorPanel.generated.h"

/**
 * Live parameter inspector (the "details" window of a lighting console).
 *
 * Follows the owning AModularPlayerController's USelectionComponent. On every selection change it
 * asks the target for its IStageParameterInterface sections and builds one section widget per
 * section and one row widget per parameter, choosing the row class by parameter type. It then
 * listens to AModularBaseActor::OnParameterChanged, so values edited elsewhere (gizmo drag,
 * cue playback, incoming DMX) update in place without a rebuild.
 *
 * Layout and style live entirely in the Widget Blueprint:
 *   SectionContainer (required, e.g. a Scroll Box), TitleText / SubtitleText / EmptyState (optional).
 */
UCLASS(Abstract, Blueprintable)
class MODULARSCENEBUILDER_API UStageInspectorPanel : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Shows any object implementing IStageParameterInterface; nullptr clears the panel. Selection changes call this automatically. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Inspector")
	void Inspect(UObject* Target);

	/** Re-reads all sections from the current target (e.g. after its catalog item was swapped). */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Inspector")
	void Rebuild();

	UFUNCTION(BlueprintPure, Category = "StageCraft|Inspector")
	UObject* GetInspectedObject() const { return InspectedObject.Get(); }

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//~ End UUserWidget Interface

	UFUNCTION(BlueprintImplementableEvent, Category = "StageCraft|Inspector", meta = (DisplayName = "On Inspected Object Changed"))
	void BP_OnInspectedObjectChanged(UObject* NewTarget);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StageCraft|Inspector")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StageCraft|Inspector")
	TSubclassOf<class UStageParameterSectionWidget> SectionWidgetClass;

	/** Row class per parameter type. Missing types fall back to FallbackRowWidgetClass (typically a read-only text row). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StageCraft|Inspector")
	TMap<EStageParameterType, TSubclassOf<class UStageParameterRowWidget>> RowWidgetClasses;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StageCraft|Inspector")
	TSubclassOf<class UStageParameterRowWidget> FallbackRowWidgetClass;

	/** Read-only rows use this class if set, so specs can render as compact labels instead of disabled editors. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StageCraft|Inspector")
	TSubclassOf<class UStageParameterRowWidget> ReadOnlyRowWidgetClass;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Inspector", meta = (BindWidget))
	TObjectPtr<class UPanelWidget> SectionContainer = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Inspector", meta = (BindWidgetOptional))
	TObjectPtr<class UTextBlock> TitleText = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Inspector", meta = (BindWidgetOptional))
	TObjectPtr<class UTextBlock> SubtitleText = nullptr;

	/** Shown when nothing inspectable is selected. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Inspector", meta = (BindWidgetOptional))
	TObjectPtr<class UWidget> EmptyState = nullptr;

private:
	UFUNCTION()
	void HandleSelectionChanged(AActor* NewSelection, AActor* PreviousSelection);

	UFUNCTION()
	void HandleParameterChanged(class AModularBaseActor* Actor, FGameplayTag ParameterId);

	void HandleRowCommitted(const FGameplayTag& ParameterId, const FStageParameterValue& Value);

	TSubclassOf<class UStageParameterRowWidget> ChooseRowClass(const FStageParameterDescriptor& Descriptor) const;
	void RefreshHeader();
	void ClearRows();
	void UnbindTarget();

	TWeakObjectPtr<class UObject> InspectedObject;
	TWeakObjectPtr<class AModularBaseActor> BoundActor;
	TWeakObjectPtr<class USelectionComponent> BoundSelection;

	UPROPERTY(Transient)
	TMap<FGameplayTag, TObjectPtr<class UStageParameterRowWidget>> RowsById;
};
