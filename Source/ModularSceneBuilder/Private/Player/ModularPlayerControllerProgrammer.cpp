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
#include "History/StageCueCommands.h"
#include "History/StageEditHistoryComponent.h"
#include "History/StageShowCommands.h"
#include "Interaction/StageParameterInterface.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Show/StageEffectTypes.h"
#include "Show/StageImageLookReader.h"
#include "Show/StageImagePalette.h"
#include "Show/StageShowMix.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageAudioReactiveSubsystem.h"
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

bool AModularPlayerController::RequestColorLookFromPhoto(const FString& FilePath)
{
	if (bColorLookPending)
	{
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("ColorLookBusy", "A photo's colours are already being read.")));
		return false;
	}
	TArray<TWeakObjectPtr<ALightingFixtureActor>> Fixtures;
	for (ALightingFixtureActor* Fixture : GetSelectedFixtures())
	{
		Fixtures.Add(Fixture);
	}
	if (Fixtures.IsEmpty())
	{
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest,
			LOCTEXT("ColorLookNoFixtures", "Select the fixtures that should take the photo's colours first.")));
		return false;
	}

	// As many colours as fixtures (up to the palette's limit), so a small selection gets the photo's main colours, not its noise.
	const int32 Colors = FMath::Clamp(Fixtures.Num(), StageImagePalette::MinColors, StageImagePalette::MaxColors);
	bColorLookPending = true;
	const FString FileName = FPaths::GetCleanFilename(FilePath);
	UE_LOG(LogStageCraft, Log, TEXT("%s: reading the colours of %s for %d fixture(s)."), *GetName(), *FileName, Fixtures.Num());
	StageImageLookReader::ReadAsync(FilePath, Colors, [WeakThis = TWeakObjectPtr<AModularPlayerController>(this), Fixtures = MoveTemp(Fixtures), FileName](FStageImageLookRead&& Read)
	{
		AModularPlayerController* Controller = WeakThis.Get();
		if (!Controller)
		{
			return;
		}
		Controller->bColorLookPending = false;
		if (!Read.Error.IsEmpty())
		{
			Controller->ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest,
				FText::Format(LOCTEXT("ColorLookUnreadable", "{0}: {1}"), FText::FromString(FileName), Read.Error)));
			return;
		}
		Controller->ApplyColorLook(Fixtures, Read.Swatches, FileName);
	});
	return true;
}

void AModularPlayerController::ApplyColorLook(const TArray<TWeakObjectPtr<ALightingFixtureActor>>& Fixtures, TConstArrayView<FStageImageSwatch> Swatches, const FString& FileName)
{
	if (Swatches.IsEmpty())
	{
		return;
	}
	float Brightest = 0.f;
	for (const FStageImageSwatch& Swatch : Swatches)
	{
		Brightest = FMath::Max(Brightest, Swatch.Luminance);
	}

	// Many writes, one undo step. When another edit is already open (a slider being dragged), these join it, as any edit would.
	const bool bOwnTransaction = BeginEditTransaction();
	int32 Lit = 0;
	int32 Index = 0;
	for (const TWeakObjectPtr<ALightingFixtureActor>& Weak : Fixtures)
	{
		ALightingFixtureActor* Fixture = Weak.Get();
		if (!IsValid(Fixture))
		{
			continue; // Deleted while the photo was read.
		}
		const FStageImageSwatch& Swatch = Swatches[Index++ % Swatches.Num()];
		// One fixture per write: each gets its own colour, and the fan never spreads one fixture's change over the others.
		const TArray<ALightingFixtureActor*> Single = { Fixture };
		const bool bColour = RequestProgrammerChange(Single, *Fixture, StageCraftTags::Attribute_ColorRGB, FStageParameterValue::MakeColor(Swatch.LightColor)).IsSuccess();
		const bool bDimmer = RequestProgrammerChange(Single, *Fixture, StageCraftTags::Attribute_Dimmer,
			FStageParameterValue::MakeFloat(StageImagePalette::DimmerFor(Swatch, Brightest))).IsSuccess();
		Lit += (bColour || bDimmer) ? 1 : 0;
	}
	if (bOwnTransaction)
	{
		EndEditTransaction();
	}
	UE_LOG(LogStageCraft, Log, TEXT("%s: colour look from %s: %d colour(s) on %d of %d fixture(s)."), *GetName(), *FileName, Swatches.Num(), Lit, Fixtures.Num());
}

