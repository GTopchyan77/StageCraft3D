// Copyright Epic Games, Inc. All Rights Reserved.

// Integration tests for the DMX patch and input against real sockets and a real game world (Docs/ADR/0019 §2-4):
//  - Receiver: real Art-Net and sACN datagrams sent over loopback to the receiver threads arrive, mapped onto StageCraft
//    universes, with one hand-off per batch; an ArtPoll is answered; garbage is counted, not shown; nothing open on failure.
//  - PatchWorld: fixtures auto-patch on BeginPlay, the per-universe index follows patch changes, conflicts are found and
//    repaired, and a received universe updates only the fixtures whose channels changed.
// Test ports (16454 / 15568) are not the protocol ports, so a DMX tool running on this PC does not interfere.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.DMX; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/LightingFixtureActor.h"
#include "Common/UdpSocketBuilder.h"
#include "Data/LightingFixtureData.h"
#include "Data/StageParameterTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformProcess.h"
#include "IPAddress.h"
#include "Misc/AutomationTest.h"
#include "Show/StageDMXNetworkReceiver.h"
#include "Show/StageDMXNetProtocol.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageDMXPatchSubsystem.h"

#include <atomic>

namespace StageDMXReceiverTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;
	constexpr uint16 TestArtNetPort = 16454;
	constexpr uint16 TestSACNPort = 15568;

	/** A UDP socket on an ephemeral loopback port that sends to the receiver and reads its replies. */
	struct FScopedSender
	{
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		FSocket* Socket = FUdpSocketBuilder(TEXT("StageCraft DMX test sender")).AsNonBlocking().BoundToAddress(FIPv4Address(127, 0, 0, 1)).BoundToPort(0).Build();

		~FScopedSender()
		{
			if (Socket)
			{
				Socket->Close();
				Sockets->DestroySocket(Socket);
			}
		}

		bool Send(const TArray<uint8>& Packet, uint16 Port) const
		{
			TSharedRef<FInternetAddr> Destination = Sockets->CreateInternetAddr();
			Destination->SetLoopbackAddress();
			Destination->SetPort(Port);
			int32 Sent = 0;
			return Socket && Socket->SendTo(Packet.GetData(), Packet.Num(), Sent, *Destination) && Sent == Packet.Num();
		}

		/** The next datagram within Seconds, or empty. */
		TArray<uint8> Receive(double Seconds) const
		{
			TArray<uint8> Data;
			if (Socket && Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromSeconds(Seconds)))
			{
				Data.SetNumUninitialized(1500);
				int32 Read = 0;
				TSharedRef<FInternetAddr> From = Sockets->CreateInternetAddr();
				Data.SetNum(Socket->RecvFrom(Data.GetData(), Data.Num(), Read, *From) ? Read : 0);
			}
			return Data;
		}
	};

	/** Drains Receiver until Wanted universes arrived or two seconds passed; the frames by universe. */
	TMap<int32, TArray<uint8>> DrainUntil(FStageDMXNetworkReceiver& Receiver, int32 Wanted)
	{
		TMap<int32, TArray<uint8>> Frames;
		const double Deadline = FPlatformTime::Seconds() + 2.0;
		while (Frames.Num() < Wanted && FPlatformTime::Seconds() < Deadline)
		{
			FPlatformProcess::Sleep(0.01f);
			Receiver.Drain([&Frames](int32 Universe, TConstArrayView<uint8> Slots) { Frames.Add(Universe, TArray<uint8>(Slots.GetData(), Slots.Num())); });
		}
		return Frames;
	}

	/** A game world that has begun play, destroyed with the scope. Fixtures register with its subsystems on BeginPlay. */
	struct FScopedWorld
	{
		UWorld* World = nullptr;

		FScopedWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageDMXTestWorld"));
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

		/** A moving head with the default DMX layout, initialised the way the spawn system does it. */
		ALightingFixtureActor* SpawnFixture(ULightingFixtureData* Data) const
		{
			ALightingFixtureActor* Fixture = World->SpawnActorDeferred<ALightingFixtureActor>(ALightingFixtureActor::StaticClass(), FTransform::Identity);
			Fixture->InitializeFromItemData(Data);
			Fixture->FinishSpawning(FTransform::Identity);
			return Fixture;
		}
	};

	FStageDMXPatch MakePatch(int32 Universe, int32 Address)
	{
		FStageDMXPatch Patch;
		Patch.Universe = Universe;
		Patch.Address = Address;
		return Patch;
	}

	/** The fixture's dimmer channel within its mode (1-based; the coarse one when 16-bit), or 0. */
	int32 FindDimmerChannel(const ULightingFixtureData& Data)
	{
		const FStageFixtureDMXChannel* Channel = Data.DMXChannels.FindByPredicate([](const FStageFixtureDMXChannel& Candidate)
		{
			return Candidate.Attribute == StageCraftTags::Attribute_Dimmer;
		});
		return Channel ? Channel->Channel : 0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDMXReceiverTest, "StageCraft.DMX.Receiver", StageDMXReceiverTests::Flags)

