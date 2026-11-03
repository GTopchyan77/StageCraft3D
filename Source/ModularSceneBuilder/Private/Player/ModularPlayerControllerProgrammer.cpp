// Copyright Epic Games, Inc. All Rights Reserved.

// AModularPlayerController's programmer, highlight and group requests (Docs/ADR/0025-programmer-and-output-mix.md), kept apart from
// the rest of the request bridge. Same rules as every request: the GameMode judges each fixture write, subsystems hold the state, the
// edit history records the step.

#include "Player/ModularPlayerController.h"

#include "Actors/LightingFixtureActor.h"
#include "Components/SelectionComponent.h"
#include "Data/StageParameterTypes.h"
#include "Engine/World.h"
#include "Game/StageCraftGameModeBase.h"
#include "History/StageEditHistoryComponent.h"
#include "Interaction/StageParameterInterface.h"
#include "Show/StageShowMix.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageProgrammerSubsystem.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "Subsystems/StageShowLibrarySubsystem.h"

#define LOCTEXT_NAMESPACE "StageCraftProgrammer"

TArray<ALightingFixtureActor*> AModularPlayerController::GetSelectedFixtures() const
{
	TArray<ALightingFixtureActor*> Fixtures;
	for (AActor* Selected : Selection->GetSelectedActors())
	{
		if (ALightingFixtureActor* Fixture = Cast<ALightingFixtureActor>(Selected); IsValid(Fixture))
		{
			Fixtures.Add(Fixture);
		}
	}
	return Fixtures;
}

FStageEconomyResultInfo AModularPlayerController::RequestProgrammerChange(TConstArrayView<ALightingFixtureActor*> Fixtures, const ALightingFixtureActor& Primary, FGameplayTag ParameterId, const FStageParameterValue& Value)
{
	const AStageCraftGameModeBase* GameMode = GetStageGameMode();
	UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(GetWorld());
	UStageProgrammerSubsystem* Programmer = UWorld::GetSubsystem<UStageProgrammerSubsystem>(GetWorld());
	if (!GameMode || !Session || !Programmer)
	{
		const FStageEconomyResultInfo Result = FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest,
			LOCTEXT("NoProgrammer", "Editing fixtures needs the StageCraft game mode."));
		ReportRejection(Result);
		return Result;
	}

	// The fan spreads the change made on the primary fixture; it needs a number to spread.
	double Change = 0.0;
	bool bFan = FanMode != EStageFanMode::Off && Value.Type == EStageParameterType::Float && Fixtures.Num() > 1;
	FStageParameterValue PrimaryBefore;
	if (bFan && IStageParameterInterface::Execute_GetParameterValue(&Primary, ParameterId, PrimaryBefore) && PrimaryBefore.Type == EStageParameterType::Float)
	{
		Change = Value.Float - PrimaryBefore.Float;
	}
	else
	{
		bFan = false;
	}

	TArray<int32> FixtureIds;
	for (const ALightingFixtureActor* Fixture : Fixtures)
	{
		if (IsValid(Fixture))
		{
			FixtureIds.AddUnique(Fixture->GetFixtureId());
		}
	}
	const TArray<FShowCueFixtureState> Before = Programmer->CaptureFixtures(FixtureIds);

	TOptional<FStageEconomyResultInfo> FirstRefusal;
	int32 Applied = 0;
	for (int32 Index = 0; Index < Fixtures.Num(); ++Index)
	{
		ALightingFixtureActor* Fixture = Fixtures[Index];
		if (!IsValid(Fixture))
		{
			continue;
		}
		FStageParameterValue FixtureValue = Value;
		if (bFan)
		{
			FStageParameterValue Current;
			if (!IStageParameterInterface::Execute_GetParameterValue(Fixture, ParameterId, Current) || Current.Type != EStageParameterType::Float)
			{
				continue;
			}
			FixtureValue.Float = Current.Float + Change * StageShowMix::FanFactor(FanMode, Index, Fixtures.Num());
		}

		const FStageEconomyResultInfo Verdict = GameMode->EvaluateParameterChange(this, Fixture, ParameterId, FixtureValue);
		if (!Verdict.IsSuccess())
		{
			if (!FirstRefusal.IsSet())
			{
				FirstRefusal = Verdict;
			}
			continue;
		}

		// The fixture translates the row into attribute values (a colour into its emitters, a gel into its filters); what that write
		// changed, and the attribute itself, is what the user now holds by hand.
		const TMap<FGameplayTag, float> OutputBefore = Fixture->GetAttributes();
		if (!Session->ApplyParameterChange(Fixture, ParameterId, FixtureValue))
		{
			continue;
		}
		TMap<FGameplayTag, float> Touched;
		for (const TPair<FGameplayTag, float>& Pair : Fixture->GetAttributes())
		{
			const float* Old = OutputBefore.Find(Pair.Key);
			if (!Old || !FMath::IsNearlyEqual(*Old, Pair.Value) || Pair.Key == ParameterId)
			{
				Touched.Add(Pair.Key, Pair.Value);
			}
		}
		Programmer->SetValues(Fixture->GetFixtureId(), Touched);
		++Applied;
	}

	if (FirstRefusal.IsSet())
	{
		ReportRejection(FirstRefusal.GetValue());
	}
	if (Applied == 0)
	{
		return FirstRefusal.IsSet() ? FirstRefusal.GetValue()
			: FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NotAcceptedByFixtures", "The value was not accepted."));
	}

	const FText Description = Applied == 1
		? FText::Format(LOCTEXT("ProgramOne", "{0}: {1}"), FindParameterName(Primary, ParameterId), Primary.GetInstanceLabel())
		: FText::Format(LOCTEXT("ProgramMany", "{0}: {1} fixtures"), FindParameterName(Primary, ParameterId), FText::AsNumber(Applied));
	EditHistory->RecordProgrammerChange(Description, FixtureIds, Before);
	return FStageEconomyResultInfo::Ok();
}

