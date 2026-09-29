// Copyright Epic Games, Inc. All Rights Reserved.

#include "Game/StageCraftGameModeBase.h"

#include "GameFramework/DefaultPawn.h"
#include "Player/ModularPlayerController.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageCraftGameModeBase)

AStageCraftGameModeBase::AStageCraftGameModeBase()
{
	PlayerControllerClass = AModularPlayerController::StaticClass();
	// Free-flying WASD/QE camera until a dedicated stage camera pawn exists.
	DefaultPawnClass = ADefaultPawn::StaticClass();
}
