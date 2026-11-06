// Copyright Epic Games, Inc. All Rights Reserved.

#include "Subsystems/ShowControlSubsystem.h"

#include "Actors/LightingFixtureActor.h"
#include "Camera/PlayerCameraManager.h"
#include "Data/StageParameterTypes.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "ModularSceneBuilder.h"
#include "Show/StageDMXBridge.h"
#include "Show/StageCueTracking.h"
#include "Show/StageDMXPatching.h"
#include "Show/StageShadowBudget.h"
#include "Show/StageShowMix.h"
#include "Subsystems/StageAudioReactiveSubsystem.h"
#include "Subsystems/StageDMXPatchSubsystem.h"
#include "Subsystems/StageProgrammerSubsystem.h"
#include "Subsystems/StageShowLibrarySubsystem.h"
#include "TimerManager.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ShowControlSubsystem)

namespace
{
	constexpr int32 DMXUniverseSize = StageDMXPatching::UniverseSize;

	/** Whether Fixture's channels hold the same values in Previous as in Current (both full 512-slot frames). */
	bool AreFixtureChannelsUnchanged(const ALightingFixtureActor& Fixture, TConstArrayView<uint8> Previous, TConstArrayView<uint8> Current)
	{
		const int32 First = Fixture.GetPatch().Address - 1;
		const int32 Count = FMath::Min(Fixture.GetDMXFootprint(), DMXUniverseSize - First);
		return First >= 0 && Count > 0 && FMemory::Memcmp(Previous.GetData() + First, Current.GetData() + First, Count) == 0;
	}
}

bool UShowControlSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Shows run in play sessions only; editor worlds have no fixtures registering.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UShowControlSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Patch = Collection.InitializeDependency<UStageDMXPatchSubsystem>();
	Programmer = Collection.InitializeDependency<UStageProgrammerSubsystem>();
	if (Programmer)
	{
		ProgrammerChangedHandle = Programmer->OnFixturesChanged.AddUObject(this, &ThisClass::HandleProgrammerFixturesChanged);
		ProgrammerEffectsHandle = Programmer->OnEffectsChanged.AddUObject(this, &ThisClass::HandleProgrammerEffectsChanged);
	}
	AudioReactive = Collection.InitializeDependency<UStageAudioReactiveSubsystem>();
	if (AudioReactive)
	{
		AudioStatusHandle = AudioReactive->OnAudioStatusChanged.AddUObject(this, &ThisClass::HandleAudioStatusChanged);
		HandleAudioStatusChanged();
	}
	Library =Collection.InitializeDependency<UStageShowLibrarySubsystem>();
	if (Library)
	{
		Library->OnPalettesChanged.AddUniqueDynamic(this, &ThisClass::HandlePalettesChanged);
	}
	Playbacks.SetNum(StagePlaybackRules::Count);
	PlaybackRuntime.SetNum(StagePlaybackRules::Count);
}

void UShowControlSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ShadowBudgetTimer);
	}
	SetDMXBridge(nullptr);
	if (Programmer)
	{
		Programmer->OnFixturesChanged.Remove(ProgrammerChangedHandle);
		Programmer->OnEffectsChanged.Remove(ProgrammerEffectsHandle);
	}
	ProgrammerChangedHandle.Reset();
	ProgrammerEffectsHandle.Reset();
	EffectRunner = FStageEffectRunner();
	MainEffects.Reset();
	Programmer = nullptr;
	if (AudioReactive)
	{
		AudioReactive->OnAudioStatusChanged.Remove(AudioStatusHandle);
	}
	AudioStatusHandle.Reset();
	AudioReactive = nullptr;
	if (Library)
	{
		Library->OnPalettesChanged.RemoveDynamic(this, &ThisClass::HandlePalettesChanged);
	}
	Library = nullptr;
	PlaybackValues.Reset();
	Playbacks.Reset();
	PlaybackRuntime.Reset();
	Fixtures.Reset();
	Fades.Reset();
	Strobes.Reset();
	LastInputFrames.Reset();
	Patch = nullptr;

	Super::Deinitialize();
}

TStatId UShowControlSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UShowControlSubsystem, STATGROUP_Tickables);
}

bool UShowControlSubsystem::IsTickable() const
{
	// Output ticks only while a universe waits to be sent: the sender keeps an unchanged stream alive on its own thread.
	return !Fades.IsEmpty() || PendingAutoGo.IsSet() || !Strobes.IsEmpty() || AreEffectsLive() || (IsDMXOutputActive() && HasPendingDMXOutput());
}

void UShowControlSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
#if !UE_BUILD_SHIPPING
	const double TickStart = FPlatformTime::Seconds();
#endif

	// The effect clocks move first, so a fading fixture an effect also drives is mixed with this frame's effect value. Fades before
	// strobes: a fade may change a fixture's Shutter, which changes its strobe rate this frame.
	const bool bEffectsLive = AreEffectsLive();
	TArray<int32> ReleasedByEffects;
	if (bEffectsLive)
	{
		// One snapshot of the music per tick, and only while an effect follows it (Docs/ADR/0029 §4): a lock-free copy, no allocation.
		TOptional<StageEffects::FAudioInput> Audio;
		if (EffectRunner.NeedsAudio() && AudioReactive)
		{
			Audio = StageEffects::FAudioInput::FromFeatures(AudioReactive->GetFeatures(), FPlatformTime::Seconds());
#if !UE_BUILD_SHIPPING
			++AudioReadsForTests;
#endif
		}
		EffectRunner.Advance(DeltaTime, ReleasedByEffects, Audio.GetPtrOrNull());
	}
	TickFades(DeltaTime);
	if (bEffectsLive)
	{
		TickEffects();
		// An effect that finished fading out leaves its fixtures: they go back to their static look in full.
		ResolveFixtures(ReleasedByEffects);
	}
	TickAutoGo(DeltaTime);
	TickStrobes(DeltaTime);
	TickDMXOutput(DeltaTime);

#if !UE_BUILD_SHIPPING
	const double TickSeconds = FPlatformTime::Seconds() - TickStart;
	++TickCostStats.Ticks;
	TickCostStats.TotalSeconds += TickSeconds;
	TickCostStats.MaxSeconds = FMath::Max(TickCostStats.MaxSeconds, TickSeconds);
#endif
}

// --- Strobe clock ---

void UShowControlSubsystem::SetFixtureStrobing(ALightingFixtureActor* Fixture, bool bStrobing)
{
	if (!Fixture)
	{
		return;
	}

	const int32 Index = Strobes.IndexOfByPredicate([Fixture](const FStrobeState& State) { return State.Fixture == Fixture; });
	if (bStrobing && Index == INDEX_NONE)
	{
		// Starts open, so a strobe begins with a flash rather than a gap.
		FStrobeState& State = Strobes.AddDefaulted_GetRef();
		State.Fixture = Fixture;
	}
	else if (!bStrobing && Index != INDEX_NONE)
	{
		Strobes.RemoveAtSwap(Index);
		Fixture->SetStrobeOpen(true);
	}
}

void UShowControlSubsystem::TickStrobes(float DeltaTime)
{
	for (int32 Index = Strobes.Num() - 1; Index >= 0; --Index)
	{
		FStrobeState& State = Strobes[Index];
		ALightingFixtureActor* Fixture = State.Fixture.Get();
		const float Rate = Fixture ? Fixture->GetStrobeRateHz() : 0.f;
		if (!Fixture || Rate <= 0.f)
		{
			// Destroyed without unregistering, or its shutter opened through a path that bypassed SetFixtureStrobing.
			if (Fixture)
			{
				Fixture->SetStrobeOpen(true);
			}
			Strobes.RemoveAtSwap(Index);
			continue;
		}

		State.Phase = FMath::Frac(State.Phase + static_cast<double>(DeltaTime) * Rate);
		const bool bOpen = State.Phase < StrobeDutyCycle;
		if (bOpen != State.bOpen)
		{
			State.bOpen = bOpen;
			Fixture->SetStrobeOpen(bOpen);
		}
	}
}

// --- Fixtures & patch ---

void UShowControlSubsystem::RegisterFixture(ALightingFixtureActor* Fixture)
{
	if (!Fixture || Fixtures.Contains(Fixture))
	{
		return;
	}

	if (Fixture->GetFixtureId() <= 0 || !IsFixtureIdFree(Fixture->GetFixtureId(), Fixture))
	{
		Fixture->SetFixtureId(GetNextFreeFixtureId());
	}

	Fixtures.Add(Fixture);
	UE_LOG(LogStageCraft, Log, TEXT("Fixture %d registered (%s)."), Fixture->GetFixtureId(), *Fixture->GetName());
	// A fixture the show already has data for (a re-spawn, an undone delete, a reused ID) takes its place in the mix at once; a colour
	// palette the playing cue refers to could only be mixed now that the fixture is here.
	const int32 Id = Fixture->GetFixtureId();
	RefreshPlaybackReferences(Id);
	bool bPlaybackHolds = false;
	for (int32 Index = 0; Index < PlaybackRuntime.Num(); ++Index)
	{
		if (PlaybackRuntime[Index].Output.Contains(Id))
		{
			TArray<int32> Ignored;
			RefreshPlaybackOutput(Index, Ignored);
			bPlaybackHolds = true;
		}
	}
	if (bPlaybackHolds || PlaybackValues.Contains(Id) || (Programmer && (Programmer->FindValues(Id) || Programmer->IsHighlighted(Id))))
	{
		ResolveFixture(*Fixture);
	}
	if (EffectRunner.IsRunning())
	{
		RefreshEffectFixtures();
		if (EffectRunner.DrivesFixture(Id))
		{
			ResolveFixture(*Fixture);
		}
	}
	MarkShadowBudgetDirty();
	OnFixturesChanged.Broadcast();
}

