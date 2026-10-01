// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Data/StageParameterTypes.h"
#include "StageParameterViewWidget.generated.h"

/**
 * Base of every panel that shows the parameters of one object: the inspector, the fader bank, and
 * any later view (patch sheet, quick-access popup). Owns the whole binding so panels only lay out
 * controls:
 *
 *  - Follows the owning AModularPlayerController's USelectionComponent (bFollowSelection), or shows
 *    whatever Inspect() is given.
 *  - Binds AModularBaseActor::OnParameterChanged and pushes each change into the registered
 *    controls for that id, directly (no tick, no polling), so gizmo drags, cue fades and DMX input
 *    show up the same frame.
 *  - Commits control edits through the owning AModularPlayerController's request bridge (GameMode
 *    rules + ownership, then the session applies them) and always reads the value back, so a
 *    clamped, refused or rejected edit snaps the control to what the object really holds.
 *  - Lets the controller mark locked parameters read-only before controls are built, and rebuilds
 *    when the player's entitlements change (a purchase unlocks rows without reselecting).
 *  - Structural changes (an invalid tag, e.g. after the item type was swapped) rebuild on the next
 *    tick, never inside the widget callback that caused them, and repeated requests coalesce.
 */
UCLASS(Abstract, Blueprintable)
class MODULARSCENEBUILDER_API UStageParameterViewWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Shows any object implementing IStageParameterInterface; nullptr clears the view. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Inspector")
	void Inspect(UObject* Target);

	/** Re-reads all sections from the current target and rebuilds the controls now. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Inspector")
	void Rebuild();

	/** Rebuilds on the next tick. Safe to call from inside widget callbacks; repeated calls coalesce. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Inspector")
	void RequestRebuild();

	/** Writes a value to the inspected object and refreshes every control showing it. Returns whether the object accepted it. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Inspector")
	bool CommitParameter(FGameplayTag ParameterId, const FStageParameterValue& Value);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Inspector")
	UObject* GetInspectedObject() const { return InspectedObject.Get(); }

	/** Number of controls currently bound (for tests and empty-state logic). */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Inspector")
	int32 GetNumControls() const { return Controls.Num(); }

	/** First control bound to a parameter, or null. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Inspector")
	class UStageParameterControlWidget* FindControl(FGameplayTag ParameterId) const;

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//~ End UUserWidget Interface

	/** Removes all controls and section widgets. Registered controls are already unbound when this runs. */
	virtual void ClearView() {}

	/** Creates controls for Target's sections; call RegisterControl for each one. Target is never null. */
	virtual void BuildView(UObject& Target, const TArray<FStageParameterSection>& Sections) {}

	/** After a target change and after every rebuild (Target may be null). Update headers and empty states here. */
	virtual void OnViewUpdated(UObject* Target) {}

	/** Every live value change of the target, after the matching controls were refreshed. */
	virtual void OnParameterRefreshed(const FGameplayTag& ParameterId, const FStageParameterValue& Value) {}

	/** Hooks a control into live refresh and commits. Panels call this for every control they create. */
	void RegisterControl(class UStageParameterControlWidget& Control);

	UFUNCTION(BlueprintImplementableEvent, Category = "StageCraft|Inspector", meta = (DisplayName = "On Inspected Object Changed"))
	void BP_OnInspectedObjectChanged(UObject* NewTarget);

	/** Follow the player's selection. Turn off for views that are pointed at a fixed object via Inspect(). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StageCraft|Inspector")
	bool bFollowSelection = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StageCraft|Inspector")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

	/** Feature-group color from Theme, or a neutral default when no theme is assigned. */
	FLinearColor GetGroupColor(const FGameplayTag& FeatureGroup) const;

private:
	UFUNCTION()
	void HandleSelectionChanged(AActor* NewSelection, AActor* PreviousSelection);

	UFUNCTION()
	void HandleParameterChanged(class AModularBaseActor* Actor, FGameplayTag ParameterId);

	UFUNCTION()
	void HandleEntitlementsChanged();

	void HandleControlCommitted(const FGameplayTag& ParameterId, const FStageParameterValue& Value);
	void RefreshControls(const FGameplayTag& ParameterId);
	void UnregisterControls();
	void UnbindTarget();

	TWeakObjectPtr<class UObject> InspectedObject;
	TWeakObjectPtr<class AModularBaseActor> BoundActor;
	TWeakObjectPtr<class USelectionComponent> BoundSelection;
	TWeakObjectPtr<class UStageProfileSubsystem> BoundProfile;

	/** Few dozen at most, so a flat list beats a map: lookups are by linear scan per change. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageParameterControlWidget>> Controls;

	bool bRebuildPending = false;
};
