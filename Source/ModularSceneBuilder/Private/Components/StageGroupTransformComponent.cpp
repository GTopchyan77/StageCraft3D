// Copyright Epic Games, Inc. All Rights Reserved.

#include "Components/StageGroupTransformComponent.h"

#include "Actors/ModularBaseActor.h"
#include "Components/SelectionComponent.h"
#include "GameFramework/Actor.h"
#include "Interaction/StageGroupTransform.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageGroupTransformComponent)

UStageGroupTransformComponent::UStageGroupTransformComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UStageGroupTransformComponent::BeginPlay()
{
	Super::BeginPlay();
	Selection = GetOwner()->FindComponentByClass<USelectionComponent>();
	ensureMsgf(Selection, TEXT("%s: UStageGroupTransformComponent needs a USelectionComponent on the same actor; groups will not follow."),
		*GetNameSafe(GetOwner()));
}

void UStageGroupTransformComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	EndGroupEdit();
	Super::EndPlay(EndPlayReason);
}

void UStageGroupTransformComponent::BeginGroupEdit(AActor& InLeader)
{
	EndGroupEdit();

	bEditing = true;
	Leader = &InLeader;
	LeaderStart = InLeader.GetActorTransform();
	if (!Selection || !Selection->IsActorSelected(&InLeader))
	{
		// An item edited outside the selection moves alone.
		return;
	}

	for (AActor* Selected : Selection->GetSelectedActors())
	{
		// Only stage items follow: the selection is interface-based, but only items are transformable stage content.
		if (Selected != &InLeader && IsValid(Selected) && Selected->IsA<AModularBaseActor>())
		{
			Followers.Add({ Selected, Selected->GetActorTransform() });
		}
	}
}

void UStageGroupTransformComponent::FollowLeader()
{
	const AActor* LeaderActor = Leader.Get();
	if (!LeaderActor || Followers.IsEmpty())
	{
		return;
	}

	const FTransform LeaderNow = LeaderActor->GetActorTransform();
	for (const FStageTransformChange& Follower : Followers)
	{
		AActor* FollowerActor = Follower.Item.Get();
		if (FollowerActor && !FollowerActor->IsActorBeingDestroyed())
		{
			FollowerActor->SetActorTransform(StageGroupTransform::ApplyLeaderChange(LeaderStart, LeaderNow, Follower.Before));
		}
	}
}

TArray<FStageTransformChange> UStageGroupTransformComponent::EndGroupEdit()
{
	TArray<FStageTransformChange> Changes;
	if (!bEditing)
	{
		return Changes;
	}

	// A leader or follower destroyed mid-edit stays in the list as a dead weak pointer; recording skips it.
	Changes.Reserve(Followers.Num() + 1);
	Changes.Add({ Leader, LeaderStart });
	Changes.Append(MoveTemp(Followers));

	bEditing = false;
	Leader.Reset();
	Followers.Reset();
	return Changes;
}

TArray<const AActor*> UStageGroupTransformComponent::GetFollowers() const
{
	TArray<const AActor*> Result;
	Result.Reserve(Followers.Num());
	for (const FStageTransformChange& Follower : Followers)
	{
		if (const AActor* Actor = Follower.Item.Get())
		{
			Result.Add(Actor);
		}
	}
	return Result;
}
