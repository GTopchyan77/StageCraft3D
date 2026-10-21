// Copyright Epic Games, Inc. All Rights Reserved.

// World tests for restored item state (Docs/ADR/0013-preferences-key-bindings-and-audit-fixes.md §3.2, audit risk R6): values
// that arrive through a snapshot (a scene file or an undo step) are brought within the limits every live write has, each repair
// is counted, and clean state restores with no repairs. The snapshots are tampered with through reflection, the way a damaged or
// hand-edited scene file would deliver them.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Scene.RestoredState; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/AudioEquipmentActor.h"
#include "Actors/LightingFixtureActor.h"
#include "Data/AudioEquipmentData.h"
#include "Data/LightingFixtureData.h"
#include "Data/StageParameterTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "History/StageItemSnapshot.h"
#include "Misc/AutomationTest.h"

#include <limits>

namespace StageRestoredStateTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;
	constexpr float NaN = std::numeric_limits<float>::quiet_NaN();

	/** A game world that has begun play, destroyed with the scope. */
	struct FScopedWorld
	{
		UWorld* World = nullptr;

		FScopedWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageRestoredStateTestWorld"));
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			World->GetWorldSettings()->NotifyBeginPlay();
		}

		~FScopedWorld()
		{
			World->BeginTearingDown();
			World->EndPlay(EEndPlayReason::Quit);
			World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			GEngine->DestroyWorldContext(World);
		}

		/** Spawns ActorClass for Data the way USpawnSystemComponent does; with Snapshot, its state is restored first. */
		template <typename TActor>
		TActor* Spawn(UBaseItemData* Data, const FStageItemSnapshot* Snapshot = nullptr) const
		{
			TActor* Actor = World->SpawnActorDeferred<TActor>(TActor::StaticClass(), FTransform::Identity);
			Actor->AssignInstanceId(FGuid::NewGuid());
			if (Snapshot)
			{
				Actor->RestoreSnapshotState(*Snapshot);
			}
			Actor->InitializeFromItemData(Data);
			Actor->FinishSpawning(FTransform::Identity);
			return Actor;
		}
	};

	/** Writes a protected SaveGame property as a damaged file would deliver it. */
	template <typename TValue>
	TValue* Field(UObject& Object, const TCHAR* Name)
	{
		const FProperty* Property = Object.GetClass()->FindPropertyByName(Name);
		return Property ? Property->ContainerPtrToValuePtr<TValue>(&Object) : nullptr;
	}

	FStageDMXPatch MakePatch(int32 Universe, int32 Address)
	{
		FStageDMXPatch Patch;
		Patch.Universe = Universe;
		Patch.Address = Address;
		return Patch;
	}

	ULightingFixtureData* MakeMovingHead()
	{
		ULightingFixtureData* Data = NewObject<ULightingFixtureData>(GetTransientPackage());
		Data->ColorSystem = EStageFixtureColorSystem::RGBW;
		Data->GoboWheel.SetNum(3);
		Data->bHasPanTilt = true;
		Data->BeamAngleMin = 5.f;
		Data->BeamAngleMax = 50.f;
		return Data;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageRestoredStateTest, "StageCraft.Scene.RestoredState", StageRestoredStateTests::Flags)

