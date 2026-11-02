// Copyright Epic Games, Inc. All Rights Reserved.

#include "Marketplace/StageDispatchRules.h"

#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "StageCraftDispatch"

EStageDispatchAvailability StageDispatchRules::EvaluateTarget(const FStageDispatchTarget& Target, const FStageDispatchContext& Context)
{
	if (!Target.LeavesApp())
	{
		return EStageDispatchAvailability::Available;
	}
	if (!Context.bListingAllowsExport)
	{
		return EStageDispatchAvailability::NeedsExportRights;
	}

	switch (Target.Kind)
	{
	case EStageDispatchKind::SaveFile:
		return EStageDispatchAvailability::Available;

	case EStageDispatchKind::BridgeApp:
	{
		const FStageBridgeClientInfo* Client = Context.bBridgeRunning
			? Context.Clients.FindByPredicate([&Target](const FStageBridgeClientInfo& Info) { return Info.ClientId == Target.BridgeClientId; })
			: nullptr;
		if (!Client)
		{
			return EStageDispatchAvailability::NotConnected;
		}
		if (!Client->bImportsGlb)
		{
			return EStageDispatchAvailability::NeedsUsd;
		}
		return Context.bBridgeSending ? EStageDispatchAvailability::Busy : EStageDispatchAvailability::Available;
	}

	case EStageDispatchKind::AITool:
	{
		const FStageAIToolInfo* Tool = Context.Tools.FindByPredicate([&Target](const FStageAIToolInfo& Info) { return Info.ProviderId == Target.ProviderId; });
		if (!Tool || !Tool->Can(EStageAIToolCapability::AcceptModel))
		{
			return EStageDispatchAvailability::Unavailable;
		}
		if (Tool->KeyState == EStageAIToolKeyState::Missing)
		{
			return EStageDispatchAvailability::NeedsKey;
		}
		if (Tool->KeyState == EStageAIToolKeyState::Rejected)
		{
			return EStageDispatchAvailability::KeyRejected;
		}
		return Context.bToolBusy ? EStageDispatchAvailability::Busy : EStageDispatchAvailability::Available;
	}

	default:
		return EStageDispatchAvailability::Unavailable;
	}
}

bool StageDispatchRules::HasValidPath(const FStageDispatchTarget& Target)
{
	return Target.Kind != EStageDispatchKind::SaveFile
		|| (!Target.FilePath.IsEmpty() && !FPaths::IsRelative(Target.FilePath) && Target.FilePath.EndsWith(TEXT(".glb"), ESearchCase::IgnoreCase));
}

FText StageDispatchRules::Describe(EStageDispatchAvailability Availability)
{
	switch (Availability)
	{
	case EStageDispatchAvailability::Available: return FText::GetEmpty();
	case EStageDispatchAvailability::NeedsExportRights: return LOCTEXT("NeedsExportRights", "The publisher keeps this stage inside StageCraft.");
	case EStageDispatchAvailability::NotConnected: return LOCTEXT("NotConnected", "Not connected. Open the app's StageCraft add-on and switch on the bridge.");
	case EStageDispatchAvailability::NeedsUsd: return LOCTEXT("NeedsUsd", "Needs USD, which comes in a later update.");
	case EStageDispatchAvailability::NeedsKey: return LOCTEXT("NeedsKey", "Needs an API key (Window > Services & Keys).");
	case EStageDispatchAvailability::KeyRejected: return LOCTEXT("KeyRejected", "The saved key was refused. Save a new one in Window > Services & Keys.");
	case EStageDispatchAvailability::Busy: return LOCTEXT("Busy", "Busy with another hand-off.");
	case EStageDispatchAvailability::Unavailable:
	default: return LOCTEXT("Unavailable", "Not available.");
	}
}

#undef LOCTEXT_NAMESPACE
