// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "StageCraftGameModeBase.generated.h"

/**
 * Wires the stage editor framework classes together. Set as GlobalDefaultGameMode in DefaultEngine.ini.
 * Derives from AGameModeBase rather than AGameMode: the editor has no match flow, so AGameMode's
 * match-state machine would be dead weight.
 */
UCLASS()
class MODULARSCENEBUILDER_API AStageCraftGameModeBase : public AGameModeBase
{
	GENERATED_BODY()

public:
	AStageCraftGameModeBase();
};