bool FStageDMXReceiverTest::RunTest(const FString& Parameters)
{
	using namespace StageDMXReceiverTests;

	FStageDMXReceiverConfig Config;
	Config.Settings.BindAddress = TEXT("127.0.0.1");
	Config.ArtNetPort = TestArtNetPort;
	Config.SACNPort = TestSACNPort;
	Config.NodeName = TEXT("StageCraftTest");

	// Nothing to listen to: refused, nothing open.
	{
		FStageDMXNetworkReceiver Receiver;
		FStageDMXReceiverConfig Off = Config;
		Off.Settings.bArtNet = false;
		Off.Settings.bSACN = false;
		FText Error;
		TestFalse(TEXT("no protocol refuses"), Receiver.Start(Off, nullptr, Error));
		TestFalse(TEXT("with a reason"), Error.IsEmpty());
		TestFalse(TEXT("nothing running"), Receiver.IsRunning());
	}

	FStageDMXNetworkReceiver Receiver;
	std::atomic<int32> Scheduled{ 0 };
	FText Error;
	if (!TestTrue(TEXT("receiver starts"), Receiver.Start(Config, [&Scheduled]() { ++Scheduled; }, Error)))
	{
		AddError(Error.ToString());
		return false;
	}
	FScopedSender Sender;
	if (!TestNotNull(TEXT("sender socket"), Sender.Socket))
	{
		return false;
	}

	// One Art-Net frame for port-address 0 (universe 1), two sACN frames for sACN universe 2: the newer one wins.
	TArray<uint8> Slots;
	Slots.SetNumZeroed(512);
	Slots[0] = 200;
	Slots[511] = 17;
	TestTrue(TEXT("Art-Net sent"), Sender.Send(StageDMXNet::BuildArtDmx(0, 1, Slots), TestArtNetPort));
	Slots[0] = 50;
	const FGuid Console(11, 22, 33, 44);
	TestTrue(TEXT("sACN sent"), Sender.Send(StageDMXNet::BuildSACNData(2, Console, 100, 1, Slots, false, TEXT("Test console")), TestSACNPort));
	Slots[0] = 60;
	TestTrue(TEXT("second sACN sent"), Sender.Send(StageDMXNet::BuildSACNData(2, Console, 100, 2, Slots, false, TEXT("Test console")), TestSACNPort));
	TestTrue(TEXT("garbage sent"), Sender.Send(TArray<uint8>{ 1, 2, 3 }, TestArtNetPort));

	TMap<int32, TArray<uint8>> Frames = DrainUntil(Receiver, 2);
	// Give the second sACN frame time to land if the first drain caught only the first.
	FPlatformProcess::Sleep(0.1f);
	Receiver.Drain([&Frames](int32 Universe, TConstArrayView<uint8> Received) { Frames.Add(Universe, TArray<uint8>(Received.GetData(), Received.Num())); });

	TestTrue(TEXT("Art-Net 0:0:0 arrived as universe 1"), Frames.Contains(1) && Frames[1][0] == 200 && Frames[1][511] == 17);
	TestTrue(TEXT("sACN 2 arrived as universe 2, newest frame"), Frames.Contains(2) && Frames[2][0] == 60);
	TestTrue(TEXT("hand-offs are coalesced"), Scheduled.load() >= 1 && Scheduled.load() <= 3);

	const FStageDMXInputStatus Status = Receiver.GetStatus();
	TestEqual(TEXT("listening"), Status.State, EStageDMXInputState::Listening);
	TestEqual(TEXT("Art-Net frames counted"), Status.ArtNetPackets, static_cast<int64>(1));
	TestEqual(TEXT("sACN frames counted"), Status.SACNPackets, static_cast<int64>(2));
	TestEqual(TEXT("garbage counted"), Status.InvalidPackets, static_cast<int64>(1));
	TestEqual(TEXT("live universes"), Status.LiveUniverses, TArray<int32>{ 1, 2 });
	TestEqual(TEXT("two sources"), Status.LiveSources, 2);

	// ArtPoll is answered straight back to the poller.
	TestTrue(TEXT("ArtPoll sent"), Sender.Send(StageDMXNet::BuildArtPoll(), TestArtNetPort));
	const TArray<uint8> Reply = Sender.Receive(2.0);
	if (TestEqual(TEXT("ArtPollReply received"), Reply.Num(), StageDMXNet::ArtPollReplySize))
	{
		TestTrue(TEXT("it is a reply"), Reply[8] == 0x00 && Reply[9] == 0x21);
		TestTrue(TEXT("announces the bound address"), Reply[10] == 127 && Reply[13] == 1);
		TestTrue(TEXT("with our name"), FMemory::Memcmp(Reply.GetData() + 26, "StageCraftTest", 14) == 0);
	}

	Receiver.Stop();
	TestFalse(TEXT("stopped"), Receiver.IsRunning());
	TestEqual(TEXT("off after stop"), Receiver.GetStatus().State, EStageDMXInputState::Off);
	Receiver.Stop(); // Idempotent.
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDMXPatchWorldTest, "StageCraft.DMX.PatchWorld", StageDMXReceiverTests::Flags)

