// Copyright Epic Games, Inc. All Rights Reserved.

// Unit tests for scene files (Docs/ADR/0004-selection-scenes-and-rendering.md §4): the format round trip, validation of
// untrusted files, and file operations in a temporary folder. No world.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Scene; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Interaction/StageTransformRules.h"
#include "Data/StageParameterTypes.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Scene/StageSceneStore.h"

namespace StageSceneStoreTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	FStageItemSnapshot MakeItem(const TCHAR* Label, const FTransform& Transform, TArray<uint8> State = {})
	{
		FStageItemSnapshot Item;
		Item.InstanceId = FGuid::NewGuid();
		Item.Item = TSoftObjectPtr<UBaseItemData>(FSoftObjectPath(TEXT("/Game/StageCraft/Items/DA_Test_Crate.DA_Test_Crate")));
		Item.Transform = Transform;
		Item.DisplayName = FText::FromString(Label);
		Item.SavedProperties = MoveTemp(State);
		return Item;
	}

	/** A valid one-item scene whose item fields can be replaced, as JSON text. */
	FString MakeSceneJson(const FString& ItemFields, int32 Version = FStageSceneStore::FormatVersion)
	{
		return FString::Printf(TEXT("{\"format\":\"StageCraftScene\",\"version\":%d,\"name\":\"T\",\"items\":[{%s}]}"), Version, *ItemFields);
	}

	const TCHAR* ValidItemFields = TEXT("\"id\":\"8F1D3B4C-0D3A-4E0B-9C1A-2B3C4D5E6F70\",\"item\":\"/Game/StageCraft/Items/DA_Test_Crate.DA_Test_Crate\","
		"\"location\":[1,2,3],\"rotation\":[0,0,0,1],\"scale\":[1,1,1],\"label\":\"Crate\",\"state\":\"\"");

	FString Str(EStageSceneResult Result) { return UEnum::GetValueAsString(Result); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSceneFormatTest, "StageCraft.Scene.Format", StageSceneStoreTests::Flags)

