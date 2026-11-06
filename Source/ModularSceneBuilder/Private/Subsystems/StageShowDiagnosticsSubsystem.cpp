// Copyright Epic Games, Inc. All Rights Reserved.

#include "Subsystems/StageShowDiagnosticsSubsystem.h"

#include "Actors/LightingFixtureActor.h"
#include "Data/BaseItemData.h"
#include "Data/LightingFixtureData.h"
#include "Data/StageParameterTypes.h"
#include "Show/StageEffectRunner.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageAudioReactiveSubsystem.h"
#include "Subsystems/StageDMXNetworkSubsystem.h"
#include "Subsystems/StageDMXPatchSubsystem.h"
#include "Subsystems/StageProgrammerSubsystem.h"
#include "Subsystems/StageShowLibrarySubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageShowDiagnosticsSubsystem)

bool UStageShowDiagnosticsSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Diagnoses a stage being played, like the show control it reads.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UStageShowDiagnosticsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	ShowControl = Collection.InitializeDependency<UShowControlSubsystem>();
	Patch = Collection.InitializeDependency<UStageDMXPatchSubsystem>();
	Programmer = Collection.InitializeDependency<UStageProgrammerSubsystem>();
	Library = Collection.InitializeDependency<UStageShowLibrarySubsystem>();
	Network = Collection.InitializeDependency<UStageDMXNetworkSubsystem>();
	Audio = Collection.InitializeDependency<UStageAudioReactiveSubsystem>();

	if (ShowControl)
	{
		ShowControl->OnFixturesChanged.AddUniqueDynamic(this, &UStageShowDiagnosticsSubsystem::HandleDynamicChange);
		ShowControl->OnCueListChanged.AddUniqueDynamic(this, &UStageShowDiagnosticsSubsystem::HandleDynamicChange);
		ShowControl->OnPlaybacksChanged.AddUniqueDynamic(this, &UStageShowDiagnosticsSubsystem::HandleDynamicChange);
		ShowControl->OnDeskStateChanged.AddUniqueDynamic(this, &UStageShowDiagnosticsSubsystem::HandleDynamicChange);
		ShowControl->OnControlSourceChanged.AddUniqueDynamic(this, &UStageShowDiagnosticsSubsystem::HandleControlSourceChanged);
		// Effects start and stop with cues.
		ShowControl->OnCueStarted.AddUniqueDynamic(this, &UStageShowDiagnosticsSubsystem::HandleCueEvent);
	}
	if (Patch)
	{
		Patch->OnPatchChanged.AddUniqueDynamic(this, &UStageShowDiagnosticsSubsystem::HandleDynamicChange);
	}
	if (Library)
	{
		Library->OnPalettesChanged.AddUniqueDynamic(this, &UStageShowDiagnosticsSubsystem::HandleDynamicChange);
	}
	if (Programmer)
	{
		ProgrammerFixturesHandle = Programmer->OnFixturesChanged.AddUObject(this, &UStageShowDiagnosticsSubsystem::HandleProgrammerFixturesChanged);
		ProgrammerEffectsHandle = Programmer->OnEffectsChanged.AddUObject(this, &UStageShowDiagnosticsSubsystem::MarkDirty);
	}
	if (Network)
	{
		InputHandle = Network->OnInputChanged.AddUObject(this, &UStageShowDiagnosticsSubsystem::MarkDirty);
		OutputHandle = Network->OnOutputChanged.AddUObject(this, &UStageShowDiagnosticsSubsystem::MarkDirty);
	}
	if (Audio)
	{
		AudioHandle = Audio->OnAudioStatusChanged.AddUObject(this, &UStageShowDiagnosticsSubsystem::MarkDirty);
	}
}

