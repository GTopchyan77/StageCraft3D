// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"
#include "OverCrowdEngineSubsystem.generated.h"

/**
 * 
 */
UCLASS()
class OVERCROWD_API UOverCrowdEngineSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintReadWrite, Category = "OverCrowd")
	bool bShowCrowdPlacements = false;
};
