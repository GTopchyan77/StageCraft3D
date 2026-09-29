// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Engine/EngineTypes.h"

namespace StageCraftCollision
{
	/**
	 * Trace channel that only placed stage items block (default response Ignore, see
	 * [/Script/Engine.CollisionProfile] in DefaultEngine.ini). Lets delete/select traces pass
	 * through the floor and level geometry and hit only user content.
	 * Must stay in sync with the channel slot configured in DefaultEngine.ini.
	 */
	inline constexpr ECollisionChannel StageItemChannel = ECC_GameTraceChannel1;
}
