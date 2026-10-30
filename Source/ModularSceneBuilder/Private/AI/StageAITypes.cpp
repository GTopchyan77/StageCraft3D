// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/StageAITypes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageAITypes)

bool FStageAICommand::IsQuestion() const
{
	switch (Intent)
	{
	case EStageAIIntent::StageSummary:
	case EStageAIIntent::ItemCount:
	case EStageAIIntent::TotalWeight:
	case EStageAIIntent::PowerDraw:
	case EStageAIIntent::FixtureCount:
	case EStageAIIntent::CountMatching:
	case EStageAIIntent::ListCues:
	case EStageAIIntent::EnvironmentStatus:
		return true;
	default:
		return false;
	}
}

bool FStageAICommand::IsAction() const
{
	switch (Intent)
	{
	case EStageAIIntent::AddFixtures:
	case EStageAIIntent::StoreCue:
	case EStageAIIntent::GoToCue:
	case EStageAIIntent::CueGo:
	case EStageAIIntent::CueBack:
	case EStageAIIntent::SetWeather:
	case EStageAIIntent::SetTimeOfDay:
	case EStageAIIntent::SetSky:
	case EStageAIIntent::Export:
	case EStageAIIntent::Undo:
	case EStageAIIntent::Redo:
	case EStageAIIntent::DeleteItems:
	case EStageAIIntent::MoveItems:
	case EStageAIIntent::ReplaceItems:
	case EStageAIIntent::BuildShape:
	case EStageAIIntent::SetFixture:
		return true;
	default:
		return false;
	}
}

FStageAIInterpretation FStageAIInterpretation::MakeCommand(const FStageAICommand& InCommand)
{
	FStageAIInterpretation Result;
	Result.Kind = EKind::Command;
	Result.Command = InCommand;
	return Result;
}

FStageAIInterpretation FStageAIInterpretation::MakeReply(const FText& InReply, EStageAIOutcome InOutcome)
{
	FStageAIInterpretation Result;
	Result.Kind = EKind::Reply;
	Result.Reply = InReply;
	Result.ReplyOutcome = InOutcome;
	return Result;
}

int32 FStageAISceneView::GetFixtureCount() const
{
	int32 Total = 0;
	for (const TPair<FString, int32>& Entry : FixturesByName)
	{
		Total += Entry.Value;
	}
	return Total;
}
