// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Engine/EngineTypes.h"

/**
 * Custom trace channels. Both default to Ignore (see [/Script/Engine.CollisionProfile] in
 * DefaultEngine.ini), so only components that opt in block them. Must stay in sync with the
 * channel slots configured there.
 */
namespace StageCraftCollision
{
	/** Blocked only by placed stage items. Delete/select traces pass through the floor and level geometry. */
	inline constexpr ECollisionChannel StageItemChannel = ECC_GameTraceChannel1;

	/** Blocked only by transform gizmo handles, so handles never interfere with placement or selection traces. */
	inline constexpr ECollisionChannel GizmoChannel = ECC_GameTraceChannel2;
}
