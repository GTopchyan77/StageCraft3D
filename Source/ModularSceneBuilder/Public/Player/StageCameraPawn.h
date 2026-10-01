// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/DefaultPawn.h"
#include "StageCameraPawn.generated.h"

/**
 * Free-flying editor camera body. Navigation input lives in AModularPlayerController (RMB fly mode);
 * this pawn only supplies the floating movement and opts out of everything that would fight it:
 *  - no ADefaultPawn legacy WASD/mouse bindings, so the keys move the camera only while RMB is held;
 *  - no collision, so the camera passes through trusses, decks and walls like the Unreal Editor viewport.
 */
UCLASS()
class MODULARSCENEBUILDER_API AStageCameraPawn : public ADefaultPawn
{
	GENERATED_BODY()

public:
	AStageCameraPawn(const FObjectInitializer& ObjectInitializer);
};