void UShowControlSubsystem::UnregisterFixture(ALightingFixtureActor* Fixture)
{
	if (Fixtures.Remove(Fixture) > 0)
	{
		if (EffectRunner.IsRunning())
		{
			RefreshEffectFixtures();
		}
		Fades.RemoveAll([Fixture](const FAttributeFade& Fade) { return Fade.Fixture == Fixture; });
		Strobes.RemoveAll([Fixture](const FStrobeState& State) { return State.Fixture == Fixture; });
		ShadowSwitchTimes.Remove(Fixture);
		MarkShadowBudgetDirty(); // Its place in the budget is free for the next brightest.
		OnFixturesChanged.Broadcast();
	}
}

void UShowControlSubsystem::MarkShadowBudgetDirty()
{
	ArmShadowBudgetTimer(GetDefault<UStageShowPerformanceSettings>()->GetUpdateSeconds());
}

void UShowControlSubsystem::ArmShadowBudgetTimer(float DelaySeconds)
{
	UWorld* World = GetWorld();
	if (!World || World->GetTimerManager().IsTimerActive(ShadowBudgetTimer))
	{
		return; // Already armed: one re-ranking covers every change until it runs.
	}
	World->GetTimerManager().SetTimer(ShadowBudgetTimer, FTimerDelegate::CreateUObject(this, &ThisClass::UpdateShadowBudget), DelaySeconds, /*bLoop*/ false);
}

void UShowControlSubsystem::UpdateShadowBudget()
{
	const UStageShowPerformanceSettings* Settings = GetDefault<UStageShowPerformanceSettings>();
	const TArray<ALightingFixtureActor*> Current = GetFixtures();

	// The local viewer: shadows matter most on what the camera is close to. Without one, distance counts as equal.
	const UWorld* World = GetWorld();
	const APlayerController* Viewer = World ? World->GetFirstPlayerController() : nullptr;
	const TOptional<FVector> ViewLocation = Viewer && Viewer->PlayerCameraManager
		? TOptional<FVector>(Viewer->PlayerCameraManager->GetCameraLocation()) : TOptional<FVector>();

	const double Now = World ? World->GetTimeSeconds() : 0.0;
	TArray<FStageShadowCandidate> Candidates;
	Candidates.Reserve(Current.Num());
	for (ALightingFixtureActor* Fixture : Current)
	{
		FStageShadowCandidate& Candidate = Candidates.AddDefaulted_GetRef();
		const double Distance = ViewLocation.IsSet() ? FVector::Dist(ViewLocation.GetValue(), Fixture->GetActorLocation()) : 100.0;
		Candidate.Importance = StageShadowBudget::GetImportance(Fixture->GetShadowRankingFlux(), Distance);
		Candidate.bCasting = Fixture->AreBeamShadowsEnabled();
		if (const double* SwitchedAt = ShadowSwitchTimes.Find(Fixture))
		{
			Candidate.SecondsSinceSwitch = FMath::Max(0.0, Now - *SwitchedAt);
		}
	}

	const FStageShadowSelection Selection = StageShadowBudget::SelectCastersWithHold(Candidates, Settings->GetShadowBudget(), Settings->GetHysteresis(), Settings->GetMinHoldSeconds());
	int32 Switched = 0;
	for (int32 Index = 0; Index < Current.Num(); ++Index)
	{
		if (Current[Index]->AreBeamShadowsEnabled() != Selection.Casting[Index])
		{
			Current[Index]->SetBeamShadowsEnabled(Selection.Casting[Index]);
			ShadowSwitchTimes.Add(Current[Index], Now);
			++Switched;
		}
	}
	if (Switched > 0)
	{
		UE_LOG(LogStageCraft, Verbose, TEXT("Shadow budget: %d of %d fixtures cast shadows (%d switched)."), GetShadowedFixtureCount(), Current.Num(), Switched);
	}
	// A held fixture still has to follow its rank once its hold ends, even if nothing else changes by then.
	if (Selection.RetryAfterSeconds > 0.0)
	{
		ArmShadowBudgetTimer(static_cast<float>(Selection.RetryAfterSeconds) + 0.01f);
	}
}

int32 UShowControlSubsystem::GetShadowedFixtureCount() const
{
	int32 Count = 0;
	for (const TWeakObjectPtr<ALightingFixtureActor>& Fixture : Fixtures)
	{
		Count += Fixture.IsValid() && Fixture->AreBeamShadowsEnabled() ? 1 : 0;
	}
	return Count;
}

TArray<ALightingFixtureActor*> UShowControlSubsystem::GetFixtures() const
{
	TArray<ALightingFixtureActor*> Result;
	Result.Reserve(Fixtures.Num());
	for (const TWeakObjectPtr<ALightingFixtureActor>& Fixture : Fixtures)
	{
		if (ALightingFixtureActor* Resolved = Fixture.Get())
		{
			Result.Add(Resolved);
		}
	}
	Result.Sort([](const ALightingFixtureActor& A, const ALightingFixtureActor& B) { return A.GetFixtureId() < B.GetFixtureId(); });
	return Result;
}

ALightingFixtureActor* UShowControlSubsystem::FindFixture(int32 FixtureId) const
{
	for (const TWeakObjectPtr<ALightingFixtureActor>& Fixture : Fixtures)
	{
		if (Fixture.IsValid() && Fixture->GetFixtureId() == FixtureId)
		{
			return Fixture.Get();
		}
	}
	return nullptr;
}

bool UShowControlSubsystem::IsFixtureIdFree(int32 FixtureId, const ALightingFixtureActor* Ignore) const
{
	const ALightingFixtureActor* Existing = FindFixture(FixtureId);
	return !Existing || Existing == Ignore;
}

int32 UShowControlSubsystem::GetNextFreeFixtureId() const
{
	int32 Highest = 0;
	for (const TWeakObjectPtr<ALightingFixtureActor>& Fixture : Fixtures)
	{
		if (Fixture.IsValid())
		{
			Highest = FMath::Max(Highest, Fixture->GetFixtureId());
		}
	}
	return Highest + 1;
}

// --- Cues ---

int32 UShowControlSubsystem::FindCueIndex(float CueNumber) const
{
	// Stored numbers are rounded to the resolution, so half a step finds exactly one cue.
	return Cues.IndexOfByPredicate([CueNumber](const FShowCue& Cue)
	{
		return FMath::IsNearlyEqual(Cue.Number, CueNumber, StageCueRules::NumberResolution * 0.5f);
	});
}

const FShowCue* UShowControlSubsystem::FindCue(float CueNumber) const
{
	const int32 Index = FindCueIndex(CueNumber);
	return Index != INDEX_NONE ? &Cues[Index] : nullptr;
}

void UShowControlSubsystem::GetCueState(float CueNumber, StageCueTracking::FFixtureStates& OutState) const
{
	StageCueTracking::StateAt(Cues, FindCueIndex(CueNumber), OutState);
}

TArray<FShowCueFixtureState> UShowControlSubsystem::CaptureLook() const
{
	// Palette references carry into the cue (Docs/ADR/0028 §3): what the programmer holds keeps the programmer's reference, the rest
	// keeps the playing cue's. Not mid-fade: the output is then between two values, so no reference describes it.
	StageCueTracking::FFixtureStates Playing;
	if (Fades.IsEmpty())
	{
		GetCueState(ActiveCueNumber, Playing);
	}
	TArray<FShowCueFixtureState> Look;
	for (ALightingFixtureActor* Fixture : GetFixtures())
	{
		FShowCueFixtureState& State = Look.AddDefaulted_GetRef();
		State.FixtureId = Fixture->GetFixtureId();
		State.Attributes = GetRecordableValues(*Fixture);

		const int32 Id = State.FixtureId;
		const TMap<FGameplayTag, float>* Held = Programmer ? Programmer->FindValues(Id) : nullptr;
		const TMap<FGameplayTag, FGuid>* HeldRefs = Programmer ? Programmer->FindReferences(Id) : nullptr;
		const FShowCueFixtureState* PlayingState = Playing.Find(Id);
		for (const TPair<FGameplayTag, float>& Pair : State.Attributes)
		{
			const bool bHeld = Held && Held->Contains(Pair.Key);
			const TMap<FGameplayTag, FGuid>* Source = bHeld ? HeldRefs : (PlayingState ? &PlayingState->PaletteRefs : nullptr);
			if (const FGuid* Reference = Source ? Source->Find(Pair.Key) : nullptr)
			{
				State.PaletteRefs.Add(Pair.Key, *Reference);
			}
		}
	}
	return Look;
}

