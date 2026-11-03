// Copyright Epic Games, Inc. All Rights Reserved.

#include "Subsystems/ShowControlSubsystem.h"

#include "Actors/LightingFixtureActor.h"
#include "Camera/PlayerCameraManager.h"
#include "Data/StageParameterTypes.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "ModularSceneBuilder.h"
#include "Show/StageDMXBridge.h"
#include "Show/StageDMXPatching.h"
#include "Show/StageShadowBudget.h"
#include "Show/StageShowMix.h"
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
	}
	Library = Collection.InitializeDependency<UStageShowLibrarySubsystem>();
	if (Library)
	{
		Library->OnPalettesChanged.AddUniqueDynamic(this, &ThisClass::HandlePalettesChanged);
	}
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
	}
	ProgrammerChangedHandle.Reset();
	Programmer = nullptr;
	if (Library)
	{
		Library->OnPalettesChanged.RemoveDynamic(this, &ThisClass::HandlePalettesChanged);
	}
	Library = nullptr;
	PlaybackValues.Reset();
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
	return !Fades.IsEmpty() || !Strobes.IsEmpty() || IsDMXOutputActive();
}

void UShowControlSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
#if !UE_BUILD_SHIPPING
	const double TickStart = FPlatformTime::Seconds();
#endif

	// Fades first: a fade may change a fixture's Shutter, which changes its strobe rate this frame.
	TickFades(DeltaTime);
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
	if (PlaybackValues.Contains(Id) || (Programmer && (Programmer->FindValues(Id) || Programmer->IsHighlighted(Id))))
	{
		ResolveFixture(*Fixture);
	}
	MarkShadowBudgetDirty();
	OnFixturesChanged.Broadcast();
}

