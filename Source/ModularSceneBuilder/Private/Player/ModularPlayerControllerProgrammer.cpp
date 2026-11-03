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