bool UShowControlSubsystem::StoreCue(float CueNumber, const FText& Label, float FadeTime, EStageCueStoreMode Mode)
{
	if (!StageCueRules::IsValidCueNumber(CueNumber))
	{
		UE_LOG(LogStageCraft, Warning, TEXT("StoreCue: %g is not a valid cue number; nothing was stored."), CueNumber);
		return false;
	}
	const float Number = StageCueRules::RoundCueNumber(CueNumber);
	const TArray<FShowCueFixtureState> Programmed = Programmer ? Programmer->GetEntries() : TArray<FShowCueFixtureState>();
	const FStageProgrammerEffects ProgrammedEffects = Programmer ? Programmer->GetEffectState() : FStageProgrammerEffects();
	if (Mode != EStageCueStoreMode::Look && Programmed.IsEmpty() && ProgrammedEffects.IsEmpty())
	{
		UE_LOG(LogStageCraft, Warning, TEXT("StoreCue: the programmer is empty, so a tracking store of cue %.3f has nothing to record."), Number);
		return false;
	}

	int32 Index = FindCueIndex(Number);
	const bool bExisting = Index != INDEX_NONE;
	if (!bExisting)
	{
		const int32 Insert = Cues.IndexOfByPredicate([Number](const FShowCue& Other) { return Other.Number > Number; });
		Index = Insert == INDEX_NONE ? Cues.Num() : Insert;
	}

	if (Mode == EStageCueStoreMode::Look)
	{
		// The whole look as a blocked cue, replacing the cue (what every store did before tracking, Docs/ADR/0026 §3).
		FShowCue Cue;
		Cue.Number = Number;
		Cue.Label = StageCueRules::SanitizeLabel(Label);
		Cue.FadeTime = StageCueRules::SanitizeFadeTime(FadeTime);
		Cue.bBlock = true;
		Cue.Fixtures = CaptureLook();
		Cue.Effects = CaptureEffects(); // Every effect on stage, as it runs (Docs/ADR/0027 §5).
		StageCueRules::SanitizeCueEffects(Cue);
		if (bExisting)
		{
			Cue.Timing = Cues[Index].Timing; // Re-recording a look keeps how the cue plays.
			Cues[Index] = MoveTemp(Cue);
		}
		else
		{
			Cues.Insert(MoveTemp(Cue), Index);
		}
	}
	else
	{
		// Tracking: only what the programmer holds, merged in. Cue Only first notes the look the next cue had, at this position.
		StageCueTracking::FFixtureStates Before;
		StageCueTracking::StateAt(Cues, bExisting ? Index : Index - 1, Before);
		TArray<FStageEffectEntry> EffectsBefore;
		StageCueTracking::EffectsAt(Cues, bExisting ? Index : Index - 1, EffectsBefore);
		if (bExisting)
		{
			FShowCue& Cue = Cues[Index];
			const FText NewLabel = StageCueRules::SanitizeLabel(Label);
			Cue.Label = NewLabel.IsEmpty() ? Cue.Label : NewLabel;
			StageCueTracking::MergeInto(Cue, Programmed);
		}
		else
		{
			FShowCue Cue;
			Cue.Number = Number;
			Cue.Label = StageCueRules::SanitizeLabel(Label);
			Cue.FadeTime = StageCueRules::SanitizeFadeTime(FadeTime);
			Cue.Fixtures = Programmed;
			Cues.Insert(MoveTemp(Cue), Index);
		}
		// The programmer's effects and stops track like its values (Docs/ADR/0027 §5).
		StageCueTracking::MergeEffectsInto(Cues[Index], ProgrammedEffects.Effects, ProgrammedEffects.Stops);
		StageCueRules::SanitizeCueEffects(Cues[Index]);
		if (Mode == EStageCueStoreMode::CueOnly)
		{
			StageCueTracking::KeepNextCueEffects(Cues, Index, EffectsBefore, ProgrammedEffects.Effects, ProgrammedEffects.Stops);
			if (Cues.IsValidIndex(Index + 1))
			{
				StageCueRules::SanitizeCueEffects(Cues[Index + 1]);
			}
			StageCueTracking::KeepNextCue(Cues, Index, Before, Programmed, [this](int32 FixtureId, const FGameplayTag& Attribute, float& OutHome)
			{
				const ALightingFixtureActor* Fixture = FindFixture(FixtureId);
				const float* Home = Fixture ? Fixture->GetHomeAttributes().Find(Attribute) : nullptr;
				OutHome = Home ? *Home : 0.f;
				return Home != nullptr;
			});
		}
	}

	UE_LOG(LogStageCraft, Log, TEXT("Stored cue %.3f (%s, %d fixtures in the cue)."), Number, *UEnum::GetValueAsString(Mode), Cues[Index].Fixtures.Num());
	OnCueListChanged.Broadcast();
	return true;
}

bool UShowControlSubsystem::UpdateCue(float CueNumber, const FText& Label, float FadeTime)
{
	const int32 Index = FindCueIndex(CueNumber);
	if (Index == INDEX_NONE)
	{
		return false;
	}

	FShowCue& Cue = Cues[Index];
	const FText NewLabel = StageCueRules::SanitizeLabel(Label);
	const float NewFade = StageCueRules::SanitizeFadeTime(FadeTime);
	if (Cue.Label.EqualTo(NewLabel) && FMath::IsNearlyEqual(Cue.FadeTime, NewFade))
	{
		return true; // Nothing changed: no event, so the stage does not become dirty and no undo step is recorded.
	}
	Cue.Label = NewLabel;
	Cue.FadeTime = NewFade;
	OnCueListChanged.Broadcast();
	return true;
}

bool UShowControlSubsystem::UpdateCueTiming(float CueNumber, const FStageCueTiming& Timing)
{
	const int32 Index = FindCueIndex(CueNumber);
	if (Index == INDEX_NONE)
	{
		return false;
	}
	FStageCueTiming Sanitized = Timing;
	StageCueRules::SanitizeTiming(Sanitized);
	if (Cues[Index].Timing.Equals(Sanitized))
	{
		return true;
	}
	Cues[Index].Timing = MoveTemp(Sanitized);
	OnCueListChanged.Broadcast();
	return true;
}

bool UShowControlSubsystem::SetCueBlocked(float CueNumber, bool bBlocked)
{
	const int32 Index = FindCueIndex(CueNumber);
	if (Index == INDEX_NONE)
	{
		return false;
	}
	FShowCue& Cue = Cues[Index];
	if (Cue.bBlock == bBlocked)
	{
		return true;
	}
	if (bBlocked)
	{
		// Block keeps the look: what the cue inherited is written into it, so cutting the inheritance changes nothing on stage.
		StageCueTracking::FFixtureStates State;
		StageCueTracking::StateAt(Cues, Index, State);
		Cue.Fixtures = StageCueTracking::ToEntries(State);
		TArray<FStageEffectEntry> Effects;
		StageCueTracking::EffectsAt(Cues, Index, Effects);
		Cue.Effects = MoveTemp(Effects);
		Cue.EffectStops.Reset(); // A blocked cue starts from no effects: nothing before it is left to stop.
	}
	Cue.bBlock = bBlocked;
	OnCueListChanged.Broadcast();
	return true;
}

bool UShowControlSubsystem::DeleteCue(float CueNumber)
{
	const int32 Index = FindCueIndex(CueNumber);
	if (Index == INDEX_NONE)
	{
		return false;
	}
	Cues.RemoveAt(Index);
	OnCueListChanged.Broadcast();
	return true;
}

void UShowControlSubsystem::AssignSanitizedCues(const TArray<FShowCue>& InCues, const TCHAR* Source)
{
	Cues = InCues;
	const int32 Dropped = StageCueRules::SanitizeCueList(Cues);
	if (Dropped > 0)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("%s: %d cue(s) with an invalid or duplicate number were dropped."), Source, Dropped);
	}
}

void UShowControlSubsystem::SetCues(const TArray<FShowCue>& InCues)
{
	FinishFade();
	PendingAutoGo.Reset();
	AssignSanitizedCues(InCues, TEXT("SetCues"));
	ActiveCueNumber = -1.f;
	// A loaded show starts with no cue playing; the fixtures keep the output they were restored with (RestoreActiveCue follows).
	PlaybackValues.Reset();
	if (!MainEffects.IsEmpty())
	{
		MainEffects.Reset();
		RefreshEffectSources(nullptr);
	}
	OnCueListChanged.Broadcast();
}

bool UShowControlSubsystem::RestoreActiveCue(float CueNumber)
{
	const int32 Index = FindCueIndex(CueNumber);
	if (Index == INDEX_NONE || ControlSource == EStageControlSource::External)
	{
		return false;
	}
	FinishFade();
	PendingAutoGo.Reset();
	// The playback layer becomes the cue's tracked look exactly; fixtures it leaves out go back to their home look.
	TArray<int32> Changed;
	PlaybackValues.GetKeys(Changed);
	PlaybackValues.Reset();
	StageCueTracking::FFixtureStates State;
	StageCueTracking::StateAt(Cues, Index, State);
	for (const TPair<int32, FShowCueFixtureState>& Pair : State)
	{
		PlaybackValues.Add(Pair.Key, ResolveReferences(Pair.Value));
		Changed.AddUnique(Pair.Key);
	}
	ActiveCueNumber = Cues[Index].Number;
	ResolveFixtures(Changed);
	StageCueTracking::EffectsAt(Cues, Index, MainEffects);
	RefreshEffectSources(nullptr);
	OnCueListChanged.Broadcast();
	return true;
}