bool AModularPlayerController::RequestClearProgrammer()
{
	UStageProgrammerSubsystem* Programmer = UWorld::GetSubsystem<UStageProgrammerSubsystem>(GetWorld());
	if (!Programmer || Programmer->IsEmpty())
	{
		return false;
	}
	TArray<FShowCueFixtureState> Before = Programmer->GetEntries();
	TArray<int32> FixtureIds;
	for (const FShowCueFixtureState& Entry : Before)
	{
		FixtureIds.Add(Entry.FixtureId);
	}
	Programmer->ClearAll();
	EditHistory->RecordProgrammerChange(LOCTEXT("ClearProgrammer", "Clear Programmer"), FixtureIds, Before);
	return true;
}

void AModularPlayerController::RequestSetHighlight(bool bOn)
{
	bHighlightOn = bOn;
	RefreshHighlight();
}

void AModularPlayerController::RequestSetFanMode(EStageFanMode Mode)
{
	FanMode = Mode;
}

void AModularPlayerController::HandleSelectionSetChanged(int32 Count)
{
	if (bHighlightOn)
	{
		RefreshHighlight();
	}
}

void AModularPlayerController::RefreshHighlight()
{
	UStageProgrammerSubsystem* Programmer = UWorld::GetSubsystem<UStageProgrammerSubsystem>(GetWorld());
	if (!Programmer)
	{
		return;
	}
	TArray<int32> FixtureIds;
	if (bHighlightOn)
	{
		for (const ALightingFixtureActor* Fixture : GetSelectedFixtures())
		{
			FixtureIds.Add(Fixture->GetFixtureId());
		}
	}
	Programmer->SetHighlightedFixtures(FixtureIds);
}

bool AModularPlayerController::EditGroups(const FText& Description, TFunctionRef<bool(UStageShowLibrarySubsystem&)> Edit)
{
	UStageShowLibrarySubsystem* Library = UWorld::GetSubsystem<UStageShowLibrarySubsystem>(GetWorld());
	if (!Library)
	{
		return false;
	}
	const TArray<FStageFixtureGroup> Before = Library->GetGroups();
	if (!Edit(*Library))
	{
		return false;
	}
	EditHistory->RecordFixtureGroupsChange(Description, Before, Library->GetGroups());
	return true;
}

FGuid AModularPlayerController::RequestRecordGroup(const FText& Name)
{
	TArray<int32> FixtureIds;
	for (const ALightingFixtureActor* Fixture : GetSelectedFixtures())
	{
		FixtureIds.Add(Fixture->GetFixtureId());
	}
	if (FixtureIds.IsEmpty())
	{
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("GroupNeedsFixtures", "Select the fixtures for the group first.")));
		return FGuid();
	}
	FGuid Recorded;
	EditGroups(LOCTEXT("RecordGroup", "Record Group"), [&](UStageShowLibrarySubsystem& Library)
	{
		Recorded = Library.AddGroup(Name, FixtureIds);
		if (!Recorded.IsValid())
		{
			ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("GroupPoolFull", "The group pool is full.")));
		}
		return Recorded.IsValid();
	});
	return Recorded;
}

bool AModularPlayerController::RequestUpdateGroup(FGuid GroupId)
{
	TArray<int32> FixtureIds;
	for (const ALightingFixtureActor* Fixture : GetSelectedFixtures())
	{
		FixtureIds.Add(Fixture->GetFixtureId());
	}
	if (FixtureIds.IsEmpty())
	{
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("UpdateNeedsFixtures", "Select the fixtures for the group first.")));
		return false;
	}
	return EditGroups(LOCTEXT("UpdateGroup", "Update Group"), [&](UStageShowLibrarySubsystem& Library) { return Library.SetGroupFixtures(GroupId, FixtureIds); });
}

bool AModularPlayerController::RequestRenameGroup(FGuid GroupId, const FText& Name)
{
	return EditGroups(LOCTEXT("RenameGroup", "Rename Group"), [&](UStageShowLibrarySubsystem& Library) { return Library.RenameGroup(GroupId, Name); });
}

bool AModularPlayerController::RequestDeleteGroup(FGuid GroupId)
{
	return EditGroups(LOCTEXT("DeleteGroup", "Delete Group"), [&](UStageShowLibrarySubsystem& Library) { return Library.RemoveGroup(GroupId); });
}

bool AModularPlayerController::RequestSelectGroup(FGuid GroupId)
{
	const UStageShowLibrarySubsystem* Library = UWorld::GetSubsystem<UStageShowLibrarySubsystem>(GetWorld());
	const UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(GetWorld());
	const FStageFixtureGroup* Group = Library ? Library->FindGroup(GroupId) : nullptr;
	if (!Group || !Show)
	{
		return false;
	}
	TArray<AModularBaseActor*> Members;
	for (const int32 FixtureId : Group->FixtureIds)
	{
		if (ALightingFixtureActor* Fixture = Show->FindFixture(FixtureId))
		{
			Members.Add(Fixture);
		}
	}
	if (Members.IsEmpty())
	{
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest,
			FText::Format(LOCTEXT("GroupEmpty", "None of the fixtures in {0} is on the stage."), Group->Name)));
		return false;
	}
	RequestSelectItems(Members, EStageSelectionClick::Replace);
	return true;
}

#undef LOCTEXT_NAMESPACE
