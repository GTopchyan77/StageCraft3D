// Copyright DamenTools internal. Precise Transform plugin.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "FloatingCoordinatePanel.generated.h"

class UEditableTextBox;

/**
 * Native floating replacement for WB_ObjectDescription's fixed-position
 * coordinate-edit row. See PreciseTransformLibrary.h's RepositionPlacedObjectFromStrings
 * for the actual reposition/validation logic this reuses as-is.
 *
 * WHY native: today's investigation traced every prior failure (Slate
 * OnMouseEnter/OnMouseLeave, then a Blueprint Tick-based bounds-check poll)
 * back to the SAME structural root cause - WB_ObjectDescription sits at a
 * fixed screen position while the hovered object can be anywhere on screen,
 * so the mouse crosses open space and loses hover before reaching it. This
 * class instead repositions itself next to the hovered object every frame,
 * and keeps positioning + hover-detection + commit entirely inside its own
 * NativeTick - no Blueprint event or Tick ordering to get wrong.
 *
 * Must be a direct child of a CanvasPanel (NativeTick casts its own Slot to
 * UCanvasPanelSlot to reposition itself) - one instance is added to
 * WB_Demo_UI's root CanvasPanel_0. WB_ObjectDescription and its existing
 * toolbar row are untouched; this is a separate, additive widget.
 */
UCLASS()
class PRECISETRANSFORM_API UFloatingCoordinatePanel : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Called every tick from WB_Demo_UI with BP_BuildManager's current selection.
	    Locks the reference for this panel's lifetime editing it - nullptr clears + hides.
	    Same fix already proven today as WB_ObjectDescription.LockedTargetObject: the object
	    this panel edits cannot silently change out from under a coordinate commit. */
	UFUNCTION(BlueprintCallable, Category = "PreciseTransform")
	void SetLockedTarget(AActor* NewTarget);

	UFUNCTION(BlueprintPure, Category = "PreciseTransform")
	AActor* GetLockedTarget() const { return LockedTarget.Get(); }

	/** Set once from WB_Demo_UI (it already resolves this) - required by RepositionPlacedObjectFromStrings. */
	UFUNCTION(BlueprintCallable, Category = "PreciseTransform")
	void SetBuildManager(UObject* InBuildManager) { BuildManagerRef = InBuildManager; }

	/** True while the mouse is within this widget's own bounds (native MyGeometry.IsUnderLocation
	    check, evaluated every NativeTick - no Blueprint poll, no crossing events). */
	UPROPERTY(BlueprintReadOnly, Category = "PreciseTransform")
	bool bIsPanelHovered = false;

protected:
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual void NativeOnInitialized() override;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidget))
	TObjectPtr<UEditableTextBox> XInput;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget))
	TObjectPtr<UEditableTextBox> YInput;
	UPROPERTY(BlueprintReadOnly, meta = (BindWidget))
	TObjectPtr<UEditableTextBox> ZInput;

	UFUNCTION()
	void OnCoordinateCommitted(const FText& Text, ETextCommit::Type CommitMethod);

private:
	UPROPERTY() TWeakObjectPtr<AActor> LockedTarget;
	UPROPERTY() TWeakObjectPtr<UObject> BuildManagerRef;

	/** Screen offset from the target's projected point (+right, -up in Slate's Y-down space). */
	UPROPERTY(EditAnywhere, Category = "PreciseTransform")
	FVector2D ScreenOffset = FVector2D(40.f, -20.f);

	void UpdateScreenPosition();
	void RefreshFieldsFromTarget();
	void ApplyCommittedCoordinates();
};