void UShowControlSubsystem::ReplaceCues(const TArray<FShowCue>& InCues)
{
	AssignSanitizedCues(InCues, TEXT("ReplaceCues"));
	OnCueListChanged.Broadcast();
}

bool UShowControlSubsystem::GoToCue(float CueNumber)
{
	return PlayCue(FindCueIndex(CueNumber), ECueStart::Forward);
}

bool UShowControlSubsystem::PlayCue(int32 Index, ECueStart Start)
{
	if (ControlSource == EStageControlSource::External)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("A cue was asked to play while fixtures are under external DMX control; ignored."));
		return false;
	}
	if (!Cues.IsValidIndex(Index))
	{
		return false;
	}

	// A new start while fading starts from wherever the fixtures are right now, like a console's cue take-over.
	PendingAutoGo.Reset();
	if (!Fades.IsEmpty())
	{
		OnCueFinished.Broadcast(ActiveCueNumber);
	}
	Fades.Reset();

	const FShowCue& Cue = Cues[Index];
	StageCueTracking::FFixtureStates State;
	StageCueTracking::StateAt(Cues, Index, State);

	// Every fixture the cue's look or the playback layer names moves: to the look's value, or back home where the look is silent.
	TArray<int32> FixtureIds;
	State.GetKeys(FixtureIds);
	for (const TPair<int32, TMap<FGameplayTag, float>>& Pair : PlaybackValues)
	{
		FixtureIds.AddUnique(Pair.Key);
	}
	FadeDuration = 0.f;
	for (const int32 FixtureId : FixtureIds)
	{
		const FShowCueFixtureState* Target = State.Find(FixtureId);
		// Resolved once, at Go: the fade heads for the palettes' values as they are now, and its frames stay plain lerps.
		TMap<FGameplayTag, float> Final = Target ? ResolveReferences(*Target) : TMap<FGameplayTag, float>();
		ALightingFixtureActor* Fixture = FindFixture(FixtureId);
		if (!Fixture)
		{
			// Not on stage (deleted after the cue was recorded): the layer simply takes the look, kept by ID for a re-spawn.
			if (Final.IsEmpty())
			{
				PlaybackValues.Remove(FixtureId);
			}
			else
			{
				PlaybackValues.Add(FixtureId, MoveTemp(Final));
			}
			continue;
		}

		FAttributeFade& Fade = Fades.AddDefaulted_GetRef();
		Fade.Fixture = Fixture;
		Fade.To = Final;
		if (const TMap<FGameplayTag, float>* Current = PlaybackValues.Find(FixtureId))
		{
			for (const TPair<FGameplayTag, float>& Held : *Current)
			{
				if (!Final.Contains(Held.Key))
				{
					const float* Home = Fixture->GetHomeAttributes().Find(Held.Key);
					Fade.To.Add(Held.Key, Home ? *Home : Held.Value);
				}
			}
		}
		Fade.Final = MoveTemp(Final);
		for (const TPair<FGameplayTag, float>& Pair : Fade.To)
		{
			// From the playback layer, not the output: a value the programmer holds is not where the cue's fade starts.
			const float From = GetPlaybackValue(*Fixture, Pair.Key);
			if (FMath::IsNearlyEqual(From, Pair.Value))
			{
				continue;
			}
			float Delay = 0.f;
			float Duration = 0.f;
			// Out time for intensity going down, a feature time, or the In time (Docs/ADR/0026 §2).
			StageCueTiming::GetAttributeTiming(Cue, Pair.Key, Pair.Value < From, Delay, Duration);
			Fade.Changing.Add({ Pair.Key, From, Pair.Value, Delay, Duration });
			FadeDuration = FMath::Max(FadeDuration, Delay + Duration);
		}
	}

	ActiveCueNumber = Cue.Number;
	FadeElapsed = 0.f;
	FadeCurve = Cue.Timing.Curve;
	// The cue's effects fade in, out or morph with its timing (Docs/ADR/0027 §5), alongside its values.
	StageCueTracking::EffectsAt(Cues, Index, MainEffects);
	RefreshEffectSources(&Cue);
	OnCueStarted.Broadcast(ActiveCueNumber);

	// Back never runs a chain; a cue started forward arms the next cue's Follow or Wait (fired from the tick, never from here).
	if (Start == ECueStart::Forward && Cues.IsValidIndex(Index + 1))
	{
		const FShowCue& Next = Cues[Index + 1];
		if (Next.Timing.Trigger != EStageCueTrigger::Go)
		{
			FPendingAutoGo Pending;
			Pending.CueNumber = Next.Number;
			Pending.bAfterFinish = Next.Timing.Trigger == EStageCueTrigger::Follow;
			Pending.Remaining = Pending.bAfterFinish ? 0.f : Next.Timing.WaitTime;
			PendingAutoGo = Pending;
		}
	}

	if (FadeDuration <= KINDA_SMALL_NUMBER)
	{
		CompleteFade();
	}
	return true;
}

bool UShowControlSubsystem::Go()
{
	for (int32 Index = 0; Index < Cues.Num(); ++Index)
	{
		if (Cues[Index].Number > ActiveCueNumber + StageCueRules::NumberResolution * 0.5f)
		{
			return PlayCue(Index, ECueStart::Forward);
		}
	}
	return false;
}

bool UShowControlSubsystem::Back()
{
	// Nothing active means nothing to go back from; a console's Back does nothing before the first Go either.
	if (ActiveCueNumber < 0.f)
	{
		return false;
	}
	for (int32 Index = Cues.Num() - 1; Index >= 0; --Index)
	{
		if (Cues[Index].Number < ActiveCueNumber - StageCueRules::NumberResolution * 0.5f)
		{
			return PlayCue(Index, ECueStart::Back);
		}
	}
	return false;
}

void UShowControlSubsystem::FinishFade()
{
	if (!Fades.IsEmpty())
	{
		CompleteFade();
	}
}

void UShowControlSubsystem::CompleteFade()
{
	const bool bMainOnly = IsMainOnlyMix();
	for (const FAttributeFade& Fade : Fades)
	{
		if (ALightingFixtureActor* Fixture = Fade.Fixture.Get())
		{
			// The playback layer is the cue's look exactly; where it is silent the home look shows through.
			if (Fade.Final.IsEmpty())
			{
				PlaybackValues.Remove(Fixture->GetFixtureId());
			}
			else
			{
				PlaybackValues.Add(Fixture->GetFixtureId(), Fade.Final);
			}
			if (bMainOnly)
			{
				TMap<FGameplayTag, float> Out = Fade.To;
				RemoveHeldAttributes(*Fixture, Out);
				WriteFixtureOutput(*Fixture, Out);
			}
			else
			{
				ResolveFixture(*Fixture); // Playbacks or masters are in play: the whole mix decides.
			}
		}
	}
	Fades.Reset();
	if (PendingAutoGo.IsSet() && PendingAutoGo->bAfterFinish)
	{
		// Follow: the next cue starts on the next tick, never inside this call (a chain of zero-time cues cannot recurse).
		PendingAutoGo->bAfterFinish = false;
		PendingAutoGo->Remaining = 0.f;
	}
	OnCueFinished.Broadcast(ActiveCueNumber);
}

void UShowControlSubsystem::TickFades(float DeltaTime)
{
	if (Fades.IsEmpty())
	{
		return;
	}

	FadeElapsed += DeltaTime;
	if (FadeElapsed >= FadeDuration)
	{
		CompleteFade();
		return;
	}

	// With no playback up and the masters at full a frame goes straight onto the fixture; otherwise every frame is mixed (HTP, LTP, masters).
	const bool bMainOnly = IsMainOnlyMix();
	TMap<FGameplayTag, float> Frame;
	for (const FAttributeFade& Fade : Fades)
	{
		ALightingFixtureActor* Fixture = Fade.Fixture.Get();
		if (!Fixture || Fade.Changing.IsEmpty())
		{
			continue;
		}

		Frame.Reset();
		for (const FAttributeLerp& Lerp : Fade.Changing)
		{
			// Each attribute on its own delay and time; one not started yet is left alone.
			const float Progress = StageCueTiming::GetProgress(FadeElapsed, Lerp.Delay, Lerp.Duration);
			if (Progress > 0.f)
			{
				Frame.Add(Lerp.Attribute, FMath::Lerp(Lerp.From, Lerp.To, StageCueTiming::ApplyCurve(FadeCurve, Progress)));
			}
		}
		if (Frame.IsEmpty())
		{
			continue;
		}
		// The playback layer follows the fade, so a take-over or a Clear mid-fade starts from where the cue is now.
		PlaybackValues.FindOrAdd(Fixture->GetFixtureId()).Append(Frame);
		if (bMainOnly)
		{
			RemoveHeldAttributes(*Fixture, Frame);
			WriteFixtureOutput(*Fixture, Frame);
		}
		else
		{
			ResolveFixture(*Fixture);
		}
	}
}

