// Copyright Epic Games, Inc. All Rights Reserved.

#include "Subsystems/ShowControlSubsystem.h"

#include "Actors/LightingFixtureActor.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "ModularSceneBuilder.h"
#include "Show/StageDMXBridge.h"
#include "Show/StageShadowBudget.h"
#include "TimerManager.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ShowControlSubsystem)

namespace
{
	constexpr int32 DMXUniverseSize = 512;

	/** Inclusive, 1-based range a patched fixture occupies. */
	bool GetOccupiedRange(const ALightingFixtureActor& Fixture, int32& OutFirst, int32& OutLast)
	{
		const FStageDMXPatch Patch = Fixture.GetPatch();
		const int32 Footprint = Fixture.GetDMXFootprint();
		if (!Patch.IsPatched() || Footprint <= 0)
		{
			return false;
		}
		OutFirst = Patch.Address;
		OutLast = Patch.Address + Footprint - 1;
		return true;
	}
}

bool UShowControlSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Shows run in play sessions only; editor worlds have no fixtures registering.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UShowControlSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ShadowBudgetTimer);
	}
	SetDMXBridge(nullptr);
	Fixtures.Reset();
	Fades.Reset();
	Strobes.Reset();

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

	if (bAutoPatchNewFixtures && !Fixture->GetPatch().IsPatched() && Fixture->GetDMXFootprint() > 0)
	{
		Fixture->SetPatch(FindNextFreePatch(Fixture->GetDMXFootprint()));
	}

	Fixtures.Add(Fixture);
	UE_LOG(LogStageCraft, Log, TEXT("Fixture %d registered (%s), patch %d.%03d."),
		Fixture->GetFixtureId(), *Fixture->GetName(), Fixture->GetPatch().Universe, Fixture->GetPatch().Address);
	MarkShadowBudgetDirty();
	OnFixturesChanged.Broadcast();
}

void UShowControlSubsystem::UnregisterFixture(ALightingFixtureActor* Fixture)
{
	if (Fixtures.Remove(Fixture) > 0)
	{
		Fades.RemoveAll([Fixture](const FAttributeFade& Fade) { return Fade.Fixture == Fixture; });
		Strobes.RemoveAll([Fixture](const FStrobeState& State) { return State.Fixture == Fixture; });
		MarkShadowBudgetDirty(); // Its place in the budget is free for the next brightest.
		OnFixturesChanged.Broadcast();
	}
}