void UStageShowDiagnosticsSubsystem::Deinitialize()
{
	if (RefreshTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RefreshTicker);
		RefreshTicker.Reset();
	}
	if (ShowControl)
	{
		ShowControl->OnFixturesChanged.RemoveAll(this);
		ShowControl->OnCueListChanged.RemoveAll(this);
		ShowControl->OnPlaybacksChanged.RemoveAll(this);
		ShowControl->OnDeskStateChanged.RemoveAll(this);
		ShowControl->OnControlSourceChanged.RemoveAll(this);
		ShowControl->OnCueStarted.RemoveAll(this);
	}
	if (Patch)
	{
		Patch->OnPatchChanged.RemoveAll(this);
	}
	if (Library)
	{
		Library->OnPalettesChanged.RemoveAll(this);
	}
	if (Programmer)
	{
		Programmer->OnFixturesChanged.Remove(ProgrammerFixturesHandle);
		Programmer->OnEffectsChanged.Remove(ProgrammerEffectsHandle);
	}
	if (Network)
	{
		Network->OnInputChanged.Remove(InputHandle);
		Network->OnOutputChanged.Remove(OutputHandle);
	}
	if (Audio)
	{
		Audio->OnAudioStatusChanged.Remove(AudioHandle);
	}
	OnProblemsChanged.Clear();
	ShowControl = nullptr;
	Patch = nullptr;
	Programmer = nullptr;
	Library = nullptr;
	Network = nullptr;
	Audio = nullptr;
	Super::Deinitialize();
}

void UStageShowDiagnosticsSubsystem::HandleDynamicChange()
{
	MarkDirty();
}

void UStageShowDiagnosticsSubsystem::HandleControlSourceChanged(EStageControlSource NewSource)
{
	MarkDirty();
}

void UStageShowDiagnosticsSubsystem::HandleCueEvent(float CueNumber)
{
	MarkDirty();
}

void UStageShowDiagnosticsSubsystem::HandleProgrammerFixturesChanged(TConstArrayView<int32> FixtureIds)
{
	MarkDirty();
}

void UStageShowDiagnosticsSubsystem::MarkDirty()
{
	bDirty = true;
	if (OnProblemsChanged.IsBound() && !RefreshTicker.IsValid())
	{
		RefreshTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UStageShowDiagnosticsSubsystem::HandleRefreshTick));
	}
}

bool UStageShowDiagnosticsSubsystem::HandleRefreshTick(float DeltaSeconds)
{
	RefreshTicker.Reset();
	if (!bDirty)
	{
		return false;
	}
	const TArray<FStageDiagnostic> Before = Problems;
	GetProblems();
	if (Problems != Before)
	{
		OnProblemsChanged.Broadcast();
	}
	return false;
}

const TArray<FStageDiagnostic>& UStageShowDiagnosticsSubsystem::GetProblems()
{
	if (bDirty)
	{
		RefreshProblems();
	}
	return Problems;
}

const TArray<FStageDiagnostic>& UStageShowDiagnosticsSubsystem::RefreshProblems()
{
	FStageShowSnapshot Snapshot;
	MakeSnapshot(Snapshot);
	Problems = StageShowDiagnostics::Run(Snapshot);
	bDirty = false;
	return Problems;
}

void UStageShowDiagnosticsSubsystem::MakeSnapshot(FStageShowSnapshot& Snapshot)
{
	if (ShowControl)
	{
		for (const ALightingFixtureActor* Fixture : ShowControl->GetFixtures())
		{
			if (Fixture)
			{
				Snapshot.Fixtures.Add({ Fixture->GetFixtureId(), Fixture->GetPatch() });
			}
		}
		Snapshot.Cues = ShowControl->GetCues();
		Snapshot.Playbacks = ShowControl->GetPlaybacks();
		Snapshot.ControlSource = ShowControl->GetControlSource();
		Snapshot.GrandMaster = ShowControl->GetGrandMaster();
		Snapshot.bBlackout = ShowControl->IsBlackout();
		Snapshot.bAudioKill = ShowControl->IsAudioKilled();
		TArray<FStageRunningEffect> Running;
		ShowControl->GetRunningEffects(Running);
		Snapshot.bEffectsRunning = !Running.IsEmpty();
		Snapshot.bAudioEffectsRunning = Running.ContainsByPredicate([](const FStageRunningEffect& Effect) { return Effect.Entry.Effect.Audio.Mode != EStageEffectAudioMode::Off; });
	}
	if (Patch)
	{
		Snapshot.ConflictingFixtureIds = Patch->FindPatchConflicts();
	}
	if (Library)
	{
		for (const FStagePalette& Palette : Library->GetPalettes())
		{
			Snapshot.Palettes.Add(Palette.Id);
		}
	}
	if (Programmer && !Programmer->IsEmpty())
	{
		for (const FShowCueFixtureState& Entry : Programmer->GetEntries())
		{
			Snapshot.ProgrammerFixtureIds.Add(Entry.FixtureId);
		}
	}
	if (Network)
	{
		const FStageDMXInputStatus Input = Network->GetStatus();
		Snapshot.InputState = Input.State;
		Snapshot.InputError = Input.Error;
		Snapshot.LiveUniverses = Input.LiveUniverses;
		const FStageDMXOutputStatus Output = Network->GetOutputStatus();
		Snapshot.OutputState = Output.State;
		Snapshot.OutputError = Output.Error;
		Snapshot.MutedUniverses = Network->GetOutputSettings().MutedUniverses;
	}
	if (Audio)
	{
		const FStageAudioStatus Status = Audio->GetStatus();
		Snapshot.AudioSource = Status.Source;
		Snapshot.AudioState = Status.State;
		Snapshot.AudioError = Status.Error;
		Snapshot.bAudioSignal = Status.Features.bSignal;
	}
}