bool FStageSceneFormatTest::RunTest(const FString& Parameters)
{
	using namespace StageSceneStoreTests;

	FStageSceneDocument Document;
	Document.Name = TEXT("My Show");
	Document.LevelName = TEXT("L_StageTest");
	Document.SavedAtUtc = FDateTime(2026, 10, 8, 12, 30, 0);
	Document.Items.Add(MakeItem(TEXT("Crate"), FTransform(FRotator(10, 45.5, -3).Quaternion(), FVector(-163.25, 37.125, 50), FVector(2, 3, 0.5)), { 1, 2, 3, 250 }));
	Document.Items.Add(MakeItem(TEXT("Spot 101"), FTransform(FVector(0, 0, 400))));

	FStageSceneDocument Parsed;
	FStageSceneParseReport Report;
	TestEqual(TEXT("Round trip parses"), Str(FStageSceneStore::Parse(FStageSceneStore::Serialize(Document), Parsed, Report)), Str(EStageSceneResult::Success));
	TestEqual(TEXT("Nothing dropped"), Report.InvalidItems, 0);
	TestEqual(TEXT("Level kept"), Parsed.LevelName, Document.LevelName);
	TestEqual(TEXT("Save time kept"), Parsed.SavedAtUtc, Document.SavedAtUtc);
	if (TestEqual(TEXT("Both items"), Parsed.Items.Num(), 2))
	{
		const FStageItemSnapshot& In = Document.Items[0];
		const FStageItemSnapshot& Out = Parsed.Items[0];
		TestEqual(TEXT("Instance id kept"), Out.InstanceId, In.InstanceId);
		TestEqual(TEXT("Catalog item kept"), Out.Item.ToSoftObjectPath(), In.Item.ToSoftObjectPath());
		TestTrue(TEXT("Transform kept"), Out.Transform.Equals(In.Transform, 1.e-6));
		TestEqual(TEXT("Instance settings kept byte for byte"), Out.SavedProperties, In.SavedProperties);
		TestEqual(TEXT("Label kept"), Out.DisplayName.ToString(), FString(TEXT("Crate")));
		TestTrue(TEXT("Empty settings stay empty"), Parsed.Items[1].SavedProperties.IsEmpty());
	}

	// Files that are not scenes are refused as a whole and never half-read.
	TestEqual(TEXT("Not JSON"), Str(FStageSceneStore::Parse(TEXT("not json"), Parsed, Report)), Str(EStageSceneResult::Corrupt));
	TestEqual(TEXT("Other format tag"), Str(FStageSceneStore::Parse(TEXT("{\"format\":\"Layout\",\"version\":1,\"items\":[]}"), Parsed, Report)), Str(EStageSceneResult::Corrupt));
	TestEqual(TEXT("Missing items"), Str(FStageSceneStore::Parse(TEXT("{\"format\":\"StageCraftScene\",\"version\":1}"), Parsed, Report)), Str(EStageSceneResult::Corrupt));
	TestEqual(TEXT("Future version"), Str(FStageSceneStore::Parse(MakeSceneJson(ValidItemFields, FStageSceneStore::FormatVersion + 1), Parsed, Report)), Str(EStageSceneResult::VersionMismatch));
	TestEqual(TEXT("Valid minimal scene"), Str(FStageSceneStore::Parse(MakeSceneJson(ValidItemFields), Parsed, Report)), Str(EStageSceneResult::Success));
	TestEqual(TEXT("... with its item"), Parsed.Items.Num(), 1);

	// Damaged items are dropped one by one; the rest of the scene still loads.
	const TCHAR* BadItems[] = {
		TEXT("\"id\":\"not-a-guid\",\"item\":\"/Game/A.A\",\"location\":[0,0,0],\"rotation\":[0,0,0,1],\"scale\":[1,1,1]"),
		TEXT("\"id\":\"00000000-0000-0000-0000-000000000000\",\"item\":\"/Game/A.A\",\"location\":[0,0,0],\"rotation\":[0,0,0,1],\"scale\":[1,1,1]"),
		TEXT("\"id\":\"8F1D3B4C-0D3A-4E0B-9C1A-2B3C4D5E6F71\",\"item\":\"/Script/Engine.Actor\",\"location\":[0,0,0],\"rotation\":[0,0,0,1],\"scale\":[1,1,1]"),
		TEXT("\"id\":\"8F1D3B4C-0D3A-4E0B-9C1A-2B3C4D5E6F72\",\"item\":\"/Game/A.A\",\"location\":[0,0],\"rotation\":[0,0,0,1],\"scale\":[1,1,1]"),
		TEXT("\"id\":\"8F1D3B4C-0D3A-4E0B-9C1A-2B3C4D5E6F73\",\"item\":\"/Game/A.A\",\"location\":[0,0,1e9],\"rotation\":[0,0,0,1],\"scale\":[1,1,1]"),
		TEXT("\"id\":\"8F1D3B4C-0D3A-4E0B-9C1A-2B3C4D5E6F74\",\"item\":\"/Game/A.A\",\"location\":[0,0,0],\"rotation\":[0,0,0,0],\"scale\":[1,1,1]"),
		TEXT("\"id\":\"8F1D3B4C-0D3A-4E0B-9C1A-2B3C4D5E6F75\",\"item\":\"/Game/A.A\",\"location\":[0,0,0],\"rotation\":[0,0,0,1],\"scale\":[1,\"x\",1]"),
		TEXT("\"id\":\"8F1D3B4C-0D3A-4E0B-9C1A-2B3C4D5E6F76\",\"item\":\"/Game/A.A\",\"location\":[0,0,0],\"rotation\":[0,0,0,1],\"scale\":[1,1,1],\"state\":\"%%%not base64\"")
	};
	for (const TCHAR* Bad : BadItems)
	{
		const FString Json = FString::Printf(TEXT("{\"format\":\"StageCraftScene\",\"version\":1,\"items\":[{%s},{%s}]}"), Bad, ValidItemFields);
		TestEqual(FString::Printf(TEXT("Scene with a bad item still loads: %s"), Bad), Str(FStageSceneStore::Parse(Json, Parsed, Report)), Str(EStageSceneResult::Success));
		TestEqual(FString::Printf(TEXT("Bad item dropped: %s"), Bad), Report.InvalidItems, 1);
		TestEqual(FString::Printf(TEXT("Good item kept: %s"), Bad), Parsed.Items.Num(), 1);
	}

	// Oversized per-item state is refused before decoding.
	const FString HugeState = FString::ChrN((FStageSceneStore::MaxStateBytes / 3 + 2) * 4, TEXT('A'));
	TestEqual(TEXT("Oversized state parses"), Str(FStageSceneStore::Parse(MakeSceneJson(FString::Printf(TEXT("%s,\"state\":\"%s\""),
		TEXT("\"id\":\"8F1D3B4C-0D3A-4E0B-9C1A-2B3C4D5E6F77\",\"item\":\"/Game/A.A\",\"location\":[0,0,0],\"rotation\":[0,0,0,1],\"scale\":[1,1,1]"), *HugeState)), Parsed, Report)),
		Str(EStageSceneResult::Success));
	TestEqual(TEXT("... and the item is dropped"), Report.InvalidItems, 1);

	// Scale goes through the shared clamp; a rotation is normalized.
	TestEqual(TEXT("Clamped scene parses"), Str(FStageSceneStore::Parse(MakeSceneJson(
		TEXT("\"id\":\"8F1D3B4C-0D3A-4E0B-9C1A-2B3C4D5E6F78\",\"item\":\"/Game/A.A\",\"location\":[0,0,0],\"rotation\":[0,0,0,2],\"scale\":[500,0,1]")), Parsed, Report)),
		Str(EStageSceneResult::Success));
	if (TestEqual(TEXT("Clamped item kept"), Parsed.Items.Num(), 1))
	{
		TestTrue(TEXT("Scale clamped"), Parsed.Items[0].Transform.GetScale3D().Equals(FVector(StageTransformRules::MaxScale, StageTransformRules::MinScale, 1.0)));
		TestTrue(TEXT("Rotation normalized"), Parsed.Items[0].Transform.GetRotation().IsNormalized());
	}

	// A repeated id gets a fresh one, so two live items never share an identity.
	const FString Duplicate = FString::Printf(TEXT("{\"format\":\"StageCraftScene\",\"version\":1,\"items\":[{%s},{%s}]}"), ValidItemFields, ValidItemFields);
	TestEqual(TEXT("Duplicate ids parse"), Str(FStageSceneStore::Parse(Duplicate, Parsed, Report)), Str(EStageSceneResult::Success));
	TestEqual(TEXT("One id repaired"), Report.RepairedIds, 1);
	if (TestEqual(TEXT("Both items kept"), Parsed.Items.Num(), 2))
	{
		TestNotEqual(TEXT("Ids are unique after parsing"), Parsed.Items[0].InstanceId, Parsed.Items[1].InstanceId);
	}

	// A hostile item count is refused outright.
	FString TooMany = TEXT("{\"format\":\"StageCraftScene\",\"version\":1,\"items\":[");
	for (int32 Index = 0; Index <= FStageSceneStore::MaxItems; ++Index)
	{
		TooMany += Index == 0 ? TEXT("{}") : TEXT(",{}");
	}
	TooMany += TEXT("]}");
	TestEqual(TEXT("Too many items"), Str(FStageSceneStore::Parse(TooMany, Parsed, Report)), Str(EStageSceneResult::Corrupt));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSceneMigrationTest, "StageCraft.Scene.Migration", StageSceneStoreTests::Flags)

