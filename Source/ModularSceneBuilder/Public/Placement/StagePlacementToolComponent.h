// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Components/SpawnSystemComponent.h"
#include "Placement/StagePlacementTypes.h"
#include "Placement/StageSnapTypes.h"
#include "StagePlacementToolComponent.generated.h"

/** Why a placement click did nothing before reaching the rules. Rule refusals are reported by the validator's owner instead. */
UENUM(BlueprintType)
enum class EStagePlacementFailure : uint8
{
	NoItemArmed,
	NoSurface,
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnStageEditModeChanged, EStageEditMode, NewMode, EStageEditMode, PreviousMode);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageArmedItemChanged, class UBaseItemData*, ArmedItem);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageItemPlaced, class AModularBaseActor*, PlacedActor);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStagePlacementSnapped, FVector, LandingPoint);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStagePlacementFailed, EStagePlacementFailure, Reason);

/**
 * The placement tool of one local player: owns the edit mode (Select / Place), the armed catalog item
 * and the placement preview, and turns one Place-mode click into exactly one spawned item.
 *
 * Stamping workflow: Place mode and the ghost stay active after each placement, so every further click places
 * another copy. Only an explicit exit (EnterSelectMode / TogglePlaceMode: Esc, P, the Library toggle, the Edit menu)
 * returns to Select. Holding the button never places more than the one item of its press.
 *
 * Like USpawnSystemComponent it knows nothing about input or tracing: the owning controller pushes
 * cursor hits (UpdateTarget / ClearTarget) whenever the cursor or camera moves, and calls TryPlace on a
 * click. Nothing ticks or polls; the armed item is cached from UStageItemSubsystem::OnSelectedItemChanged.
 *
 * Requires a USpawnSystemComponent on the same actor (the spawn executor). Refusals by the rules are
 * reported by the spawn validator's owner; this component reports only pre-rule failures (OnPlacementFailed).
 */
UCLASS(ClassGroup = (StageCraft), meta = (BlueprintSpawnableComponent))
class MODULARSCENEBUILDER_API UStagePlacementToolComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UStagePlacementToolComponent();

	// --- Mode ---

	UFUNCTION(BlueprintPure, Category = "StageCraft|Placement")
	EStageEditMode GetEditMode() const { return EditMode; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Placement")
	bool IsPlaceMode() const { return EditMode == EStageEditMode::Place; }

	/** Enters Place mode. Allowed without an armed item: the ghost appears as soon as one is armed. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Placement")
	void EnterPlaceMode();

	/** Leaves Place mode and hides the preview. Idempotent. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Placement")
	void EnterSelectMode();

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Placement")
	void TogglePlaceMode();

	// --- Armed item and preview ---

	UFUNCTION(BlueprintPure, Category = "StageCraft|Placement")
	class UBaseItemData* GetArmedItem() const { return ArmedItem; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Placement")
	EStagePlacementPreviewState GetPreviewState() const { return PreviewState; }

	/** Read-only access for dev diagnostics (StageCraft.Edit.Status). Null until the first preview is needed. */
	const class AStagePlacementPreview* GetPreviewForDiagnostics() const { return Preview; }

	/** The ghost and landing-marker actor, so still renders can leave it out. Null until the first preview is needed. */
	class AStagePlacementPreview* GetPreviewActor() const { return Preview; }

	/**
	 * Moves the preview to the landing transform for Hit. Hides it when not in Place mode, when nothing is
	 * armed or when Hit has no blocking surface. Called by the controller on cursor or camera movement.
	 */
	void UpdateTarget(const struct FHitResult& Hit);

	/** Hides the preview (cursor left the viewport, camera navigation started). */
	void ClearTarget();

	/**
	 * Place-mode click: spawns exactly one armed item at Hit's landing transform. Place mode and the preview stay
	 * active (stamping), and the preview's verdict is re-evaluated in case the placement used up a session limit.
	 * Returns the placed actor, or null if nothing was placed (the mode is unchanged either way).
	 */
	class AModularBaseActor* TryPlace(const struct FHitResult& Hit);

	/**
	 * Display-only check used to colour the preview (Valid / Refused). Bound by the controller to the
	 * GameMode's rules without reporting; TryPlace is still validated by the spawn system.
	 */
	FStagePlacementValidator PreviewEvaluator;

	/**
	 * Optional object snapping of the landing point onto nearby placed items, applied identically to the
	 * preview and the placement. Bound by the controller to UStageSnappingComponent; unbound means grid only.
	 */
	FStagePlacementSnapper PlacementSnapper;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Placement")
	FOnStageEditModeChanged OnEditModeChanged;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Placement")
	FOnStageArmedItemChanged OnArmedItemChanged;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Placement")
	FOnStageItemPlaced OnItemPlaced;

	/** The landing point moved to a different grid point (only for items with grid snapping). */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Placement")
	FOnStagePlacementSnapped OnPlacementSnapped;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Placement")
	FOnStagePlacementFailed OnPlacementFailed;

protected:
	//~ Begin UActorComponent Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End UActorComponent Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Placement")
	TSubclassOf<class AStagePlacementPreview> PreviewClass;

private:
	UFUNCTION()
	void HandleSelectedItemChanged(class UBaseItemData* NewItem, class UBaseItemData* PreviousItem);

	/**
	 * The one landing transform for Hit (grid, surface alignment, pivot offset, then object snapping), shared by
	 * the preview and TryPlace so the ghost is exactly where the item lands. OutGuides receives the snap guides.
	 */
	FTransform ComputeLandingTransform(const class UBaseItemData& Item, const struct FHitResult& Hit, FStageSnapGuideList* OutGuides);

	void SetEditMode(EStageEditMode NewMode);
	void SetArmedItem(class UBaseItemData* NewItem);
	void EvaluateArmedItem();
	void HidePreview();
	void NotifyIfSnapped(const FVector& LandingPoint);
	class AStagePlacementPreview* GetOrCreatePreview();

	UPROPERTY(Transient)
	TObjectPtr<class USpawnSystemComponent> SpawnSystem = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageItemSubsystem> ItemSubsystem = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UBaseItemData> ArmedItem = nullptr;

	/** Spawned on first use, so controllers that never place (remote, tests) never create one. */
	UPROPERTY(Transient)
	TObjectPtr<class AStagePlacementPreview> Preview = nullptr;

	EStageEditMode EditMode = EStageEditMode::Select;
	EStagePlacementPreviewState PreviewState = EStagePlacementPreviewState::Hidden;

	/** Rules verdict for the armed item, refreshed when the item or the mode changes (not per cursor move). */
	EStagePlacementPreviewState ArmedItemVerdict = EStagePlacementPreviewState::Valid;

	/** Last landing point shown, for snap feedback. Unset while hidden. */
	TOptional<FVector> LastLandingPoint;
};
