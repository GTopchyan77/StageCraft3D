// Copyright Epic Games, Inc. All Rights Reserved.

// Export completeness (Docs/ADR/0022-dcc-bridge.md §13): a stage with one of every catalog item, exported to .glb, carries every
// item with geometry and every fixture light, and names each thing it leaves out (hidden on stage, not owned or not in the
// Library, damaged in the file). Runs on the real content, so Blueprint-added components count too.
//  - ExportCompleteness: the synchronous export of a scene built from the whole catalog, read back from the .glb; then the same
//    stage saved with one damaged entry and exported through the bridge's frame-sliced job.
// Needs a renderer for the export itself; with -nullrhi it asserts the refusal only.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Bridge.ExportCompleteness; Quit" -unattended

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/ModularBaseActor.h"
#include "Bridge/StageBridgeSubsystem.h"
#include "Bridge/StageSceneExporter.h"
#include "Data/BaseItemData.h"
#include "Data/LightingFixtureData.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Data/StageParameterTypes.h"
#include "Interaction/StageParameterInterface.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Scene/StageSceneStore.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystems/StageItemSubsystem.h"
#include "Tests/StageTestFixtures.h"

namespace StageExportCompletenessTests
{
	FString Str(EStageBridgeResult Result) { return UEnum::GetValueAsString(Result); }

	/** What a .glb's JSON chunk holds: nodes with a mesh, and nodes carrying a punctual light. */
	struct FGlbContent
	{
		bool bRead = false;
		int32 MeshNodes = 0;
		int32 LightNodes = 0;
	};