FStageFixtureFacts UStageShowDiagnosticsSubsystem::GatherFixtureFacts(int32 FixtureId) const
{
	FStageFixtureFacts Facts;
	Facts.FixtureId = FixtureId;
	const ALightingFixtureActor* Fixture = ShowControl ? ShowControl->FindFixture(FixtureId) : nullptr;
	if (!Fixture)
	{
		return Facts;
	}
	Facts.bExists = true;
	if (const UBaseItemData* Item = Fixture->GetItemData())
	{
		Facts.Name = Item->DisplayName.ToString();
	}

	Facts.ControlSource = ShowControl->GetControlSource();
	Facts.Patch = Fixture->GetPatch();
	if (Network)
	{
		Facts.InputState = Network->GetState();
		Facts.bUniverseLive = Facts.Patch.IsPatched() && Network->GetStatus().LiveUniverses.Contains(Facts.Patch.Universe);
		Facts.OutputState = Network->GetOutputState();
		Facts.bUniverseMuted = Facts.Patch.IsPatched() && StageDMXOutput::IsMuted(Network->GetOutputSettings(), Facts.Patch.Universe);
	}
	Facts.bPatchConflict = Patch && Patch->FindPatchConflicts().Contains(FixtureId);

	const FGameplayTag Dimmer = StageCraftTags::Attribute_Dimmer;
	Facts.bHasDimmer = Fixture->GetAttributes().Contains(Dimmer);
	Facts.Dimmer = ShowControl->ExplainAttribute(FixtureId, Dimmer);
	Facts.OutputDimmer = Fixture->GetAttribute(Dimmer);
	if (ShowControl->GetActiveCueNumber() >= 0.f)
	{
		Facts.ActiveCue = ShowControl->GetActiveCueNumber();
	}
	Facts.DimmerCue = ShowControl->FindSettingCue(FixtureId, Dimmer);
	if (ShowControl->GetPlaybacks().IsValidIndex(Facts.Dimmer.Playback))
	{
		Facts.PlaybackName = ShowControl->GetPlaybacks()[Facts.Dimmer.Playback].Name;
		Facts.PlaybackLevel = ShowControl->GetPlaybackLevel(Facts.Dimmer.Playback);
	}

	Facts.ColorFraction = static_cast<float>(Fixture->GetBeamMix().LuminousFraction);
	if (const ULightingFixtureData* Data = Fixture->GetFixtureData())
	{
		Facts.bHasIris = Data->bHasIris;
	}
	Facts.Iris = Facts.bHasIris ? Fixture->GetAttribute(StageCraftTags::Attribute_Iris) : 0.f;

	Facts.GrandMaster = ShowControl->GetGrandMaster();
	Facts.bBlackout = ShowControl->IsBlackout();
	Facts.bAudioKill = ShowControl->IsAudioKilled();
	Facts.FlashLimitHz = ShowControl->GetEffectFlashLimitHz();
	TArray<FStageRunningEffect> Running;
	ShowControl->GetRunningEffects(Running);
	for (const FStageRunningEffect& Effect : Running)
	{
		if (Effect.Entry.Effect.Target == EStageEffectTarget::Intensity && Effect.Entry.FixtureIds.Contains(FixtureId))
		{
			Facts.IntensityEffects.Add(Effect.Entry.Effect.Name);
			Facts.bAudioEffect |= Effect.Entry.Effect.Audio.Mode != EStageEffectAudioMode::Off;
		}
	}
	return Facts;
}

FStageFixtureVerdict UStageShowDiagnosticsSubsystem::ExplainFixture(int32 FixtureId, EStageAILanguage Language) const
{
	return StageFixtureExplain::Explain(GatherFixtureFacts(FixtureId), Language);
}
