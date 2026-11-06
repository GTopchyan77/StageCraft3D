// Copyright Epic Games, Inc. All Rights Reserved.

// Integration test of the DCC bridge subsystem over real loopback sockets (Docs/ADR/0022-dcc-bridge.md §5, §6): the live
// UStageBridgeSubsystem of an isolated game instance (FStageTestGameInstance: its own port, session file and outbox, never the
// user's), with test clients standing in for the Blender, Maya, 3ds Max and Cinema 4D add-ons.
//  - The session file names the port and token; a wrong token is refused; each app is listed with what it imports.
//  - The dispatch rules see the live clients: Maya waits for USD, the others take glTF.
//  - A send reaches only its app, with a path inside the outbox and a SHA-1 that matches the file; acks ok and with an error,
//    a client that leaves mid-send, and an app that never answers (send timeout). Needs a renderer to export; without one the
//    send is refused as Unavailable (asserted), and the rest still runs.
//  - An oversized line closes the connection; stopping the bridge removes the session file and every client.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Bridge; Quit" -unattended

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Algo/Find.h"
#include "Bridge/StageBridgeProtocol.h"
#include "Bridge/StageBridgeSubsystem.h"
#include "Bridge/StageSceneExportJob.h"
#include "Bridge/StageSceneExporter.h"
#include "Bridge/Tests/StageBridgeTestClient.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Subsystems/StageItemSubsystem.h"
#include "HAL/FileManager.h"
#include "Marketplace/StageAcquireDispatchSubsystem.h"
#include "Marketplace/StageDispatchRules.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Scene/StageSceneStore.h"
#include "Tests/StageTestEventRecorder.h"
#include "Tests/StageTestFixtures.h"
#include "UObject/StrongObjectPtr.h"

namespace StageBridgeLoopbackTests
{
	FString Str(EStageBridgeResult Result) { return UEnum::GetValueAsString(Result); }
	FString Str(EStageDispatchAvailability Availability) { return UEnum::GetValueAsString(Availability); }

	struct FAppCase
	{
		const TCHAR* Key;
		EStageBridgeApp App;
		bool bImportsGlb;
	};
	const FAppCase Apps[] = {
		{ TEXT("blender"), EStageBridgeApp::Blender, true },
		{ TEXT("maya"), EStageBridgeApp::Maya, false },
		{ TEXT("max"), EStageBridgeApp::Max, true },
		{ TEXT("cinema4d"), EStageBridgeApp::Cinema4D, true },
	};

	FString Sha1OfFile(const FString& Path)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent))
		{
			return FString();
		}
		FSHAHash Hash;
		FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Hash.Hash);
		return Hash.ToString().ToLower();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageBridgeLoopbackAppsTest, "StageCraft.Bridge.LoopbackApps", StageTestFixtures::Flags)