bool AModularPlayerController::RequestClearProgrammer()
{
	UStageProgrammerSubsystem* Programmer = UWorld::GetSubsystem<UStageProgrammerSubsystem>(GetWorld());
	if (!Programmer || (Programmer->IsEmpty() && Programmer->GetEffectState().IsEmpty()))
	{
		return false;
	}
	// Clear empties the whole programmer, values, effects and stops (cue effects come back), as one undo step (Docs/ADR/0027 §4, §5).
	const FText Description = LOCTEXT("ClearProgrammer", "Clear Programmer");
	TArray<TSharedRef<IStageEditCommand>> Commands;
	AddProgrammerClear(*Programmer, Description, Commands);
	EditHistory->RecordAsOneStep(MoveTemp(Commands), Description);
	return true;
}

void AModularPlayerController::AddProgrammerClear(UStageProgrammerSubsystem& Programmer, const FText& Description, TArray<TSharedRef<IStageEditCommand>>& OutCommands)
{
	TArray<FShowCueFixtureState> Before = Programmer.GetEntries();
	TArray<int32> FixtureIds;
	for (const FShowCueFixtureState& Entry : Before)
	{
		FixtureIds.Add(Entry.FixtureId);
	}
	FStageProgrammerEffects EffectsBefore = Programmer.GetEffectState();
	Programmer.ClearAll();
	Programmer.SetEffectState(FStageProgrammerEffects());
	EffectsBeforePreview.Reset();
	if (!Before.IsEmpty())
	{
		OutCommands.Add(MakeShared<FStageProgrammerCommand>(Description, MoveTemp(FixtureIds), MoveTemp(Before), TArray<FShowCueFixtureState>()));
	}
	if (!EffectsBefore.IsEmpty())
	{
		OutCommands.Add(MakeShared<FStageProgrammerEffectsCommand>(Description, MoveTemp(EffectsBefore), FStageProgrammerEffects()));
	}
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

// --- Playbacks and masters (Docs/ADR/0026-cue-timing-tracking-playbacks.md §6) ---

bool AModularPlayerController::RequestRecordPlayback(int32 Index)
{
	UShowControlSubsystem* Show = ApproveCueRequest(EStageCueRequest::PlaybackEdit, static_cast<float>(Index));
	UStageProgrammerSubsystem* Programmer = UWorld::GetSubsystem<UStageProgrammerSubsystem>(GetWorld());
	if (!Show || !Programmer)
	{
		return false;
	}
	if (Programmer->IsEmpty() && Programmer->GetEffectState().IsEmpty())
	{
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest,
			LOCTEXT("PlaybackNeedsProgrammer", "The programmer is empty: set the look you want on this fader first.")));
		return false;
	}
	const TArray<FStagePlayback> Before = Show->GetPlaybacks();
	if (!Show->RecordPlayback(Index, Programmer->GetEntries(), Programmer->GetEffectState()))
	{
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("PlaybackFull", "This playback cannot take another cue.")));
		return false;
	}
	// As a cue store: the recorded values, effects and stops leave the programmer, and all of it is one undo step.
	const FText Description = FText::Format(LOCTEXT("RecordPlayback", "Record Playback {0}"), FText::AsNumber(Index + 1));
	TArray<TSharedRef<IStageEditCommand>> Commands;
	Commands.Add(MakeShared<FStagePlaybacksCommand>(Description, Before, Show->GetPlaybacks()));
	AddProgrammerClear(*Programmer, Description, Commands);
	EditHistory->RecordAsOneStep(MoveTemp(Commands), Description);
	return true;
}

bool AModularPlayerController::RequestClearPlayback(int32 Index)
{
	UShowControlSubsystem* Show = ApproveCueRequest(EStageCueRequest::PlaybackEdit, static_cast<float>(Index));
	if (!Show)
	{
		return false;
	}
	const TArray<FStagePlayback> Before = Show->GetPlaybacks();
	if (!Show->ClearPlayback(Index))
	{
		return false;
	}
	const FText Description = FText::Format(LOCTEXT("ClearPlayback", "Clear Playback {0}"), FText::AsNumber(Index + 1));
	TArray<TSharedRef<IStageEditCommand>> Commands;
	Commands.Add(MakeShared<FStagePlaybacksCommand>(Description, Before, Show->GetPlaybacks()));
	EditHistory->RecordAsOneStep(MoveTemp(Commands), Description);
	return true;
}

