// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the Art-Net / sACN packets and source rules (Docs/ADR/0019 §3): ArtDmx, ArtPoll and ArtPollReply, E1.31 data
// packets, rejection of every malformed field, sACN priority / timeout / sequence / termination, and universe numbering
// between the wire and StageCraft.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.DMX; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Show/StageDMXNetProtocol.h"
#include "Show/StageDMXNetworkTypes.h"

namespace StageDMXNetProtocolTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	const FGuid ConsoleA(1, 2, 3, 4);
	const FGuid ConsoleB(5, 6, 7, 8);

	EStageDMXNetPacket Parse(const TArray<uint8>& Packet, FStageDMXNetFrame& OutFrame)
	{
		return StageDMXNet::Parse(Packet, OutFrame);
	}

	EStageDMXNetPacket Parse(const TArray<uint8>& Packet)
	{
		FStageDMXNetFrame Frame;
		return StageDMXNet::Parse(Packet, Frame);
	}

	FStageDMXNetFrame MakeFrame(EStageDMXNetProtocol Protocol, const FGuid& Source, uint8 Priority, uint8 Sequence, bool bTerminated = false)
	{
		FStageDMXNetFrame Frame;
		Frame.Protocol = Protocol;
		Frame.Source = Source;
		Frame.Priority = Priority;
		Frame.Sequence = Sequence;
		Frame.bTerminated = bTerminated;
		Frame.SlotCount = 512;
		return Frame;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDMXArtNetTest, "StageCraft.DMX.Protocol.ArtNet", StageDMXNetProtocolTests::Flags)

