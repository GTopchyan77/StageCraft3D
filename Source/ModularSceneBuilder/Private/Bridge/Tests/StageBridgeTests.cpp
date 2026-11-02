// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the DCC bridge (Docs/ADR/0022-dcc-bridge.md): the wire protocol, the scene origin that keeps bought stages inside the
// app, the export refusals, procedural-to-static conversion, and the localhost server with real loopback sockets.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Bridge; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Bridge/StageBridgeProtocol.h"
#include "Bridge/StageBridgeServer.h"
#include "Bridge/StageSceneExporter.h"
#include "Bridge/Tests/StageBridgeTestClient.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "ProceduralMeshComponent.h"
#include "Scene/StageSceneStore.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "StaticMeshResources.h"

#include <atomic>

namespace StageBridgeTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	FString Str(EStageBridgeResult Result) { return UEnum::GetValueAsString(Result); }
	FString Str(EStageSceneResult Result) { return UEnum::GetValueAsString(Result); }

	TArray<uint8> Bytes(const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text, Text.Len());
		return TArray<uint8>(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	}

	FStageItemSnapshot MakeItem()
	{
		FStageItemSnapshot Item;
		Item.InstanceId = FGuid::NewGuid();
		Item.Item = TSoftObjectPtr<UBaseItemData>(FSoftObjectPath(TEXT("/Game/StageCraft/Items/DA_Test_Crate.DA_Test_Crate")));
		Item.Transform = FTransform(FVector(100.0, 0.0, 0.0));
		return Item;
	}

	/** The loopback stand-in for an add-on, shared with the integration tests. */
	using FTestClient = FStageBridgeTestClient;

	/** Drains Server until Predicate holds for an event or the time runs out. The worker posts from its own thread. */
	bool DrainUntil(FStageBridgeServer& Server, TArray<FStageBridgeEvent>& OutEvents, TFunctionRef<bool(const FStageBridgeEvent&)> Predicate, double Seconds = 3.0)
	{
		const double Deadline = FPlatformTime::Seconds() + Seconds;
		while (FPlatformTime::Seconds() < Deadline)
		{
			bool bFound = false;
			Server.Drain([&](const FStageBridgeEvent& Event)
			{
				OutEvents.Add(Event);
				bFound |= Predicate(Event);
			});
			if (bFound)
			{
				return true;
			}
			FPlatformProcess::Sleep(0.02f);
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageBridgeProtocolTest, "StageCraft.Bridge.Protocol", StageBridgeTests::Flags)

bool FStageBridgeProtocolTest::RunTest(const FString& Parameters)
{
	using namespace StageBridgeProtocol;
	using namespace StageBridgeTests;

	// Lines: split on \n, \r trimmed, empty lines skipped, a partial line kept for the next chunk.
	{
		FLineReader Reader;
		TArray<FString> Lines;
		TestTrue(TEXT("Chunks are accepted"), Reader.Append(Bytes(TEXT("{\"a\":1}\r\n\n{\"b\"")), Lines));
		TestEqual(TEXT("One complete line so far"), Lines.Num(), 1);
		TestEqual(TEXT("CR is trimmed"), Lines.IsEmpty() ? FString() : Lines[0], FString(TEXT("{\"a\":1}")));
		TestTrue(TEXT("The rest arrives"), Reader.Append(Bytes(TEXT(":2}\n")), Lines));
		TestEqual(TEXT("A line split across chunks is joined"), Lines.Num() == 2 ? Lines[1] : FString(), FString(TEXT("{\"b\":2}")));
		TestTrue(TEXT("UTF-8 survives"), Reader.Append(Bytes(TEXT("Բեմ\n")), Lines) && Lines.Last() == TEXT("Բեմ"));

		FLineReader Flood;
		TArray<uint8> Long;
		Long.Init('x', MaxLineBytes);
		TestFalse(TEXT("A line over the limit is refused, even unterminated"), Flood.Append(Long, Lines));
	}

	// Client messages.
	{
		FClientMessage Message;
		FString Error;
		TestTrue(TEXT("Hello parses"), ParseClientMessage(TEXT("{\"type\":\"hello\",\"protocol\":1,\"token\":\"abc\",\"app\":\"Blender\",\"version\":\"4.3\\n\\u0007.0\"}"), Message, Error));
		TestTrue(TEXT("Hello fields"), Message.Type == EClientMessage::Hello && Message.Hello.Protocol == 1 && Message.Hello.Token == TEXT("abc") && Message.Hello.App == EStageBridgeApp::Blender);
		TestEqual(TEXT("Control characters are stripped from the version"), Message.Hello.AppVersion, FString(TEXT("4.3.0")));
		TestFalse(TEXT("Hello without a token"), ParseClientMessage(TEXT("{\"type\":\"hello\",\"protocol\":1,\"app\":\"maya\"}"), Message, Error));
		TestTrue(TEXT("Unknown app still parses, as Unknown"), ParseClientMessage(TEXT("{\"type\":\"hello\",\"protocol\":1,\"token\":\"t\",\"app\":\"houdini\"}"), Message, Error)
			&& Message.Hello.App == EStageBridgeApp::Unknown);

		TestTrue(TEXT("Ack parses"), ParseClientMessage(TEXT("{\"type\":\"ack\",\"id\":\"42\",\"ok\":false,\"error\":\"no importer\"}"), Message, Error));
		TestTrue(TEXT("Ack fields"), Message.Type == EClientMessage::Ack && Message.Ack.Id == TEXT("42") && !Message.Ack.bOk && Message.Ack.Error == TEXT("no importer"));
		TestFalse(TEXT("Ack without ok"), ParseClientMessage(TEXT("{\"type\":\"ack\",\"id\":\"42\"}"), Message, Error));
		TestFalse(TEXT("Ack with an over-long id"), ParseClientMessage(FString::Printf(TEXT("{\"type\":\"ack\",\"id\":\"%s\",\"ok\":true}"), *FString::ChrN(MaxIdLength + 1, TEXT('a'))), Message, Error));
		TestFalse(TEXT("Unknown type"), ParseClientMessage(TEXT("{\"type\":\"run\",\"code\":\"rm -rf\"}"), Message, Error));
		TestFalse(TEXT("Not JSON"), ParseClientMessage(TEXT("hello"), Message, Error));
		TestFalse(TEXT("Not an object"), ParseClientMessage(TEXT("[1,2]"), Message, Error));
	}

	// Tokens.
	{
		const FString Token = MakeToken();
		TestEqual(TEXT("Token length"), Token.Len(), 64);
		TestNotEqual(TEXT("Tokens differ per run"), Token, MakeToken());
		TestTrue(TEXT("Same token matches"), IsTokenMatch(Token, FString(Token)));
		TestFalse(TEXT("Last character differs"), IsTokenMatch(Token, Token.LeftChop(1) + (Token.EndsWith(TEXT("0")) ? TEXT("1") : TEXT("0"))));
		TestFalse(TEXT("Shorter"), IsTokenMatch(Token, Token.LeftChop(1)));
		TestFalse(TEXT("An empty expected token never matches"), IsTokenMatch(FString(), FString()));
	}

	// Apps and server lines.
	TestTrue(TEXT("Blender imports glb"), ImportsGlb(EStageBridgeApp::Blender));
	TestFalse(TEXT("Maya waits for USD"), ImportsGlb(EStageBridgeApp::Maya));
	TestFalse(TEXT("Unknown apps get nothing"), ImportsGlb(EStageBridgeApp::Unknown));
	TestTrue(TEXT("App keys round-trip"), ParseApp(GetAppKey(EStageBridgeApp::Cinema4D)) == EStageBridgeApp::Cinema4D);
	FAsset Asset;
	Asset.Id = TEXT("1");
	Asset.Name = TEXT("Line\nBreak");
	const FString Line = MakeAsset(Asset);
	TestTrue(TEXT("A server line is one line"), Line.EndsWith(TEXT("\n")) && Line.LeftChop(1).Find(TEXT("\n")) == INDEX_NONE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageBridgeSceneOriginTest, "StageCraft.Bridge.SceneOrigin", StageBridgeTests::Flags)

bool FStageBridgeSceneOriginTest::RunTest(const FString& Parameters)
{
	using namespace StageBridgeTests;

	FStageSceneDocument Own;
	Own.Name = TEXT("Mine");
	Own.Items.Add(MakeItem());
	const FString OwnText = FStageSceneStore::Serialize(Own);
	TestFalse(TEXT("The user's own stage writes no origin"), OwnText.Contains(TEXT("originListingId")));

	FStageSceneDocument Bought = Own;
	Bought.OriginListingId = FGuid::NewGuid();
	FStageSceneDocument Parsed;
	FStageSceneParseReport Report;
	TestEqual(TEXT("A bought stage round-trips"), Str(FStageSceneStore::Parse(FStageSceneStore::Serialize(Bought), Parsed, Report)), Str(EStageSceneResult::Success));
	TestEqual(TEXT("Its origin survives"), Parsed.OriginListingId, Bought.OriginListingId);
	TestTrue(TEXT("It counts as from the marketplace"), Parsed.IsFromMarketplace());

	// Older files are the user's own, even with a field a version 3 file could not have meant.
	const FString Version3 = FStageSceneStore::Serialize(Bought).Replace(TEXT("\"version\": 4"), TEXT("\"version\": 3"));
	TestEqual(TEXT("A version 3 file loads"), Str(FStageSceneStore::Parse(Version3, Parsed, Report)), Str(EStageSceneResult::Success));
	TestFalse(TEXT("A version 3 file is the user's own"), Parsed.IsFromMarketplace());

	// Deny by default: an origin that is there but unreadable is never read as "own".
	const FString Damaged = FStageSceneStore::Serialize(Bought).Replace(*Bought.OriginListingId.ToString(EGuidFormats::DigitsWithHyphens), TEXT("not-a-guid"));
	TestEqual(TEXT("An unreadable origin makes the file corrupt"), Str(FStageSceneStore::Parse(Damaged, Parsed, Report)), Str(EStageSceneResult::Corrupt));
	const FString Zero = FStageSceneStore::Serialize(Bought).Replace(*Bought.OriginListingId.ToString(EGuidFormats::DigitsWithHyphens), TEXT("00000000-0000-0000-0000-000000000000"));
	TestEqual(TEXT("A zero origin makes the file corrupt"), Str(FStageSceneStore::Parse(Zero, Parsed, Report)), Str(EStageSceneResult::Corrupt));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageBridgeExportRefusalTest, "StageCraft.Bridge.ExportRefusals", StageBridgeTests::Flags)

bool FStageBridgeExportRefusalTest::RunTest(const FString& Parameters)
{
	using namespace StageBridgeTests;

	const FString Directory = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("StageBridgeTests"), FGuid::NewGuid().ToString());
	const FStageSceneStore Store(Directory);
	FStageSceneDocument Bought;
	Bought.Name = TEXT("Bought Stage");
	Bought.Items.Add(MakeItem());
	Bought.OriginListingId = FGuid::NewGuid();
	TestEqual(TEXT("Saved"), Str(Store.Save(Bought)), Str(EStageSceneResult::Success));

	// No library or profile is needed to refuse: refusals come before anything is spawned.
	UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(Directory, TEXT("out.glb")));
	// The bare game instance has no marketplace to grant export rights (ADR 0022 §11.1): deny by default.
	TestEqual(TEXT("A bought stage without export rights is refused"),Str(FStageSceneExporter::ExportSavedScene(*GameInstance, Store, Bought.Name, Path).Code), Str(EStageBridgeResult::NotOriginal));
	TestEqual(TEXT("A missing scene"), Str(FStageSceneExporter::ExportSavedScene(*GameInstance, Store, TEXT("Nope"), Path).Code), Str(EStageBridgeResult::NotFound));
	TestEqual(TEXT("A bad scene name"), Str(FStageSceneExporter::ExportSavedScene(*GameInstance, Store, TEXT("../x"), Path).Code), Str(EStageBridgeResult::InvalidRequest));
	TestEqual(TEXT("A relative target"), Str(FStageSceneExporter::ExportSavedScene(*GameInstance, Store, Bought.Name, TEXT("out.glb")).Code), Str(EStageBridgeResult::InvalidRequest));
	TestEqual(TEXT("Another extension"), Str(FStageSceneExporter::ExportSavedScene(*GameInstance, Store, Bought.Name, Path + TEXT(".exe")).Code), Str(EStageBridgeResult::InvalidRequest));

	FStageSceneDocument Own = Bought;
	Own.Name = TEXT("Own Stage");
	Own.OriginListingId.Invalidate();
	Store.Save(Own);
	TestEqual(TEXT("An own stage passes the origin check (and then needs the library)"),
		Str(FStageSceneExporter::ExportSavedScene(*GameInstance, Store, Own.Name, Path).Code), Str(EStageBridgeResult::Unavailable));
	TestFalse(TEXT("Nothing was written"), IFileManager::Get().FileExists(*Path));

	IFileManager::Get().DeleteDirectory(*Directory, /*RequireExists*/ false, /*Tree*/ true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageBridgeProceduralTest, "StageCraft.Bridge.ProceduralConversion", StageBridgeTests::Flags)

bool FStageBridgeProceduralTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::GamePreview, /*bInformEngineOfWorld*/ false, TEXT("StageBridgeProceduralTestWorld"));
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::GamePreview);
	Context.SetCurrentWorld(World);
	World->InitializeActorsForPlay(FURL());

	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(FVector(0.0, 0.0, 50.0)));
	UProceduralMeshComponent* Procedural = NewObject<UProceduralMeshComponent>(Actor);
	Actor->SetRootComponent(Procedural);
	Procedural->RegisterComponent();
	// A 1 m quad: two triangles.
	const TArray<FVector> Vertices = { FVector(0, 0, 0), FVector(100, 0, 0), FVector(100, 100, 0), FVector(0, 100, 0) };
	const TArray<int32> Triangles = { 0, 2, 1, 0, 3, 2 };
	Procedural->CreateMeshSection(0, Vertices, Triangles, {}, { FVector2D(0, 0), FVector2D(1, 0), FVector2D(1, 1), FVector2D(0, 1) }, {}, {}, false);
	UProceduralMeshComponent* Empty = NewObject<UProceduralMeshComponent>(Actor);
	Empty->SetupAttachment(Procedural);
	Empty->RegisterComponent();

	TestEqual(TEXT("The procedural part with geometry is converted; the empty one is not"), FStageSceneExporter::ConvertProceduralMeshes(*Actor), 1);

	TInlineComponentArray<UStaticMeshComponent*> StaticMeshes(Actor);
	TestEqual(TEXT("One static copy"), StaticMeshes.Num(), 1);
	if (StaticMeshes.Num() == 1)
	{
		const UStaticMesh* Mesh = StaticMeshes[0]->GetStaticMesh();
		const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
		TestTrue(TEXT("The copy has render data"), RenderData && !RenderData->LODResources.IsEmpty());
		if (RenderData && !RenderData->LODResources.IsEmpty())
		{
			TestEqual(TEXT("Same triangles"), RenderData->LODResources[0].GetNumTriangles(), 2);
		}
		TestTrue(TEXT("Same place"), StaticMeshes[0]->GetComponentLocation().Equals(Procedural->GetComponentLocation(), 0.01));
		TestEqual(TEXT("One material slot"), Mesh ? Mesh->GetStaticMaterials().Num() : 0, 1);
	}

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(/*bInformEngineOfWorld*/ false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageBridgeServerTest, "StageCraft.Bridge.Server", StageBridgeTests::Flags)