bool FStageBridgeLoopbackAppsTest::RunTest(const FString& Parameters)
{
	using namespace StageTestFixtures;
	using namespace StageBridgeLoopbackTests;

	// The intruder below is refused on purpose, and that is logged as a warning.
	AddExpectedMessage(TEXT("presented a wrong token"), EAutomationExpectedMessageFlags::Contains, 0);

	FStageTestGameInstanceOptions Options;
	Options.SendTimeoutSeconds = 0.5f;
	FStageTestGameInstance Game(Options);
	if (!TestTrue(TEXT("The test game instance starts"), Game.IsValid()))
	{
		return false;
	}
	UStageBridgeSubsystem* Bridge = Game.Subsystem<UStageBridgeSubsystem>();
	UStageAcquireDispatchSubsystem* Dispatch = Game.Subsystem<UStageAcquireDispatchSubsystem>();
	if (!TestNotNull(TEXT("Bridge"), Bridge) || !TestNotNull(TEXT("Dispatch"), Dispatch))
	{
		return false;
	}
	TStrongObjectPtr<UStageTestEventRecorder> Recorder(NewObject<UStageTestEventRecorder>());
	Recorder->Bind(Game.Get());
	ON_SCOPE_EXIT
	{
		Recorder->Unbind();
	};
	const auto Pump = [&Game]() { Game.PumpFor(0.01); };

	FText Error;
	if (!TestTrue(TEXT("The bridge starts on a free port"), Bridge->StartBridgeOnAnyPortForTests(Error)))
	{
		AddError(Error.ToString());
		return false;
	}
	const uint16 Port = static_cast<uint16>(Bridge->GetPort());
	TestTrue(TEXT("It listens"), Port > 0);
	TestTrue(TEXT("Its session file is in the test's folder, not the user's"), UStageBridgeSubsystem::GetSessionFilePath().StartsWith(Game.GetRoot()));

	FString SessionText;
	TSharedPtr<FJsonObject> Session;
	TestTrue(TEXT("The session file is readable JSON"), FFileHelper::LoadFileToString(SessionText, *UStageBridgeSubsystem::GetSessionFilePath())
		&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SessionText), Session) && Session.IsValid());
	FString Token;
	int32 SessionPort = 0;
	if (!TestTrue(TEXT("with the token and port"), Session.IsValid() && Session->TryGetStringField(TEXT("token"), Token) && Session->TryGetNumberField(TEXT("port"), SessionPort)))
	{
		return false;
	}
	TestEqual(TEXT("The port is the one listening"), SessionPort, static_cast<int32>(Port));

	AddInfo(TEXT("A wrong token is refused and never listed."));
	{
		FStageBridgeTestClient Intruder;
		TestTrue(TEXT("Connects"), Intruder.Connect(Port));
		Intruder.SendHello(TEXT("00"), TEXT("blender"), TEXT("4.3"));
		TestTrue(TEXT("Told so"), Intruder.WaitFor(TEXT("wrong token")));
		TestTrue(TEXT("and closed"), Intruder.WaitClosed());
	}

	AddInfo(TEXT("Each app's add-on connects with the session token and is listed with what it imports."));
	FStageBridgeTestClient Clients[UE_ARRAY_COUNT(Apps)];
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Apps); ++Index)
	{
		TestTrue(*FString::Printf(TEXT("%s connects"), Apps[Index].Key), Clients[Index].Connect(Port));
		Clients[Index].SendHello(Token, Apps[Index].Key, TEXT("1.0"));
		TestTrue(*FString::Printf(TEXT("%s is welcomed"), Apps[Index].Key), Clients[Index].TakeMessage(TEXT("welcome"), Pump).IsValid());
	}
	TestTrue(TEXT("All four are listed"), Game.Pump([Bridge]() { return Bridge->GetClients().Num() == UE_ARRAY_COUNT(Apps); }));
	TMap<EStageBridgeApp, int32> ClientIds;
	for (const FStageBridgeClientInfo& Client : Bridge->GetClients())
	{
		ClientIds.Add(Client.App, Client.ClientId);
		const FAppCase* Expected = Algo::FindByPredicate(Apps, [&Client](const FAppCase& App) { return App.App == Client.App; });
		TestTrue(*FString::Printf(TEXT("Client %d is a known app with the right import format"), Client.ClientId), Expected && Expected->bImportsGlb == Client.bImportsGlb);
	}

	AddInfo(TEXT("The dispatch rules see the live clients."));
	const FGuid Listing = PublishListing(TEXT("Bridge Rig"), 0, /*bAllowExport*/ true);
	TestTrue(TEXT("The query answers"), QueryAll(Game));
	for (const FAppCase& App : Apps)
	{
		FStageDispatchTarget Target;
		Target.Kind = EStageDispatchKind::BridgeApp;
		Target.BridgeClientId = ClientIds.FindRef(App.App);
		TestEqual(*FString::Printf(TEXT("%s as a target"), App.Key), Str(Dispatch->EvaluateTarget(Listing, Target)),
			Str(App.bImportsGlb ? EStageDispatchAvailability::Available : EStageDispatchAvailability::NeedsUsd));
	}

	AddInfo(TEXT("Sends: only the target app hears of it, the file matches its SHA-1, and every way a send can end."));
	const FString SceneName = TEXT("Loopback Stage");
	TestEqual(TEXT("A scene of the user's own is saved"), UEnum::GetValueAsString(FStageSceneStore(FStageSceneStore::GetDefaultDirectory()).Save(MakeScene(SceneName))),
		UEnum::GetValueAsString(EStageSceneResult::Success));
	const int32 Blender = ClientIds.FindRef(EStageBridgeApp::Blender);
	FStageBridgeTestClient& BlenderClient = Clients[0];
	FStageBridgeTestClient& MaxClient = Clients[2];

	if (!FApp::CanEverRender())
	{
		const FStageBridgeResultInfo Refused = Bridge->SendSceneToApp(SceneName, Blender);
		TestEqual(TEXT("Without a renderer a send cannot export"), Str(Refused.Code), Str(EStageBridgeResult::Unavailable));
		AddInfo(TEXT("No renderer: the send, ack, disconnect and timeout steps need one and were skipped."));
	}
	else
	{
		TestTrue(TEXT("Sent to Blender"), Bridge->SendSceneToApp(SceneName, Blender).IsSuccess());
		TestEqual(TEXT("One send at a time"), Str(Bridge->SendSceneToApp(SceneName, Blender).Code), Str(EStageBridgeResult::Busy));
		const TSharedPtr<FJsonObject> Asset = BlenderClient.TakeMessage(TEXT("asset"), Pump);
		if (TestTrue(TEXT("Blender is told about the asset"), Asset.IsValid()))
		{
			const FString Path = Asset->GetStringField(TEXT("path"));
			TestTrue(TEXT("The file is inside the outbox"), FPaths::IsUnderDirectory(Path, UStageBridgeSubsystem::GetOutboxDirectory()));
			TestEqual(TEXT("The SHA-1 matches the file"), Asset->GetStringField(TEXT("sha1")).ToLower(), Sha1OfFile(Path));
			TestEqual(TEXT("The size matches"), static_cast<int64>(Asset->GetNumberField(TEXT("bytes"))), IFileManager::Get().FileSize(*Path));
			TestEqual(TEXT("It is binary glTF"), Asset->GetStringField(TEXT("format")), FString(TEXT("glb")));
			TestFalse(TEXT("3ds Max is not told about it"), MaxClient.TakeMessage(TEXT("asset"), Pump, 0.3).IsValid());
			BlenderClient.SendAck(Asset->GetStringField(TEXT("id")), true);
		}
		TestTrue(TEXT("The ack ends the send"), Game.Pump([&Recorder]() { return Recorder->Sends.Num() == 1; }));
		TestTrue(TEXT("in success"), Recorder->Sends.Num() == 1 && Recorder->Sends[0].IsSuccess());

		TestTrue(TEXT("Sent again"), Bridge->SendSceneToApp(SceneName, Blender).IsSuccess());
		const TSharedPtr<FJsonObject> Second = BlenderClient.TakeMessage(TEXT("asset"), Pump);
		if (Second.IsValid())
		{
			BlenderClient.SendAck(Second->GetStringField(TEXT("id")), false, TEXT("Import failed in the add-on"));
		}
		TestTrue(TEXT("A refusing ack ends the send"), Game.Pump([&Recorder]() { return Recorder->Sends.Num() == 2; }));
		TestTrue(TEXT("as AppFailed with the add-on's reason"), Recorder->Sends.Num() == 2 && Recorder->Sends[1].Code == EStageBridgeResult::AppFailed
			&& Recorder->Sends[1].Message.ToString().Contains(TEXT("Import failed in the add-on")));

		const int32 Max = ClientIds.FindRef(EStageBridgeApp::Max);
		TestTrue(TEXT("Sent to 3ds Max"), Bridge->SendSceneToApp(SceneName, Max).IsSuccess());
		TestTrue(TEXT("3ds Max is told"), MaxClient.TakeMessage(TEXT("asset"), Pump).IsValid());
		MaxClient.Close();
		TestTrue(TEXT("Its leaving ends the send"), Game.Pump([&Recorder]() { return Recorder->Sends.Num() == 3; }));
		TestTrue(TEXT("as AppFailed"), Recorder->Sends.Num() == 3 && Recorder->Sends[2].Code == EStageBridgeResult::AppFailed);

		TestTrue(TEXT("Sent to Blender, which never answers"), Bridge->SendSceneToApp(SceneName, Blender).IsSuccess());
		TestTrue(TEXT("The send times out"), Game.Pump([&Recorder]() { return Recorder->Sends.Num() == 4; }, 3.0));
		TestTrue(TEXT("as AppFailed"), Recorder->Sends.Num() == 4 && Recorder->Sends[3].Code == EStageBridgeResult::AppFailed);
		TestFalse(TEXT("The bridge is free again"), Bridge->IsSending());
	}

	AddInfo(TEXT("A line over the limit closes that connection only."));
	const int32 Before = Bridge->GetClients().Num();
	FStageBridgeTestClient& Cinema = Clients[3];
	Cinema.Send(FString::ChrN(StageBridgeProtocol::MaxLineBytes + 16, TEXT('x')));
	TestTrue(TEXT("Closed"), Cinema.WaitClosed());
	TestTrue(TEXT("and no longer listed"), Game.Pump([Bridge, Before]() { return Bridge->GetClients().Num() == Before - 1; }));

	Bridge->StopBridge();
	TestFalse(TEXT("Stopped"), Bridge->IsRunning());
	TestTrue(TEXT("No clients"), Bridge->GetClients().IsEmpty());
	TestFalse(TEXT("The session file is removed"), IFileManager::Get().FileExists(*UStageBridgeSubsystem::GetSessionFilePath()));
	return true;
}