bool AModularPlayerController::RequestRenamePlayback(int32 Index, const FText& Name)
{
	UShowControlSubsystem* Show = ApproveCueRequest(EStageCueRequest::PlaybackEdit, static_cast<float>(Index));
	if (!Show)
	{
		return false;
	}
	const TArray<FStagePlayback> Before = Show->GetPlaybacks();
	if (!Show->RenamePlayback(Index, Name))
	{
		return false;
	}
	if (!Before[Index].Name.EqualTo(Show->GetPlaybacks()[Index].Name))
	{
		const FText Description = FText::Format(LOCTEXT("RenamePlayback", "Rename Playback {0}"), FText::AsNumber(Index + 1));
		TArray<TSharedRef<IStageEditCommand>> Commands;
		Commands.Add(MakeShared<FStagePlaybacksCommand>(Description, Before, Show->GetPlaybacks()));
		EditHistory->RecordAsOneStep(MoveTemp(Commands), Description);
	}
	return true;
}

bool AModularPlayerController::RequestSetPlaybackLevel(int32 Index, float Level)
{
	UShowControlSubsystem* Show = ApproveCueRequest(EStageCueRequest::PlaybackLive, static_cast<float>(Index));
	return Show && Show->SetPlaybackLevel(Index, Level);
}

bool AModularPlayerController::RequestPlaybackFlash(int32 Index, bool bDown)
{
	// Letting go of a flash is always allowed: a refusal must never leave a playback stuck at full.
	UShowControlSubsystem* Show = bDown ? ApproveCueRequest(EStageCueRequest::PlaybackLive, static_cast<float>(Index))
		: UWorld::GetSubsystem<UShowControlSubsystem>(GetWorld());
	return Show && Show->SetPlaybackFlash(Index, bDown);
}

bool AModularPlayerController::RequestPlaybackGo(int32 Index)
{
	UShowControlSubsystem* Show = ApproveCueRequest(EStageCueRequest::PlaybackLive, static_cast<float>(Index));
	return Show && Show->PlaybackGo(Index);
}

bool AModularPlayerController::RequestReleasePlayback(int32 Index)
{
	UShowControlSubsystem* Show = ApproveCueRequest(EStageCueRequest::PlaybackLive, static_cast<float>(Index));
	return Show && Show->ReleasePlayback(Index);
}

bool AModularPlayerController::RequestSetGrandMaster(float Level)
{
	UShowControlSubsystem* Show = ApproveCueRequest(EStageCueRequest::Masters, 0.f);
	if (Show)
	{
		Show->SetGrandMaster(Level);
	}
	return Show != nullptr;
}

bool AModularPlayerController::RequestSetBlackout(bool bOn)
{
	UShowControlSubsystem* Show = ApproveCueRequest(EStageCueRequest::Masters, 0.f);
	if (Show)
	{
		Show->SetBlackout(bOn);
	}
	return Show != nullptr;
}

bool AModularPlayerController::RequestSetAudioMaster(float Level)
{
	UShowControlSubsystem* Show = ApproveCueRequest(EStageCueRequest::Masters, 0.f);
	if (Show)
	{
		Show->SetAudioMaster(Level);
	}
	return Show != nullptr;
}

bool AModularPlayerController::RequestSetAudioKill(bool bOn)
{
	UShowControlSubsystem* Show = ApproveCueRequest(EStageCueRequest::Masters, 0.f);
	if (Show)
	{
		Show->SetAudioKill(bOn);
	}
	return Show != nullptr;
}

// --- Effects (Docs/ADR/0027-effects-engine.md §4, §5) ---

UStageProgrammerSubsystem* AModularPlayerController::ApproveEffectRequest()
{
	UStageProgrammerSubsystem* Programmer = UWorld::GetSubsystem<UStageProgrammerSubsystem>(GetWorld());
	if (!Programmer || !ApproveCueRequest(EStageCueRequest::Effects, 0.f))
	{
		return nullptr;
	}
	return Programmer;
}