void UShowControlSubsystem::TickAutoGo(float DeltaTime)
{
	if (!PendingAutoGo.IsSet() || PendingAutoGo->bAfterFinish)
	{
		return;
	}
	PendingAutoGo->Remaining -= DeltaTime;
	if (PendingAutoGo->Remaining > 0.f)
	{
		return;
	}
	const float Number = PendingAutoGo->CueNumber;
	PendingAutoGo.Reset();
	// The cue may have been deleted or renumbered since it was armed: then the chain simply stops.
	PlayCue(FindCueIndex(Number), ECueStart::Forward);
}

// --- Playbacks and masters (Docs/ADR/0026 §6) ---

bool UShowControlSubsystem::IsMainOnlyMix() const
{
	// An effect sits above the cue in the mix (Docs/ADR/0027 §3), so a fade frame must go through it too.
	if (GetIntensityScale() != 1.f || AreEffectsLive())
	{
		return false;
	}
	for (const FPlaybackRuntime& Runtime : PlaybackRuntime)
	{
		if (Runtime.IsActive())
		{
			return false;
		}
	}
	return true;
}

void UShowControlSubsystem::RefreshPlaybackOutput(int32 Index, TArray<int32>& OutChanged)
{
	FPlaybackRuntime& Runtime = PlaybackRuntime[Index];
	for (const TPair<int32, TMap<FGameplayTag, float>>& Pair : Runtime.Output)
	{
		OutChanged.AddUnique(Pair.Key);
	}
	Runtime.Output.Reset();
	StageCueTracking::FFixtureStates State;
	StageCueTracking::StateAt(Playbacks[Index].Cues, Runtime.CueIndex, State);
	for (const TPair<int32, FShowCueFixtureState>& Pair : State)
	{
		Runtime.Output.Add(Pair.Key, ResolveReferences(Pair.Value));
		OutChanged.AddUnique(Pair.Key);
	}
	StageCueTracking::EffectsAt(Playbacks[Index].Cues, Runtime.CueIndex, Runtime.Effects);
}

void UShowControlSubsystem::ApplyPlaybackChange(int32 Index, TArray<int32> Changed)
{
	for (const TPair<int32, TMap<FGameplayTag, float>>& Pair : PlaybackRuntime[Index].Output)
	{
		Changed.AddUnique(Pair.Key);
	}
	ResolveFixtures(Changed);
	// A playback's effects start, stop and rescale with it at once: its fader is its fade (Docs/ADR/0027 §5).
	RefreshEffectSources(nullptr);
	OnDeskStateChanged.Broadcast();
}

bool UShowControlSubsystem::RecordPlayback(int32 Index, TConstArrayView<FShowCueFixtureState> Entries, const FStageProgrammerEffects& Effects)
{
	if (!Playbacks.IsValidIndex(Index) || (Entries.IsEmpty() && Effects.IsEmpty()) || Playbacks[Index].Cues.Num() >= StagePlaybackRules::MaxCuesPerPlayback)
	{
		return false;
	}
	TArray<FShowCue>& List = Playbacks[Index].Cues;
	FShowCue& Cue = List.AddDefaulted_GetRef();
	Cue.Number = List.Num() > 1 ? FMath::FloorToFloat(List[List.Num() - 2].Number) + 1.f : 1.f;
	Cue.FadeTime = 0.f; // A playback steps at once: its fader is the fade.
	Cue.Fixtures = TArray<FShowCueFixtureState>(Entries);
	Cue.Effects = Effects.Effects;
	Cue.EffectStops = Effects.Stops;
	if (!StageCueRules::IsValidCueNumber(Cue.Number))
	{
		List.Pop();
		return false;
	}
	StageCueRules::SanitizeCueList(List);
	TArray<int32> Changed;
	if (PlaybackRuntime[Index].IsActive())
	{
		RefreshPlaybackOutput(Index, Changed); // A tracking cue recorded before the current one changes what it shows.
		ApplyPlaybackChange(Index, MoveTemp(Changed));
	}
	OnPlaybacksChanged.Broadcast();
	return true;
}

bool UShowControlSubsystem::ClearPlayback(int32 Index)
{
	if (!Playbacks.IsValidIndex(Index) || Playbacks[Index].Cues.IsEmpty())
	{
		return false;
	}
	ReleasePlayback(Index);
	Playbacks[Index].Cues.Reset();
	OnPlaybacksChanged.Broadcast();
	return true;
}

bool UShowControlSubsystem::RenamePlayback(int32 Index, const FText& Name)
{
	if (!Playbacks.IsValidIndex(Index))
	{
		return false;
	}
	const FText Sanitized = StageCueRules::SanitizeLabel(Name);
	if (!Playbacks[Index].Name.EqualTo(Sanitized))
	{
		Playbacks[Index].Name = Sanitized;
		OnPlaybacksChanged.Broadcast();
	}
	return true;
}

void UShowControlSubsystem::SetPlaybacks(const TArray<FStagePlayback>& InPlaybacks)
{
	// A loaded show starts with every playback released; the masters stay where the desk has them.
	TArray<int32> Changed;
	for (int32 Index = 0; Index < PlaybackRuntime.Num(); ++Index)
	{
		for (const TPair<int32, TMap<FGameplayTag, float>>& Pair : PlaybackRuntime[Index].Output)
		{
			Changed.AddUnique(Pair.Key);
		}
		PlaybackRuntime[Index] = FPlaybackRuntime();
	}
	Playbacks = InPlaybacks;
	const int32 Dropped = StagePlaybackRules::SanitizePlaybacks(Playbacks);
	if (Dropped > 0)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("SetPlaybacks: %d invalid playback cue(s) or playback(s) were dropped."), Dropped);
	}
	ResolveFixtures(Changed);
	RefreshEffectSources(nullptr);
	OnPlaybacksChanged.Broadcast();
	OnDeskStateChanged.Broadcast();
}

void UShowControlSubsystem::ReplacePlaybacks(const TArray<FStagePlayback>& InPlaybacks)
{
	Playbacks = InPlaybacks;
	StagePlaybackRules::SanitizePlaybacks(Playbacks);
	TArray<int32> Changed;
	for (int32 Index = 0; Index < PlaybackRuntime.Num(); ++Index)
	{
		FPlaybackRuntime& Runtime = PlaybackRuntime[Index];
		if (!Playbacks[Index].Cues.IsValidIndex(Runtime.CueIndex))
		{
			Runtime.CueIndex = Playbacks[Index].Cues.IsEmpty() ? INDEX_NONE : Playbacks[Index].Cues.Num() - 1;
		}
		RefreshPlaybackOutput(Index, Changed);
	}
	ResolveFixtures(Changed);
	RefreshEffectSources(nullptr);
	OnPlaybacksChanged.Broadcast();
	OnDeskStateChanged.Broadcast();
}

bool UShowControlSubsystem::SetPlaybackLevel(int32 Index, float Level)
{
	if (!PlaybackRuntime.IsValidIndex(Index) || !FMath::IsFinite(Level))
	{
		return false;
	}
	FPlaybackRuntime& Runtime = PlaybackRuntime[Index];
	const float Clamped = FMath::Clamp(Level, 0.f, 1.f);
	if (FMath::IsNearlyEqual(Runtime.Level, Clamped))
	{
		return true;
	}
	const bool bWasActive = Runtime.IsActive();
	const bool bWasUp = Runtime.Level > UE_KINDA_SMALL_NUMBER;
	Runtime.Level = Clamped;
	TArray<int32> Changed;
	if (!bWasUp && Clamped > UE_KINDA_SMALL_NUMBER && !Playbacks[Index].Cues.IsEmpty())
	{
		// A fader leaving 0 activates its playback (at its first cue if released) and takes LTP, like a console's fader start.
		Runtime.Order = ++ActivationCounter;
		if (Runtime.CueIndex == INDEX_NONE)
		{
			Runtime.CueIndex = 0;
			RefreshPlaybackOutput(Index, Changed);
		}
	}
	if (bWasActive || Runtime.IsActive())
	{
		ApplyPlaybackChange(Index, MoveTemp(Changed));
	}
	else
	{
		OnDeskStateChanged.Broadcast();
	}
	return true;
}

bool UShowControlSubsystem::SetPlaybackFlash(int32 Index, bool bDown)
{
	if (!PlaybackRuntime.IsValidIndex(Index))
	{
		return false;
	}
	FPlaybackRuntime& Runtime = PlaybackRuntime[Index];
	if (Runtime.bFlash == bDown)
	{
		return true;
	}
	Runtime.bFlash = bDown;
	TArray<int32> Changed;
	if (bDown && !Playbacks[Index].Cues.IsEmpty())
	{
		Runtime.Order = ++ActivationCounter;
		if (Runtime.CueIndex == INDEX_NONE)
		{
			Runtime.CueIndex = 0;
			RefreshPlaybackOutput(Index, Changed);
		}
	}
	ApplyPlaybackChange(Index, MoveTemp(Changed));
	return true;
}

bool UShowControlSubsystem::PlaybackGo(int32 Index)
{
	if (!PlaybackRuntime.IsValidIndex(Index))
	{
		return false;
	}
	FPlaybackRuntime& Runtime = PlaybackRuntime[Index];
	const int32 Next = Runtime.CueIndex == INDEX_NONE ? 0 : Runtime.CueIndex + 1;
	if (!Playbacks[Index].Cues.IsValidIndex(Next))
	{
		return false; // At the end (or empty): a console's playback stops at its last cue.
	}
	Runtime.CueIndex = Next;
	Runtime.Order = ++ActivationCounter;
	TArray<int32> Changed;
	RefreshPlaybackOutput(Index, Changed);
	ApplyPlaybackChange(Index, MoveTemp(Changed));
	return true;
}

