// Copyright Epic Games, Inc. All Rights Reserved.

// The shipped starter stages (Docs/ADR/0024-startup-atmospheres-starter-stages.md §6), checked against the live catalog on a
// test game instance: every file parses through the scene parser with no damaged entry, names only catalog items that every
// user owns (no entitlement), and sets an environment in range: the venue's own lighting or one of the shipped atmospheres. Opening one as an untitled stage
// with fresh ids is exercised by the scripted -game run (STATE.md #58); this harness has no stage world to apply it to.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Scene.Templates; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/BaseItemData.h"
#include "Environment/StageAtmospherePreset.h"
#include "Environment/StageAtmosphereSubsystem.h"
#include "Misc/AutomationTest.h"
#include "Scene/StageSceneComponent.h"
#include "Scene/StageSceneStore.h"
#include "Subsystems/StageItemSubsystem.h"
#include "Tests/StageTestFixtures.h"
#include "Tests/StageTestGameInstance.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSceneTemplatesTest, "StageCraft.Scene.Templates", StageTestFixtures::Flags)

bool FStageSceneTemplatesTest::RunTest(const FString& Parameters)
{
	using namespace StageTestFixtures;

	FStageTestGameInstance Game;
	if (!TestTrue(TEXT("The test game instance starts"), Game.IsValid()))
	{
		return false;
	}
	const UStageItemSubsystem* Items = Game.Subsystem<UStageItemSubsystem>();
	const UStageAtmosphereSubsystem* Atmospheres = Game.Subsystem<UStageAtmosphereSubsystem>();
	if (!TestNotNull(TEXT("Item catalog"), Items) || !TestNotNull(TEXT("Atmospheres"), Atmospheres)
		|| !TestTrue(TEXT("Catalogs load"), Game.Pump([Items, Atmospheres]() { return Items->IsCatalogLoaded() && Atmospheres->IsLoaded(); }, 20.0)))
	{
		return false;
	}
	TMap<FSoftObjectPath, const UBaseItemData*> Catalog;
	for (const UBaseItemData* Item : Items->GetCatalog())
	{
		Catalog.Add(FSoftObjectPath(Item), Item);
	}

	// The real folder: these are the files that ship (read only, nothing is written).
	const FStageSceneStore Templates(UStageSceneComponent::GetTemplatesDirectory());
	const TArray<FString> Names = Templates.ListScenes();
	TestEqual(TEXT("Four starter stages ship"), Names.Num(), 4);
	TestTrue(TEXT("Including an empty one"), Names.Contains(TEXT("Empty Stage")));

	for (const FString& Name : Names)
	{
		FStageSceneDocument Document;
		FStageSceneParseReport Report;
		if (!TestTrue(*FString::Printf(TEXT("%s parses"), *Name), Templates.Load(Name, Document, Report) == EStageSceneResult::Success))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("%s has no damaged entry"), *Name), Report.InvalidItems, 0);
		TestEqual(*FString::Printf(TEXT("%s has no damaged cue"), *Name), Report.InvalidCueEntries, 0);
		TestFalse(*FString::Printf(TEXT("%s comes from no marketplace listing"), *Name), Document.OriginListingId.IsValid());
		TestTrue(*FString::Printf(TEXT("%s has an environment in range"), *Name), StageEnvironment::Sanitize(Document.Environment).Equals(Document.Environment));
		// The venue's own lighting (the theatre) is no preset; everything else is one of the shipped atmospheres.
		if (Document.Environment.Lighting != EStageLightingSource::Level)
		{
			TestNotNull(*FString::Printf(TEXT("%s sets a shipped atmosphere"), *Name), Atmospheres->FindMatchingAtmosphere(Document.Environment));
		}

		TSet<FGuid> Ids;
		for (const FStageItemSnapshot& Snapshot : Document.Items)
		{
			const UBaseItemData* const* Item = Catalog.Find(Snapshot.Item.ToSoftObjectPath());
			if (!TestNotNull(*FString::Printf(TEXT("%s: %s is in the catalog"), *Name, *Snapshot.Item.ToString()), Item))
			{
				continue;
			}
			TestFalse(*FString::Printf(TEXT("%s: %s is free for everyone"), *Name, *(*Item)->GetName()), (*Item)->RequiredEntitlement.IsValid());
			bool bDuplicate = false;
			Ids.Add(Snapshot.InstanceId, &bDuplicate);
			TestFalse(*FString::Printf(TEXT("%s: item ids are unique"), *Name), bDuplicate);
		}
		if (Name != TEXT("Empty Stage"))
		{
			TestTrue(*FString::Printf(TEXT("%s has a stage on it"), *Name), Document.Items.Num() >= 5);
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
