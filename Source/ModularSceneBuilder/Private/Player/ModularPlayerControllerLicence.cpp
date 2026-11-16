// Copyright Epic Games, Inc. All Rights Reserved.

// AModularPlayerController: StageCraft Pro requests and the licence's limits on the network DMX (Docs/ADR/0034 §17), kept apart from the
// main file.

#include "Player/ModularPlayerController.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Game/StageCraftGameModeBase.h"
#include "Licence/StageLicenceSubsystem.h"
#include "Remote/StageRemoteControlSubsystem.h"
#include "Remote/StageWebRemoteSubsystem.h"
#include "Show/StageDMXNetworkTypes.h"
#include "Subsystems/StageDMXNetworkSubsystem.h"

#define LOCTEXT_NAMESPACE "StageCraftLicence"

namespace StageControllerLicence
{
	UStageLicenceSubsystem* GetLicence(const AModularPlayerController& Controller)
	{
		const UGameInstance* GameInstance = Controller.GetGameInstance();
		return GameInstance ? GameInstance->GetSubsystem<UStageLicenceSubsystem>() : nullptr;
	}

	/**
	 * MIDI, OSC and the Web Remote are open network ports and devices: refusing every command while they stay open would still leave a
	 * listener on the LAN for a capability the user no longer has. So a refusal closes them, and the saved settings bring them back.
	 * Deny by default: without a GameMode there is no one to allow them.
	 */
	void PushRemoteCapabilities(const AModularPlayerController& Controller, const AStageCraftGameModeBase* GameMode)
	{
		UWorld* World = Controller.GetWorld();
		if (UStageRemoteControlSubsystem* Remote = UWorld::GetSubsystem<UStageRemoteControlSubsystem>(World))
		{
			const FStageEconomyResultInfo Verdict = GameMode ? GameMode->EvaluateRemoteControl(&Controller)
				: FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoRemoteRules", "MIDI and OSC control is not available here."));
			Remote->SetCapabilityAllowed(Verdict.IsSuccess(), Verdict.Message);
		}
		if (UStageWebRemoteSubsystem* Web = UWorld::GetSubsystem<UStageWebRemoteSubsystem>(World))
		{
			const FStageEconomyResultInfo Verdict = GameMode ? GameMode->EvaluateWebRemote(&Controller)
				: FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoWebRules", "The Web Remote is not available here."));
			Web->SetCapabilityAllowed(Verdict.IsSuccess(), Verdict.Message);
		}
	}
}

FStageEconomyResultInfo AModularPlayerController::RefuseLicenceRequest(const FText& Error)
{
	const FStageEconomyResultInfo Refusal = FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest,
		Error.IsEmpty() ? LOCTEXT("NoLicence", "StageCraft Pro is not available here.") : Error);
	ReportRejection(Refusal);
	return Refusal;
}

FStageEconomyResultInfo AModularPlayerController::RequestProCheckout(EStageProPlan Plan)
{
	UStageLicenceSubsystem* Licence = StageControllerLicence::GetLicence(*this);
	FText Error;
	return Licence && Licence->StartCheckout(Plan, Error) ? FStageEconomyResultInfo::Ok() : RefuseLicenceRequest(Error);
}

FStageEconomyResultInfo AModularPlayerController::RequestManageSubscription()
{
	UStageLicenceSubsystem* Licence = StageControllerLicence::GetLicence(*this);
	FText Error;
	return Licence && Licence->OpenManageSubscription(Error) ? FStageEconomyResultInfo::Ok() : RefuseLicenceRequest(Error);
}

FStageEconomyResultInfo AModularPlayerController::RequestRefreshLicence()
{
	UStageLicenceSubsystem* Licence = StageControllerLicence::GetLicence(*this);
	FText Error;
	return Licence && Licence->RequestRefresh(Error) ? FStageEconomyResultInfo::Ok() : RefuseLicenceRequest(Error);
}

FStageEconomyResultInfo AModularPlayerController::RequestReleaseLicence()
{
	UStageLicenceSubsystem* Licence = StageControllerLicence::GetLicence(*this);
	FText Error;
	return Licence && Licence->ReleaseThisMachine(Error) ? FStageEconomyResultInfo::Ok() : RefuseLicenceRequest(Error);
}

void AModularPlayerController::ApplyLicenceLimits()
{
	// Deny by default: without a GameMode there is no one to grant Pro, so the Free limits apply.
	const AStageCraftGameModeBase* GameMode = GetStageGameMode();
	StageControllerLicence::PushRemoteCapabilities(*this, GameMode);

	UStageDMXNetworkSubsystem* Network = UWorld::GetSubsystem<UStageDMXNetworkSubsystem>(GetWorld());
	if (!Network)
	{
		return;
	}
	FStageDMXCapabilityLimits Limits;
	Limits.bInputAllowed = GameMode && GameMode->EvaluateDmxInput(this).IsSuccess();
	Limits.MaxOutputUniverses = GameMode ? GameMode->GetDmxOutputUniverseLimit(this) : AStageCraftGameModeBase::FreeDmxOutputUniverses;
	if (Network->SetCapabilityLimits(Limits))
	{
		// Input and External go together (RequestSetDMXInput): a show left on External without input would freeze.
		RequestSetControlSource(Network->GetState() == EStageDMXInputState::Listening ? EStageControlSource::External : EStageControlSource::Internal);
	}
}

#undef LOCTEXT_NAMESPACE
