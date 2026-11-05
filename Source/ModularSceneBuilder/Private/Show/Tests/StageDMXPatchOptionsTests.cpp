// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the pan / tilt patch options and the fixture schedule import (Docs/ADR/0019 §10):
//  - PatchOptionsMap (pure): StageDMXPatchOptions::GetChannelSource and GetEffective for every combination.
//  - PatchOptions (world): every invert / swap combination round-trips WriteDMX -> ReadDMX; invert and swap put the expected
//    bytes on the wire; the attributes (the beam) never change; a pan-only fixture ignores swap and tilt; the parameters are
//    offered per axis, are patch parameters, and an edit is undone through the history like any patch edit; the options
//    survive the item's snapshot (undo, scenes).
//  - ScheduleParse (pure): the patch sheet's own CSV reads back; BOM, CRLF, quotes, semicolons, the apostrophe guard, missing
//    columns and hostile input.
//  - ScheduleImport (world): PlanSchedule over real fixtures (unknown, duplicate, footprint overflow, unchanged, conflicts,
//    options carried into the assignments) and LoadScheduleFile's refusals.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.DMX; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/LightingFixtureActor.h"
#include "Components/SelectionComponent.h"
#include "Components/SpawnSystemComponent.h"
#include "Data/LightingFixtureData.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "History/StageEditHistoryComponent.h"
#include "History/StageItemSnapshot.h"
#include "Interaction/StageParameterInterface.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Report/StagePatchSheet.h"
#include "Show/StageDMXPatching.h"
#include "Show/StagePatchSchedule.h"
#include "Subsystems/StageDMXPatchSubsystem.h"

namespace StageDMXPatchOptionsTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	FStageDMXPatchOptions MakeOptions(int32 Bits)
	{
		FStageDMXPatchOptions Options;
		Options.bInvertPan = (Bits & 1) != 0;
		Options.bInvertTilt = (Bits & 2) != 0;
		Options.bSwapPanTilt = (Bits & 4) != 0;
		return Options;
	}

	/** A game world that has begun play, destroyed with the scope. */
	struct FScopedWorld
	{
		UWorld* World = nullptr;

		FScopedWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageDMXPatchOptionsTestWorld"));
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

		ALightingFixtureActor* SpawnFixture(ULightingFixtureData* Data) const
		{
			ALightingFixtureActor* Fixture = World->SpawnActorDeferred<ALightingFixtureActor>(ALightingFixtureActor::StaticClass(), FTransform::Identity);
			Fixture->InitializeFromItemData(Data);
			Fixture->FinishSpawning(FTransform::Identity);
			return Fixture;
		}
	};

	const FStageFixtureDMXChannel* FindChannel(const ULightingFixtureData& Data, const FGameplayTag& Attribute)
	{
		return Data.DMXChannels.FindByPredicate([&Attribute](const FStageFixtureDMXChannel& Channel) { return Channel.Attribute == Attribute; });
	}

	/** The 16-bit value at a fixture's channel (coarse at Channel, fine after). */
	int32 Read16(const TArray<uint8>& Universe, const ALightingFixtureActor& Fixture, const FStageFixtureDMXChannel& Channel)
	{
		const int32 Index = Fixture.GetPatch().Address - 1 + Channel.Channel - 1;
		return (Universe[Index] << 8) | Universe[Index + 1];
	}

	TArray<uint8> Render(const ALightingFixtureActor& Fixture)
	{
		TArray<uint8> Universe;
		Universe.SetNumZeroed(512);
		Fixture.WriteDMX(Universe);
		return Universe;
	}

	bool HasParameter(const ALightingFixtureActor& Fixture, const FGameplayTag& ParameterId)
	{
		for (const FStageParameterSection& Section : IStageParameterInterface::Execute_GetParameterSections(&Fixture))
		{
			if (Section.Parameters.ContainsByPredicate([&ParameterId](const FStageParameterDescriptor& Descriptor) { return Descriptor.Id == ParameterId; }))
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDMXPatchOptionsMapTest, "StageCraft.DMX.PatchOptionsMap", StageDMXPatchOptionsTests::Flags)

bool FStageDMXPatchOptionsMapTest::RunTest(const FString& Parameters)
{
	using namespace StageDMXPatchOptionsTests;
	const FGameplayTag Pan = StageCraftTags::Attribute_Pan;
	const FGameplayTag Tilt = StageCraftTags::Attribute_Tilt;
	const FGameplayTag Dimmer = StageCraftTags::Attribute_Dimmer;

	for (int32 Bits = 0; Bits < 8; ++Bits)
	{
		const FStageDMXPatchOptions Options = MakeOptions(Bits);
		const FStageDMXChannelSource PanChannel = StageDMXPatchOptions::GetChannelSource(Pan, Options);
		const FStageDMXChannelSource TiltChannel = StageDMXPatchOptions::GetChannelSource(Tilt, Options);
		const FString Case = FString::Printf(TEXT("options %d"), Bits);
		TestEqual(*(Case + TEXT(": pan channel carries")), PanChannel.Attribute, Options.bSwapPanTilt ? Tilt : Pan);
		TestEqual(*(Case + TEXT(": tilt channel carries")), TiltChannel.Attribute, Options.bSwapPanTilt ? Pan : Tilt);
		// The invert belongs to the console axis, whichever channel it travels on.
		TestEqual(*(Case + TEXT(": pan channel invert")), PanChannel.bInvert, PanChannel.Attribute == Pan ? Options.bInvertPan : Options.bInvertTilt);
		TestEqual(*(Case + TEXT(": tilt channel invert")), TiltChannel.bInvert, TiltChannel.Attribute == Pan ? Options.bInvertPan : Options.bInvertTilt);
		const FStageDMXChannelSource Other = StageDMXPatchOptions::GetChannelSource(Dimmer, Options);
		TestTrue(*(Case + TEXT(": other attributes untouched")), Other.Attribute == Dimmer && !Other.bInvert);
	}

	const FStageDMXPatchOptions All = MakeOptions(7);
	TestTrue(TEXT("pan only: no swap, no tilt invert"), StageDMXPatchOptions::GetEffective(All, true, false) == MakeOptions(1));
	TestTrue(TEXT("tilt only: tilt invert only"), StageDMXPatchOptions::GetEffective(All, false, true) == MakeOptions(2));
	TestTrue(TEXT("both: all kept"), StageDMXPatchOptions::GetEffective(All, true, true) == All);
	TestTrue(TEXT("neither: none"), StageDMXPatchOptions::GetEffective(All, false, false).IsDefault());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDMXPatchOptionsTest, "StageCraft.DMX.PatchOptions", StageDMXPatchOptionsTests::Flags)

bool FStageDMXPatchOptionsTest::RunTest(const FString& Parameters)
{
	using namespace StageDMXPatchOptionsTests;
	const FGameplayTag Pan = StageCraftTags::Attribute_Pan;
	const FGameplayTag Tilt = StageCraftTags::Attribute_Tilt;

	FScopedWorld Scope;
	ULightingFixtureData* Data = NewObject<ULightingFixtureData>(GetTransientPackage());
	const FStageFixtureDMXChannel* PanChannel = FindChannel(*Data, Pan);
	const FStageFixtureDMXChannel* TiltChannel = FindChannel(*Data, Tilt);
	if (!TestTrue(TEXT("a moving head with 16-bit pan and tilt"), PanChannel && TiltChannel && PanChannel->b16Bit && TiltChannel->b16Bit))
	{
		return false;
	}
	ALightingFixtureActor* Fixture = Scope.SpawnFixture(Data);
	if (!TestTrue(TEXT("patched"), Fixture->GetPatch().IsPatched()))
	{
		return false;
	}

	// A look away from the middle, so an invert or a swap shows in the bytes.
	TMap<FGameplayTag, float> Look;
	Look.Add(Pan, 120.f);
	Look.Add(Tilt, -40.f);
	Fixture->SetAttributes(Look);
	const float LookPan = Fixture->GetAttribute(Pan);
	const float LookTilt = Fixture->GetAttribute(Tilt);
	const TArray<uint8> Plain = Render(*Fixture);
	const int32 PlainPan = Read16(Plain, *Fixture, *PanChannel);
	const int32 PlainTilt = Read16(Plain, *Fixture, *TiltChannel);
	TestTrue(TEXT("a look away from the middle"), FMath::Abs(PlainPan - 32768) > 1000 && FMath::Abs(PlainTilt - 32768) > 1000);

	for (int32 Bits = 0; Bits < 8; ++Bits)
	{
		const FStageDMXPatchOptions Options = MakeOptions(Bits);
		const FString Case = FString::Printf(TEXT("options %d"), Bits);
		Fixture->SetPatchOptions(Options);
		TestTrue(*(Case + TEXT(": the beam does not move")), FMath::IsNearlyEqual(Fixture->GetAttribute(Pan), LookPan) && FMath::IsNearlyEqual(Fixture->GetAttribute(Tilt), LookTilt));

		const TArray<uint8> Wire = Render(*Fixture);
		const int32 PanOnWire = Read16(Wire, *Fixture, *PanChannel);
		const int32 TiltOnWire = Read16(Wire, *Fixture, *TiltChannel);
		const int32 PanSource = Options.bSwapPanTilt ? PlainTilt : PlainPan;
		const int32 TiltSource = Options.bSwapPanTilt ? PlainPan : PlainTilt;
		const bool bPanChannelInverted = Options.bSwapPanTilt ? Options.bInvertTilt : Options.bInvertPan;
		const bool bTiltChannelInverted = Options.bSwapPanTilt ? Options.bInvertPan : Options.bInvertTilt;
		TestTrue(*(Case + TEXT(": pan channel bytes")), FMath::Abs(PanOnWire - (bPanChannelInverted ? 65535 - PanSource : PanSource)) <= 1);
		TestTrue(*(Case + TEXT(": tilt channel bytes")), FMath::Abs(TiltOnWire - (bTiltChannelInverted ? 65535 - TiltSource : TiltSource)) <= 1);

		// Back through ReadDMX: the same look.
		TMap<FGameplayTag, float> Elsewhere;
		Elsewhere.Add(Pan, 0.f);
		Elsewhere.Add(Tilt, 0.f);
		Fixture->SetAttributes(Elsewhere);
		Fixture->ReadDMX(Wire);
		TestTrue(*(Case + TEXT(": round trip pan")), FMath::IsNearlyEqual(Fixture->GetAttribute(Pan), LookPan, 0.05f));
		TestTrue(*(Case + TEXT(": round trip tilt")), FMath::IsNearlyEqual(Fixture->GetAttribute(Tilt), LookTilt, 0.05f));
		Fixture->SetAttributes(Look);
	}
	Fixture->SetPatchOptions(FStageDMXPatchOptions());
	TestEqual(TEXT("options off: the plain bytes again"), Render(*Fixture), Plain);

	// Parameters: one per axis, patch parameters (undoable), read back as set.
	TestTrue(TEXT("Invert Pan offered"), HasParameter(*Fixture, StageCraftTags::Param_Patch_InvertPan));
	TestTrue(TEXT("Invert Tilt offered"), HasParameter(*Fixture, StageCraftTags::Param_Patch_InvertTilt));
	TestTrue(TEXT("Swap offered"), HasParameter(*Fixture, StageCraftTags::Param_Patch_SwapPanTilt));
	TestTrue(TEXT("patch parameters"), StageCraftTags::IsPatchParameter(StageCraftTags::Param_Patch_InvertPan)
		&& StageCraftTags::IsPatchParameter(StageCraftTags::Param_Patch_InvertTilt) && StageCraftTags::IsPatchParameter(StageCraftTags::Param_Patch_SwapPanTilt));
	const uint32 Revision = UWorld::GetSubsystem<UStageDMXPatchSubsystem>(Scope.World)->GetPatchRevision();
	TestTrue(TEXT("written as a parameter"), IStageParameterInterface::Execute_SetParameterValue(Fixture, StageCraftTags::Param_Patch_SwapPanTilt, FStageParameterValue::MakeBool(true)));
	FStageParameterValue Read;
	TestTrue(TEXT("read back"), IStageParameterInterface::Execute_GetParameterValue(Fixture, StageCraftTags::Param_Patch_SwapPanTilt, Read) && Read.bBool);
	TestTrue(TEXT("a new patch revision (output re-sends, input re-reads)"), UWorld::GetSubsystem<UStageDMXPatchSubsystem>(Scope.World)->GetPatchRevision() != Revision);
	TestFalse(TEXT("a float is not taken"), IStageParameterInterface::Execute_SetParameterValue(Fixture, StageCraftTags::Param_Patch_InvertPan, FStageParameterValue::MakeFloat(1.0)));

	// Undo, as the controller records a patch parameter edit: written, then recorded with the value before (the history reads
	// the value after from the item).
	{
		AActor* Owner = Scope.World->SpawnActor<AActor>();
		for (UClass* ComponentClass : { USpawnSystemComponent::StaticClass(), USelectionComponent::StaticClass(), UStageEditHistoryComponent::StaticClass() })
		{
			UActorComponent* Component = NewObject<UActorComponent>(Owner, ComponentClass);
			Component->RegisterComponent();
		}
		UStageEditHistoryComponent* History = Owner->FindComponentByClass<UStageEditHistoryComponent>();
		IStageParameterInterface::Execute_SetParameterValue(Fixture, StageCraftTags::Param_Patch_InvertTilt, FStageParameterValue::MakeBool(true));
		History->RecordParameterChange(Fixture, StageCraftTags::Param_Patch_InvertTilt, FText::FromString(TEXT("Invert Tilt")), FStageParameterValue::MakeBool(false));
		TestTrue(TEXT("set"), Fixture->GetPatchOptions().bInvertTilt);
		History->Undo();
		TestFalse(TEXT("undone"), Fixture->GetPatchOptions().bInvertTilt);
		History->Redo();
		TestTrue(TEXT("redone"), Fixture->GetPatchOptions().bInvertTilt);
	}

	// Saved with the item; a state saved before the options existed loads with none.
	{
		const FStageDMXPatchOptions Saved = Fixture->GetPatchOptions();
		const FStageItemSnapshot Snapshot = Fixture->CaptureSnapshot();
		Fixture->SetPatchOptions(FStageDMXPatchOptions());
		TestTrue(TEXT("snapshot restored"), Fixture->RestoreSnapshotState(Snapshot));
		TestTrue(TEXT("restored from the saved state"), Fixture->GetPatchOptions() == Saved);
	}

	// A fixture with a pan channel only: swap and tilt invert do nothing on the wire, and are not offered.
	ULightingFixtureData* PanOnly = NewObject<ULightingFixtureData>(GetTransientPackage());
	PanOnly->DMXChannels.RemoveAll([](const FStageFixtureDMXChannel& Channel) { return Channel.Attribute == StageCraftTags::Attribute_Tilt; });
	ALightingFixtureActor* Single = Scope.SpawnFixture(PanOnly);
	bool bHasPan = false;
	bool bHasTilt = false;
	Single->GetPanTiltChannels(bHasPan, bHasTilt);
	TestTrue(TEXT("pan only"), bHasPan && !bHasTilt);
	Single->SetAttributes(Look);
	const TArray<uint8> SinglePlain = Render(*Single);
	FStageDMXPatchOptions SwapAndTilt;
	SwapAndTilt.bSwapPanTilt = true;
	SwapAndTilt.bInvertTilt = true;
	Single->SetPatchOptions(SwapAndTilt);
	TestEqual(TEXT("swap and tilt invert ignored"), Render(*Single), SinglePlain);
	TestTrue(TEXT("kept as given"), Single->GetPatchOptions() == SwapAndTilt);
	TestTrue(TEXT("Invert Pan offered"), HasParameter(*Single, StageCraftTags::Param_Patch_InvertPan));
	TestFalse(TEXT("Invert Tilt not offered"), HasParameter(*Single, StageCraftTags::Param_Patch_InvertTilt));
	TestFalse(TEXT("Swap not offered"), HasParameter(*Single, StageCraftTags::Param_Patch_SwapPanTilt));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDMXScheduleParseTest, "StageCraft.DMX.ScheduleParse", StageDMXPatchOptionsTests::Flags)

bool FStageDMXScheduleParseTest::RunTest(const FString& Parameters)
{
	// The patch sheet's own CSV reads back: title and summary skipped, guard stripped, unpatched rows unpatch.
	FStagePatchSheetRow A;
	A.FixtureId = 1;
	A.Label = TEXT("=Spot, left");
	A.FixtureType = TEXT("Spot 300");
	A.Universe = 2;
	A.Address = 17;
	A.Footprint = 16;
	A.bInvertPan = true;
	A.bSwapPanTilt = true;
	FStagePatchSheetRow B;
	B.FixtureId = 2;
	B.Label = TEXT("Par");
	B.Footprint = 7;
	const FString Csv = StagePatchSheet::ToCsv(StagePatchSheet::Build({ A, B }, TEXT("Arena"), FDateTime(2026, 11, 4)));
	FStagePatchScheduleParse Parsed = StagePatchSchedule::Parse(Csv);
	TestTrue(TEXT("export: header found"), Parsed.bHeaderFound);
	TestEqual(TEXT("export: no errors"), Parsed.Errors.Num(), 0);
	if (TestEqual(TEXT("export: two rows"), Parsed.Rows.Num(), 2))
	{
		const FStagePatchScheduleRow& RowA = Parsed.Rows[0];
		TestTrue(TEXT("export: 1 at 2.017"), RowA.FixtureId == 1 && RowA.Patch.Universe == 2 && RowA.Patch.Address == 17);
		TestTrue(TEXT("export: options read"), RowA.bInvertPan.Get(false) && !RowA.bInvertTilt.Get(true) && RowA.bSwapPanTilt.Get(false));
		TestTrue(TEXT("export: unpatched row unpatches"), Parsed.Rows[1].FixtureId == 2 && !Parsed.Rows[1].Patch.IsPatched());
		TestTrue(TEXT("export: lines counted"), RowA.Line > 1);
	}

	// A hand-made sheet: semicolons, quotes, CRLF, columns in another order with an extra one, a BOM, an apostrophe guard, no
	// option columns. The title line holds a comma, which must not decide the separator.
	const FString Hand = FString(TEXT("\xFEFF")) + TEXT("My rig, night 1\r\n\r\nAddress;Notes;\"Fixture ID\";Universe\r\n'5;\"a;b \"\"q\"\"\";10;1\r\n+12;;11;003\r\n\r\n");
	Parsed = StagePatchSchedule::Parse(Hand);
	TestTrue(TEXT("hand: header"), Parsed.bHeaderFound);
	TestEqual(TEXT("hand: no errors"), Parsed.Errors.Num(), 0);
	if (TestEqual(TEXT("hand: two rows"), Parsed.Rows.Num(), 2))
	{
		TestTrue(TEXT("hand: 10 at 1.005"), Parsed.Rows[0].FixtureId == 10 && Parsed.Rows[0].Patch.Universe == 1 && Parsed.Rows[0].Patch.Address == 5);
		TestTrue(TEXT("hand: 11 at 3.012"), Parsed.Rows[1].FixtureId == 11 && Parsed.Rows[1].Patch.Universe == 3 && Parsed.Rows[1].Patch.Address == 12);
		TestFalse(TEXT("hand: no option columns, none set"), Parsed.Rows[0].bInvertPan.IsSet() || Parsed.Rows[0].bSwapPanTilt.IsSet());
		TestEqual(TEXT("hand: line numbers"), Parsed.Rows[0].Line, 4);
	}

	// Refusals of the whole file.
	Parsed = StagePatchSchedule::Parse(TEXT("Name,Universe\nA,1\n"));
	TestTrue(TEXT("no header: nothing read, one reason"), !Parsed.bHeaderFound && Parsed.Rows.IsEmpty() && Parsed.Errors.Num() == 1);
	Parsed = StagePatchSchedule::Parse(TEXT("Fixture ID,Label\n1,A\n"));
	TestTrue(TEXT("no Universe / Address columns refused"), !Parsed.bHeaderFound && Parsed.Rows.IsEmpty() && Parsed.Errors.Num() == 1);
	Parsed = StagePatchSchedule::Parse(FString::ChrN(StagePatchSchedule::MaxFileCharacters + 1, TEXT('x')));
	TestTrue(TEXT("too large refused"), !Parsed.bHeaderFound && Parsed.Errors.Num() == 1 && Parsed.Errors[0].Line == 0);
	Parsed = StagePatchSchedule::Parse(TEXT(""));
	TestTrue(TEXT("empty: no header"), !Parsed.bHeaderFound && Parsed.Rows.IsEmpty());

	// Hostile rows: each reported on its line, the good one still read.
	FString Hostile = TEXT("Fixture ID,Universe,Address,Invert Pan\n");
	Hostile += TEXT("1,1,1,Yes\n");                        // 2: good
	Hostile += TEXT("abc,1,1,\n");                         // 3: not a fixture ID
	Hostile += TEXT("2,99999999999,1,\n");                 // 4: universe too large
	Hostile += TEXT("3,1,513,\n");                         // 5: address past 512
	Hostile += TEXT("4,-1,5,\n");                          // 6: negative
	Hostile += TEXT("5,1,,\n");                            // 7: half a patch
	Hostile += TEXT("6,1,1,Maybe\n");                      // 8: not a flag
	Hostile += TEXT("7,\"1,1,\n");                         // 9: open quote
	Hostile += TEXT("8,1,1,") + FString::ChrN(StagePatchSchedule::MaxFieldCharacters + 1, TEXT('y')) + TEXT("\n"); // 10: long field
	Hostile += TEXT("9,1") + FString(1, TEXT("\x0001")) + TEXT(",1,\n"); // 11: a control character in a number
	Hostile += TEXT("0,1,1,\n");                           // 12: fixture ID 0
	Hostile += TEXT("1.5,1,1,\n");                         // 13: a fraction
	Parsed = StagePatchSchedule::Parse(Hostile);
	if (TestEqual(TEXT("hostile: one good row"), Parsed.Rows.Num(), 1))
	{
		TestTrue(TEXT("hostile: the good row"), Parsed.Rows[0].FixtureId == 1 && Parsed.Rows[0].bInvertPan.Get(false));
	}
	TestEqual(TEXT("hostile: every bad row reported"), Parsed.Errors.Num(), 11);
	TArray<int32> Lines;
	for (const FStagePatchScheduleIssue& Issue : Parsed.Errors)
	{
		Lines.Add(Issue.Line);
		TestFalse(TEXT("hostile: each with a reason"), Issue.Reason.IsEmpty());
	}
	TestEqual(TEXT("hostile: on their lines"), Lines, TArray<int32>{ 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 });

	// A NUL in a row: reported, not read.
	FString WithNul = TEXT("Fixture ID,Universe,Address\n1,1,1\n2,1");
	WithNul.AppendChar(TCHAR(0));
	WithNul += TEXT("9\n");
	Parsed = StagePatchSchedule::Parse(FStringView(*WithNul, WithNul.Len()));
	TestTrue(TEXT("NUL: the row is refused"), Parsed.Rows.Num() == 1 && Parsed.Errors.Num() == 1 && Parsed.Errors[0].Line == 3);

	// More rows than allowed: reading stops with one reason.
	FString Many = TEXT("Fixture ID,Universe,Address\n");
	for (int32 Index = 1; Index <= StagePatchSchedule::MaxRows + 5; ++Index)
	{
		Many += FString::Printf(TEXT("%d,1,1\n"), Index);
	}
	Parsed = StagePatchSchedule::Parse(Many);
	TestEqual(TEXT("too many rows: the first MaxRows read"), Parsed.Rows.Num(), StagePatchSchedule::MaxRows);
	TestEqual(TEXT("too many rows: one reason"), Parsed.Errors.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDMXScheduleImportTest, "StageCraft.DMX.ScheduleImport", StageDMXPatchOptionsTests::Flags)

bool FStageDMXScheduleImportTest::RunTest(const FString& Parameters)
{
	using namespace StageDMXPatchOptionsTests;

	// Pure plan first: every kind of rejection, unchanged rows and conflicts.
	{
		const auto MakeFixture = [](int32 Id, int32 Footprint, int32 Universe, int32 Address)
		{
			FStagePatchScheduleFixture Fixture;
			Fixture.FixtureId = Id;
			Fixture.Footprint = Footprint;
			Fixture.Patch.Universe = Universe;
			Fixture.Patch.Address = Address;
			return Fixture;
		};
		const TArray<FStagePatchScheduleFixture> Fixtures = {
			MakeFixture(1, 16, 1, 1),
			MakeFixture(2, 16, 1, 17),
			MakeFixture(3, 0, 0, 0), // takes no DMX
			MakeFixture(4, 16, 2, 1),
		};
		TArray<FStagePatchScheduleRow> Rows;
		const auto AddRow = [&Rows](int32 Line, int32 Id, int32 Universe, int32 Address)
		{
			FStagePatchScheduleRow& Row = Rows.AddDefaulted_GetRef();
			Row.Line = Line;
			Row.FixtureId = Id;
			Row.Patch.Universe = Universe;
			Row.Patch.Address = Address;
			return &Row;
		};
		AddRow(2, 1, 1, 1);                       // unchanged
		AddRow(3, 2, 1, 5)->bInvertTilt = true;   // moves onto fixture 1's channels: a conflict, allowed; and an option
		AddRow(4, 3, 1, 100);                     // takes no DMX
		AddRow(5, 4, 1, 500);                     // 16 channels do not fit at 500
		AddRow(6, 77, 1, 1);                      // unknown
		AddRow(7, 5, 1, 1);                       // unknown too
		AddRow(8, 6, 1, 1);
		AddRow(9, 6, 1, 2);                       // 6 twice (and unknown): both rows rejected as duplicates
		AddRow(10, 4, 0, 0);                      // ... fixture 4 is also unpatched here: a duplicate of line 5
		const FStagePatchSchedulePlan Plan = StagePatchSchedule::Plan(Rows, Fixtures);
		TestEqual(TEXT("plan: one unchanged"), Plan.Unchanged, 1);
		if (TestEqual(TEXT("plan: one change"), Plan.Changes.Num(), 1))
		{
			TestTrue(TEXT("plan: fixture 2 to 1.005 with tilt inverted"), Plan.Changes[0].FixtureId == 2 && Plan.Changes[0].Patch.Address == 5
				&& Plan.Changes[0].Options.bInvertTilt && Plan.Changes[0].bOptionsFromFile);
		}
		TArray<int32> RejectedLines;
		for (const FStagePatchScheduleIssue& Issue : Plan.Rejected)
		{
			RejectedLines.Add(Issue.Line);
		}
		RejectedLines.Sort();
		TestEqual(TEXT("plan: rejected lines"), RejectedLines, TArray<int32>{ 4, 5, 6, 7, 8, 9, 10 });
		TestEqual(TEXT("plan: the overlap reported"), Plan.Conflicts, TArray<int32>{ 1, 2 });
	}

	// Over real fixtures: assignments carry the fixture and, only when the file set them, the options.
	FScopedWorld Scope;
	UStageDMXPatchSubsystem* Patching = UWorld::GetSubsystem<UStageDMXPatchSubsystem>(Scope.World);
	ULightingFixtureData* Data = NewObject<ULightingFixtureData>(GetTransientPackage());
	ALightingFixtureActor* A = Scope.SpawnFixture(Data);
	ALightingFixtureActor* B = Scope.SpawnFixture(Data);
	if (!TestNotNull(TEXT("patch subsystem"), Patching) || !TestTrue(TEXT("fixtures with IDs"), A->GetFixtureId() > 0 && B->GetFixtureId() > 0))
	{
		return false;
	}
	FStagePatchScheduleImport Import = Patching->PlanSchedule(FString::Printf(
		TEXT("Fixture ID,Universe,Address\n%d,3,1\n%d,3,101\n999,1,1\n"), A->GetFixtureId(), B->GetFixtureId()));
	if (TestEqual(TEXT("import: two assignments"), Import.Assignments.Num(), 2))
	{
		TestTrue(TEXT("import: A to 3.001"), Import.Assignments[0].Fixture == A && Import.Assignments[0].Patch.Universe == 3 && Import.Assignments[0].Patch.Address == 1);
		TestFalse(TEXT("import: no option columns, options kept"), Import.Assignments[0].Options.IsSet());
	}
	TestFalse(TEXT("import: a summary"), Import.Summary.IsEmpty());
	TestTrue(TEXT("import: the unknown fixture explained"), Import.Details.ToString().Contains(TEXT("999")));
	TestTrue(TEXT("import: nothing applied"), A->GetPatch().Universe == 1);

	Import = Patching->PlanSchedule(FString::Printf(TEXT("Fixture ID,Universe,Address,Swap Pan/Tilt\n%d,1,%d,Yes\n"), A->GetFixtureId(), A->GetPatch().Address));
	if (TestEqual(TEXT("options only: one assignment"), Import.Assignments.Num(), 1))
	{
		TestTrue(TEXT("options only: the swap carried"), Import.Assignments[0].Options.IsSet() && Import.Assignments[0].Options->bSwapPanTilt);
	}

	// File refusals.
	FString Text;
	FText Error;
	TestFalse(TEXT("missing file refused"), UStageDMXPatchSubsystem::LoadScheduleFile(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NoSuchSchedule_7f3a.csv")), Text, Error));
	TestFalse(TEXT("with a reason"), Error.IsEmpty());
	const FString Temp = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("StageScheduleTest.csv"));
	if (TestTrue(TEXT("temp file written"), FFileHelper::SaveStringToFile(TEXT("Fixture ID,Universe,Address\n1,1,1\n"), *Temp)))
	{
		TestTrue(TEXT("small file read"), UStageDMXPatchSubsystem::LoadScheduleFile(Temp, Text, Error) && Text.Contains(TEXT("Fixture ID")));
		IFileManager::Get().Delete(*Temp);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
