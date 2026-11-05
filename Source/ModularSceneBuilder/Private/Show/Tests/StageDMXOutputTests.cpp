// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the Art-Net / sACN output (Docs/ADR/0019 §9):
//  - OutputSettings: StageDMXOutput::Sanitize (ranges, addresses, muted universes) and the user settings' Cid.
//  - Output: FStageDMXNetworkSender against real loopback sockets: mapped universes, slots, sequence numbers, priority and CID,
//    keep-alive of an unchanged universe, the newest frame wins, a dropped universe and Close terminate their sACN streams,
//    nothing is sent for a universe that is not set, refusals leave nothing open; the receiver ignores its own CID.
//  - OutputShow: the show renders only changed universes and only while Internal: idle it does not tick, a full refresh and a
//    masters change send exactly the rendered universe, External sends nothing.
// Test ports (16454 / 15568) are not the protocol ports, so a DMX tool running on this PC does not interfere.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.DMX; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/LightingFixtureActor.h"
#include "Common/UdpSocketBuilder.h"
#include "Data/LightingFixtureData.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "IPAddress.h"
#include "Misc/AutomationTest.h"
#include "Settings/StageCraftUserSettings.h"
#include "Show/StageDMXNetProtocol.h"
#include "Show/StageDMXNetworkReceiver.h"
#include "Show/StageDMXNetworkSender.h"
#include "Show/StageNetworkDMXBridge.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageDMXPatchSubsystem.h"
#include "Subsystems/StageProgrammerSubsystem.h"

namespace StageDMXOutputTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;
	constexpr uint16 TestArtNetPort = 16454;
	constexpr uint16 TestSACNPort = 15568;

	/** A UDP socket bound to 127.0.0.1:Port that collects what the sender sends there. */
	struct FScopedListener
	{
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		FSocket* Socket = nullptr;

		explicit FScopedListener(uint16 Port)
		{
			Socket = FUdpSocketBuilder(TEXT("StageCraft DMX output test listener")).AsNonBlocking().AsReusable()
				.BoundToAddress(FIPv4Address(127, 0, 0, 1)).BoundToPort(Port).WithReceiveBufferSize(1 << 20).Build();
		}

		~FScopedListener()
		{
			if (Socket)
			{
				Socket->Close();
				Sockets->DestroySocket(Socket);
			}
		}

		/** Every datagram that arrives within Seconds, parsed; invalid ones are kept with Protocol unset. */
		TArray<FStageDMXNetFrame> Collect(double Seconds) const
		{
			TArray<FStageDMXNetFrame> Frames;
			const double Deadline = FPlatformTime::Seconds() + Seconds;
			TArray<uint8> Data;
			while (Socket && FPlatformTime::Seconds() < Deadline)
			{
				if (!Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromSeconds(0.02)))
				{
					continue;
				}
				Data.SetNumUninitialized(1500);
				int32 Read = 0;
				TSharedRef<FInternetAddr> From = Sockets->CreateInternetAddr();
				while (Socket->RecvFrom(Data.GetData(), Data.Num(), Read, *From) && Read > 0)
				{
					FStageDMXNetFrame Frame;
					const EStageDMXNetPacket Kind = StageDMXNet::Parse(TConstArrayView<uint8>(Data.GetData(), Read), Frame);
					if (Kind == EStageDMXNetPacket::ArtDmx || Kind == EStageDMXNetPacket::SACNData)
					{
						Frames.Add(Frame);
					}
				}
			}
			return Frames;
		}
	};

	TArray<uint8> MakeSlots(uint8 First, uint8 Last)
	{
		TArray<uint8> Slots;
		Slots.SetNumZeroed(512);
		Slots[0] = First;
		Slots[511] = Last;
		return Slots;
	}

	FStageDMXSenderConfig MakeLoopbackConfig(double KeepAliveSeconds)
	{
		FStageDMXSenderConfig Config;
		Config.Settings.ArtNet = EStageArtNetOutputMode::Unicast;
		Config.Settings.SACN = EStageSACNOutputMode::Unicast;
		Config.Settings.UnicastAddress = TEXT("127.0.0.1");
		Config.Settings.SACNPriority = 150;
		Config.Settings.Cid = FGuid(0x0A11CE00, 0x5C0FFEE0, 0x00F5A000, 0x00000001);
		Config.Numbering.ArtNetFirstPortAddress = 0; // Universe 1 = Art-Net 0:0:0.
		Config.Numbering.SACNFirstUniverse = 1;
		Config.ArtNetPort = TestArtNetPort;
		Config.SACNPort = TestSACNPort;
		Config.KeepAliveSeconds = KeepAliveSeconds;
		Config.SourceName = TEXT("StageCraft test");
		return Config;
	}

	TArray<FStageDMXNetFrame> FramesOf(const TArray<FStageDMXNetFrame>& Frames, int32 WireUniverse, bool bTerminated = false)
	{
		return Frames.FilterByPredicate([WireUniverse, bTerminated](const FStageDMXNetFrame& Frame)
		{
			return Frame.WireUniverse == WireUniverse && Frame.bTerminated == bTerminated;
		});
	}

	/** A game world that has begun play, destroyed with the scope. */
	struct FScopedWorld
	{
		UWorld* World = nullptr;

		FScopedWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageDMXOutputTestWorld"));
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDMXOutputSettingsTest, "StageCraft.DMX.OutputSettings", StageDMXOutputTests::Flags)

