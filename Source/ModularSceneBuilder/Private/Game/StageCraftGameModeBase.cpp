// Copyright Epic Games, Inc. All Rights Reserved.

#include "Game/StageCraftGameModeBase.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Player/ModularPlayerController.h"
#include "Player/StageCameraPawn.h"
#include "Subsystems/StageEconomySubsystem.h"
#include "Subsystems/StageSessionSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageCraftGameModeBase)

#define LOCTEXT_NAMESPACE "StageCraftGameMode"

AStageCraftGameModeBase::AStageCraftGameModeBase()
{
	PlayerControllerClass = AModularPlayerController::StaticClass();
	// RMB fly navigation is driven by the controller; the pawn is a collision-free floating body.
	DefaultPawnClass = AStageCameraPawn::StaticClass();
}

void AStageCraftGameModeBase::StartPlay()
{
	// Before Super: StartPlay dispatches BeginPlay, and items register with an already-configured session.
	if (UStageSessionSubsystem* Session = GetSession())
	{
		Session->BeginSession(SessionRules);
	}

	Super::StartPlay();
}

UStageSessionSubsystem* AStageCraftGameModeBase::GetSession() const
{
	return UWorld::GetSubsystem<UStageSessionSubsystem>(GetWorld());
}

UStageEconomySubsystem* AStageCraftGameModeBase::GetEconomy() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UStageEconomySubsystem>() : nullptr;
}

FStageEconomyResultInfo AStageCraftGameModeBase::EvaluatePlacement_Implementation(const APlayerController* Player, const UBaseItemData* Item) const
{
	const UStageSessionSubsystem* Session = GetSession();
	const UStageEconomySubsystem* Economy = GetEconomy();
	if (!Player || !Item || !Session || (SessionRules.bEnforceEntitlements && !Economy))
	{
		// Deny by default: a request that cannot be checked is not allowed.
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("CannotPlace", "Placement is not available."));
	}

	if (SessionRules.bEnforceEntitlements)
	{
		const FStageEconomyResultInfo Ownership = Economy->ValidateItemUse(Item);
		if (!Ownership.IsSuccess())
		{
			return Ownership;
		}
	}
	return Session->EvaluateSessionLimits(Item);
}

FStageEconomyResultInfo AStageCraftGameModeBase::EvaluateParameterChange_Implementation(const APlayerController* Player, const UObject* Target, FGameplayTag ParameterId, const FStageParameterValue& Value) const
{
	const UStageEconomySubsystem* Economy = GetEconomy();
	if (!Player || !Target || (SessionRules.bEnforceEntitlements && !Economy))
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("CannotEdit", "Editing is not available."));
	}

	return SessionRules.bEnforceEntitlements
		? Economy->ValidateParameterChange(Target, ParameterId, Value)
		: FStageEconomyResultInfo::Ok();
}

#undef LOCTEXT_NAMESPACE