void AModularPlayerController::RecordEffectsStep(const FText& Description, FStageProgrammerEffects Before, const UStageProgrammerSubsystem& Programmer)
{
	TArray<TSharedRef<IStageEditCommand>> Commands;
	Commands.Add(MakeShared<FStageProgrammerEffectsCommand>(Description, MoveTemp(Before), Programmer.GetEffectState()));
	EditHistory->RecordAsOneStep(MoveTemp(Commands), Description);
}

FGuid AModularPlayerController::RequestStartEffect(const FStageEffect& Effect)
{
	const TArray<FGuid> Ids = RequestStartEffects(MakeArrayView(&Effect, 1), FText::GetEmpty());
	return Ids.IsEmpty() ? FGuid() : Ids[0];
}

TArray<FGuid> AModularPlayerController::RequestStartEffects(TConstArrayView<FStageEffect> Effects, const FText& StepName)
{
	TArray<FGuid> Ids;
	UStageProgrammerSubsystem* Programmer = Effects.IsEmpty() ? nullptr : ApproveEffectRequest();
	if (!Programmer)
	{
		return Ids;
	}
	TArray<int32> FixtureIds;
	for (const ALightingFixtureActor* Fixture : GetSelectedFixtures())
	{
		FixtureIds.AddUnique(Fixture->GetFixtureId());
	}
	if (FixtureIds.IsEmpty())
	{
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest,
			LOCTEXT("EffectNeedsFixtures", "Select the fixtures the effect should run on first.")));
		return Ids;
	}
	FStageProgrammerEffects Before = Programmer->GetEffectState();
	for (const FStageEffect& Effect : Effects)
	{
		const FGuid Id = Programmer->AddEffect(Effect, FixtureIds);
		if (!Id.IsValid())
		{
			// All or nothing: the ones already added go again, so a half-started look never runs.
			Programmer->SetEffectState(MoveTemp(Before));
			ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, FText::Format(
				LOCTEXT("EffectsFull", "The programmer runs at most {0} effects. Stop one first."), FText::AsNumber(StageEffectRules::MaxEffects))));
			return TArray<FGuid>();
		}
		Ids.Add(Id);
	}
	EffectsBeforePreview.Reset();
	const FStageEffectEntry* First = Programmer->FindEffect(Ids[0]);
	const FText Description = !StepName.IsEmpty() ? StepName
		: FText::Format(LOCTEXT("StartEffect", "Start Effect: {0}"), First ? First->Effect.Name : FText::GetEmpty());
	RecordEffectsStep(Description, MoveTemp(Before), *Programmer);
	return Ids;
}

TArray<FGuid> AModularPlayerController::RequestFollowMusic()
{
	const TArray<FGuid> Ids = RequestStartEffects(StageEffectRules::MakeMusicLook(), LOCTEXT("FollowMusicStep", "Follow Music"));
	// Effects that follow music do nothing without a source. Starting SYSTEM is a request like the panel's SOURCE buttons (judged
	// by the GameMode); a source the user chose is left alone. Not part of the undo step: the source is a setting, not show data.
	const UStageAudioReactiveSubsystem* Audio = UWorld::GetSubsystem<UStageAudioReactiveSubsystem>(GetWorld());
	if (!Ids.IsEmpty() && Audio && Audio->GetState() == EStageAudioSourceState::Off)
	{
		UE_LOG(LogStageCraft, Log, TEXT("Follow Music: no source was listening; starting SYSTEM (this PC's sound)."));
		RequestSetAudioSource(EStageAudioSource::SystemAudio);
	}
	return Ids;
}

bool AModularPlayerController::RequestPreviewEffect(FGuid EffectId, const FStageEffect& Effect)
{
	UStageProgrammerSubsystem* Programmer = ApproveEffectRequest();
	if (!Programmer || !Programmer->FindEffect(EffectId))
	{
		return false;
	}
	if (!EffectsBeforePreview.IsSet())
	{
		EffectsBeforePreview = Programmer->GetEffectState();
	}
	Programmer->UpdateEffect(EffectId, Effect);
	return true;
}