bool UShowControlSubsystem::ReleasePlayback(int32 Index)
{
	if (!PlaybackRuntime.IsValidIndex(Index))
	{
		return false;
	}
	FPlaybackRuntime& Runtime = PlaybackRuntime[Index];
	TArray<int32> Changed;
	for (const TPair<int32, TMap<FGameplayTag, float>>& Pair : Runtime.Output)
	{
		Changed.Add(Pair.Key);
	}
	Runtime = FPlaybackRuntime();
	ResolveFixtures(Changed);
	RefreshEffectSources(nullptr);
	OnDeskStateChanged.Broadcast();
	return true;
}

float UShowControlSubsystem::GetPlaybackLevel(int32 Index) const
{
	return PlaybackRuntime.IsValidIndex(Index) ? PlaybackRuntime[Index].Level : 0.f;
}

bool UShowControlSubsystem::IsPlaybackFlashing(int32 Index) const
{
	return PlaybackRuntime.IsValidIndex(Index) && PlaybackRuntime[Index].bFlash;
}

int32 UShowControlSubsystem::GetPlaybackCueIndex(int32 Index) const
{
	return PlaybackRuntime.IsValidIndex(Index) ? PlaybackRuntime[Index].CueIndex : INDEX_NONE;
}

bool UShowControlSubsystem::IsPlaybackActive(int32 Index) const
{
	return PlaybackRuntime.IsValidIndex(Index) && PlaybackRuntime[Index].IsActive();
}

void UShowControlSubsystem::SetGrandMaster(float Level)
{
	const float Clamped = FMath::IsFinite(Level) ? FMath::Clamp(Level, 0.f, 1.f) : GrandMaster;
	if (!FMath::IsNearlyEqual(Clamped, GrandMaster))
	{
		GrandMaster = Clamped;
		ApplyMastersChange();
	}
}

void UShowControlSubsystem::SetBlackout(bool bOn)
{
	if (bOn != bBlackout)
	{
		bBlackout = bOn;
		ApplyMastersChange();
	}
}

void UShowControlSubsystem::SetAudioMaster(float Level)
{
	const float Clamped = FMath::IsFinite(Level) ? FMath::Clamp(Level, 0.f, 1.f) : AudioMaster;
	if (!FMath::IsNearlyEqual(Clamped, AudioMaster))
	{
		AudioMaster = Clamped;
		EffectRunner.SetAudioMaster(bAudioKill ? 0.f : AudioMaster);
		// Only effects that follow the music change, and they are mixed again on every tick: no re-mix here.
		OnDeskStateChanged.Broadcast();
	}
}

void UShowControlSubsystem::SetAudioKill(bool bOn)
{
	if (bOn != bAudioKill)
	{
		bAudioKill = bOn;
		EffectRunner.SetAudioMaster(bAudioKill ? 0.f : AudioMaster);
		UE_LOG(LogStageCraft, Log, TEXT("AUDIO KILL %s."), bOn ? TEXT("on: effects run as without music") : TEXT("off"));
		OnDeskStateChanged.Broadcast();
	}
}

void UShowControlSubsystem::ApplyMastersChange()
{
	if (ControlSource == EStageControlSource::External)
	{
		// The console's values are not kept apart from the output: every universe is read again in full on its next frame, then scaled.
		LastInputFrames.Reset();
	}
	ResolveAllFixtures();
	OnDeskStateChanged.Broadcast();
}

void UShowControlSubsystem::ApplyMastersToInput(ALightingFixtureActor& Fixture) const
{
	const float Scale = GetIntensityScale();
	if (Scale == 1.f)
	{
		return;
	}
	TMap<FGameplayTag, float> Scaled;
	for (const TPair<FGameplayTag, float>& Pair : Fixture.GetAttributes())
	{
		if (StagePaletteRules::GetAttributeKind(Pair.Key) == EStagePaletteKind::Intensity)
		{
			Scaled.Add(Pair.Key, Pair.Value * Scale);
		}
	}
	Fixture.SetAttributes(Scaled);
}

// --- DMX ---

void UShowControlSubsystem::SetDMXBridge(UStageDMXBridge* NewBridge)
{
	if (NewBridge == DMXBridge)
	{
		return;
	}
	if (DMXBridge)
	{
		DMXBridge->Stop();
	}
	DMXBridge = NewBridge;
	DMXOutputAccumulator = 0.f;
	DirtyOutputUniverses.Reset(); // The new bridge's owner asks for a full frame (RefreshDMXOutput) once its output is open.
	if (DMXBridge)
	{
		DMXBridge->Start(*this);
	}
}

void UShowControlSubsystem::SetControlSource(EStageControlSource NewSource)
{
	if (NewSource == ControlSource)
	{
		return;
	}
	if (NewSource == EStageControlSource::External)
	{
		// The external console owns the fixtures from now on; a half-finished fade would fight it.
		Fades.Reset();
		PendingAutoGo.Reset();
	}
	ControlSource = NewSource;
	// Frames received under the other source were never applied, so none may count as "unchanged".
	LastInputFrames.Reset();
	if (ControlSource == EStageControlSource::Internal)
	{
		// The show takes the fixtures back: the console's last look gives way to the mix (home look, cue, programmer).
		ResolveAllFixtures();
	}
	OnControlSourceChanged.Broadcast(ControlSource);
}

// --- Output mix ---

void UShowControlSubsystem::HandleProgrammerFixturesChanged(TConstArrayView<int32> FixtureIds)
{
	ResolveFixtures(FixtureIds);
}

// --- Effects (Docs/ADR/0027-effects-engine.md §3) ---

void UShowControlSubsystem::HandleProgrammerEffectsChanged()
{
	RefreshEffectSources(nullptr);
}

void UShowControlSubsystem::HandleAudioStatusChanged()
{
	if (AudioReactive)
	{
		EffectRunner.SetFlashLimitHz(AudioReactive->GetSettings().FlashLimitHz);
	}
}

void UShowControlSubsystem::RefreshEffectSources(const FShowCue* TimedBy)
{
	// One list, latest source last: the same id runs once, as the latest (Docs/ADR/0027 §5).
	TArray<FStageEffectRunner::FSourcedEntry> Sourced;
	const auto Add = [&Sourced](const FStageEffectEntry& Entry, EStageEffectSource Source, int32 Playback, float Scale)
	{
		Sourced.RemoveAll([&Entry](const FStageEffectRunner::FSourcedEntry& Earlier) { return Earlier.Entry.Id == Entry.Id; });
		Sourced.Add({ Entry, Source, Playback, Scale });
	};
	for (const FStageEffectEntry& Entry : MainEffects)
	{
		Add(Entry, EStageEffectSource::Cue, INDEX_NONE, 1.f);
	}
	TArray<int32, TInlineAllocator<StagePlaybackRules::Count>> ActivePlaybacks;
	for (int32 Index = 0; Index < PlaybackRuntime.Num(); ++Index)
	{
		if (PlaybackRuntime[Index].IsActive() && !PlaybackRuntime[Index].Effects.IsEmpty())
		{
			ActivePlaybacks.Add(Index);
		}
	}
	ActivePlaybacks.Sort([this](int32 A, int32 B) { return PlaybackRuntime[A].Order < PlaybackRuntime[B].Order; });
	for (const int32 Index : ActivePlaybacks)
	{
		for (const FStageEffectEntry& Entry : PlaybackRuntime[Index].Effects)
		{
			Add(Entry, EStageEffectSource::Playback, Index, PlaybackRuntime[Index].GetEffectiveLevel());
		}
	}
	if (Programmer)
	{
		for (const FStageEffectEntry& Entry : Programmer->GetEffects())
		{
			Add(Entry, EStageEffectSource::Programmer, INDEX_NONE, 1.f);
		}
		for (const FGuid& Stop : Programmer->GetEffectStops())
		{
			Sourced.RemoveAll([&Stop](const FStageEffectRunner::FSourcedEntry& Entry) { return Entry.Entry.Id == Stop; });
		}
	}

	// Every fixture an effect drove or now drives re-mixes once in full, so a stopped effect leaves its fixtures on their static look.
	// A fader move only rescales: the tick writes the new values, so nothing is re-mixed here.
	TSet<int32> Touched;
	for (const FStageEffectRunner::FDrivenFixture& Driven : EffectRunner.GetDrivenFixtures())
	{
		Touched.Add(Driven.FixtureId);
	}
	if (!EffectRunner.SetEffects(Sourced, [this](int32 FixtureId) { return FindFixture(FixtureId); }, TimedBy))
	{
		return;
	}
	for (const FStageEffectRunner::FDrivenFixture& Driven : EffectRunner.GetDrivenFixtures())
	{
		Touched.Add(Driven.FixtureId);
	}
	ResolveFixtures(Touched.Array());
}

