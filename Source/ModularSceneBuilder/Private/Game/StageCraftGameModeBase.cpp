// Copyright Epic Games, Inc. All Rights Reserved.

#include "Game/StageCraftGameModeBase.h"

#include "Player/StageCameraPawn.h"
#include "Player/ModularPlayerController.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageCraftGameModeBase)

AStageCraftGameModeBase::AStageCraftGameModeBase()
{
	PlayerControllerClass = AModularPlayerController::StaticClass();
	// RMB fly navigation is driven by the controller; the pawn is a collision-free floating body.
	DefaultPawnClass = AStageCameraPawn::StaticClass();
}