bool AModularPlayerController::RequestUpdateEffect(FGuid EffectId, const FStageEffect& Effect)
{
	UStageProgrammerSubsystem* Programmer = ApproveEffectRequest();
	if (!Programmer || !Programmer->FindEffect(EffectId))
	{
		EffectsBeforePreview.Reset();
		return false;
	}
	FStageProgrammerEffects Before = EffectsBeforePreview.IsSet() ? MoveTemp(EffectsBeforePreview.GetValue()) : Programmer->GetEffectState();
	EffectsBeforePreview.Reset();
	Programmer->UpdateEffect(EffectId, Effect);
	if (Before.Equals(Programmer->GetEffectState()))
	{
		return true; // Dragged back to where it was: nothing to undo.
	}
	const FStageEffectEntry* Changed = Programmer->FindEffect(EffectId);
	RecordEffectsStep(FText::Format(LOCTEXT("EditEffect", "Edit Effect: {0}"), Changed ? Changed->Effect.Name : FText::GetEmpty()), MoveTemp(Before), *Programmer);
	return true;
}

bool AModularPlayerController::RequestStopEffect(FGuid EffectId)
{
	UStageProgrammerSubsystem* Programmer = ApproveEffectRequest();
	const UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(GetWorld());
	if (!Programmer || !Show)
	{
		return false;
	}
	EffectsBeforePreview.Reset();
	FStageProgrammerEffects Before = Programmer->GetEffectState();

	// What runs on stage, from any source: a cue's or playback's effect is held off by a stop in the programmer (Docs/ADR/0027 §5).
	TArray<FStageRunningEffect> Running;
	Show->GetRunningEffects(Running);
	TArray<FStageEffectEntry> Targets;
	for (const FStageRunningEffect& Effect : Running)
	{
		if (!Effect.bStopping && (!EffectId.IsValid() || Effect.Entry.Id == EffectId))
		{
			Targets.Add(Effect.Entry);
		}
	}
	if (const FStageEffectEntry* Own = EffectId.IsValid() ? Programmer->FindEffect(EffectId) : nullptr; Own && Targets.IsEmpty())
	{
		Targets.Add(*Own); // Not running (none of its fixtures could be found): still the programmer's to stop.
	}
	if (!EffectId.IsValid())
	{
		for (const FStageEffectEntry& Own : Programmer->GetEffects())
		{
			if (!Targets.ContainsByPredicate([&Own](const FStageEffectEntry& Target) { return Target.Id == Own.Id; }))
			{
				Targets.Add(Own);
			}
		}
	}
	if (Targets.IsEmpty())
	{
		return false;
	}

	FStageProgrammerEffects After = Before;
	for (const FStageEffectEntry& Target : Targets)
	{
		After.Effects.RemoveAll([&Target](const FStageEffectEntry& Own) { return Own.Id == Target.Id; });
		if (Show->FindShowEffect(Target.Id))
		{
			After.Stops.AddUnique(Target.Id);
		}
	}
	Programmer->SetEffectState(After);
	if (Before.Equals(Programmer->GetEffectState()))
	{
		return false;
	}
	const FText Description = EffectId.IsValid() ? FText::Format(LOCTEXT("StopEffect", "Stop Effect: {0}"), Targets[0].Effect.Name)
		: LOCTEXT("StopEffects", "Stop All Effects");
	RecordEffectsStep(Description, MoveTemp(Before), *Programmer);
	return true;
}

bool AModularPlayerController::RequestEditEffect(FGuid EffectId)
{
	UStageProgrammerSubsystem* Programmer = ApproveEffectRequest();
	const UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(GetWorld());
	const FStageEffectEntry* Entry = Show ? Show->FindShowEffect(EffectId) : nullptr;
	if (!Programmer || !Entry)
	{
		return false;
	}
	if (Programmer->FindEffect(EffectId))
	{
		return true; // Already the programmer's: nothing to take.
	}
	EffectsBeforePreview.Reset();
	FStageProgrammerEffects Before = Programmer->GetEffectState();
	const FText Name = Entry->Effect.Name;
	if (!Programmer->TakeEffect(*Entry))
	{
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, FText::Format(
			LOCTEXT("EffectsFullEdit", "The programmer runs at most {0} effects. Stop one first."), FText::AsNumber(StageEffectRules::MaxEffects))));
		return false;
	}
	RecordEffectsStep(FText::Format(LOCTEXT("TakeEffect", "Edit Effect from Cue: {0}"), Name), MoveTemp(Before), *Programmer);
	return true;
}

// --- Palettes (Docs/ADR/0028-palettes-and-references.md §4) ---