const FStageEffectEntry* UShowControlSubsystem::FindShowEffect(const FGuid& EffectId) const
{
	const auto HasId = [&EffectId](const FStageEffectEntry& Entry) { return Entry.Id == EffectId; };
	const FStageEffectEntry* Found = nullptr;
	uint32 FoundOrder = 0;
	for (const FPlaybackRuntime& Runtime : PlaybackRuntime)
	{
		const FStageEffectEntry* Entry = Runtime.IsActive() ? Runtime.Effects.FindByPredicate(HasId) : nullptr;
		if (Entry && (!Found || Runtime.Order > FoundOrder))
		{
			Found = Entry;
			FoundOrder = Runtime.Order;
		}
	}
	return Found ? Found : MainEffects.FindByPredicate(HasId);
}

TArray<FStageEffectEntry> UShowControlSubsystem::CaptureEffects() const
{
	TArray<FStageRunningEffect> Running;
	EffectRunner.GetRunning(Running);
	TArray<FStageEffectEntry> Captured;
	for (const FStageRunningEffect& Effect : Running)
	{
		if (!Effect.bStopping)
		{
			Captured.Add(Effect.Entry);
		}
	}
	return Captured;
}

void UShowControlSubsystem::RefreshEffectFixtures()
{
	EffectRunner.RefreshFixtures([this](int32 FixtureId) { return FindFixture(FixtureId); });
}

void UShowControlSubsystem::TickEffects()
{
	TArray<ALightingFixtureActor*, TInlineAllocator<4>> Renumbered;
	for (const FStageEffectRunner::FDrivenFixture& Driven : EffectRunner.GetDrivenFixtures())
	{
		ALightingFixtureActor* Fixture = Driven.Fixture.Get();
		if (!Fixture)
		{
			continue;
		}
		if (Fixture->GetFixtureId() != Driven.FixtureId)
		{
			Renumbered.Add(Fixture); // The effect names the old ID, so the fixture leaves it.
			continue;
		}
		ResolveFixture(*Fixture, EResolveScope::EffectAttributes);
	}
	if (!Renumbered.IsEmpty())
	{
		RefreshEffectFixtures();
		for (ALightingFixtureActor* Fixture : Renumbered)
		{
			ResolveFixture(*Fixture);
		}
	}
}

void UShowControlSubsystem::ResolveFixtures(TConstArrayView<int32> FixtureIds)
{
	for (const int32 FixtureId : FixtureIds)
	{
		if (ALightingFixtureActor* Fixture = FindFixture(FixtureId))
		{
			ResolveFixture(*Fixture);
		}
	}
}

void UShowControlSubsystem::ResolveAllFixtures()
{
	for (ALightingFixtureActor* Fixture : GetFixtures())
	{
		ResolveFixture(*Fixture);
	}
}

void UShowControlSubsystem::MakeHighlightValues(const ALightingFixtureActor& Fixture, TMap<FGameplayTag, float>& OutValues)
{
	OutValues = Fixture.MatchColor(FLinearColor::White);
	const TMap<FGameplayTag, float>& Own = Fixture.GetAttributes();
	if (Own.Contains(StageCraftTags::Attribute_Dimmer))
	{
		OutValues.Add(StageCraftTags::Attribute_Dimmer, 1.f);
	}
	if (Own.Contains(StageCraftTags::Attribute_Shutter))
	{
		OutValues.Add(StageCraftTags::Attribute_Shutter, 0.f);
	}
}

void UShowControlSubsystem::BuildLayers(const ALightingFixtureActor& Fixture, FLayerScratch& Scratch, StageShowMix::FFixtureLayers& OutLayers) const
{
	const int32 Id = Fixture.GetFixtureId();
	const bool bHighlighted = Programmer && Programmer->IsHighlighted(Id);
	if (bHighlighted)
	{
		MakeHighlightValues(Fixture, Scratch.Highlight);
	}

	// Under External DMX the console owns the cue and home layers; only what the user holds by hand is written over it.
	const bool bInternal = ControlSource == EStageControlSource::Internal;
	OutLayers = StageShowMix::FFixtureLayers();
	OutLayers.Home = bInternal ? &Fixture.GetHomeAttributes() : nullptr;
	OutLayers.Playback = bInternal ? PlaybackValues.Find(Id) : nullptr;
	OutLayers.Programmer = GetProgrammerLayer(Fixture, Scratch.Programmer);
	OutLayers.Highlight = bHighlighted ? &Scratch.Highlight : nullptr;
	for (int32 Index = 0; Index < PlaybackRuntime.Num(); ++Index)
	{
		const FPlaybackRuntime& Runtime = PlaybackRuntime[Index];
		const TMap<FGameplayTag, float>* Values = bInternal && Runtime.IsActive() ? Runtime.Output.Find(Id) : nullptr;
		if (Values)
		{
			OutLayers.Playbacks.Add({ Values, Runtime.GetEffectiveLevel(), Runtime.Order, Index });
		}
	}
	// The masters scale intensity last, the programmer and highlight included (under External: what the user holds by hand).
	OutLayers.IntensityScale = GetIntensityScale();
	if (bInternal && EffectRunner.Evaluate(Fixture, Scratch.Effects))
	{
		OutLayers.Effects = &Scratch.Effects;
	}
}

StageShowMix::FAttributeExplanation UShowControlSubsystem::ExplainAttribute(int32 FixtureId, const FGameplayTag& Attribute) const
{
	const ALightingFixtureActor* Fixture = FindFixture(FixtureId);
	if (!Fixture)
	{
		return StageShowMix::FAttributeExplanation();
	}
	FLayerScratch Scratch;
	StageShowMix::FFixtureLayers Layers;
	BuildLayers(*Fixture, Scratch, Layers);
	return StageShowMix::ExplainAttribute(Attribute, Layers);
}

TOptional<float> UShowControlSubsystem::FindSettingCue(int32 FixtureId, const FGameplayTag& Attribute) const
{
	const int32 Index = FindCueIndex(ActiveCueNumber);
	const int32 Setting = StageCueTracking::FindSettingCue(Cues, Index, FixtureId, Attribute);
	return Cues.IsValidIndex(Setting) ? TOptional<float>(Cues[Setting].Number) : TOptional<float>();
}

void UShowControlSubsystem::ResolveFixture(ALightingFixtureActor& Fixture, EResolveScope Scope)
{
	FLayerScratch Scratch;
	StageShowMix::FFixtureLayers Layers;
	BuildLayers(Fixture, Scratch, Layers);

	TArray<FGameplayTag> Attributes;
	if (Scope == EResolveScope::EffectAttributes)
	{
		Scratch.Effects.GetKeys(Attributes);
	}
	else
	{
		Fixture.GetAttributes().GetKeys(Attributes);
	}
	TMap<FGameplayTag, float> Mixed;
	StageShowMix::ResolveFixture(Attributes, Layers, Mixed);
	// SetAttributes stores and applies only the values that differ from the output.
	WriteFixtureOutput(Fixture, Mixed);
}

void UShowControlSubsystem::RemoveHeldAttributes(const ALightingFixtureActor& Fixture, TMap<FGameplayTag, float>& Frame) const
{
	if (!Programmer)
	{
		return;
	}
	const int32 Id = Fixture.GetFixtureId();
	const TMap<FGameplayTag, float>* Held = Programmer->FindValues(Id);
	if (Programmer->IsHighlighted(Id))
	{
		TMap<FGameplayTag, float> HighlightValues;
		MakeHighlightValues(Fixture, HighlightValues);
		for (const TPair<FGameplayTag, float>& Pair : HighlightValues)
		{
			Frame.Remove(Pair.Key);
		}
	}
	if (Held)
	{
		for (const TPair<FGameplayTag, float>& Pair : *Held)
		{
			Frame.Remove(Pair.Key);
		}
	}
}

float UShowControlSubsystem::GetPlaybackValue(const ALightingFixtureActor& Fixture, const FGameplayTag& Attribute) const
{
	if (const TMap<FGameplayTag, float>* Playback = PlaybackValues.Find(Fixture.GetFixtureId()))
	{
		if (const float* Value = Playback->Find(Attribute))
		{
			return *Value;
		}
	}
	if (const float* Home = Fixture.GetHomeAttributes().Find(Attribute))
	{
		return *Home;
	}
	return Fixture.GetAttribute(Attribute);
}

TMap<FGameplayTag, float> UShowControlSubsystem::GetRecordableValues(const ALightingFixtureActor& Fixture) const
{
	const int32 Id = Fixture.GetFixtureId();
	if ((!Programmer || !Programmer->IsHighlighted(Id)) && GetIntensityScale() == 1.f && !(AreEffectsLive() && EffectRunner.DrivesFixture(Id)))
	{
		return Fixture.GetAttributes();
	}
	// Highlighted, the masters are down or an effect moves it: the output is not the look, so the look underneath is mixed again
	// without them (an effect is recorded as an effect, never as the value it happened to show).
	TMap<FGameplayTag, float> Values = Fixture.GetAttributes();
	TMap<FGameplayTag, float> ProgrammerScratch;
	StageShowMix::FFixtureLayers Layers;
	Layers.Home = &Fixture.GetHomeAttributes();
	Layers.Playback = PlaybackValues.Find(Id);
	Layers.Programmer = GetProgrammerLayer(Fixture, ProgrammerScratch);
	for (const FPlaybackRuntime& Runtime : PlaybackRuntime)
	{
		if (const TMap<FGameplayTag, float>* PlaybackLook = Runtime.IsActive() ? Runtime.Output.Find(Id) : nullptr)
		{
			Layers.Playbacks.Add({ PlaybackLook, Runtime.GetEffectiveLevel(), Runtime.Order });
		}
	}
	TArray<FGameplayTag> Attributes;
	Values.GetKeys(Attributes);
	TMap<FGameplayTag, float> Mixed;
	StageShowMix::ResolveFixture(Attributes, Layers, Mixed);
	Values.Append(Mixed);
	return Values;
}