	FGlbContent ReadGlb(const FString& Path)
	{
		FGlbContent Content;
		TArray<uint8> Bytes;
		// Header (magic, version, length), then the first chunk: length, type "JSON", data.
		if (!FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent) || Bytes.Num() < 20 || FMemory::Memcmp(Bytes.GetData(), "glTF", 4) != 0
			|| FMemory::Memcmp(Bytes.GetData() + 16, "JSON", 4) != 0)
		{
			return Content;
		}
		uint32 JsonLength = 0;
		FMemory::Memcpy(&JsonLength, Bytes.GetData() + 12, sizeof(JsonLength));
		if (20 + int64(JsonLength) > Bytes.Num())
		{
			return Content;
		}
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + 20), JsonLength);
		const FString Json(Converted.Length(), Converted.Get());
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
		{
			return Content;
		}
		Content.bRead = true;
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (Root->TryGetArrayField(TEXT("nodes"), Nodes))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Nodes)
			{
				const TSharedPtr<FJsonObject> Node = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Node.IsValid())
				{
					continue;
				}
				Content.MeshNodes += Node->HasField(TEXT("mesh")) ? 1 : 0;
				const TSharedPtr<FJsonObject>* Extensions = nullptr;
				Content.LightNodes += Node->TryGetObjectField(TEXT("extensions"), Extensions) && (*Extensions)->HasField(TEXT("KHR_lights_punctual")) ? 1 : 0;
			}
		}
		return Content;
	}

	/** Spawns Item in World as a stage would place it (default state) and captures its snapshot; Configure runs before the capture. */
	bool CaptureItem(UWorld& World, UBaseItemData& Item, const FVector& Location, FStageItemSnapshot& OutSnapshot, TFunctionRef<void(AModularBaseActor&)> Configure)
	{
		UClass* ActorClass = Item.ActorClass.LoadSynchronous();
		AModularBaseActor* Actor = ActorClass ? World.SpawnActorDeferred<AModularBaseActor>(ActorClass, FTransform(Location), nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn) : nullptr;
		if (!Actor)
		{
			return false;
		}
		Actor->InitializeFromItemData(&Item);
		Actor->FinishSpawning(FTransform(Location));
		Configure(*Actor);
		OutSnapshot = Actor->CaptureSnapshot();
		// The spawn system gives placed items their identity; spawned here directly, the item gets one as a placement would.
		OutSnapshot.InstanceId = FGuid::NewGuid();
		Actor->Destroy();
		return true;
	}

	/** Replaces the instance id of the entry Id in a saved scene file with text that is no id, so the parser drops that entry. */
	bool DamageEntry(const FString& FilePath, const FGuid& Id)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *FilePath))
		{
			return false;
		}
		for (const EGuidFormats Format : { EGuidFormats::DigitsWithHyphens, EGuidFormats::Digits, EGuidFormats::DigitsWithHyphensLower, EGuidFormats::DigitsLower,
			EGuidFormats::DigitsWithHyphensInBraces })
		{
			const FString Written = Id.ToString(Format);
			if (Text.Contains(Written, ESearchCase::CaseSensitive))
			{
				Text.ReplaceInline(*Written, TEXT("not-an-id"), ESearchCase::CaseSensitive);
				return FFileHelper::SaveStringToFile(Text, *FilePath);
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageBridgeExportCompletenessTest, "StageCraft.Bridge.ExportCompleteness", StageTestFixtures::Flags)

bool FStageBridgeExportCompletenessTest::RunTest(const FString& Parameters)
{
	using namespace StageTestFixtures;
	using namespace StageExportCompletenessTests;

	FStageTestGameInstance Game;
	if (!TestTrue(TEXT("The test game instance starts"), Game.IsValid()))
	{
		return false;
	}
	UStageItemSubsystem* Items = Game.Subsystem<UStageItemSubsystem>();
	UStageProfileSubsystem* Profile = Game.Subsystem<UStageProfileSubsystem>();
	UStageBridgeSubsystem* Bridge = Game.Subsystem<UStageBridgeSubsystem>();
	if (!TestNotNull(TEXT("Items"), Items) || !TestNotNull(TEXT("Profile"), Profile) || !TestNotNull(TEXT("Bridge"), Bridge)
		|| !TestTrue(TEXT("The catalog has items"), Items->GetCatalog().Num() > 0))
	{
		return false;
	}

	if (!FApp::CanEverRender())
	{
		FStageSceneDocument Empty;
		Empty.Name = TEXT("NoRenderer");
		TestEqual(TEXT("Without a renderer the export is refused"),
			Str(FStageSceneExporter::ExportDocument(Empty, {}, FPaths::Combine(Game.GetRoot(), TEXT("NoRenderer.glb"))).Code), Str(EStageBridgeResult::Unavailable));
		AddInfo(TEXT("No renderer: the export itself was skipped."));
		return true;
	}

	// One of every catalog item in its default state, each in its own 6 m cell, plus one hidden copy and one entry this build
	// does not have. The scene is captured from real spawns, so every item's state is the one a user's stage would save.
	AddInfo(TEXT("A stage with one of every catalog item."));
	TMap<FSoftObjectPath, UBaseItemData*> AllItems;
	FStageSceneDocument Scene = MakeScene(TEXT("Everything"));
	Scene.Items.Reset();
	int32 Fixtures = 0;
	UWorld* World = FStageSceneExporter::CreateExportWorld();
	for (UBaseItemData* Item : Items->GetCatalog())
	{
		if (!Item)
		{
			continue;
		}
		AllItems.Add(FSoftObjectPath(Item), Item);
		FStageItemSnapshot Snapshot;
		if (TestTrue(FString::Printf(TEXT("%s spawns"), *Item->GetName()), CaptureItem(*World, *Item, FVector(600.0 * Scene.Items.Num(), 0.0, 0.0), Snapshot, [](AModularBaseActor&) {})))
		{
			Scene.Items.Add(Snapshot);
			Fixtures += Item->IsA<ULightingFixtureData>() ? 1 : 0;
		}
	}
	const int32 Placed = Scene.Items.Num();
	UBaseItemData* First = Items->GetCatalog()[0];
	FStageItemSnapshot Hidden;
	TestTrue(TEXT("A hidden copy is captured"), CaptureItem(*World, *First, FVector(0.0, 1200.0, 0.0), Hidden, [](AModularBaseActor& Actor)
	{
		IStageParameterInterface::Execute_SetParameterValue(&Actor, StageCraftTags::Param_Info_Visible, FStageParameterValue::MakeBool(false));
	}));
	FStageSceneExporter::DestroyExportWorld(World);
	Scene.Items.Add(Hidden);
	FStageItemSnapshot Unknown = Scene.Items[0];
	Unknown.InstanceId = FGuid::NewGuid();
	Unknown.Item = TSoftObjectPtr<UBaseItemData>(FSoftObjectPath(TEXT("/Game/StageCraft/Items/DA_NotInThisBuild.DA_NotInThisBuild")));
	Scene.Items.Add(Unknown);

	const FString Path = FPaths::Combine(Game.GetRoot(), TEXT("Completeness"), TEXT("Everything.glb"));
	const FStageBridgeResultInfo Result = FStageSceneExporter::ExportDocument(Scene, AllItems, Path);
	TestTrue(TEXT("The export succeeds"), Result.IsSuccess());
	TestEqual(TEXT("Every catalog item is exported"), Result.ExportedItems, Placed);
	TestEqual(TEXT("every one with geometry"), Result.ItemsWithoutGeometry, 0);
	TestEqual(TEXT("The hidden copy is left out and counted"), Result.HiddenItems, 1);
	TestEqual(TEXT("The entry this build does not have is counted"), Result.SkippedItems, 1);
	TestTrue(FString::Printf(TEXT("Every fixture brings its light (%d lights, %d fixtures)"), Result.ExportedLights, Fixtures), Result.ExportedLights >= Fixtures);
	TestTrue(TEXT("The message names what was left out"), Result.Message.ToString().Contains(TEXT("1 hidden on stage")) && Result.Message.ToString().Contains(TEXT("1 not owned")));
	const FGlbContent File = ReadGlb(Path);
	if (TestTrue(TEXT("The file is a readable .glb"), File.bRead))
	{
		TestTrue(FString::Printf(TEXT("It has a mesh node for every item (%d mesh nodes, %d items)"), File.MeshNodes, Placed), File.MeshNodes >= Placed);
		TestEqual(TEXT("and every light the result counts"), File.LightNodes, Result.ExportedLights);
	}
	AddInfo(Result.Message.ToString());

	AddInfo(TEXT("Saved with one damaged entry and sent through the bridge's export job."));
	FStageSceneDocument Saved = Scene;
	Saved.Name = TEXT("Everything Saved");
	FStageItemSnapshot Damaged = Scene.Items[0];
	Damaged.InstanceId = FGuid::NewGuid();
	Damaged.Transform.SetLocation(FVector(0.0, 2400.0, 0.0));
	Saved.Items.Add(Damaged);
	const FStageSceneStore Store(FStageSceneStore::GetDefaultDirectory());
	TestTrue(TEXT("Saved"), Store.Save(Saved) == EStageSceneResult::Success);
	TestTrue(TEXT("One entry damaged on disk"), DamageEntry(FPaths::Combine(FStageSceneStore::GetDefaultDirectory(), Saved.Name + TEXT(".json")), Damaged.InstanceId));

	{
		FStageSceneDocument ReadBack;
		FStageSceneParseReport Report;
		const EStageSceneResult Loaded = Store.Load(Saved.Name, ReadBack, Report);
		TestTrue(TEXT("The damaged file still loads"), Loaded == EStageSceneResult::Success);
		TestEqual(TEXT("without the damaged entry"), ReadBack.Items.Num(), Saved.Items.Num() - 1);
		TestEqual(TEXT("which the parser reports"), Report.InvalidItems, 1);
	}

	const int32 Owned = FStageSceneExporter::CollectExportableItems(*Items, *Profile).Num();
	TOptional<FStageBridgeResultInfo> Job;
	TestTrue(TEXT("The bridge starts the export"), Bridge->BeginExportSceneToFile(Saved.Name, FPaths::Combine(Game.GetRoot(), TEXT("Completeness"), TEXT("Saved.glb")),
		FOnStageBridgeExportFinished::CreateLambda([&Job](const FStageBridgeResultInfo& Done) { Job = Done; })).IsSuccess());
	TestTrue(TEXT("and finishes"), Game.Pump([&Job]() { return Job.IsSet(); }, 60.0));
	if (Job.IsSet())
	{
		TestTrue(TEXT("in success"), Job->IsSuccess());
		TestEqual(TEXT("The damaged entry is named"), Job->InvalidItems, 1);
		TestEqual(TEXT("The hidden copy is left out"), Job->HiddenItems, 1);
		TestEqual(TEXT("Items the profile does not own are left out, with the unknown entry"), Job->SkippedItems, 1 + (Placed - Owned));
		TestEqual(TEXT("Everything owned is exported"), Job->ExportedItems, Owned);
		TestEqual(TEXT("with geometry"), Job->ItemsWithoutGeometry, 0);
		TestTrue(TEXT("The message names the damaged entry"), Job->Message.ToString().Contains(TEXT("1 damaged in the scene file")));
		AddInfo(Job->Message.ToString());
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