// The export job (ADR 0022 §12): the same export as FStageSceneExporter, spread over frames; refusals before any work; a cancel
// that leaves no world, no callback and no file; one export at a time through the bridge.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageBridgeExportJobTest, "StageCraft.Bridge.ExportJob", StageTestFixtures::Flags)

bool FStageBridgeExportJobTest::RunTest(const FString& Parameters)
{
	using namespace StageTestFixtures;
	using namespace StageBridgeLoopbackTests;

	FStageTestGameInstance Game;
	UStageBridgeSubsystem* Bridge = Game.Subsystem<UStageBridgeSubsystem>();
	if (!TestTrue(TEXT("The test game instance starts"), Game.IsValid()) || !TestNotNull(TEXT("Bridge"), Bridge))
	{
		return false;
	}
	FStageSceneDocument Scene = MakeScene(TEXT("Job Stage"));
	// Enough items that a small budget needs several frames to spawn them.
	const TArray<FStageItemSnapshot> Pair = Scene.Items;
	for (int32 Copy = 1; Copy < 10; ++Copy)
	{
		for (FStageItemSnapshot Item : Pair)
		{
			Item.InstanceId = FGuid::NewGuid();
			Item.Transform.AddToTranslation(FVector(500.0 * Copy, 0.0, 0.0));
			Scene.Items.Add(MoveTemp(Item));
		}
	}

	FStageSceneExportJobParams Relative;
	Relative.Document = Scene;
	Relative.Path = TEXT("relative.glb");
	FStageBridgeResultInfo Refusal;
	TestFalse(TEXT("A relative path is refused before any work"), FStageSceneExportJob::Start(Game.Get(), Relative, FOnStageSceneExportFinished(), Refusal).IsValid());
	TestEqual(TEXT("as InvalidRequest"), Str(Refusal.Code), Str(EStageBridgeResult::InvalidRequest));

	if (!FApp::CanEverRender())
	{
		FStageSceneExportJobParams NoRenderer;
		NoRenderer.Document = Scene;
		NoRenderer.Path = FPaths::Combine(Game.GetRoot(), TEXT("NoRenderer.glb"));
		TestFalse(TEXT("Without a renderer the job is refused"), FStageSceneExportJob::Start(Game.Get(), NoRenderer, FOnStageSceneExportFinished(), Refusal).IsValid());
		TestEqual(TEXT("as Unavailable"), Str(Refusal.Code), Str(EStageBridgeResult::Unavailable));
		AddInfo(TEXT("No renderer: the export itself was skipped."));
		return true;
	}

	const int32 ContextsBefore = GEngine->GetWorldContexts().Num();
	FStageSceneExportJobParams Params;
	Params.Document = Scene;
	Params.Path = FPaths::Combine(Game.GetRoot(), TEXT("Jobs"), TEXT("Job.glb"));
	Params.bComputeSha1 = true;
	Params.FrameBudgetMs = 1.0;
	TOptional<FStageSceneExportOutcome> Outcome;
	TSharedPtr<FStageSceneExportJob> Job = FStageSceneExportJob::Start(Game.Get(), Params,
		FOnStageSceneExportFinished::CreateLambda([&Outcome](const FStageSceneExportOutcome& Done) { Outcome = Done; }), Refusal);
	if (!TestTrue(TEXT("The job starts"), Job.IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("Nothing is done inside Start"), Outcome.IsSet());
	TestTrue(TEXT("The job finishes"), Game.Pump([&Outcome]() { return Outcome.IsSet(); }, 30.0));
	if (Outcome.IsSet())
	{
		TestTrue(TEXT("in success"), Outcome->Result.IsSuccess());
		TestEqual(TEXT("with every item"), Outcome->Result.ExportedItems, Scene.Items.Num() - Outcome->Result.SkippedItems);
		TestTrue(TEXT("over several frames"), Outcome->Frames > 4);
		TestEqual(TEXT("The SHA-1 is the file's"), Outcome->Sha1, Sha1OfFile(Params.Path));
		TestFalse(TEXT("No temporary file is left"), IFileManager::Get().FileExists(*FStageSceneExporter::GetExportTempPath(Params.Path)));
		AddInfo(FString::Printf(TEXT("%d frames, longest slice %.1f ms; %s"), Outcome->Frames, Outcome->LongestSliceMs, *Outcome->Timings.ToString()));
	}
	TestFalse(TEXT("The job is done"), Job->IsRunning());
	TestEqual(TEXT("Its world is gone"), GEngine->GetWorldContexts().Num(), ContextsBefore);

	const FString SyncPath = FPaths::Combine(Game.GetRoot(), TEXT("Jobs"), TEXT("Sync.glb"));
	UStageItemSubsystem* Items = Game.Subsystem<UStageItemSubsystem>();
	UStageProfileSubsystem* Profile = Game.Subsystem<UStageProfileSubsystem>();
	if (Items && Profile && Outcome.IsSet())
	{
		const FStageBridgeResultInfo Sync = FStageSceneExporter::ExportDocument(Scene, FStageSceneExporter::CollectExportableItems(*Items, *Profile), SyncPath);
		TestEqual(TEXT("The synchronous export places the same items"), Sync.ExportedItems, Outcome->Result.ExportedItems);
		TestEqual(TEXT("and writes a file of the same size"), IFileManager::Get().FileSize(*SyncPath), IFileManager::Get().FileSize(*Params.Path));
	}

	AddInfo(TEXT("Cancelled half-way: no callback, no world, no file."));
	bool bCalled = false;
	FStageSceneExportJobParams Cancelled = Params;
	Cancelled.Path = FPaths::Combine(Game.GetRoot(), TEXT("Jobs"), TEXT("Cancelled.glb"));
	Cancelled.FrameBudgetMs = 1.0;
	Job = FStageSceneExportJob::Start(Game.Get(), Cancelled, FOnStageSceneExportFinished::CreateLambda([&bCalled](const FStageSceneExportOutcome&) { bCalled = true; }), Refusal);
	TestTrue(TEXT("Started"), Job.IsValid());
	TestTrue(TEXT("It gets as far as its world"), Game.Pump([ContextsBefore]() { return GEngine->GetWorldContexts().Num() > ContextsBefore; }, 5.0));
	Job.Reset();
	Game.PumpFor(0.2);
	TestFalse(TEXT("No callback after a cancel"), bCalled);
	TestEqual(TEXT("No world left"), GEngine->GetWorldContexts().Num(), ContextsBefore);
	TestFalse(TEXT("No file written"), IFileManager::Get().FileExists(*Cancelled.Path));
	TestFalse(TEXT("No temporary file left"), IFileManager::Get().FileExists(*FStageSceneExporter::GetExportTempPath(Cancelled.Path)));

	AddInfo(TEXT("Through the bridge: one export at a time; the outcome goes to its requester."));
	TOptional<FStageBridgeResultInfo> First;
	const FString SceneName = TEXT("Job Saved");
	FStageSceneDocument Saved = MakeScene(SceneName);
	FStageSceneStore(FStageSceneStore::GetDefaultDirectory()).Save(Saved);
	TestTrue(TEXT("Started"), Bridge->BeginExportSceneToFile(SceneName, FPaths::Combine(Game.GetRoot(), TEXT("Bridge.glb")),
		FOnStageBridgeExportFinished::CreateLambda([&First](const FStageBridgeResultInfo& Result) { First = Result; })).IsSuccess());
	TestTrue(TEXT("Exporting"), Bridge->IsExporting());
	TestEqual(TEXT("A second export meanwhile"), Str(Bridge->BeginExportSceneToFile(SceneName, FPaths::Combine(Game.GetRoot(), TEXT("Second.glb")),
		FOnStageBridgeExportFinished()).Code), Str(EStageBridgeResult::Busy));
	TestTrue(TEXT("The first finishes"), Game.Pump([&First]() { return First.IsSet(); }, 30.0));
	TestTrue(TEXT("in success"), First.IsSet() && First->IsSuccess());
	TestFalse(TEXT("Free again"), Bridge->IsExporting());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