/** Format version 2 (Docs/ADR/0010-edit-rules-autosave-scene-v2.md §4): the environment round trip, v1 -> v2 migration, refusal of unknown versions. */
bool FStageSceneMigrationTest::RunTest(const FString& Parameters)
{
	using namespace StageSceneStoreTests;

	const FStageEnvironmentSettings Defaults;
	FStageSceneDocument Parsed;
	FStageSceneParseReport Report;

	// The version every save writes; a stage's environment survives a round trip exactly.
	FStageSceneDocument Document;
	Document.Name = TEXT("Open Air");
	Document.Items.Add(MakeItem(TEXT("Crate"), FTransform(FVector(1, 2, 3))));
	Document.Environment.bSkyEnabled = true;
	Document.Environment.TimeOfDay = 19.5f;
	Document.Environment.SunElevation = 30.f;
	Document.Environment.SunAzimuth = 120.f;
	Document.Environment.CloudCoverage = 0.8f;
	Document.Environment.Rain = 0.4f;
	Document.Environment.Snow = 0.f;
	Document.Environment.Fog = 0.6f;
	const FString Saved = FStageSceneStore::Serialize(Document);
	TestTrue(TEXT("Saves write the current version"), Saved.Contains(FString::Printf(TEXT("\"version\": %d"), FStageSceneStore::FormatVersion)));
	TestEqual(TEXT("The current version parses"), Str(FStageSceneStore::Parse(Saved, Parsed, Report)), Str(EStageSceneResult::Success));
	TestTrue(TEXT("Environment round-trips"), Parsed.Environment.Equals(Document.Environment));
	TestEqual(TEXT("Items still round-trip"), Parsed.Items.Num(), 1);

	// Migration 1 -> 2: every version 1 file still opens, items intact, with the default environment (the indoor stage).
	TestEqual(TEXT("Version 1 parses"), Str(FStageSceneStore::Parse(MakeSceneJson(ValidItemFields, 1), Parsed, Report)), Str(EStageSceneResult::Success));
	TestEqual(TEXT("... with its item"), Parsed.Items.Num(), 1);
	TestTrue(TEXT("... and the default environment"), Parsed.Environment.Equals(Defaults));
	TestFalse(TEXT("... sky off"), Parsed.Environment.bSkyEnabled);

	// A version 1 file never had an environment; one found in it is not trusted to mean anything.
	const FString V1WithEnvironment = TEXT("{\"format\":\"StageCraftScene\",\"version\":1,\"items\":[],\"environment\":{\"skyEnabled\":true}}");
	TestEqual(TEXT("Version 1 with a stray environment parses"), Str(FStageSceneStore::Parse(V1WithEnvironment, Parsed, Report)), Str(EStageSceneResult::Success));
	TestFalse(TEXT("... and ignores it"), Parsed.Environment.bSkyEnabled);

	// Versions outside [MinReadableVersion, FormatVersion] are refused whole, never half-read.
	TestEqual(TEXT("Version 0"), Str(FStageSceneStore::Parse(MakeSceneJson(ValidItemFields, FStageSceneStore::MinReadableVersion - 1), Parsed, Report)), Str(EStageSceneResult::VersionMismatch));
	TestEqual(TEXT("The next version"), Str(FStageSceneStore::Parse(MakeSceneJson(ValidItemFields, FStageSceneStore::FormatVersion + 1), Parsed, Report)), Str(EStageSceneResult::VersionMismatch));

	// A version 2 file is untrusted input: missing values keep their defaults, out-of-range ones are wrapped or clamped.
	const FString NoEnvironment = TEXT("{\"format\":\"StageCraftScene\",\"version\":2,\"items\":[]}");
	TestEqual(TEXT("Version 2 without environment parses"), Str(FStageSceneStore::Parse(NoEnvironment, Parsed, Report)), Str(EStageSceneResult::Success));
	TestTrue(TEXT("... default environment"), Parsed.Environment.Equals(Defaults));

	const FString Hostile = TEXT("{\"format\":\"StageCraftScene\",\"version\":2,\"items\":[],\"environment\":"
		"{\"skyEnabled\":true,\"timeOfDay\":25,\"sunElevation\":200,\"sunAzimuth\":-90,\"rain\":7,\"fog\":-1,\"cloudCoverage\":\"heavy\"}}");
	TestEqual(TEXT("Out-of-range environment parses"), Str(FStageSceneStore::Parse(Hostile, Parsed, Report)), Str(EStageSceneResult::Success));
	TestEqual(TEXT("Time wraps (25 h = 01:00)"), Parsed.Environment.TimeOfDay, 1.f, 1.e-4f);
	TestEqual(TEXT("Sun height clamped"), Parsed.Environment.SunElevation, StageEnvironment::MaxSunElevation);
	TestEqual(TEXT("Direction wraps"), Parsed.Environment.SunAzimuth, 270.f, 1.e-4f);
	TestEqual(TEXT("Rain clamped"), Parsed.Environment.Rain, 1.f);
	TestEqual(TEXT("Fog clamped"), Parsed.Environment.Fog, 0.f);
	TestEqual(TEXT("A value of the wrong type keeps its default"), Parsed.Environment.CloudCoverage, Defaults.CloudCoverage);
	TestEqual(TEXT("A missing value keeps its default"), Parsed.Environment.Snow, Defaults.Snow);
	TestTrue(TEXT("The sky switch is read"), Parsed.Environment.bSkyEnabled);

	// On disk: a version 1 file loads, and saving it again upgrades it to the current version.
	const FString Directory = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("StageCraftScenes"), FGuid::NewGuid().ToString());
	const FStageSceneStore Store(Directory);
	IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
	FFileHelper::SaveStringToFile(MakeSceneJson(ValidItemFields, 1), *Store.GetScenePath(TEXT("Old Show")));
	FStageSceneDocument Loaded;
	TestEqual(TEXT("Version 1 file loads"), Str(Store.Load(TEXT("Old Show"), Loaded, Report)), Str(EStageSceneResult::Success));
	TestEqual(TEXT("Re-saved"), Str(Store.Save(Loaded)), Str(EStageSceneResult::Success));
	FString Upgraded;
	FFileHelper::LoadFileToString(Upgraded, *Store.GetScenePath(TEXT("Old Show")));
	TestTrue(TEXT("The re-saved file is the current version"), Upgraded.Contains(FString::Printf(TEXT("\"version\": %d"), FStageSceneStore::FormatVersion)));
	TestEqual(TEXT("... and loads"), Str(Store.Load(TEXT("Old Show"), Loaded, Report)), Str(EStageSceneResult::Success));
	TestEqual(TEXT("... with its item"), Loaded.Items.Num(), 1);
	IFileManager::Get().DeleteDirectory(*Directory, /*RequireExists*/ false, /*Tree*/ true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSceneCuesTest, "StageCraft.Scene.Cues", StageSceneStoreTests::Flags)

