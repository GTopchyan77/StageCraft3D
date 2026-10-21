// Copyright Epic Games, Inc. All Rights Reserved.

#include "Game/StageCraftGameModeBase.h"

#include "Actors/ModularBaseActor.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Interaction/StageTransformRules.h"
#include "Player/ModularPlayerController.h"
#include "Player/StageCameraPawn.h"
#include "Subsystems/ShowControlSubsystem.h"
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

FStageEconomyResultInfo AStageCraftGameModeBase::EvaluatePlacement_Implementation(const APlayerController* Player, const UBaseItemData* Item, int32 Quantity) const
{
	const UStageSessionSubsystem* Session = GetSession();
	const UStageEconomySubsystem* Economy = GetEconomy();
	if (!Player || !Item || Quantity < 1 || !Session || (SessionRules.bEnforceEntitlements && !Economy))
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
	return Session->EvaluateSessionLimits(Item, Quantity);
}

FStageEconomyResultInfo AStageCraftGameModeBase::EvaluateCueRequest_Implementation(const APlayerController* Player, EStageCueRequest Request, float CueNumber) const
{
	const UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(GetWorld());
	if (!Player || !Show)
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoShowControl", "Cues are not available in this level."));
	}

	switch (Request)
	{
	case EStageCueRequest::Store:
		if (!StageCueRules::IsValidCueNumber(CueNumber))
		{
			return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, FText::Format(
				LOCTEXT("BadCueNumber", "Cue numbers run from {0} to {1}."),
				FText::AsNumber(StageCueRules::MinCueNumber), FText::AsNumber(StageCueRules::MaxCueNumber)));
		}
		return FStageEconomyResultInfo::Ok();

	case EStageCueRequest::Update:
	case EStageCueRequest::Delete:
		if (!StageCueRules::IsValidCueNumber(CueNumber) || !Show->FindCue(CueNumber))
		{
			return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoSuchCue", "That cue does not exist."));
		}
		return FStageEconomyResultInfo::Ok();

	case EStageCueRequest::Play:
		// The subsystem refuses too; asking here gives the user a reason instead of a silent no.
		if (Show->GetControlSource() == EStageControlSource::External)
		{
			return FStageEconomyResultInfo::Fail(EStageEconomyResult::SessionRuleViolation,
				LOCTEXT("ExternalControl", "An external console is driving the fixtures. Switch to Internal to play cues."));
		}
		return FStageEconomyResultInfo::Ok();

	case EStageCueRequest::SetControlSource:
		return FStageEconomyResultInfo::Ok();
	}

	// Deny by default: a request kind added without a rule here is refused.
	return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("UnknownCueRequest", "That cue action is not available."));
}

FStageEconomyResultInfo AStageCraftGameModeBase::EvaluateModelImport_Implementation(const APlayerController* Player) const
{
	if (!Player)
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("CannotImport", "Model import is not available."));
	}
	if (!SessionRules.bAllowModelImport)
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::SessionRuleViolation, LOCTEXT("ImportDisabled", "Importing models is switched off for this session."));
	}
	return FStageEconomyResultInfo::Ok();
}

FStageEconomyResultInfo AStageCraftGameModeBase::EvaluateParameterChange_Implementation(const APlayerController* Player, const UObject* Target, FGameplayTag ParameterId, const FStageParameterValue& Value) const
{
	const UStageEconomySubsystem* Economy = GetEconomy();
	if (!Player || !Target || (SessionRules.bEnforceEntitlements && !Economy))
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("CannotEdit", "Editing is not available."));
	}

	// Targets are usually placed items; other parameter owners (future show objects) only have ownership rules.
	if (const AModularBaseActor* Item = Cast<AModularBaseActor>(Target))
	{
		const FStageEconomyResultInfo ItemVerdict = EvaluateItemEdit(*Item, ParameterId, Value);
		if (!ItemVerdict.IsSuccess())
		{
			return ItemVerdict;
		}
	}

	return SessionRules.bEnforceEntitlements
		? Economy->ValidateParameterChange(Target, ParameterId, Value)
		: FStageEconomyResultInfo::Ok();
}

FStageEconomyResultInfo AStageCraftGameModeBase::EvaluateTransformChange(const APlayerController* Player, const AModularBaseActor& Item, const FTransform& NewTransform) const
{
	if (!StageTransformRules::IsSaneTransform(NewTransform))
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest,
			FText::Format(LOCTEXT("InsaneTransform", "{0} cannot be placed there: the position is invalid or more than 10 km away."), Item.GetInstanceLabel()));
	}

	// Only the parts that change are judged, so restoring a rotation is not refused for a rule that only concerns scale.
	const FTransform Current = Item.GetActorTransform();
	if (!Current.GetLocation().Equals(NewTransform.GetLocation(), UE_KINDA_SMALL_NUMBER))
	{
		const FStageEconomyResultInfo Verdict = EvaluateParameterChange(Player, &Item, StageCraftTags::Param_Transform_Location, FStageParameterValue::MakeVector(NewTransform.GetLocation()));
		if (!Verdict.IsSuccess())
		{
			return Verdict;
		}
	}
	if (!Current.GetRotation().Equals(NewTransform.GetRotation(), UE_KINDA_SMALL_NUMBER))
	{
		const FStageEconomyResultInfo Verdict = EvaluateParameterChange(Player, &Item, StageCraftTags::Param_Transform_Rotation, FStageParameterValue::MakeRotator(NewTransform.Rotator()));
		if (!Verdict.IsSuccess())
		{
			return Verdict;
		}
	}
	if (!Current.GetScale3D().Equals(NewTransform.GetScale3D(), UE_KINDA_SMALL_NUMBER))
	{
		return EvaluateParameterChange(Player, &Item, StageCraftTags::Param_Transform_Scale, FStageParameterValue::MakeVector(NewTransform.GetScale3D()));
	}
	return FStageEconomyResultInfo::Ok();
}

FStageEconomyResultInfo AStageCraftGameModeBase::EvaluateItemEdit(const AModularBaseActor& Item, const FGameplayTag& ParameterId, const FStageParameterValue& Value) const
{
	const bool bShapesItem = StageCraftTags::IsTransformParameter(ParameterId) || StageCraftTags::IsShapeParameter(ParameterId);
	if (bShapesItem && Item.IsTransformLocked())
	{
		// Not EStageEconomyResult::Locked: that code means "not owned" and makes the inspector offer a purchase.
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::SessionRuleViolation,
			FText::Format(LOCTEXT("ItemLocked", "{0} is locked. Unlock it in the Outliner or the inspector to move or resize it."), Item.GetInstanceLabel()));
	}

	if (ParameterId == StageCraftTags::Param_Transform_Scale && !Item.SupportsScaling())
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest,
			FText::Format(LOCTEXT("NoScale", "{0} is sized by its Length, not by scale."), Item.GetInstanceLabel()));
	}

	if (StageCraftTags::IsQuantityParameter(ParameterId))
	{
		const UStageSessionSubsystem* Session = GetSession();
		if (!Session)
		{
			return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("CannotResize", "Resizing is not available."));
		}
		return Session->EvaluateQuantityIncrease(Item.GetItemData(), Item.PredictQuantity(ParameterId, Value) - Item.GetQuantity());
	}
	return FStageEconomyResultInfo::Ok();
}

#undef LOCTEXT_NAMESPACE