namespace StageProgrammerRequests
{
	/**
	 * Turns State's references to PaletteId into the palette's current value for that fixture (what the stage shows), so the palette
	 * can go without anything changing. True when State had any.
	 */
	bool BakeReferences(const UShowControlSubsystem& Show, FShowCueFixtureState& State, const FGuid& PaletteId)
	{
		FShowCueFixtureState Probe;
		Probe.FixtureId = State.FixtureId;
		Probe.Attributes = State.Attributes;
		for (const TPair<FGameplayTag, FGuid>& Reference : State.PaletteRefs)
		{
			if (Reference.Value == PaletteId)
			{
				Probe.PaletteRefs.Add(Reference.Key, Reference.Value);
			}
		}
		if (Probe.PaletteRefs.IsEmpty())
		{
			return false;
		}
		const TMap<FGameplayTag, float> Resolved = Show.ResolveReferences(Probe);
		for (const TPair<FGameplayTag, FGuid>& Reference : Probe.PaletteRefs)
		{
			State.Attributes.Add(Reference.Key, Resolved.FindRef(Reference.Key));
			State.PaletteRefs.Remove(Reference.Key);
		}
		return true;
	}
}

bool AModularPlayerController::EditPalettes(const FText& Description, TFunctionRef<bool(UStageShowLibrarySubsystem&)> Edit)
{
	UStageShowLibrarySubsystem* Library = UWorld::GetSubsystem<UStageShowLibrarySubsystem>(GetWorld());
	if (!Library)
	{
		return false;
	}
	TArray<FStagePalette> Before = Library->GetPalettes();
	if (!Edit(*Library))
	{
		return false;
	}
	TArray<TSharedRef<IStageEditCommand>> Commands;
	Commands.Add(MakeShared<FStagePalettesCommand>(Description, MoveTemp(Before), Library->GetPalettes()));
	EditHistory->RecordAsOneStep(MoveTemp(Commands), Description);
	return true;
}

bool AModularPlayerController::CapturePaletteContent(EStagePaletteKind Kind, EStagePaletteScope Scope, FStagePalette& OutContent)
{
	const UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(GetWorld());
	const TArray<ALightingFixtureActor*> Fixtures = GetSelectedFixtures();
	if (!Show || Fixtures.IsEmpty())
	{
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("PaletteNeedsFixtures", "Select the fixtures to record the palette from first.")));
		return false;
	}

	OutContent = FStagePalette();
	OutContent.Kind = Kind;
	if (Scope == EStagePaletteScope::PerFixture)
	{
		for (const ALightingFixtureActor* Fixture : Fixtures)
		{
			TMap<FGameplayTag, float> Values = StagePaletteRules::FilterValues(Kind, Show->GetRecordableValues(*Fixture));
			if (!Values.IsEmpty())
			{
				FShowCueFixtureState& Entry = OutContent.FixtureValues.AddDefaulted_GetRef();
				Entry.FixtureId = Fixture->GetFixtureId();
				Entry.Attributes = MoveTemp(Values);
			}
		}
	}
	else
	{
		// One look for any fixture, from the primary (the last selected). Colour is kept as a colour each fixture mixes its own way,
		// never as attribute values, whose meaning differs between colour systems (ADR 0028 §2).
		const ALightingFixtureActor& Primary = *Fixtures.Last();
		for (const TPair<FGameplayTag, float>& Pair : StagePaletteRules::FilterValues(Kind, Show->GetRecordableValues(Primary)))
		{
			if (StagePaletteRules::GetAttributeKind(Pair.Key) != EStagePaletteKind::Color)
			{
				OutContent.GlobalValues.Add(Pair.Key, Pair.Value);
			}
		}
		FStageParameterValue ColorValue;
		if (Kind == EStagePaletteKind::Color && IStageParameterInterface::Execute_GetParameterValue(&Primary, StageCraftTags::Attribute_ColorRGB, ColorValue)
			&& ColorValue.Type == EStageParameterType::Color)
		{
			OutContent.bHasColor = true;
			OutContent.Color = ColorValue.Color;
		}
	}

	if (!StagePaletteRules::HasContent(OutContent))
	{
		ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("PaletteNothingToRecord", "The selected fixtures have nothing of that kind to record.")));
		return false;
	}
	return true;
}