bool FStageDMXOutputSettingsTest::RunTest(const FString& Parameters)
{
	FStageDMXOutputSettings Settings;
	Settings.RateHz = 500;
	Settings.SACNPriority = -4;
	Settings.UnicastAddress = TEXT("  10.0.0.7 ");
	Settings.MutedUniverses = { 3, 1, 3, 0, 64000, 2 };
	FStageDMXOutputSettings Clean = StageDMXOutput::Sanitize(Settings);
	TestEqual(TEXT("rate clamped high"), Clean.RateHz, StageDMXOutput::MaxRateHz);
	TestEqual(TEXT("priority clamped low"), Clean.SACNPriority, 0);
	TestEqual(TEXT("address trimmed"), Clean.UnicastAddress, FString(TEXT("10.0.0.7")));
	TestEqual(TEXT("muted sorted, unique, in range"), Clean.MutedUniverses, TArray<int32>{ 1, 2, 3 });
	TestTrue(TEXT("muted lookup"), StageDMXOutput::IsMuted(Clean, 2) && !StageDMXOutput::IsMuted(Clean, 4));

	Settings.RateHz = 0;
	Settings.SACNPriority = 900;
	Settings.UnicastAddress = TEXT("not.an.ip");
	Settings.ArtNet = static_cast<EStageArtNetOutputMode>(77);
	Clean = StageDMXOutput::Sanitize(Settings);
	TestEqual(TEXT("rate clamped low"), Clean.RateHz, StageDMXOutput::MinRateHz);
	TestEqual(TEXT("priority clamped high"), Clean.SACNPriority, StageDMXOutput::MaxSACNPriority);
	TestTrue(TEXT("a bad address is emptied"), Clean.UnicastAddress.IsEmpty());
	TestTrue(TEXT("an unknown mode falls back to broadcast"), Clean.ArtNet == EStageArtNetOutputMode::Broadcast);
	TestFalse(TEXT("Sanitize makes no Cid"), Clean.Cid.IsValid());

	// The user settings always hold a Cid, and a reset to defaults keeps it (the same sACN source for receivers).
	UStageCraftUserSettings* UserSettings = NewObject<UStageCraftUserSettings>();
	UserSettings->SetToDefaults();
	const FGuid Cid = UserSettings->GetDMXOutputSettings().Cid;
	TestTrue(TEXT("defaults have a Cid"), Cid.IsValid());
	FStageDMXOutputSettings Changed = UserSettings->GetDMXOutputSettings();
	Changed.Cid = FGuid::NewGuid();
	Changed.RateHz = 30;
	TestTrue(TEXT("a change is stored"), UserSettings->SetDMXOutputSettings(Changed));
	TestEqual(TEXT("but not the Cid"), UserSettings->GetDMXOutputSettings().Cid, Cid);
	UserSettings->SetToDefaults();
	TestEqual(TEXT("reset keeps the Cid"), UserSettings->GetDMXOutputSettings().Cid, Cid);
	TestEqual(TEXT("reset restores the rate"), UserSettings->GetDMXOutputSettings().RateHz, StageDMXOutput::MaxRateHz);
	TestFalse(TEXT("reset leaves output off"), UserSettings->GetDMXOutputSettings().bEnabled);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDMXOutputTest, "StageCraft.DMX.Output", StageDMXOutputTests::Flags)