/** Format version 3 (Docs/ADR/0011-cue-list-and-patch-sheet-export.md §2.5): the cue round trip, v2 -> v3 migration, damaged cues. */
bool FStageSceneCuesTest::RunTest(const FString& Parameters)
{
	using namespace StageSceneStoreTests;

	FStageSceneDocument Parsed;
	FStageSceneParseReport Report;

	// A cue list survives a round trip exactly: numbers, labels, fades, fixtures and attribute values.
	FStageSceneDocument Document;
	Document.Name = TEXT("Show");
	FShowCue& First = Document.Cues.AddDefaulted_GetRef();
	First.Number = 1.f;
	First.Label = FText::FromString(TEXT("Walk-in, \"house\" lights"));
	First.FadeTime = 3.5f;
	FShowCueFixtureState& State = First.Fixtures.AddDefaulted_GetRef();
	State.FixtureId = 101;
	State.Attributes.Add(StageCraftTags::Attribute_Dimmer, 0.75f);
	State.Attributes.Add(StageCraftTags::Attribute_Pan, -0.25f);
	FShowCue& Second = Document.Cues.AddDefaulted_GetRef();
	Second.Number = 2.5f;
	Second.FadeTime = 0.f;

	const FString Saved = FStageSceneStore::Serialize(Document);
	TestEqual(TEXT("Version 3 parses"), Str(FStageSceneStore::Parse(Saved, Parsed, Report)), Str(EStageSceneResult::Success));
	if (TestEqual(TEXT("Both cues"), Parsed.Cues.Num(), 2))
	{
		TestEqual(TEXT("Number"), Parsed.Cues[0].Number, 1.f);
		TestEqual(TEXT("Label"), Parsed.Cues[0].Label.ToString(), First.Label.ToString());
		TestEqual(TEXT("Fade"), Parsed.Cues[0].FadeTime, 3.5f);
		TestEqual(TEXT("Decimal number"), Parsed.Cues[1].Number, 2.5f);
		if (TestEqual(TEXT("Fixture"), Parsed.Cues[0].Fixtures.Num(), 1))
		{
			TestEqual(TEXT("Fixture id"), Parsed.Cues[0].Fixtures[0].FixtureId, 101);
			const float* Dimmer = Parsed.Cues[0].Fixtures[0].Attributes.Find(StageCraftTags::Attribute_Dimmer);
			TestTrue(TEXT("Dimmer value"), Dimmer && FMath::IsNearlyEqual(*Dimmer, 0.75f));
			TestEqual(TEXT("Both attributes"), Parsed.Cues[0].Fixtures[0].Attributes.Num(), 2);
		}
	}
	TestEqual(TEXT("Nothing dropped"), Report.InvalidCueEntries, 0);

	// Migration 2 -> 3: a version 2 file has no cues and opens with an empty list; a stray "cues" in it is ignored.
	const FString V2 = TEXT("{\"format\":\"StageCraftScene\",\"version\":2,\"items\":[],\"cues\":[{\"number\":1,\"fade\":1,\"fixtures\":[]}]}");
	TestEqual(TEXT("Version 2 parses"), Str(FStageSceneStore::Parse(V2, Parsed, Report)), Str(EStageSceneResult::Success));
	TestEqual(TEXT("... with no cues"), Parsed.Cues.Num(), 0);
	TestEqual(TEXT("Version 1 parses with no cues"), Str(FStageSceneStore::Parse(MakeSceneJson(ValidItemFields, 1), Parsed, Report)), Str(EStageSceneResult::Success));
	TestEqual(TEXT("... no cues"), Parsed.Cues.Num(), 0);

	// A damaged cue list loads what it can: bad cues, fixtures and attributes are dropped and counted, duplicates repaired.
	const FString Damaged = TEXT("{\"format\":\"StageCraftScene\",\"version\":3,\"items\":[],\"cues\":["
		"{\"number\":2,\"label\":\"B\",\"fade\":9999,\"fixtures\":[{\"id\":7,\"attributes\":{\"StageCraft.Attribute.Dimmer\":0.5,\"StageCraft.Attribute.NoSuchThing\":1,\"StageCraft.Attribute.Pan\":\"left\"}},{\"id\":-3,\"attributes\":{}},\"junk\"]},"
		"{\"number\":0,\"fade\":1},"
		"{\"number\":\"one\"},"
		"{\"number\":1,\"label\":\"A\"},"
		"{\"number\":2.0001,\"label\":\"duplicate\"},"
		"17]}");
	TestEqual(TEXT("A damaged cue list parses"), Str(FStageSceneStore::Parse(Damaged, Parsed, Report)), Str(EStageSceneResult::Success));
	if (TestEqual(TEXT("Two good cues"), Parsed.Cues.Num(), 2))
	{
		TestEqual(TEXT("Sorted"), Parsed.Cues[0].Label.ToString(), FString(TEXT("A")));
		TestEqual(TEXT("The first of a duplicate number wins"), Parsed.Cues[1].Label.ToString(), FString(TEXT("B")));
		TestEqual(TEXT("A huge fade is clamped"), Parsed.Cues[1].FadeTime, StageCueRules::MaxFadeTime);
		TestEqual(TEXT("A missing fade is 0"), Parsed.Cues[0].FadeTime, 0.f);
		if (TestEqual(TEXT("Only the good fixture"), Parsed.Cues[1].Fixtures.Num(), 1))
		{
			TestEqual(TEXT("... with only its good attribute"), Parsed.Cues[1].Fixtures[0].Attributes.Num(), 1);
		}
	}
	// Unknown tag + wrong-typed value + bad fixture id + junk fixture + cue 0 + text number + duplicate + junk cue.
	TestEqual(TEXT("Every dropped entry is counted"), Report.InvalidCueEntries, 8);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSceneFilesTest, "StageCraft.Scene.Files", StageSceneStoreTests::Flags)

