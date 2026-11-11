// Copyright Epic Games, Inc. All Rights Reserved.

// AModularPlayerController: MIDI and OSC remote control requests (Docs/ADR/0035), kept apart from the main file.

#include "Player/ModularPlayerController.h"

#include "Engine/World.h"
#include "Game/StageCraftGameModeBase.h"
#include "Remote/StageRemoteControlSubsystem.h"
#include "Remote/StageRemoteMapping.h"

#define LOCTEXT_NAMESPACE "StageCraftRemote"

namespace StageControllerRemote
{
	UStageRemoteControlSubsystem* GetRemote(const AModularPlayerController& Controller)
	{
		return UWorld::GetSubsystem<UStageRemoteControlSubsystem>(Controller.GetWorld());
	}

	/** The desk request a remote command is judged as (EvaluateCueRequest), and the number that goes with it. */
	EStageCueRequest CueRequestFor(const FStageRemoteCommand& Command, float& OutCueNumber)
	{
		switch (Command.Action)
		{
		case EStageRemoteAction::CueGo:
		case EStageRemoteAction::CueBack:
			OutCueNumber = 0.f;
			return EStageCueRequest::Play;
		case EStageRemoteAction::CueGoTo:
			OutCueNumber = Command.CueNumber;
			return EStageCueRequest::Play;
		case EStageRemoteAction::PlaybackLevel:
		case EStageRemoteAction::PlaybackFlash:
		case EStageRemoteAction::PlaybackGo:
		case EStageRemoteAction::PlaybackRelease:
			OutCueNumber = static_cast<float>(Command.Playback);
			return EStageCueRequest::PlaybackLive;
		default:
			OutCueNumber = 0.f;
			return EStageCueRequest::Masters;
		}
	}
}

void AModularPlayerController::BindRemoteControl()
{
	if (UStageRemoteControlSubsystem* Remote = StageControllerRemote::GetRemote(*this))
	{
		Remote->SetCommandHandler(UStageRemoteControlSubsystem::FCommandHandler::CreateUObject(this, &ThisClass::RequestRemoteCommand));
	}
}

void AModularPlayerController::UnbindRemoteControl()
{
	if (UStageRemoteControlSubsystem* Remote = StageControllerRemote::GetRemote(*this))
	{
		Remote->ClearCommandHandler(this);
	}
}

FStageEconomyResultInfo AModularPlayerController::RequestSetRemoteControl(const FStageRemoteSettings& Settings)
{
	UStageRemoteControlSubsystem* Remote = StageControllerRemote::GetRemote(*this);
	if (!Remote)
	{
		const FStageEconomyResultInfo Refusal = FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoRemote", "MIDI and OSC control is not available here."));
		ReportRejection(Refusal);
		return Refusal;
	}

	// Switching an input on is a capability the session may withhold; switching off never is.
	const bool bTurnsSomethingOn = Settings.OscScope != EStageOscScope::Off || !Settings.MidiInputName.TrimStartAndEnd().IsEmpty();
	if (bTurnsSomethingOn)
	{
		const AStageCraftGameModeBase* GameMode = GetStageGameMode();
		const FStageEconomyResultInfo Verdict = GameMode ? GameMode->EvaluateRemoteControl(this)
			: FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoRules", "MIDI and OSC control is not available here."));
		if (!Verdict.IsSuccess())
		{
			ReportRejection(Verdict);
			return Verdict;
		}
	}

	FText Error;
	if (!Remote->ApplySettings(Settings, Error))
	{
		const FStageEconomyResultInfo Refusal = FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, Error);
		ReportRejection(Refusal);
		return Refusal;
	}
	return FStageEconomyResultInfo::Ok();
}

FStageEconomyResultInfo AModularPlayerController::RequestRemoteCommand(const FStageRemoteCommand& Command)
{
	const AStageCraftGameModeBase* GameMode = GetStageGameMode();
	FStageEconomyResultInfo Verdict = GameMode ? GameMode->EvaluateRemoteControl(this)
		: FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoStage", "There is no stage to control here."));

	// Judged silently first, as the desk would be, so a refused fader does not raise a message per move. Letting go of a flash is
	// always allowed (RequestPlaybackFlash), so a refusal can never leave a playback stuck at full.
	const bool bFlashRelease = Command.Action == EStageRemoteAction::PlaybackFlash && !Command.IsDown();
	if (Verdict.IsSuccess() && !bFlashRelease)
	{
		float CueNumber = 0.f;
		const EStageCueRequest Request = StageControllerRemote::CueRequestFor(Command, CueNumber);
		Verdict = GameMode->EvaluateCueRequest(this, Request, CueNumber);
	}
	if (!Verdict.IsSuccess())
	{
		const FString Reason = Verdict.Message.ToString();
		if (Reason != LastRemoteRefusal)
		{
			LastRemoteRefusal = Reason;
			ReportRejection(Verdict);
		}
		return Verdict;
	}

	// The desk's own requests: the same write path as a click, re-judged by the GameMode there.
	bool bDone = false;
	switch (Command.Action)
	{
	case EStageRemoteAction::CueGo: bDone = RequestCueGo(); break;
	case EStageRemoteAction::CueBack: bDone = RequestCueBack(); break;
	case EStageRemoteAction::CueGoTo: bDone = RequestGoToCue(Command.CueNumber); break;
	case EStageRemoteAction::PlaybackLevel: bDone = RequestSetPlaybackLevel(Command.Playback, Command.Value); break;
	case EStageRemoteAction::PlaybackFlash: bDone = RequestPlaybackFlash(Command.Playback, Command.IsDown()); break;
	case EStageRemoteAction::PlaybackGo: bDone = RequestPlaybackGo(Command.Playback); break;
	case EStageRemoteAction::PlaybackRelease: bDone = RequestReleasePlayback(Command.Playback); break;
	case EStageRemoteAction::GrandMaster: bDone = RequestSetGrandMaster(Command.Value); break;
	case EStageRemoteAction::Blackout: bDone = RequestSetBlackout(Command.IsDown()); break;
	case EStageRemoteAction::AudioMaster: bDone = RequestSetAudioMaster(Command.Value); break;
	default: break;
	}
	if (!bDone)
	{
		// The show could not do it (the last cue, an empty playback, a level already there); the request reported what it had to.
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest,
			FText::Format(LOCTEXT("NotDone", "{0}: nothing to do."), StageRemote::DescribeAction(Command.Action, Command.Playback + 1)));
	}
	LastRemoteRefusal.Reset();
	return FStageEconomyResultInfo::Ok();
}

FStageEconomyResultInfo AModularPlayerController::RequestMidiLearn(EStageRemoteAction Action, int32 PlaybackNumber, float CueNumber)
{
	UStageRemoteControlSubsystem* Remote = StageControllerRemote::GetRemote(*this);
	const AStageCraftGameModeBase* GameMode = GetStageGameMode();
	FStageEconomyResultInfo Verdict = Remote && GameMode ? GameMode->EvaluateRemoteControl(this)
		: FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoLearn", "MIDI learn is not available here."));
	if (Verdict.IsSuccess() || (Remote && Action == EStageRemoteAction::None))
	{
		FText Error;
		if (Remote->StartMidiLearn(Action, PlaybackNumber, CueNumber, Error))
		{
			return FStageEconomyResultInfo::Ok();
		}
		Verdict = FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, Error);
	}
	ReportRejection(Verdict);
	return Verdict;
}

#undef LOCTEXT_NAMESPACE
