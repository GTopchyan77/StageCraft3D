// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "History/StageEditHistoryComponent.h"
#include "StageGroupTransformComponent.generated.h"

/**
 * Keeps a multi-selection together while one of its items is transformed (Docs/ADR/0004-selection-scenes-and-rendering.md §3.2).
 *
 * The edited item is the leader: the gizmo's target during a drag, or the inspected item during a numeric Location /
 * Rotation / Scale edit. Between BeginGroupEdit and EndGroupEdit, FollowLeader moves every other selected stage item by
 * the leader's change from its start (StageGroupTransform::ApplyLeaderChange), so the group moves, turns and scales as one.
 * EndGroupEdit returns every item's start transform, which the controller records as one undo step.
 *
 * Followers are written directly, exactly like the gizmo writes its target (clamped scale, root TransformUpdated keeps the
 * session dirty flag and the inspector current). Holds weak references only for the length of one edit.
 * Requires USelectionComponent on the same actor. No tick: the controller calls FollowLeader when the leader moved.
 */
UCLASS(ClassGroup = (StageCraft), meta = (BlueprintSpawnableComponent))
class MODULARSCENEBUILDER_API UStageGroupTransformComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UStageGroupTransformComponent();

	/**
	 * Starts an edit of Leader, capturing its transform and those of the other selected stage items. With nothing else
	 * selected, the edit has no followers and only the leader is reported at the end. An edit already running is ended first.
	 */
	void BeginGroupEdit(AActor& Leader);

	/** Moves the followers to match the leader's change since BeginGroupEdit. No-op outside an edit. */
	void FollowLeader();

	/** Ends the edit. Returns the leader's and every follower's start transform (empty outside an edit). Idempotent. */
	TArray<FStageTransformChange> EndGroupEdit();

	bool IsEditing() const { return bEditing; }

	/** Items moving with the leader in the current edit, for the snapping component to ignore as neighbours. */
	TArray<const AActor*> GetFollowers() const;

protected:
	//~ Begin UActorComponent Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End UActorComponent Interface

private:
	UPROPERTY(Transient)
	TObjectPtr<class USelectionComponent> Selection = nullptr;

	TWeakObjectPtr<class AActor> Leader;
	FTransform LeaderStart = FTransform::Identity;
	TArray<FStageTransformChange> Followers;

	/** True from BeginGroupEdit to EndGroupEdit, even if the leader is destroyed in between. */
	bool bEditing = false;
};