bool FStageDMXPatchWorldTest::RunTest(const FString& Parameters)
{
	using namespace StageDMXReceiverTests;

	FScopedWorld Scope;
	UStageDMXPatchSubsystem* Patching = UWorld::GetSubsystem<UStageDMXPatchSubsystem>(Scope.World);
	UShowControlSubsystem* ShowControl = UWorld::GetSubsystem<UShowControlSubsystem>(Scope.World);
	if (!TestNotNull(TEXT("patch subsystem"), Patching) || !TestNotNull(TEXT("show control"), ShowControl))
	{
		return false;
	}

	ULightingFixtureData* Data = NewObject<ULightingFixtureData>(GetTransientPackage());
	const int32 Footprint = Data->GetDMXFootprint();
	const int32 DimmerChannel = FindDimmerChannel(*Data);
	if (!TestTrue(TEXT("a default layout with a dimmer"), Footprint > 0 && DimmerChannel > 0))
	{
		return false;
	}

	// Auto-patch on BeginPlay, packed from 1.001.
	ALightingFixtureActor* A = Scope.SpawnFixture(Data);
	ALightingFixtureActor* B = Scope.SpawnFixture(Data);
	ALightingFixtureActor* C = Scope.SpawnFixture(Data);
	TestTrue(TEXT("A at 1.001"), A->GetPatch() == MakePatch(1, 1));
	TestTrue(TEXT("B next"), B->GetPatch() == MakePatch(1, 1 + Footprint));
	TestTrue(TEXT("C next"), C->GetPatch() == MakePatch(1, 1 + 2 * Footprint));
	TestEqual(TEXT("three in universe 1"), Patching->GetFixturesInUniverse(1).Num(), 3);
	TestEqual(TEXT("no conflicts"), Patching->FindPatchConflicts().Num(), 0);

	// The index follows a re-patch.
	const uint32 Revision = Patching->GetPatchRevision();
	B->SetPatch(MakePatch(2, 1));
	TestTrue(TEXT("revision moved"), Patching->GetPatchRevision() != Revision);
	TestEqual(TEXT("two left in universe 1"), Patching->GetFixturesInUniverse(1).Num(), 2);
	TestEqual(TEXT("B in universe 2"), Patching->GetFixturesInUniverse(2).Num(), 1);
	TestEqual(TEXT("universes"), Patching->GetPatchedUniverses(), TArray<int32>{ 1, 2 });

	// A conflict, found and repaired: A keeps its address, C moves.
	C->SetPatch(MakePatch(1, 5));
	TestEqual(TEXT("A and C conflict"), Patching->FindPatchConflicts(), TArray<int32>{ FMath::Min(A->GetFixtureId(), C->GetFixtureId()), FMath::Max(A->GetFixtureId(), C->GetFixtureId()) });
	const TArray<FStageDMXPatchAssignment> Repair = Patching->PlanConflictRepair();
	if (TestEqual(TEXT("one move"), Repair.Num(), 1))
	{
		TestTrue(TEXT("C moves"), Repair[0].Fixture == C);
		Repair[0].Fixture->SetPatch(Repair[0].Patch);
	}
	TestEqual(TEXT("repaired"), Patching->FindPatchConflicts().Num(), 0);

	// Consecutive plan for B and C from 3.010.
	const TArray<FStageDMXPatchAssignment> Plan = Patching->PlanConsecutive({ C, B }, MakePatch(3, 10));
	if (TestEqual(TEXT("two planned"), Plan.Num(), 2))
	{
		TestTrue(TEXT("fixture-ID order"), Plan[0].Fixture == (B->GetFixtureId() < C->GetFixtureId() ? B : C));
		TestTrue(TEXT("from 3.010"), Plan[0].Patch == MakePatch(3, 10) && Plan[1].Patch == MakePatch(3, 10 + Footprint));
	}

	// Received DMX: everything the first time, nothing for a repeat, one fixture for one changed channel.
	TArray<uint8> Frame;
	Frame.SetNumZeroed(512);
	const int32 DimmerIndex = A->GetPatch().Address - 1 + DimmerChannel - 1;
	Frame[DimmerIndex] = 255;
	Frame[DimmerIndex + 1] = 255; // The fine byte of a 16-bit dimmer (the default layout's); A's next channel otherwise.
	TestEqual(TEXT("ignored while Internal"), ShowControl->ReceiveDMXUniverse(1, Frame), 0);
	ShowControl->SetControlSource(EStageControlSource::External);
	TestEqual(TEXT("first frame: both fixtures of universe 1"), ShowControl->ReceiveDMXUniverse(1, Frame), 2);
	TestTrue(TEXT("A at full"), FMath::IsNearlyEqual(A->GetAttribute(StageCraftTags::Attribute_Dimmer), 1.f, 0.01f));
	TestEqual(TEXT("a repeat costs nothing"), ShowControl->ReceiveDMXUniverse(1, Frame), 0);
	Frame[DimmerIndex] = 0;
	Frame[DimmerIndex + 1] = 0;
	TestEqual(TEXT("one changed channel, one fixture"), ShowControl->ReceiveDMXUniverse(1, Frame), 1);
	TestTrue(TEXT("A dark"), FMath::IsNearlyEqual(A->GetAttribute(StageCraftTags::Attribute_Dimmer), 0.f, 0.01f));
	TestEqual(TEXT("short frames are refused"), ShowControl->ReceiveDMXUniverse(1, TArray<uint8>{ 1, 2, 3 }), 0);
	C->SetPatch(MakePatch(1, 400));
	TestEqual(TEXT("after a re-patch the next frame applies in full"), ShowControl->ReceiveDMXUniverse(1, Frame), 2);

	// Removal and auto-patch off.
	A->Destroy();
	TestEqual(TEXT("A gone from the index"), Patching->GetFixturesInUniverse(1).Num(), 1);
	Patching->bAutoPatchNewFixtures = false;
	ALightingFixtureActor* D = Scope.SpawnFixture(Data);
	TestFalse(TEXT("stays unpatched"), D->GetPatch().IsPatched());
	const TArray<FStageDMXPatchAssignment> Unpatched = Patching->PlanUnpatched();
	TestTrue(TEXT("D planned into the first gap"), Unpatched.Num() == 1 && Unpatched[0].Fixture == D && Unpatched[0].Patch == MakePatch(1, 1));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