bool FStageSceneFilesTest::RunTest(const FString& Parameters)
{
	using namespace StageSceneStoreTests;

	const FString Directory = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("StageCraftScenes"), FGuid::NewGuid().ToString());
	const FStageSceneStore Store(Directory);

	TestTrue(TEXT("Plain names are valid"), FStageSceneStore::IsValidSceneName(TEXT("Main Show (v2)")));
	TestFalse(TEXT("Path separators are not"), FStageSceneStore::IsValidSceneName(TEXT("../evil")));
	TestFalse(TEXT("Device names are not"), FStageSceneStore::IsValidSceneName(TEXT("CON")));
	TestFalse(TEXT("Empty is not"), FStageSceneStore::IsValidSceneName(TEXT("")));

	TestTrue(TEXT("No scenes yet"), Store.ListScenes().IsEmpty());

	FStageSceneDocument Document;
	Document.Name = TEXT("Main Show");
	Document.Items.Add(MakeItem(TEXT("Crate"), FTransform(FVector(10, 20, 30))));
	TestEqual(TEXT("Save"), Str(Store.Save(Document)), Str(EStageSceneResult::Success));
	TestTrue(TEXT("Exists"), Store.SceneExists(TEXT("Main Show")));
	TestFalse(TEXT("No temporary file left"), IFileManager::Get().FileExists(*(Store.GetScenePath(TEXT("Main Show")) + TEXT(".tmp"))));

	Document.Name = TEXT("a scene");
	Store.Save(Document);
	// A file a save would never write is not offered.
	FFileHelper::SaveStringToFile(TEXT("{}"), *FPaths::Combine(Directory, TEXT("bad.name!.json")));
	TestEqual(TEXT("Listed sorted, invalid names ignored"), FString::Join(Store.ListScenes(), TEXT("|")), FString(TEXT("a scene|Main Show")));

	FStageSceneDocument Loaded;
	FStageSceneParseReport Report;
	TestEqual(TEXT("Load"), Str(Store.Load(TEXT("Main Show"), Loaded, Report)), Str(EStageSceneResult::Success));
	TestEqual(TEXT("The file name is the scene name"), Loaded.Name, FString(TEXT("Main Show")));
	TestEqual(TEXT("Loaded item"), Loaded.Items.Num(), 1);

	TestEqual(TEXT("Missing scene"), Str(Store.Load(TEXT("Nope"), Loaded, Report)), Str(EStageSceneResult::NotFound));
	TestEqual(TEXT("Invalid name on save"), Str(Store.Save(FStageSceneDocument{ TEXT("../x") })), Str(EStageSceneResult::InvalidName));

	// A damaged file is reported and left exactly as it is: it is the user's data.
	const FString DamagedPath = Store.GetScenePath(TEXT("Damaged"));
	FFileHelper::SaveStringToFile(TEXT("{ broken"), *DamagedPath);
	TestEqual(TEXT("Damaged file is Corrupt"), Str(Store.Load(TEXT("Damaged"), Loaded, Report)), Str(EStageSceneResult::Corrupt));
	TestTrue(TEXT("Damaged file is kept"), IFileManager::Get().FileExists(*DamagedPath));

	TestEqual(TEXT("Delete"), Str(Store.Delete(TEXT("Main Show"))), Str(EStageSceneResult::Success));
	TestFalse(TEXT("Deleted"), Store.SceneExists(TEXT("Main Show")));
	TestEqual(TEXT("Delete missing"), Str(Store.Delete(TEXT("Main Show"))), Str(EStageSceneResult::NotFound));

	IFileManager::Get().DeleteDirectory(*Directory, /*RequireExists*/ false, /*Tree*/ true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
