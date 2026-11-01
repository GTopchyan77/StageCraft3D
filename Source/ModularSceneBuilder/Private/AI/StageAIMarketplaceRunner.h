// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AI/StageAITypes.h"
#include "Player/ModularPlayerController.h"

/**
 * Runs the assistant's marketplace actions for one player (Docs/ADR/0021-community-marketplace.md §13):
 *  - SearchMarketplace through UStageMarketplaceSubsystem::RequestQuery.
 *  - PreviewListing through RequestPreview, shown in the Marketplace window, which it opens.
 *  - AcquireListing through AModularPlayerController::RequestMarketplaceAcquire, the panel's own request.
 *
 * Contracts:
 *  - The listing was resolved by UStageAICommandSubsystem (FStageAIListingRef::Id); it is re-read from the marketplace here, never
 *    trusted from the command.
 *  - A paid listing without ConfirmedCredits is offered (FStageAIActionOutcome::PurchaseOffer), not bought.
 *  - Searches, previews and acquisitions answer later: the outcome is Pending and UStageAICommandSubsystem::AwaitMarketplace ends it.
 *
 * Holds no rules: the marketplace and the economy decide, and their refusals are quoted back. Game thread only.
 */
namespace StageAIMarketplaceRunner
{
	FStageAIActionOutcome Run(AModularPlayerController& Controller, const FStageAICommand& Command);
}