bool FStageDMXArtNetTest::RunTest(const FString& Parameters)
{
	using namespace StageDMXNetProtocolTests;

	// Round trip; an odd slot count is padded to an even length.
	const TArray<uint8> Slots = { 255, 128, 7 };
	TArray<uint8> Packet = StageDMXNet::BuildArtDmx(0x0123, 9, Slots);
	FStageDMXNetFrame Frame;
	TestEqual(TEXT("ArtDmx"), Parse(Packet, Frame), EStageDMXNetPacket::ArtDmx);
	TestEqual(TEXT("port-address"), Frame.WireUniverse, 0x0123);
	TestEqual(TEXT("sequence"), static_cast<int32>(Frame.Sequence), 9);
	TestEqual(TEXT("padded length"), Frame.SlotCount, 4);
	TestTrue(TEXT("slots"), Frame.Slots[0] == 255 && Frame.Slots[1] == 128 && Frame.Slots[2] == 7 && Frame.Slots[3] == 0 && Frame.Slots[511] == 0);
	TestEqual(TEXT("Art-Net counts as priority 100"), static_cast<int32>(Frame.Priority), 100);

	// 15-bit port-address; the reserved top bit of Net is ignored.
	Packet = StageDMXNet::BuildArtDmx(32767, 0, Slots);
	Packet[15] |= 0x80;
	Parse(Packet, Frame);
	TestEqual(TEXT("highest port-address"), Frame.WireUniverse, 32767);

	// Malformed packets are invalid; other opcodes are ignored.
	TArray<uint8> Bad = StageDMXNet::BuildArtDmx(0, 0, Slots);
	Bad.SetNum(10);
	TestEqual(TEXT("truncated"), Parse(Bad), EStageDMXNetPacket::Invalid);
	Bad = StageDMXNet::BuildArtDmx(0, 0, Slots);
	Bad[0] = 'X';
	TestEqual(TEXT("wrong ID"), Parse(Bad), EStageDMXNetPacket::Invalid);
	Bad = StageDMXNet::BuildArtDmx(0, 0, Slots);
	Bad[11] = 13;
	TestEqual(TEXT("protocol 13"), Parse(Bad), EStageDMXNetPacket::Invalid);
	Bad = StageDMXNet::BuildArtDmx(0, 0, Slots);
	Bad[16] = 0;
	Bad[17] = 0;
	TestEqual(TEXT("length 0"), Parse(Bad), EStageDMXNetPacket::Invalid);
	Bad = StageDMXNet::BuildArtDmx(0, 0, Slots);
	Bad[17] = 200;
	TestEqual(TEXT("length beyond the datagram"), Parse(Bad), EStageDMXNetPacket::Invalid);
	Bad = StageDMXNet::BuildArtDmx(0, 0, TArray<uint8>());
	Bad[16] = 0x02;
	Bad[17] = 0x58; // 600
	Bad.SetNumZeroed(18 + 600);
	TestEqual(TEXT("length above 512"), Parse(Bad), EStageDMXNetPacket::Invalid);
	Bad = StageDMXNet::BuildArtDmx(0, 0, Slots);
	Bad[9] = 0x52; // ArtSync
	TestEqual(TEXT("ArtSync ignored"), Parse(Bad), EStageDMXNetPacket::Ignored);
	TestEqual(TEXT("random datagram"), Parse(TArray<uint8>{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }), EStageDMXNetPacket::Invalid);
	TestEqual(TEXT("empty datagram"), Parse(TArray<uint8>()), EStageDMXNetPacket::Invalid);

	// ArtPoll.
	TArray<uint8> Poll = StageDMXNet::BuildArtPoll();
	TestEqual(TEXT("ArtPoll"), Parse(Poll), EStageDMXNetPacket::ArtPoll);
	Poll[11] = 13;
	TestEqual(TEXT("old ArtPoll"), Parse(Poll), EStageDMXNetPacket::Invalid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDMXArtPollReplyTest, "StageCraft.DMX.Protocol.ArtPollReply", StageDMXNetProtocolTests::Flags)

bool FStageDMXArtPollReplyTest::RunTest(const FString& Parameters)
{
	using namespace StageDMXNetProtocolTests;

	FStageArtPollReplyInfo Info;
	Info.IPv4 = (192u << 24) | (168u << 16) | (1u << 8) | 20u;
	Info.ShortName = TEXT("StageCraft");
	Info.LongName = TEXT("StageCraft stage visualiser with a name far longer than the sixty-four bytes Art-Net allows for it");
	Info.PortAddresses = { 16, 3, 0, 1, 2, 4, 2, -1, 40000 };
	Info.bReceiving = true;

	const TArray<TArray<uint8>> Replies = StageDMXNet::BuildArtPollReplies(Info);
	if (!TestEqual(TEXT("four ports per reply, one Sub-Net each"), Replies.Num(), 3))
	{
		return false;
	}
	for (int32 Index = 0; Index < Replies.Num(); ++Index)
	{
		const TArray<uint8>& Reply = Replies[Index];
		TestEqual(TEXT("size"), Reply.Num(), StageDMXNet::ArtPollReplySize);
		TestTrue(TEXT("OpPollReply, little-endian"), Reply[8] == 0x00 && Reply[9] == 0x21);
		TestTrue(TEXT("IP"), Reply[10] == 192 && Reply[11] == 168 && Reply[12] == 1 && Reply[13] == 20);
		TestTrue(TEXT("port 6454, little-endian"), Reply[14] == 0x36 && Reply[15] == 0x19);
		TestEqual(TEXT("bind index"), static_cast<int32>(Reply[211]), Index + 1);
		TestEqual(TEXT("long name ends in NUL"), static_cast<int32>(Reply[44 + 63]), 0);
		TestEqual(TEXT("our own reply is ignored, not shown"), Parse(Reply), EStageDMXNetPacket::Ignored);
	}
	TestEqual(TEXT("first: four ports"), static_cast<int32>(Replies[0][173]), 4);
	TestTrue(TEXT("first: universes 0-3"), Replies[0][190] == 0 && Replies[0][193] == 3);
	TestTrue(TEXT("output ports with data"), Replies[0][174] == 0x80 && Replies[0][182] == 0x80);
	TestEqual(TEXT("second: universe 4"), static_cast<int32>(Replies[1][190]), 4);
	TestTrue(TEXT("third: Sub-Net 1, universe 0"), Replies[2][19] == 1 && Replies[2][190] == 0 && Replies[2][173] == 1);
	TestTrue(TEXT("short name"), FMemory::Memcmp(Replies[0].GetData() + 26, "StageCraft", 10) == 0);

	Info.PortAddresses.Reset();
	const TArray<TArray<uint8>> Bare = StageDMXNet::BuildArtPollReplies(Info);
	TestTrue(TEXT("no ports: one reply with none"), Bare.Num() == 1 && Bare[0][173] == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDMXSACNTest, "StageCraft.DMX.Protocol.SACN", StageDMXNetProtocolTests::Flags)

bool FStageDMXSACNTest::RunTest(const FString& Parameters)
{
	using namespace StageDMXNetProtocolTests;

	TArray<uint8> Slots;
	Slots.SetNumZeroed(512);
	Slots[0] = 10;
	Slots[511] = 99;
	TArray<uint8> Packet = StageDMXNet::BuildSACNData(7, ConsoleA, 150, 42, Slots, false, TEXT("Console"));
	FStageDMXNetFrame Frame;
	TestEqual(TEXT("data packet"), Parse(Packet, Frame), EStageDMXNetPacket::SACNData);
	TestEqual(TEXT("universe"), Frame.WireUniverse, 7);
	TestEqual(TEXT("priority"), static_cast<int32>(Frame.Priority), 150);
	TestEqual(TEXT("sequence"), static_cast<int32>(Frame.Sequence), 42);
	TestEqual(TEXT("CID"), Frame.Source, ConsoleA);
	TestEqual(TEXT("slots"), Frame.SlotCount, 512);
	TestTrue(TEXT("slot values"), Frame.Slots[0] == 10 && Frame.Slots[511] == 99);
	TestFalse(TEXT("not terminated"), Frame.bTerminated);

	// A short frame is zero-filled.
	Parse(StageDMXNet::BuildSACNData(7, ConsoleA, 100, 1, TArray<uint8>{ 5, 6 }, false, TEXT("Console")), Frame);
	TestTrue(TEXT("short frame"), Frame.SlotCount == 2 && Frame.Slots[1] == 6 && Frame.Slots[2] == 0);

	Parse(StageDMXNet::BuildSACNData(7, ConsoleA, 100, 1, Slots, true, TEXT("Console")), Frame);
	TestTrue(TEXT("stream terminated"), Frame.bTerminated);

	const auto Corrupt = [&Slots](int32 Offset, uint8 Value)
	{
		TArray<uint8> Bad = StageDMXNet::BuildSACNData(7, ConsoleA, 100, 1, Slots, false, TEXT("Console"));
		Bad[Offset] = Value;
		return Parse(Bad);
	};
	TestEqual(TEXT("preamble"), Corrupt(1, 0x11), EStageDMXNetPacket::Invalid);
	TestEqual(TEXT("ACN identifier"), Corrupt(4, 'X'), EStageDMXNetPacket::Invalid);
	TestEqual(TEXT("root flags"), Corrupt(16, 0x00), EStageDMXNetPacket::Invalid);
	TestEqual(TEXT("extended root vector ignored"), Corrupt(21, 0x08), EStageDMXNetPacket::Ignored);
	TestEqual(TEXT("framing vector"), Corrupt(43, 0x01), EStageDMXNetPacket::Invalid);
	TestEqual(TEXT("priority above 200"), Corrupt(108, 201), EStageDMXNetPacket::Invalid);
	TestEqual(TEXT("universe 0"), Corrupt(114, 0), EStageDMXNetPacket::Invalid);
	TestEqual(TEXT("universe above 63999"), Corrupt(113, 0xFA), EStageDMXNetPacket::Invalid);
	TestEqual(TEXT("DMP address type"), Corrupt(118, 0xA0), EStageDMXNetPacket::Invalid);
	TestEqual(TEXT("property count above 513"), Corrupt(123, 0x03), EStageDMXNetPacket::Invalid);
	TestEqual(TEXT("per-address priority (0xDD) ignored"), Corrupt(125, 0xDD), EStageDMXNetPacket::Ignored);

	TArray<uint8> Truncated = Packet;
	Truncated.SetNum(126 + 100);
	TestEqual(TEXT("slots missing from the datagram"), Parse(Truncated), EStageDMXNetPacket::Invalid);
	Truncated.SetNum(100);
	TestEqual(TEXT("truncated header"), Parse(Truncated), EStageDMXNetPacket::Invalid);

	TestEqual(TEXT("multicast of universe 1"), StageDMXNet::GetSACNMulticastAddress(1), (239u << 24) | (255u << 16) | 1u);
	TestEqual(TEXT("multicast of universe 258"), StageDMXNet::GetSACNMulticastAddress(258), (239u << 24) | (255u << 16) | (1u << 8) | 2u);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDMXArbiterTest, "StageCraft.DMX.Protocol.Sources", StageDMXNetProtocolTests::Flags)

bool FStageDMXArbiterTest::RunTest(const FString& Parameters)
{
	using namespace StageDMXNetProtocolTests;
	const EStageDMXNetProtocol SACN = EStageDMXNetProtocol::SACN;

	FStageDMXSourceArbiter Arbiter;
	TestTrue(TEXT("first source"), Arbiter.Accept(1, MakeFrame(SACN, ConsoleA, 100, 10), 0.0));
	TestTrue(TEXT("next frame"), Arbiter.Accept(1, MakeFrame(SACN, ConsoleA, 100, 11), 0.1));
	TestFalse(TEXT("duplicate"), Arbiter.Accept(1, MakeFrame(SACN, ConsoleA, 100, 11), 0.2));
	TestFalse(TEXT("late frame"), Arbiter.Accept(1, MakeFrame(SACN, ConsoleA, 100, 6), 0.2));
	TestTrue(TEXT("far behind counts as a restart"), Arbiter.Accept(1, MakeFrame(SACN, ConsoleA, 100, 200), 0.3));
	TestTrue(TEXT("on to 250"), Arbiter.Accept(1, MakeFrame(SACN, ConsoleA, 100, 250), 0.35));
	TestTrue(TEXT("wraps past 255"), Arbiter.Accept(1, MakeFrame(SACN, ConsoleA, 100, 3), 0.4));
	TestFalse(TEXT("the frame before the wrap is late"), Arbiter.Accept(1, MakeFrame(SACN, ConsoleA, 100, 254), 0.45));

	// Equal priority from another console waits; higher takes over; then the lower one is ignored.
	TestFalse(TEXT("equal priority waits"), Arbiter.Accept(1, MakeFrame(SACN, ConsoleB, 100, 1), 0.5));
	TestTrue(TEXT("higher priority takes over"), Arbiter.Accept(1, MakeFrame(SACN, ConsoleB, 150, 2), 0.6));
	TestFalse(TEXT("lower priority ignored"), Arbiter.Accept(1, MakeFrame(SACN, ConsoleA, 100, 202), 0.7));
	TestEqual(TEXT("one live source"), Arbiter.GetLiveSourceCount(0.7), 1);

	// Silence for longer than the timeout hands the universe to whoever sends next.
	TestTrue(TEXT("after the timeout"), Arbiter.Accept(1, MakeFrame(SACN, ConsoleA, 100, 203), 0.6 + FStageDMXSourceArbiter::SourceTimeoutSeconds + 0.1));

	// Stream_Terminated releases at once; its own frame is not shown.
	TestTrue(TEXT("B back with priority"), Arbiter.Accept(2, MakeFrame(SACN, ConsoleB, 120, 1), 10.0));
	TestFalse(TEXT("termination is not shown"), Arbiter.Accept(2, MakeFrame(SACN, ConsoleB, 120, 2, true), 10.1));
	TestTrue(TEXT("A takes over at once"), Arbiter.Accept(2, MakeFrame(SACN, ConsoleA, 100, 1), 10.2));

	// Art-Net frames are not sequence-checked (0 = not sequenced).
	const FGuid ArtNetNode(9, 9, 9, 9);
	TestTrue(TEXT("Art-Net"), Arbiter.Accept(3, MakeFrame(EStageDMXNetProtocol::ArtNet, ArtNetNode, 100, 0), 10.3));
	TestTrue(TEXT("Art-Net repeat"), Arbiter.Accept(3, MakeFrame(EStageDMXNetProtocol::ArtNet, ArtNetNode, 100, 0), 10.4));

	TestEqual(TEXT("live universes"), Arbiter.GetLiveUniverses(10.4), TArray<int32>{ 2, 3 });
	TestEqual(TEXT("two live sources"), Arbiter.GetLiveSourceCount(10.4), 2);
	TestEqual(TEXT("all gone later"), Arbiter.GetLiveUniverses(20.0).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDMXNumberingTest, "StageCraft.DMX.Protocol.Numbering", StageDMXNetProtocolTests::Flags)

bool FStageDMXNumberingTest::RunTest(const FString& Parameters)
{
	FStageDMXInputSettings Settings;
	TestEqual(TEXT("Art-Net 0:0:0 is universe 1"), StageDMXInput::FromArtNet(0, Settings), 1);
	TestEqual(TEXT("Art-Net 0:0:15 is universe 16"), StageDMXInput::FromArtNet(15, Settings), 16);
	TestEqual(TEXT("universe 1 is Art-Net 0"), StageDMXInput::ToArtNet(1, Settings), 0);
	TestEqual(TEXT("no Art-Net for universe 40000"), StageDMXInput::ToArtNet(40000, Settings), INDEX_NONE);
	TestEqual(TEXT("sACN 1 is universe 1"), StageDMXInput::FromSACN(1, Settings), 1);
	TestEqual(TEXT("sACN 0 is nothing"), StageDMXInput::FromSACN(0, Settings), 0);
	TestEqual(TEXT("sACN 64000 is nothing"), StageDMXInput::FromSACN(64000, Settings), 0);
	TestEqual(TEXT("no sACN for universe 0"), StageDMXInput::ToSACN(0, Settings), INDEX_NONE);

	// Offsets: a console counting from 0:0:1, an sACN rig starting at 101.
	Settings.ArtNetFirstPortAddress = 1;
	Settings.SACNFirstUniverse = 101;
	TestEqual(TEXT("Art-Net 0 below the first"), StageDMXInput::FromArtNet(0, Settings), 0);
	TestEqual(TEXT("Art-Net 1 is universe 1"), StageDMXInput::FromArtNet(1, Settings), 1);
	TestEqual(TEXT("sACN 102 is universe 2"), StageDMXInput::FromSACN(102, Settings), 2);
	TestEqual(TEXT("universe 2 is sACN 102"), StageDMXInput::ToSACN(2, Settings), 102);

	// Saved values are repaired.
	FStageDMXInputSettings Damaged;
	Damaged.ArtNetFirstPortAddress = 40000;
	Damaged.SACNFirstUniverse = 0;
	Damaged.BindAddress = TEXT("not an address");
	FStageDMXInputSettings Sanitized = StageDMXInput::Sanitize(Damaged);
	TestEqual(TEXT("Art-Net first clamped"), Sanitized.ArtNetFirstPortAddress, 32767);
	TestEqual(TEXT("sACN first clamped"), Sanitized.SACNFirstUniverse, 1);
	TestTrue(TEXT("bad address means every card"), Sanitized.BindAddress.IsEmpty());
	Damaged.BindAddress = TEXT(" 192.168.0.5 ");
	TestEqual(TEXT("address trimmed"), StageDMXInput::Sanitize(Damaged).BindAddress, FString(TEXT("192.168.0.5")));
	TestFalse(TEXT("off by default"), FStageDMXInputSettings().bEnabled);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