bool FStageRestoredStateTest::RunTest(const FString& Parameters)
{
	using namespace StageRestoredStateTests;
	FScopedWorld Scope;

	// A clean fixture restores with no repairs.
	ULightingFixtureData* Head = MakeMovingHead();
	ALightingFixtureActor* Source = Scope.Spawn<ALightingFixtureActor>(Head);
	if (!TestNotNull(TEXT("Fixture"), Source))
	{
		return false;
	}
	Source->SetAttribute(StageCraftTags::Attribute_Dimmer, 0.4f);
	Source->SetPatch(MakePatch(2, 101));
	const FStageItemSnapshot CleanState = Source->CaptureSnapshot();
	const ALightingFixtureActor* Clean = Scope.Spawn<ALightingFixtureActor>(Head, &CleanState);
	TestEqual(TEXT("Clean: no repairs"), Clean->GetStateRepairCount(), 0);
	TestEqual(TEXT("Clean: the value comes back"), Clean->GetAttribute(StageCraftTags::Attribute_Dimmer), 0.4f);
	// Regression: the patch's fields were not SaveGame, so snapshots and scene files dropped the patch.
	TestTrue(TEXT("Clean: the DMX patch comes back"), Clean->GetPatch() == MakePatch(2, 101));

	// The same fixture with every kind of damage a file can carry.
	TMap<FGameplayTag, float>* Attributes = Field<TMap<FGameplayTag, float>>(*Source, TEXT("Attributes"));
	FStageDMXPatch* Patch = Field<FStageDMXPatch>(*Source, TEXT("Patch"));
	int32* FixtureId = Field<int32>(*Source, TEXT("FixtureId"));
	FText* Label = Field<FText>(*Source, TEXT("InstanceLabel"));
	if (!TestTrue(TEXT("Reflection reaches the saved fields"), Attributes && Patch && FixtureId && Label))
	{
		return false;
	}
	Attributes->Add(StageCraftTags::Attribute_Dimmer, NaN);
	Attributes->Add(StageCraftTags::Attribute_Zoom, 999.f);
	Attributes->Add(StageCraftTags::Attribute_Gobo, 1.4f);
	Patch->Universe = 70000;
	Patch->Address = 3;
	*FixtureId = -5;
	*Label = FText::FromString(FString::ChrN(300, TEXT('x')));

	const FStageItemSnapshot DamagedState = Source->CaptureSnapshot();
	const ALightingFixtureActor* Repaired = Scope.Spawn<ALightingFixtureActor>(Head, &DamagedState);
	float ZoomMin = 0.f;
	float ZoomMax = 0.f;
	Head->GetAttributeRange(StageCraftTags::Attribute_Zoom, ZoomMin, ZoomMax);
	TestEqual(TEXT("Damaged: a NaN dimmer takes the default"), Repaired->GetAttribute(StageCraftTags::Attribute_Dimmer), Head->GetAttributeDefault(StageCraftTags::Attribute_Dimmer));
	TestEqual(TEXT("Damaged: an out-of-range zoom is clamped"), Repaired->GetAttribute(StageCraftTags::Attribute_Zoom), ZoomMax);
	TestEqual(TEXT("Damaged: a stepped gobo is rounded to a slot"), Repaired->GetAttribute(StageCraftTags::Attribute_Gobo), 1.f);
	TestEqual(TEXT("Damaged: the universe is clamped"), Repaired->GetPatch().Universe, 63999);
	TestEqual(TEXT("Damaged: a valid address is kept"), Repaired->GetPatch().Address, 3);
	TestTrue(TEXT("Damaged: a negative fixture ID is replaced (then auto-assigned)"), Repaired->GetFixtureId() >= 0);
	TestEqual(TEXT("Damaged: the label is shortened"), Repaired->GetInstanceLabel().ToString().Len(), AModularBaseActor::MaxLabelLength);
	TestEqual(TEXT("Damaged: every repair is counted"), Repaired->GetStateRepairCount(), 6);

	// Audio: gain, delay and splay rely on editor-only metadata in the inspector; restored values get the same limits.
	UAudioEquipmentData* Cabinet = NewObject<UAudioEquipmentData>(GetTransientPackage());
	Cabinet->AudioKind = EStageAudioKind::LineArrayElement;
	Cabinet->MaxSplayAngle = 10.f;
	AAudioEquipmentActor* Speaker = Scope.Spawn<AAudioEquipmentActor>(Cabinet);
	float* Gain = Field<float>(*Speaker, TEXT("GainDb"));
	float* Delay = Field<float>(*Speaker, TEXT("DelayMs"));
	float* Splay = Field<float>(*Speaker, TEXT("SplayAngle"));
	if (!TestTrue(TEXT("Reflection reaches the audio fields"), Gain && Delay && Splay))
	{
		return false;
	}
	*Gain = NaN;
	*Delay = 5000.f;
	*Splay = -3.f;
	const FStageItemSnapshot DamagedAudio = Speaker->CaptureSnapshot();
	const AAudioEquipmentActor* RepairedSpeaker = Scope.Spawn<AAudioEquipmentActor>(Cabinet, &DamagedAudio);
	TestEqual(TEXT("Audio: a NaN gain becomes 0 dB"), RepairedSpeaker->GetGainDb(), 0.f);
	TestEqual(TEXT("Audio: the delay is clamped"), RepairedSpeaker->GetDelayMs(), 1000.f);
	TestEqual(TEXT("Audio: a negative splay becomes 0"), RepairedSpeaker->GetSplayAngle(), 0.f);
	TestEqual(TEXT("Audio: every repair is counted"), RepairedSpeaker->GetStateRepairCount(), 3);

	// Undamaged audio state, beyond a swapped-in cabinet's splay, is a model change and not counted as damage.
	*Gain = -6.f;
	*Delay = 12.f;
	*Splay = 8.f;
	UAudioEquipmentData* Narrow = NewObject<UAudioEquipmentData>(GetTransientPackage());
	Narrow->AudioKind = EStageAudioKind::LineArrayElement;
	Narrow->MaxSplayAngle = 5.f;
	const FStageItemSnapshot WideSplay = Speaker->CaptureSnapshot();
	const AAudioEquipmentActor* Swapped = Scope.Spawn<AAudioEquipmentActor>(Narrow, &WideSplay);
	TestEqual(TEXT("Audio swap: splay limited to the cabinet"), Swapped->GetSplayAngle(), 5.f);
	TestEqual(TEXT("Audio swap: not counted as a repair"), Swapped->GetStateRepairCount(), 0);
	TestEqual(TEXT("Audio swap: gain kept"), Swapped->GetGainDb(), -6.f);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
