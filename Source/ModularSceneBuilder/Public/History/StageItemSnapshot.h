// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPtr.h"

/**
 * Names the device-library fixture type a placed fixture uses (Docs/ADR/0033 §7.1), so a scene that is opened on a PC without
 * that type can say which GDTF file is missing. Written to scene files from format version 12; unset for every other item.
 */
struct FStageFixtureTypeRef
{
	FGuid Id;
	FString Manufacturer;
	FString Name;
	FString Mode;

	bool IsSet() const { return Id.IsValid(); }
};

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

	/** Set for fixtures of a device-library type: kept with an item a load could not place, and written back on save. */
	FStageFixtureTypeRef FixtureType;

	bool IsValid() const { return InstanceId.IsValid() && !Item.IsNull(); }
};