bool FStageBridgeServerTest::RunTest(const FString& Parameters)
{
	using namespace StageBridgeTests;

	FStageBridgeServer Server;
	std::atomic<int32> Drains{ 0 };
	FText Error;
	const FString Token = StageBridgeProtocol::MakeToken();
	if (!TestTrue(TEXT("Starts on a free port"), Server.Start(0, Token, [&Drains]() { ++Drains; }, Error)))
	{
		AddError(Error.ToString());
		return false;
	}
	const uint16 Port = Server.GetPort();
	TestTrue(TEXT("Bound to a port"), Port > 0);

	// A wrong token is told why and closed; nothing reaches the game thread.
	{
		FTestClient Intruder;
		TestTrue(TEXT("Intruder connects"), Intruder.Connect(Port));
		Intruder.Send(TEXT("{\"type\":\"hello\",\"protocol\":1,\"token\":\"0000\",\"app\":\"blender\"}"));
		TestTrue(TEXT("A wrong token gets bye"), Intruder.WaitFor(TEXT("wrong token")));
		TestTrue(TEXT("and is closed"), Intruder.WaitClosed());
	}
	// Anything before hello is refused too.
	{
		FTestClient Rude;
		TestTrue(TEXT("Rude client connects"), Rude.Connect(Port));
		Rude.Send(TEXT("{\"type\":\"ack\",\"id\":\"1\",\"ok\":true}"));
		TestTrue(TEXT("Ack before hello gets bye"), Rude.WaitFor(TEXT("say hello first")));
	}

	TArray<FStageBridgeEvent> Events;
	{
		FTestClient App;
		TestTrue(TEXT("App connects"), App.Connect(Port));
		App.Send(FString::Printf(TEXT("{\"type\":\"hello\",\"protocol\":1,\"token\":\"%s\",\"app\":\"blender\",\"version\":\"4.3.0\"}"), *Token));
		TestTrue(TEXT("The right token is welcomed"), App.WaitFor(TEXT("\"welcome\"")));
		TestTrue(TEXT("The game thread hears of it"), DrainUntil(Server, Events, [](const FStageBridgeEvent& Event) { return Event.Kind == FStageBridgeEvent::EKind::Connected; }));
		TestTrue(TEXT("Only after a drain was asked for"), Drains.load() > 0);
		const FStageBridgeEvent* Connected = Events.FindByPredicate([](const FStageBridgeEvent& Event) { return Event.Kind == FStageBridgeEvent::EKind::Connected; });
		TestTrue(TEXT("The intruders produced no events before it"), Connected == &Events[0]);
		TestTrue(TEXT("It is Blender 4.3.0"), Connected && Connected->Client.App == EStageBridgeApp::Blender && Connected->Client.AppVersion == TEXT("4.3.0") && Connected->Client.bImportsGlb);

		const int32 ClientId = Connected ? Connected->ClientId : 0;
		StageBridgeProtocol::FAsset Asset;
		Asset.Id = TEXT("abc");
		Asset.Format = TEXT("glb");
		Server.Send(ClientId, StageBridgeProtocol::MakeAsset(Asset));
		TestTrue(TEXT("The asset message arrives"), App.WaitFor(TEXT("\"asset\"")));
		App.Send(TEXT("{\"type\":\"ack\",\"id\":\"abc\",\"ok\":true}"));
		TestTrue(TEXT("The ack reaches the game thread"), DrainUntil(Server, Events, [](const FStageBridgeEvent& Event) { return Event.Kind == FStageBridgeEvent::EKind::Ack && Event.Ack.Id == TEXT("abc") && Event.Ack.bOk; }));

		App.Send(TEXT("{\"type\":\"exec\",\"code\":\"x\"}"));
		TestTrue(TEXT("An unknown message closes the app's connection"), App.WaitClosed());
		TestTrue(TEXT("and the game thread hears it left"), DrainUntil(Server, Events, [](const FStageBridgeEvent& Event) { return Event.Kind == FStageBridgeEvent::EKind::Disconnected; }));
	}

	Server.Stop();
	Server.Stop();
	TestFalse(TEXT("Stopped (twice is fine)"), Server.IsRunning());
	TestEqual(TEXT("No port while stopped"), static_cast<int32>(Server.GetPort()), 0);
	FTestClient Late;
	TestFalse(TEXT("Nothing listens after stop"), Late.Connect(Port));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
