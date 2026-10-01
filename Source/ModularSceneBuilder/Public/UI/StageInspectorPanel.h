// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UI/StageParameterViewWidget.h"
#include "StageInspectorPanel.generated.h"

/**
 * Live parameter inspector (the "details" window of a lighting console).
 *
 * Builds one section widget per FStageParameterSection and one row widget per parameter, choosing
 * the row class by parameter type. Selection following, live refresh and commit/read-back come
 * from UStageParameterViewWidget, so values edited elsewhere (gizmo drag, cue playback, incoming
 * DMX, the fader bank) update in place without a rebuild.
 *
 * Layout and style live entirely in the Widget Blueprint:
 *   SectionContainer (required, e.g. a Scroll Box), TitleText / SubtitleText / EmptyState (optional).
 */
UCLASS(Abstract, Blueprintable)
class MODULARSCENEBUILDER_API UStageInspectorPanel : public UStageParameterViewWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UStageParameterViewWidget Interface
	virtual void ClearView() override;
	virtual void BuildView(UObject& Target, const TArray<FStageParameterSection>& Sections) override;
	virtual void OnViewUpdated(UObject* Target) override;
	virtual void OnParameterRefreshed(const FGameplayTag& ParameterId, const FStageParameterValue& Value) override;
	//~ End UStageParameterViewWidget Interface

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StageCraft|Inspector")
	TSubclassOf<class UStageParameterSectionWidget> SectionWidgetClass;

	/** Row class per parameter type. Missing types fall back to FallbackRowWidgetClass (one generic row serves every type). */
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
	TSubclassOf<class UStageParameterRowWidget> ChooseRowClass(const FStageParameterDescriptor& Descriptor) const;
	void RefreshHeader(UObject* Target);
};