void UShowControlSubsystem::MarkShadowBudgetDirty()
{
	UWorld* World = GetWorld();
	if (!World || World->GetTimerManager().IsTimerActive(ShadowBudgetTimer))
	{
		return; // Already armed: one re-ranking covers every change until it runs.
	}
	const float Interval = GetDefault<UStageShowPerformanceSettings>()->GetUpdateSeconds();
	World->GetTimerManager().SetTimer(ShadowBudgetTimer, FTimerDelegate::CreateUObject(this, &ThisClass::UpdateShadowBudget), Interval, /*bLoop*/ false);
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

	TArray<FStageShadowCandidate> Candidates;
	Candidates.Reserve(Current.Num());
	for (const ALightingFixtureActor* Fixture : Current)
	{
		FStageShadowCandidate& Candidate = Candidates.AddDefaulted_GetRef();
		const double Distance = ViewLocation.IsSet() ? FVector::Dist(ViewLocation.GetValue(), Fixture->GetActorLocation()) : 100.0;
		Candidate.Importance = StageShadowBudget::GetImportance(Fixture->GetShadowRankingFlux(), Distance);
		Candidate.bCasting = Fixture->AreBeamShadowsEnabled();
	}

	const TArray<bool> Casting = StageShadowBudget::SelectCasters(Candidates, Settings->GetShadowBudget(), Settings->GetHysteresis());
	int32 Switched = 0;
	for (int32 Index = 0; Index < Current.Num(); ++Index)
	{
		if (Current[Index]->AreBeamShadowsEnabled() != Casting[Index])
		{
			Current[Index]->SetBeamShadowsEnabled(Casting[Index]);
			++Switched;
		}
	}
	if (Switched > 0)
	{
		UE_LOG(LogStageCraft, Verbose, TEXT("Shadow budget: %d of %d fixtures cast shadows (%d switched)."), GetShadowedFixtureCount(), Current.Num(), Switched);
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

FStageDMXPatch UShowControlSubsystem::FindNextFreePatch(int32 Footprint, int32 StartUniverse) const
{
	FStageDMXPatch Result;
	if (Footprint <= 0 || Footprint > DMXUniverseSize)
	{
		return Result;
	}

	for (int32 Universe = FMath::Max(1, StartUniverse); Universe <= 63999; ++Universe)
	{
		TBitArray<> Used(false, DMXUniverseSize + 1);
		bool bUniverseEmpty = true;
		for (const TWeakObjectPtr<ALightingFixtureActor>& Fixture : Fixtures)
		{
			int32 First = 0;
			int32 Last = 0;
			if (Fixture.IsValid() && Fixture->GetPatch().Universe == Universe && GetOccupiedRange(*Fixture, First, Last))
			{
				bUniverseEmpty = false;
				for (int32 Slot = First; Slot <= FMath::Min(Last, DMXUniverseSize); ++Slot)
				{
					Used[Slot] = true;
				}
			}
		}

		if (bUniverseEmpty)
		{
			Result.Universe = Universe;
			Result.Address = 1;
			return Result;
		}

		int32 RunStart = 1;
		for (int32 Slot = 1; Slot <= DMXUniverseSize; ++Slot)
		{
			if (Used[Slot])
			{
				RunStart = Slot + 1;
			}
			else if (Slot - RunStart + 1 >= Footprint)
			{
				Result.Universe = Universe;
				Result.Address = RunStart;
				return Result;
			}
		}
	}
	return Result;
}

TArray<int32> UShowControlSubsystem::FindPatchConflicts() const
{
	TArray<ALightingFixtureActor*> Sorted = GetFixtures();
	TSet<int32> Conflicts;

	for (int32 IndexA = 0; IndexA < Sorted.Num(); ++IndexA)
	{
		int32 FirstA = 0;
		int32 LastA = 0;
		if (!GetOccupiedRange(*Sorted[IndexA], FirstA, LastA))
		{
			continue;
		}
		for (int32 IndexB = IndexA + 1; IndexB < Sorted.Num(); ++IndexB)
		{
			int32 FirstB = 0;
			int32 LastB = 0;
			if (Sorted[IndexB]->GetPatch().Universe == Sorted[IndexA]->GetPatch().Universe
				&& GetOccupiedRange(*Sorted[IndexB], FirstB, LastB)
				&& FirstA <= LastB && FirstB <= LastA)
			{
				Conflicts.Add(Sorted[IndexA]->GetFixtureId());
				Conflicts.Add(Sorted[IndexB]->GetFixtureId());
			}
		}
	}

	TArray<int32> Result = Conflicts.Array();
	Result.Sort();
	return Result;
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

	const TArray<ALightingFixtureActor*> Recorded = GetFixtures();
	for (ALightingFixtureActor* Fixture : Recorded)
	{
		FShowCueFixtureState& State = Cue.Fixtures.AddDefaulted_GetRef();
		State.FixtureId = Fixture->GetFixtureId();
		State.Attributes = Fixture->GetAttributes();
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
	OnCueListChanged.Broadcast();
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
		Fade.To = State.Attributes;
		for (const TPair<FGameplayTag, float>& Pair : State.Attributes)
		{
			const float From = Fixture->GetAttribute(Pair.Key);
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
			Fixture->SetAttributes(Fade.To);
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
	OnControlSourceChanged.Broadcast(ControlSource);
}

bool UShowControlSubsystem::IsDMXOutputActive() const
{
	return DMXBridge && DMXBridge->IsOutputEnabled() && ControlSource == EStageControlSource::Internal;
}

void UShowControlSubsystem::ReceiveDMXUniverse(int32 Universe, const TArray<uint8>& Channels)
{
	if (ControlSource != EStageControlSource::External || Channels.Num() < DMXUniverseSize)
	{
		return;
	}

	for (const TWeakObjectPtr<ALightingFixtureActor>& Fixture : Fixtures)
	{
		if (Fixture.IsValid() && Fixture->GetPatch().Universe == Universe)
		{
			Fixture->ReadDMX(Channels);
		}
	}
}

TArray<uint8> UShowControlSubsystem::RenderDMXUniverse(int32 Universe) const
{
	TArray<uint8> Data;
	Data.SetNumZeroed(DMXUniverseSize);
	for (const TWeakObjectPtr<ALightingFixtureActor>& Fixture : Fixtures)
	{
		if (Fixture.IsValid() && Fixture->GetPatch().Universe == Universe)
		{
			Fixture->WriteDMX(Data);
		}
	}
	return Data;
}

TArray<int32> UShowControlSubsystem::GetPatchedUniverses() const
{
	TSet<int32> Universes;
	for (const TWeakObjectPtr<ALightingFixtureActor>& Fixture : Fixtures)
	{
		if (Fixture.IsValid() && Fixture->GetPatch().IsPatched())
		{
			Universes.Add(Fixture->GetPatch().Universe);
		}
	}
	TArray<int32> Result = Universes.Array();
	Result.Sort();
	return Result;
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

	for (const int32 Universe : GetPatchedUniverses())
	{
		DMXBridge->SendUniverse(Universe, RenderDMXUniverse(Universe));
	}
}