void UShowControlSubsystem::UnregisterFixture(ALightingFixtureActor* Fixture)
{
	if (Fixtures.Remove(Fixture) > 0)
	{
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

bool UShowControlSubsystem::StoreCue(float CueNumber, const FText& Label, float FadeTime)
{
	if (!StageCueRules::IsValidCueNumber(CueNumber))
	{
		UE_LOG(LogStageCraft, Warning, TEXT("StoreCue: %g is not a valid cue number; nothing was stored."), CueNumber);
		return false;
	}

	FShowCue Cue;
	Cue.Number = StageCueRules::RoundCueNumber(CueNumber);
	Cue.Label = StageCueRules::SanitizeLabel(Label);
	Cue.FadeTime = StageCueRules::SanitizeFadeTime(FadeTime);

	// Palette references carry into the cue (Docs/ADR/0028 §3): what the programmer holds keeps the programmer's reference, the rest
	// keeps the playing cue's. Not mid-fade: the output is then between two values, so no reference describes it.
	const FShowCue* Playing = Fades.IsEmpty() ? FindCue(ActiveCueNumber) : nullptr;
	const TArray<ALightingFixtureActor*> Recorded = GetFixtures();
	for (ALightingFixtureActor* Fixture : Recorded)
	{
		FShowCueFixtureState& State = Cue.Fixtures.AddDefaulted_GetRef();
		State.FixtureId = Fixture->GetFixtureId();
		State.Attributes = GetRecordableValues(*Fixture);

		const int32 Id = State.FixtureId;
		const TMap<FGameplayTag, float>* Held = Programmer ? Programmer->FindValues(Id) : nullptr;
		const TMap<FGameplayTag, FGuid>* HeldRefs = Programmer ? Programmer->FindReferences(Id) : nullptr;
		const FShowCueFixtureState* PlayingState = Playing
			? Playing->Fixtures.FindByPredicate([Id](const FShowCueFixtureState& Other) { return Other.FixtureId == Id; }) : nullptr;
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

	const int32 Existing = FindCueIndex(Cue.Number);
	if (Existing != INDEX_NONE)
	{
		Cues[Existing] = MoveTemp(Cue);
	}
	else
	{
		const float Number = Cue.Number;
		const int32 Insert = Cues.IndexOfByPredicate([Number](const FShowCue& Other) { return Other.Number > Number; });
		Cues.Insert(MoveTemp(Cue), Insert == INDEX_NONE ? Cues.Num() : Insert);
	}

	UE_LOG(LogStageCraft, Log, TEXT("Stored cue %.3f (%d fixtures)."), StageCueRules::RoundCueNumber(CueNumber), Recorded.Num());
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
	AssignSanitizedCues(InCues, TEXT("SetCues"));
	ActiveCueNumber = -1.f;
	// A loaded show starts with no cue playing; the fixtures keep the output they were restored with (RestoreActiveCue follows).
	PlaybackValues.Reset();
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
	TArray<int32> Restored;
	for (const FShowCueFixtureState& State : Cues[Index].Fixtures)
	{
		PlaybackValues.Add(State.FixtureId, ResolveReferences(State));
		Restored.Add(State.FixtureId);
	}
	ActiveCueNumber = Cues[Index].Number;
	ResolveFixtures(Restored);
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
	if (ControlSource == EStageControlSource::External)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("GoToCue(%.3g) ignored: fixtures are under external DMX control."), CueNumber);
		return false;
	}

	const int32 Index = FindCueIndex(CueNumber);
	if (Index == INDEX_NONE)
	{
		return false;
	}

	// A new Go while fading starts from wherever the fixtures are right now, like a console's cue take-over.
	if (!Fades.IsEmpty())
	{
		OnCueFinished.Broadcast(ActiveCueNumber);
	}
	Fades.Reset();

	const FShowCue& Cue = Cues[Index];
	for (const FShowCueFixtureState& State : Cue.Fixtures)
	{
		ALightingFixtureActor* Fixture = FindFixture(State.FixtureId);
		if (!Fixture)
		{
			continue; // Fixture was deleted after the cue was recorded; its values are kept in the cue.
		}

		FAttributeFade& Fade = Fades.AddDefaulted_GetRef();
		Fade.Fixture = Fixture;
		// Resolved once, at Go: the fade heads for the palettes' values as they are now, and its frames stay plain lerps.
		Fade.To = ResolveReferences(State);
		for (const TPair<FGameplayTag, float>& Pair : Fade.To)
		{
			// From the playback layer, not the output: a value the programmer holds is not where the cue's fade starts.
			const float From = GetPlaybackValue(*Fixture, Pair.Key);
			if (!FMath::IsNearlyEqual(From, Pair.Value))
			{
				Fade.Changing.Add({ Pair.Key, From, Pair.Value });
			}
		}
	}

	ActiveCueNumber = Cue.Number;
	FadeDuration = Cue.FadeTime;
	FadeElapsed = 0.f;
	OnCueStarted.Broadcast(ActiveCueNumber);

	if (FadeDuration <= KINDA_SMALL_NUMBER || Fades.IsEmpty())
	{
		CompleteFade();
	}
	return true;
}

bool UShowControlSubsystem::Go()
{
	for (const FShowCue& Cue : Cues)
	{
		if (Cue.Number > ActiveCueNumber + StageCueRules::NumberResolution * 0.5f)
		{
			return GoToCue(Cue.Number);
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
			return GoToCue(Cues[Index].Number);
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
	for (const FAttributeFade& Fade : Fades)
	{
		if (ALightingFixtureActor* Fixture = Fade.Fixture.Get())
		{
			PlaybackValues.FindOrAdd(Fixture->GetFixtureId()).Append(Fade.To);
			TMap<FGameplayTag, float> Final = Fade.To;
			RemoveHeldAttributes(*Fixture, Final);
			Fixture->SetAttributes(Final);
		}
	}
	Fades.Reset();
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

	const float Alpha = FadeElapsed / FadeDuration;
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
			Frame.Add(Lerp.Attribute, FMath::Lerp(Lerp.From, Lerp.To, Alpha));
		}
		// The playback layer follows the fade, so a take-over or a Clear mid-fade starts from where the cue is now.
		PlaybackValues.FindOrAdd(Fixture->GetFixtureId()).Append(Frame);
		RemoveHeldAttributes(*Fixture, Frame);
		Fixture->SetAttributes(Frame);
	}
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

void UShowControlSubsystem::ResolveFixture(ALightingFixtureActor& Fixture)
{
	const int32 Id = Fixture.GetFixtureId();
	TMap<FGameplayTag, float> HighlightValues;
	const bool bHighlighted = Programmer && Programmer->IsHighlighted(Id);
	if (bHighlighted)
	{
		MakeHighlightValues(Fixture, HighlightValues);
	}

	// Under External DMX the console owns the cue and home layers; only what the user holds by hand is written over it.
	const bool bInternal = ControlSource == EStageControlSource::Internal;
	TMap<FGameplayTag, float> ProgrammerScratch;
	StageShowMix::FFixtureLayers Layers;
	Layers.Home = bInternal ? &Fixture.GetHomeAttributes() : nullptr;
	Layers.Playback = bInternal ? PlaybackValues.Find(Id) : nullptr;
	Layers.Programmer = GetProgrammerLayer(Fixture, ProgrammerScratch);
	Layers.Highlight = bHighlighted ? &HighlightValues : nullptr;

	TArray<FGameplayTag> Attributes;
	Fixture.GetAttributes().GetKeys(Attributes);
	TMap<FGameplayTag, float> Mixed;
	StageShowMix::ResolveFixture(Attributes, Layers, Mixed);
	// SetAttributes stores and applies only the values that differ from the output.
	Fixture.SetAttributes(Mixed);
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
	if (!Programmer || !Programmer->IsHighlighted(Id))
	{
		return Fixture.GetAttributes();
	}
	// Highlighted: the output shows the highlight, so the look underneath is mixed again without it.
	TMap<FGameplayTag, float> Values = Fixture.GetAttributes();
	TMap<FGameplayTag, float> ProgrammerScratch;
	StageShowMix::FFixtureLayers Layers;
	Layers.Home = &Fixture.GetHomeAttributes();
	Layers.Playback = PlaybackValues.Find(Id);
	Layers.Programmer = GetProgrammerLayer(Fixture, ProgrammerScratch);
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
	const FShowCue* Cue = ControlSource == EStageControlSource::Internal ? FindCue(ActiveCueNumber) : nullptr;
	if (!Cue)
	{
		return;
	}
	for (const FShowCueFixtureState& State : Cue->Fixtures)
	{
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
	Data.SetNumZeroed(DMXUniverseSize);
	if (Patch)
	{
		for (const TWeakObjectPtr<ALightingFixtureActor>& Fixture : Patch->GetFixturesInUniverse(Universe))
		{
			if (Fixture.IsValid())
			{
				Fixture->WriteDMX(Data);
			}
		}
	}
	return Data;
}

void UShowControlSubsystem::TickDMXOutput(float DeltaTime)
{
	if (!IsDMXOutputActive())
	{
		return;
	}

	DMXOutputAccumulator += DeltaTime;
	const float Interval = 1.f / FMath::Clamp(DMXOutputRateHz, 1.f, 60.f);
	if (DMXOutputAccumulator < Interval)
	{
		return;
	}
	// Drop missed frames instead of bursting: DMX is a stream of states, not events.
	DMXOutputAccumulator = FMath::Fmod(DMXOutputAccumulator, Interval);

	if (!Patch)
	{
		return;
	}
	for (const int32 Universe : Patch->GetPatchedUniverses())
	{
		DMXBridge->SendUniverse(Universe, RenderDMXUniverse(Universe));
	}
}
