// Copyright Epic Games, Inc. All Rights Reserved.

#include "Player/ModularPlayerController.h"

#include "Engine/GameInstance.h"
#include "Fixtures/StageGdtfShareSubsystem.h"
#include "Game/StageCraftGameModeBase.h"

#define LOCTEXT_NAMESPACE "StageCraftGdtfShare"

namespace StageControllerGdtfShare
{
	UStageGdtfShareSubsystem* GetShare(const AModularPlayerController& Controller)
	{
		const UGameInstance* GameInstance = Controller.GetGameInstance();
		return GameInstance ? GameInstance->GetSubsystem<UStageGdtfShareSubsystem>() : nullptr;
	}

	FStageEconomyResultInfo ToRejection(const FStageGdtfShareResultInfo& Result)
	{
		const EStageEconomyResult Code = Result.Code == EStageGdtfShareResult::NotAllowed ? EStageEconomyResult::SessionRuleViolation : EStageEconomyResult::InvalidRequest;
		return FStageEconomyResultInfo::Fail(Code, Result.Message);
	}
}

bool AModularPlayerController::RequestGdtfShareSignIn(const FString& UserName, const FString& Password)
{
	const AStageCraftGameModeBase* GameMode = GetStageGameMode();
	UStageGdtfShareSubsystem* Share = StageControllerGdtfShare::GetShare(*this);
	const FStageEconomyResultInfo Verdict = GameMode && Share
		? GameMode->EvaluateOnlineFixtureSource(this)
		: FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoGdtfShare", "GDTF Share needs the StageCraft game mode."));
	if (!Verdict.IsSuccess())
	{
		ReportRejection(Verdict);
		return false;
	}
	const FStageGdtfShareResultInfo Result = Share->SignIn(UserName, Password);
	if (!Result.IsSuccess())
	{
		ReportRejection(StageControllerGdtfShare::ToRejection(Result));
	}
	return Result.IsSuccess();
}

void AModularPlayerController::RequestGdtfShareSignOut()
{
	if (UStageGdtfShareSubsystem* Share = StageControllerGdtfShare::GetShare(*this))
	{
		Share->SignOut();
	}
}

bool AModularPlayerController::RequestGdtfShareRefresh(bool bForce)
{
	const AStageCraftGameModeBase* GameMode = GetStageGameMode();
	UStageGdtfShareSubsystem* Share = StageControllerGdtfShare::GetShare(*this);
	const FStageEconomyResultInfo Verdict = GameMode && Share
		? GameMode->EvaluateOnlineFixtureSource(this)
		: FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoGdtfShare", "GDTF Share needs the StageCraft game mode."));
	if (!Verdict.IsSuccess())
	{
		ReportRejection(Verdict);
		return false;
	}
	const FStageGdtfShareResultInfo Result = Share->RefreshList(bForce);
	if (!Result.IsSuccess())
	{
		ReportRejection(StageControllerGdtfShare::ToRejection(Result));
	}
	return Result.IsSuccess();
}

bool AModularPlayerController::RequestGdtfShareDownload(int32 RevisionId)
{
	const AStageCraftGameModeBase* GameMode = GetStageGameMode();
	UStageGdtfShareSubsystem* Share = StageControllerGdtfShare::GetShare(*this);
	const FStageEconomyResultInfo Verdict = GameMode && Share
		? GameMode->EvaluateOnlineFixtureSource(this)
		: FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoGdtfShare", "GDTF Share needs the StageCraft game mode."));
	if (!Verdict.IsSuccess())
	{
		ReportRejection(Verdict);
		return false;
	}
	const FStageGdtfShareResultInfo Result = Share->Download(RevisionId);
	if (!Result.IsSuccess())
	{
		ReportRejection(StageControllerGdtfShare::ToRejection(Result));
	}
	return Result.IsSuccess();
}

bool AModularPlayerController::RequestGdtfShareCancelDownload(int32 RevisionId)
{
	UStageGdtfShareSubsystem* Share = StageControllerGdtfShare::GetShare(*this);
	return Share && Share->CancelDownload(RevisionId);
}

#undef LOCTEXT_NAMESPACE
