// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AI/StageAITypes.h"
#include "Player/ModularPlayerController.h"

/**
 * Runs one validated action command of the AI assistant through a player's public request API (Docs/ADR/0016-ai-assistant.md
 * §2.2): fixtures through RequestBuildPlan, cues through RequestStoreCue / RequestGoToCue / RequestCueGo / RequestCueBack, the
 * environment through RequestSetEnvironment, reports through RequestExport*, history through RequestUndo / RequestRedo. It holds no
 * rules: the controller's requests and the GameMode decide, and a refusal they report is quoted back in the outcome.
 *
 * Adding fixtures is asynchronous: the outcome is Pending, the fixture's assets stream through UStageAICommandSubsystem::
 * RequestItemAssets, and the result arrives with FinishPendingAction. Game thread only.
 */
namespace StageAICommandRunner
{
	FStageAIActionOutcome Run(AModularPlayerController& Controller, const FStageAICommand& Command);
}