FStageEconomyResultInfo AModularPlayerController::RequestApplyPalette(FGuid PaletteId)
{
	const AStageCraftGameModeBase* GameMode = GetStageGameMode();
	UStageProgrammerSubsystem* Programmer = UWorld::GetSubsystem<UStageProgrammerSubsystem>(GetWorld());
	const UStageShowLibrarySubsystem* Library = UWorld::GetSubsystem<UStageShowLibrarySubsystem>(GetWorld());
	const FStagePalette* Palette = Library ? Library->FindPalette(PaletteId) : nullptr;
	const TArray<ALightingFixtureActor*> Fixtures = GetSelectedFixtures();
	TOptional<FStageEconomyResultInfo> Failure;
	if (!GameMode || !Programmer || !Library)
	{
		Failure = FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoPalettes", "Palettes need the StageCraft game mode."));
	}
	else if (!Palette)
	{
		Failure = FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("PaletteGone", "That palette no longer exists."));
	}
	else if (Fixtures.IsEmpty())
	{
		Failure = FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("ApplyNeedsFixtures", "Select the fixtures to apply the palette to first."));
	}
	if (Failure.IsSet())
	{
		ReportRejection(Failure.GetValue());
		return Failure.GetValue();
	}

	TArray<int32> FixtureIds;
	for (const ALightingFixtureActor* Fixture : Fixtures)
	{
		FixtureIds.AddUnique(Fixture->GetFixtureId());
	}
	const TArray<FShowCueFixtureState> Before = Programmer->CaptureFixtures(FixtureIds);
	const FText PaletteName = Palette->Name;

	TOptional<FStageEconomyResultInfo> FirstRefusal;
	int32 Applied = 0;
	for (ALightingFixtureActor* Fixture : Fixtures)
	{
		// What the palette gives this fixture, limited to the attributes the fixture has and the palette's kind.
		const int32 Id = Fixture->GetFixtureId();
		const TMap<FGameplayTag, float> ColorMatch = Palette->bHasColor ? Fixture->MatchColor(Palette->Color) : TMap<FGameplayTag, float>();
		TMap<FGameplayTag, float> Values;
		for (const TPair<FGameplayTag, float>& Own : Fixture->GetAttributes())
		{
			float Value = 0.f;
			if (StagePaletteRules::AllowsAttribute(Palette->Kind, Own.Key) && StagePaletteRules::FindPaletteValue(*Palette, Id, Own.Key, &ColorMatch, Value))
			{
				Values.Add(Own.Key, Value);
			}
		}
		if (Values.IsEmpty())
		{
			continue;
		}

		// Judged per attribute like any programmer edit; one refused attribute leaves the whole fixture out, so it never shows half a palette.
		bool bRefused = false;
		for (const TPair<FGameplayTag, float>& Pair : Values)
		{
			const FStageEconomyResultInfo Verdict = GameMode->EvaluateParameterChange(this, Fixture, Pair.Key, FStageParameterValue::MakeFloat(Pair.Value));
			if (!Verdict.IsSuccess())
			{
				if (!FirstRefusal.IsSet())
				{
					FirstRefusal = Verdict;
				}
				bRefused = true;
				break;
			}
		}
		if (bRefused)
		{
			continue;
		}
		Programmer->SetReferencedValues(Id, Values, PaletteId);
		++Applied;
	}

	if (FirstRefusal.IsSet())
	{
		ReportRejection(FirstRefusal.GetValue());
	}
	if (Applied == 0)
	{
		if (FirstRefusal.IsSet())
		{
			return FirstRefusal.GetValue();
		}
		const FStageEconomyResultInfo Nothing = FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest,
			FText::Format(LOCTEXT("PaletteNothingForSelection", "{0} has nothing for the selected fixtures."), PaletteName));
		ReportRejection(Nothing);
		return Nothing;
	}
	EditHistory->RecordProgrammerChange(FText::Format(LOCTEXT("ApplyPalette", "Apply {0}"), PaletteName), FixtureIds, Before);
	return FStageEconomyResultInfo::Ok();
}