void UShowControlSubsystem::ApplyReferences(const ALightingFixtureActor* Fixture, int32 FixtureId, const TMap<FGameplayTag, FGuid>& References, TMap<FGameplayTag, float>& InOutValues) const
{
	if (References.IsEmpty() || !Library)
	{
		return;
	}
	// A colour palette's match is the same for every attribute of this fixture, so it is mixed once per palette.
	TMap<FGuid, TMap<FGameplayTag, float>> ColorMatches;
	for (const TPair<FGameplayTag, FGuid>& Reference : References)
	{
		float* Value = InOutValues.Find(Reference.Key);
		const FStagePalette* Palette = Value ? Library->FindPalette(Reference.Value) : nullptr;
		if (!Palette)
		{
			continue; // Gone: the stored value stands.
		}
		const TMap<FGameplayTag, float>* Match = nullptr;
		if (Palette->bHasColor && Fixture)
		{
			Match = ColorMatches.Find(Palette->Id);
			if (!Match)
			{
				Match = &ColorMatches.Add(Palette->Id, Fixture->MatchColor(Palette->Color));
			}
		}
		StagePaletteRules::FindPaletteValue(*Palette, FixtureId, Reference.Key, Match, *Value);
	}
}

TMap<FGameplayTag, float> UShowControlSubsystem::ResolveReferences(const FShowCueFixtureState& State) const
{
	TMap<FGameplayTag, float> Values = State.Attributes;
	ApplyReferences(FindFixture(State.FixtureId), State.FixtureId, State.PaletteRefs, Values);
	return Values;
}

const TMap<FGameplayTag, float>* UShowControlSubsystem::GetProgrammerLayer(const ALightingFixtureActor& Fixture, TMap<FGameplayTag, float>& Scratch) const
{
	const int32 Id = Fixture.GetFixtureId();
	const TMap<FGameplayTag, float>* Values = Programmer ? Programmer->FindValues(Id) : nullptr;
	const TMap<FGameplayTag, FGuid>* References = Programmer ? Programmer->FindReferences(Id) : nullptr;
	if (!Values || !References)
	{
		return Values;
	}
	Scratch = *Values;
	ApplyReferences(&Fixture, Id, *References, Scratch);
	return &Scratch;
}

void UShowControlSubsystem::RefreshPlaybackReferences(int32 OnlyFixtureId)
{
	if (ControlSource != EStageControlSource::Internal || !FindCue(ActiveCueNumber))
	{
		return;
	}
	// The playing cue's tracked look: a reference set by an earlier cue reaches this one too.
	StageCueTracking::FFixtureStates Look;
	GetCueState(ActiveCueNumber, Look);
	for (const TPair<int32, FShowCueFixtureState>& Pair : Look)
	{
		const FShowCueFixtureState& State = Pair.Value;
		if (State.PaletteRefs.IsEmpty() || (OnlyFixtureId != INDEX_NONE && State.FixtureId != OnlyFixtureId))
		{
			continue;
		}
		const TMap<FGameplayTag, float> Resolved = ResolveReferences(State);
		TMap<FGameplayTag, float>& Playback = PlaybackValues.FindOrAdd(State.FixtureId);
		for (const TPair<FGameplayTag, FGuid>& Reference : State.PaletteRefs)
		{
			if (const float* Value = Resolved.Find(Reference.Key))
			{
				Playback.Add(Reference.Key, *Value);
			}
		}
	}
}

void UShowControlSubsystem::HandlePalettesChanged()
{
	// A fade holds the targets it resolved at Go; finishing it first lets the playing cue take the palettes' new values cleanly.
	FinishFade();
	RefreshPlaybackReferences();
	for (int32 Index = 0; Index < PlaybackRuntime.Num(); ++Index)
	{
		TArray<int32> Ignored;
		RefreshPlaybackOutput(Index, Ignored);
	}
	// Palette edits are rare user actions: re-mixing every fixture (only changed values are written) is simpler and as cheap as a
	// switch back to Internal, and needs no second record of which fixture follows which palette (Docs/ADR/0028 §3).
	ResolveAllFixtures();
}

bool UShowControlSubsystem::IsDMXOutputActive() const
{
	return DMXBridge && DMXBridge->IsOutputEnabled() && ControlSource == EStageControlSource::Internal;
}

int32 UShowControlSubsystem::ReceiveDMXUniverse(int32 Universe, const TArray<uint8>& Channels)
{
	if (ControlSource != EStageControlSource::External || Channels.Num() < DMXUniverseSize || !Patch)
	{
		return 0;
	}

	// A re-patched fixture's channels hold no comparable "last value", so a patch change forgets every frame.
	if (LastInputPatchRevision != Patch->GetPatchRevision())
	{
		LastInputPatchRevision = Patch->GetPatchRevision();
		LastInputFrames.Reset();
	}

	TArray<uint8>* Previous = LastInputFrames.Find(Universe);
	int32 Updated = 0;
	for (const TWeakObjectPtr<ALightingFixtureActor>& Fixture : Patch->GetFixturesInUniverse(Universe))
	{
		if (Fixture.IsValid() && !(Previous && AreFixtureChannelsUnchanged(*Fixture, *Previous, Channels)))
		{
			Fixture->ReadDMX(Channels);
			ApplyMastersToInput(*Fixture.Get());
			++Updated;
		}
	}

	if (Previous)
	{
		FMemory::Memcpy(Previous->GetData(), Channels.GetData(), DMXUniverseSize);
	}
	else
	{
		LastInputFrames.Add(Universe, TArray<uint8>(Channels.GetData(), DMXUniverseSize));
	}
	return Updated;
}

TArray<uint8> UShowControlSubsystem::RenderDMXUniverse(int32 Universe) const
{
	TArray<uint8> Data;
	Data.SetNumUninitialized(DMXUniverseSize);
	RenderDMXUniverseInto(Universe, Data);
	return Data;
}

void UShowControlSubsystem::RenderDMXUniverseInto(int32 Universe, TArrayView<uint8> Data) const
{
	FMemory::Memzero(Data.GetData(), Data.Num());
	if (!Patch)
	{
		return;
	}
	for (const TWeakObjectPtr<ALightingFixtureActor>& Fixture : Patch->GetFixturesInUniverse(Universe))
	{
		if (Fixture.IsValid())
		{
			Fixture->WriteDMX(Data);
		}
	}
}

void UShowControlSubsystem::WriteFixtureOutput(ALightingFixtureActor& Fixture, const TMap<FGameplayTag, float>& Values)
{
	if (Fixture.SetAttributes(Values) && IsDMXOutputActive())
	{
		const FStageDMXPatch FixturePatch = Fixture.GetPatch();
		if (FixturePatch.IsPatched())
		{
			DirtyOutputUniverses.Add(FixturePatch.Universe);
		}
	}
}

void UShowControlSubsystem::RefreshDMXOutput()
{
	if (Patch && IsDMXOutputActive())
	{
		DirtyOutputUniverses.Append(Patch->GetPatchedUniverses());
	}
}

bool UShowControlSubsystem::HasPendingDMXOutput() const
{
	return !DirtyOutputUniverses.IsEmpty() || (Patch && Patch->GetPatchRevision() != LastOutputPatchRevision);
}

void UShowControlSubsystem::TickDMXOutput(float DeltaTime)
{
	if (!IsDMXOutputActive() || !Patch)
	{
		return;
	}
	// A re-patch moves channels around: every universe is rendered again (the sender drops universes that are no longer sent).
	if (Patch->GetPatchRevision() != LastOutputPatchRevision)
	{
		LastOutputPatchRevision = Patch->GetPatchRevision();
		DirtyOutputUniverses.Append(Patch->GetPatchedUniverses());
	}

	const float Interval = 1.f / FMath::Clamp(DMXOutputRateHz, 1.f, 60.f);
	if (DirtyOutputUniverses.IsEmpty())
	{
		// Idle: the next change goes out at once rather than waiting out an interval.
		DMXOutputAccumulator = Interval;
		return;
	}
	DMXOutputAccumulator += DeltaTime;
	if (DMXOutputAccumulator < Interval)
	{
		return;
	}
	// Drop missed frames instead of bursting: DMX is a stream of states, not events.
	DMXOutputAccumulator = FMath::Fmod(DMXOutputAccumulator, Interval);

	OutputScratch.SetNumUninitialized(DMXUniverseSize);
	for (const int32 Universe : DirtyOutputUniverses)
	{
		RenderDMXUniverseInto(Universe, OutputScratch);
		DMXBridge->SendUniverse(Universe, OutputScratch);
	}
	DirtyOutputUniverses.Reset();
}