bool FStageDMXOutputTest::RunTest(const FString& Parameters)
{
	using namespace StageDMXOutputTests;

	// Refusals leave nothing open.
	{
		FStageDMXNetworkSender Sender;
		FText Error;
		FStageDMXSenderConfig NoProtocol = MakeLoopbackConfig(1.0);
		NoProtocol.Settings.ArtNet = EStageArtNetOutputMode::Off;
		NoProtocol.Settings.SACN = EStageSACNOutputMode::Off;
		TestFalse(TEXT("no protocol refused"), Sender.Open(NoProtocol, Error));
		TestFalse(TEXT("with a reason"), Error.IsEmpty());
		FStageDMXSenderConfig NoAddress = MakeLoopbackConfig(1.0);
		NoAddress.Settings.UnicastAddress.Reset();
		Error = FText::GetEmpty();
		TestFalse(TEXT("unicast without an address refused"), Sender.Open(NoAddress, Error));
		TestFalse(TEXT("nothing open"), Sender.IsOpen());
		Sender.Post(1, MakeSlots(1, 2)); // Harmless while closed.
		Sender.Close();
	}

	const FStageDMXSenderConfig Config = MakeLoopbackConfig(0.2);
	FStageDMXNetworkSender Sender;
	{
		FScopedListener ArtNet(TestArtNetPort);
		FScopedListener SACN(TestSACNPort);
		if (!TestTrue(TEXT("listeners bound"), ArtNet.Socket && SACN.Socket))
		{
			return false;
		}
		FText Error;
		if (!TestTrue(TEXT("sender opens"), Sender.Open(Config, Error)))
		{
			AddError(Error.ToString());
			return false;
		}

		Sender.SetUniverses(TArray<int32>{ 1, 2 });
		Sender.Post(1, MakeSlots(10, 11));
		Sender.Post(1, MakeSlots(12, 13)); // The newest frame is what receivers end up with.
		Sender.Post(2, MakeSlots(20, 21));
		Sender.Post(3, MakeSlots(30, 31)); // Not set: never sent.

		const TArray<FStageDMXNetFrame> ArtFrames = ArtNet.Collect(0.7);
		const TArray<FStageDMXNetFrame> SACNFrames = SACN.Collect(0.05);
		const TArray<FStageDMXNetFrame> ArtOne = FramesOf(ArtFrames, 0);
		const TArray<FStageDMXNetFrame> ArtTwo = FramesOf(ArtFrames, 1);
		TestTrue(TEXT("universe 1 = Art-Net 0, kept alive (at least 3 in 0.7 s at 0.2 s)"), ArtOne.Num() >= 3);
		TestTrue(TEXT("universe 2 = Art-Net 1"), ArtTwo.Num() >= 1);
		TestEqual(TEXT("universe 3 never sent"), FramesOf(ArtFrames, 2).Num(), 0);
		if (ArtOne.Num() >= 2 && ArtTwo.Num() >= 1)
		{
			TestTrue(TEXT("the newest frame of universe 1"), ArtOne.Last().Slots[0] == 12 && ArtOne.Last().Slots[511] == 13);
			TestTrue(TEXT("universe 2's slots"), ArtTwo[0].Slots[0] == 20 && ArtTwo[0].Slots[511] == 21 && ArtTwo[0].SlotCount == 512);
			bool bSequenced = true;
			for (int32 Index = 1; Index < ArtOne.Num(); ++Index)
			{
				const uint8 Expected = ArtOne[Index - 1].Sequence >= 255 ? 1 : ArtOne[Index - 1].Sequence + 1;
				bSequenced &= ArtOne[Index].Sequence == Expected && ArtOne[Index].Sequence != 0;
			}
			TestTrue(TEXT("Art-Net sequence steps 1..255, never 0"), bSequenced);
		}

		// sACN arrived on its own socket meanwhile; collect what is queued there.
		const TArray<FStageDMXNetFrame> SACNAll = SACNFrames.Num() > 0 ? SACNFrames : SACN.Collect(0.3);
		const TArray<FStageDMXNetFrame> SACNOne = FramesOf(SACNAll, 1);
		if (TestTrue(TEXT("sACN universe 1"), SACNOne.Num() >= 1))
		{
			TestEqual(TEXT("our CID"), SACNOne[0].Source, Config.Settings.Cid);
			TestEqual(TEXT("our priority"), static_cast<int32>(SACNOne[0].Priority), 150);
			TestTrue(TEXT("sACN slots"), SACNOne.Last().Slots[0] == 12);
		}
		TestTrue(TEXT("sACN universe 2"), FramesOf(SACNAll, 2).Num() >= 1);

		// Universe 2 is dropped: its sACN stream is terminated, then it goes quiet.
		Sender.SetUniverses(TArray<int32>{ 1 });
		const TArray<FStageDMXNetFrame> AfterDrop = SACN.Collect(0.5);
		TestEqual(TEXT("three terminations for universe 2"), FramesOf(AfterDrop, 2, /*bTerminated*/ true).Num(), 3);
		const int32 TerminatedAt = AfterDrop.IndexOfByPredicate([](const FStageDMXNetFrame& Frame) { return Frame.WireUniverse == 2 && Frame.bTerminated; });
		bool bQuietAfter = true;
		for (int32 Index = TerminatedAt + 3; TerminatedAt != INDEX_NONE && Index < AfterDrop.Num(); ++Index)
		{
			bQuietAfter &= AfterDrop[Index].WireUniverse != 2;
		}
		TestTrue(TEXT("universe 2 not kept alive after"), bQuietAfter);

		const FStageDMXOutputStatus Status = Sender.GetStatus();
		TestTrue(TEXT("status: sending, counted"), Status.State == EStageDMXOutputState::Sending && Status.ArtNetPackets > 0 && Status.SACNPackets > 0);
		TestEqual(TEXT("status: universes"), Status.Universes, TArray<int32>{ 1 });
		TestEqual(TEXT("no send errors over loopback"), Status.SendErrors, static_cast<int64>(0));

		// Close terminates what is still sent.
		Sender.Close();
		TestEqual(TEXT("Close terminates universe 1"), FramesOf(SACN.Collect(0.3), 1, /*bTerminated*/ true).Num(), 3);
		TestFalse(TEXT("closed"), Sender.IsOpen());
		Sender.Close(); // Idempotent.
	}

	// The receiver ignores sACN carrying this PC's own CID (multicast loops back), but still takes Art-Net.
	{
		FStageDMXNetworkReceiver Receiver;
		FStageDMXReceiverConfig ReceiverConfig;
		ReceiverConfig.ArtNetPort = TestArtNetPort;
		ReceiverConfig.SACNPort = TestSACNPort;
		ReceiverConfig.Settings.bReplyToArtPoll = false;
		ReceiverConfig.IgnoredSource = Config.Settings.Cid;
		FText Error;
		if (!TestTrue(TEXT("receiver opens"), Receiver.Start(ReceiverConfig, []() {}, Error)))
		{
			return false;
		}
		FStageDMXSenderConfig SACNOnly = Config;
		SACNOnly.Settings.ArtNet = EStageArtNetOutputMode::Off;
		TestTrue(TEXT("sender opens again"), Sender.Open(SACNOnly, Error));
		Sender.SetUniverses(TArray<int32>{ 1 });
		Sender.Post(1, MakeSlots(99, 98));
		const double Deadline = FPlatformTime::Seconds() + 1.0;
		while (Receiver.GetStatus().SACNPackets == 0 && FPlatformTime::Seconds() < Deadline)
		{
			FPlatformProcess::Sleep(0.01f);
		}
		int32 Drained = 0;
		Receiver.Drain([&Drained](int32, TConstArrayView<uint8>) { ++Drained; });
		const FStageDMXInputStatus InputStatus = Receiver.GetStatus();
		TestTrue(TEXT("our sACN arrived"), InputStatus.SACNPackets > 0);
		TestTrue(TEXT("and was ignored"), Drained == 0 && InputStatus.IgnoredFrames > 0);
		Sender.Close();
		Receiver.Stop();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDMXOutputShowTest, "StageCraft.DMX.OutputShow", StageDMXOutputTests::Flags)

bool FStageDMXOutputShowTest::RunTest(const FString& Parameters)
{
	using namespace StageDMXOutputTests;

	FScopedWorld Scope;
	UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(Scope.World);
	UStageDMXPatchSubsystem* Patching = UWorld::GetSubsystem<UStageDMXPatchSubsystem>(Scope.World);
	if (!TestNotNull(TEXT("show control"), Show) || !TestNotNull(TEXT("patch"), Patching))
	{
		return false;
	}
	ULightingFixtureData* Data = NewObject<ULightingFixtureData>(GetTransientPackage());
	ALightingFixtureActor* Lit = Scope.SpawnFixture(Data);
	Scope.SpawnFixture(Data);
	TestEqual(TEXT("both in universe 1"), Patching->GetPatchedUniverses(), TArray<int32>{ 1 });
	// One fixture at full from the programmer, so a Grand Master change has something to dim.
	if (UStageProgrammerSubsystem* Programmer = UWorld::GetSubsystem<UStageProgrammerSubsystem>(Scope.World))
	{
		Programmer->SetValues(Lit->GetFixtureId(), { { StageCraftTags::Attribute_Dimmer, 1.f } });
	}
	TestTrue(TEXT("lit"), FMath::IsNearlyEqual(Lit->GetAttribute(StageCraftTags::Attribute_Dimmer), 1.f, 0.01f));

	// No transport: nothing to send, so an idle show never ticks for output.
	Show->Tick(0.1f);
	TestFalse(TEXT("idle without output"), Show->IsTickable());

	FScopedListener ArtNet(TestArtNetPort);
	UStageNetworkDMXBridge* Bridge = NewObject<UStageNetworkDMXBridge>(Show);
	FStageDMXSenderConfig Config = MakeLoopbackConfig(10.0); // Long keep-alive: every frame seen here is a render.
	Config.Settings.SACN = EStageSACNOutputMode::Off;
	FText Error;
	if (!TestTrue(TEXT("bridge output opens"), Bridge->OpenOutput(Config, Error)) || !TestTrue(TEXT("listener"), ArtNet.Socket != nullptr))
	{
		return false;
	}
	Show->SetDMXBridge(Bridge);
	Bridge->SetOutputUniverses(Patching->GetPatchedUniverses());

	// The first frame after the patch was seen goes out once; then the show is idle again.
	Show->RefreshDMXOutput();
	TestTrue(TEXT("pending output ticks"), Show->IsTickable());
	Show->Tick(0.1f);
	TestFalse(TEXT("idle once sent"), Show->IsTickable());
	TArray<FStageDMXNetFrame> Frames = ArtNet.Collect(0.3);
	if (TestEqual(TEXT("one frame for universe 1"), FramesOf(Frames, 0).Num(), 1))
	{
		const TArray<uint8> Expected = Show->RenderDMXUniverse(1);
		TestTrue(TEXT("the rendered universe"), FMemory::Memcmp(Frames[0].Slots.GetData(), Expected.GetData(), 512) == 0);
	}
	Show->Tick(0.1f);
	TestEqual(TEXT("nothing more while idle"), ArtNet.Collect(0.2).Num(), 0);

	// A masters change re-mixes every fixture: their universe goes out once more, with the new look.
	Show->SetGrandMaster(0.5f);
	TestTrue(TEXT("a change ticks"), Show->IsTickable());
	Show->Tick(0.1f);
	Frames = ArtNet.Collect(0.3);
	if (TestEqual(TEXT("one frame after the change"), Frames.Num(), 1))
	{
		const TArray<uint8> Expected = Show->RenderDMXUniverse(1);
		TestTrue(TEXT("with the dimmed look"), FMemory::Memcmp(Frames[0].Slots.GetData(), Expected.GetData(), 512) == 0);
	}

	// External: the show follows DMX IN and renders nothing for output.
	Show->SetControlSource(EStageControlSource::External);
	Show->SetGrandMaster(1.f);
	Show->RefreshDMXOutput();
	TestFalse(TEXT("External does not tick for output"), Show->IsTickable());
	Show->Tick(0.1f);
	TestEqual(TEXT("External sends nothing"), ArtNet.Collect(0.2).Num(), 0);
	Show->SetControlSource(EStageControlSource::Internal);

	Show->SetDMXBridge(nullptr); // Stops the bridge, which closes its output.
	TestFalse(TEXT("closed with the bridge"), Bridge->IsSending());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