FGuid AModularPlayerController::RequestRecordPalette(EStagePaletteKind Kind, const FText& Name, EStagePaletteScope Scope)
{
	FStagePalette Content;
	if (!CapturePaletteContent(Kind, Scope, Content))
	{
		return FGuid();
	}
	Content.Name = Name;
	FGuid Recorded;
	EditPalettes(LOCTEXT("RecordPalette", "Record Palette"), [&](UStageShowLibrarySubsystem& Library)
	{
		Recorded = Library.AddPalette(Content);
		if (!Recorded.IsValid())
		{
			ReportRejection(FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("PalettePoolFull", "The palette pool is full.")));
		}
		return Recorded.IsValid();
	});
	return Recorded;
}

bool AModularPlayerController::RequestUpdatePalette(FGuid PaletteId, EStagePaletteScope Scope)
{
	const UStageShowLibrarySubsystem* Library = UWorld::GetSubsystem<UStageShowLibrarySubsystem>(GetWorld());
	const FStagePalette* Palette = Library ? Library->FindPalette(PaletteId) : nullptr;
	FStagePalette Content;
	if (!Palette || !CapturePaletteContent(Palette->Kind, Scope, Content))
	{
		return false;
	}
	return EditPalettes(FText::Format(LOCTEXT("UpdatePalette", "Update Palette {0}"), Palette->Name),
		[&](UStageShowLibrarySubsystem& Edited) { return Edited.SetPaletteContent(PaletteId, Content); });
}

bool AModularPlayerController::RequestRenamePalette(FGuid PaletteId, const FText& Name)
{
	return EditPalettes(LOCTEXT("RenamePalette", "Rename Palette"), [&](UStageShowLibrarySubsystem& Library) { return Library.RenamePalette(PaletteId, Name); });
}

bool AModularPlayerController::RequestDeletePalette(FGuid PaletteId)
{
	UStageShowLibrarySubsystem* Library = UWorld::GetSubsystem<UStageShowLibrarySubsystem>(GetWorld());
	UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(GetWorld());
	UStageProgrammerSubsystem* Programmer = UWorld::GetSubsystem<UStageProgrammerSubsystem>(GetWorld());
	const FStagePalette* Palette = Library ? Library->FindPalette(PaletteId) : nullptr;
	if (!Palette || !Show || !Programmer)
	{
		return false;
	}
	const FText Description = FText::Format(LOCTEXT("DeletePalette", "Delete Palette {0}"), Palette->Name);
	TArray<TSharedRef<IStageEditCommand>> Commands;

	// While the palette still exists, every reference to it becomes the value it gives now: nothing on stage changes.
	const TArray<FShowCue> CuesBefore = Show->GetCues();
	TArray<FShowCue> CuesAfter = CuesBefore;
	bool bCuesChanged = false;
	for (FShowCue& Cue : CuesAfter)
	{
		for (FShowCueFixtureState& State : Cue.Fixtures)
		{
			bCuesChanged |= StageProgrammerRequests::BakeReferences(*Show, State, PaletteId);
		}
	}
	if (bCuesChanged)
	{
		Show->ReplaceCues(CuesAfter);
		Commands.Add(MakeShared<FStageCueListCommand>(Description, CuesBefore, Show->GetCues()));
	}

	TArray<FShowCueFixtureState> ProgrammerBefore = Programmer->GetEntries();
	TArray<FShowCueFixtureState> ProgrammerAfter = ProgrammerBefore;
	TArray<int32> ProgrammerFixtures;
	bool bProgrammerChanged = false;
	for (FShowCueFixtureState& Entry : ProgrammerAfter)
	{
		ProgrammerFixtures.Add(Entry.FixtureId);
		bProgrammerChanged |= StageProgrammerRequests::BakeReferences(*Show, Entry, PaletteId);
	}
	if (bProgrammerChanged)
	{
		Programmer->RestoreFixtures(ProgrammerFixtures, ProgrammerAfter);
		Commands.Add(MakeShared<FStageProgrammerCommand>(Description, ProgrammerFixtures, MoveTemp(ProgrammerBefore), Programmer->CaptureFixtures(ProgrammerFixtures)));
	}

	TArray<FStagePalette> PalettesBefore = Library->GetPalettes();
	Library->RemovePalette(PaletteId);
	Commands.Add(MakeShared<FStagePalettesCommand>(Description, MoveTemp(PalettesBefore), Library->GetPalettes()));
	EditHistory->RecordAsOneStep(MoveTemp(Commands), Description);
	return true;
}

#undef LOCTEXT_NAMESPACE
