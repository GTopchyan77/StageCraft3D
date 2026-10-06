// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPtr.h"

/**
 * Everything needed to bring a removed stage item back exactly as it was: the same instance id (so
 * older history entries that name it keep working), catalog item, transform and per-instance state.
 *
 * Plain value type, safe to keep in long-lived undo history: the catalog item is a soft reference
 * (resolved when restoring) and the instance state is serialized bytes, so a snapshot never keeps a
 * UObject alive or points at a destroyed actor. Written by AModularBaseActor::CaptureSnapshot.
 */
struct FStageItemSnapshot
{
	/** Stable identity of the placed item for this session. See AModularBaseActor::GetInstanceId. */
	FGuid InstanceId;

	TSoftObjectPtr<class UBaseItemData> Item;

	FTransform Transform = FTransform::Identity;

	/** The actor's SaveGame properties (label, fixture ID, DMX patch, attributes, audio settings...). */
	TArray<uint8> SavedProperties;

	/** Display name at capture time, for history descriptions ("Delete Spot 101"). */
	FText DisplayName;

	bool IsValid() const { return InstanceId.IsValid() && !Item.IsNull(); }
};
