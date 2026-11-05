// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageDMXNetworkSender.h"

#include "Common/UdpSocketBuilder.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "IPAddress.h"
#include "ModularSceneBuilder.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

#define LOCTEXT_NAMESPACE "StageDMXNetworkSender"

namespace
{
	/** Room for several hundred full frames while the network card is busy. */
	constexpr int32 SendBufferBytes = 1 << 20;

	/** A receiver on the next router hop (a node behind a switch that routes) still sees multicast. */
	constexpr uint8 MulticastTtl = 16;

	/** E1.31 §6.7.1: a source that leaves sends three packets with Stream_Terminated set. */
	constexpr int32 TerminationPackets = 3;

	/** The longest the thread sleeps without being woken: bounds how late a keep-alive can be. */
	constexpr double MaxWaitSeconds = 0.25;

	uint8 NextArtNetSequence(uint8 Sequence)
	{
		// 0 means "not sequenced" in ArtDmx, so a sequencing sender counts 1..255.
		return Sequence >= 255 ? 1 : Sequence + 1;
	}
}

FStageDMXNetworkSender::FStageDMXNetworkSender() = default;

FStageDMXNetworkSender::~FStageDMXNetworkSender()
{
	Close();
}

bool FStageDMXNetworkSender::Open(const FStageDMXSenderConfig& InConfig, FText& OutError)
{
	Close();
	Config = InConfig;
	Config.Settings = StageDMXOutput::Sanitize(InConfig.Settings);
	Config.Numbering = StageDMXInput::Sanitize(InConfig.Numbering);
	Config.KeepAliveSeconds = FMath::Clamp(InConfig.KeepAliveSeconds, 0.01, 10.0);
	if (!Config.Settings.Cid.IsValid())
	{
		Config.Settings.Cid = FGuid::NewGuid(); // Only without user settings (tests, commandlets).
	}

	const FStageDMXOutputSettings& Settings = Config.Settings;
	if (Settings.ArtNet == EStageArtNetOutputMode::Off && Settings.SACN == EStageSACNOutputMode::Off)
	{
		OutError = LOCTEXT("NoProtocol", "Neither Art-Net nor sACN is switched on for DMX output.");
		return false;
	}
	const bool bUnicast = Settings.ArtNet == EStageArtNetOutputMode::Unicast || Settings.SACN == EStageSACNOutputMode::Unicast;
	FIPv4Address UnicastIp;
	if (bUnicast && !FIPv4Address::Parse(Settings.UnicastAddress, UnicastIp))
	{
		OutError = LOCTEXT("NoUnicastAddress", "DMX output is set to unicast but has no valid IPv4 address to send to.");
		return false;
	}

	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!Sockets)
	{
		OutError = LOCTEXT("NoSockets", "This platform has no network sockets.");
		return false;
	}

	// Bound to the chosen card (any free port), so broadcast and multicast leave through it; blocking, because only this
	// sender's own thread ever waits on it.
	FIPv4Address BindIp = FIPv4Address::Any;
	const bool bOnCard = !Config.Numbering.BindAddress.IsEmpty() && FIPv4Address::Parse(Config.Numbering.BindAddress, BindIp);
	FUdpSocketBuilder Builder(TEXT("StageCraft DMX Out"));
	Builder.AsBlocking()
		.BoundToAddress(BindIp)
		.BoundToPort(0)
		.WithBroadcast()
		.WithSendBufferSize(SendBufferBytes)
		.WithMulticastTtl(MulticastTtl)
		.WithMulticastLoopback(); // A visualiser on this PC sees the output too.
	if (bOnCard)
	{
		Builder.WithMulticastInterface(BindIp);
	}
	Socket = Builder.Build();
	if (!Socket)
	{
		OutError = FText::Format(LOCTEXT("SocketFailed", "DMX output could not open a network socket{0}. The network card may not be available."),
			bOnCard ? FText::FromString(TEXT(" on ") + Config.Numbering.BindAddress) : FText::GetEmpty());
		UE_LOG(LogStageCraft, Warning, TEXT("DMX output: %s"), *OutError.ToString());
		return false;
	}

	TArray<FString> Parts;
	if (Settings.ArtNet != EStageArtNetOutputMode::Off)
	{
		ArtNetDestination = Sockets->CreateInternetAddr();
		if (Settings.ArtNet == EStageArtNetOutputMode::Broadcast)
		{
			// The limited broadcast, leaving through the bound card; IPv4 explicitly (SetBroadcastAddress follows the default protocol).
			ArtNetDestination->SetIp(0xFFFFFFFFu);
			Parts.Add(TEXT("Art-Net broadcast"));
		}
		else
		{
			ArtNetDestination->SetIp(UnicastIp.Value);
			Parts.Add(TEXT("Art-Net to ") + Settings.UnicastAddress);
		}
		ArtNetDestination->SetPort(Config.ArtNetPort);
	}
	if (Settings.SACN != EStageSACNOutputMode::Off)
	{
		if (Settings.SACN == EStageSACNOutputMode::Unicast)
		{
			SACNUnicastDestination = Sockets->CreateInternetAddr();
			SACNUnicastDestination->SetIp(UnicastIp.Value);
			SACNUnicastDestination->SetPort(Config.SACNPort);
			Parts.Add(TEXT("sACN to ") + Settings.UnicastAddress);
		}
		else
		{
			Parts.Add(TEXT("sACN multicast"));
		}
	}
	Scratch = Sockets->CreateInternetAddr();
	Target = FString::Join(Parts, TEXT(", ")) + (bOnCard ? TEXT(" from ") + Config.Numbering.BindAddress : FString());

	bStopping = false;
	WakeEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset*/ false);
	Thread.Reset(FRunnableThread::Create(this, TEXT("StageCraft DMX Out"), 0, TPri_AboveNormal));
	if (!Thread)
	{
		OutError = LOCTEXT("NoThread", "DMX output could not start its thread.");
		Close();
		return false;
	}
	UE_LOG(LogStageCraft, Log, TEXT("DMX output: sending, %s (Art-Net %d = universe 1, sACN %d = universe 1, priority %d)."),
		*Target, Config.Numbering.ArtNetFirstPortAddress, Config.Numbering.SACNFirstUniverse, Settings.SACNPriority);
	return true;
}

void FStageDMXNetworkSender::Close()
{
	if (Thread)
	{
		Stop();
		Thread->WaitForCompletion(); // Run sends the stream terminations on its way out.
		Thread.Reset();
		UE_LOG(LogStageCraft, Log, TEXT("DMX output: stopped (%lld Art-Net, %lld sACN packets, %lld send errors)."),
			ArtNetPackets.load(), SACNPackets.load(), SendErrors.load());
	}
	if (WakeEvent)
	{
		FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
		WakeEvent = nullptr;
	}
	CloseSocket();
	{
		FScopeLock Guard(&Lock);
		Pending.Reset();
		Universes.Reset();
		bUniversesChanged = false;
	}
	Streams.Reset();
	ArtNetDestination.Reset();
	SACNUnicastDestination.Reset();
	Scratch.Reset();
	Target.Reset();
	ArtNetPackets = 0;
	SACNPackets = 0;
	SendErrors = 0;
}

void FStageDMXNetworkSender::Post(int32 Universe, TConstArrayView<uint8> Slots)
{
	if (!Thread)
	{
		return;
	}
	{
		FScopeLock Guard(&Lock);
		if (!Universes.Contains(Universe))
		{
			return;
		}
		TStaticArray<uint8, 512>& Frame = Pending.FindOrAdd(Universe);
		const int32 Count = FMath::Min(Slots.Num(), 512);
		FMemory::Memcpy(Frame.GetData(), Slots.GetData(), Count);
		FMemory::Memzero(Frame.GetData() + Count, 512 - Count);
	}
	WakeEvent->Trigger();
}

void FStageDMXNetworkSender::SetUniverses(TConstArrayView<int32> InUniverses)
{
	if (!Thread)
	{
		return;
	}
	{
		FScopeLock Guard(&Lock);
		TSet<int32> Wanted(InUniverses);
		if (Wanted.Num() == Universes.Num() && Wanted.Includes(Universes))
		{
			return;
		}
		Universes = MoveTemp(Wanted);
		for (auto It = Pending.CreateIterator(); It; ++It)
		{
			if (!Universes.Contains(It.Key()))
			{
				It.RemoveCurrent();
			}
		}
		bUniversesChanged = true;
	}
	WakeEvent->Trigger();
}

FStageDMXOutputStatus FStageDMXNetworkSender::GetStatus() const
{
	FStageDMXOutputStatus Status;
	Status.State = Thread ? EStageDMXOutputState::Sending : EStageDMXOutputState::Off;
	Status.Target = Target;
	Status.ArtNetPackets = ArtNetPackets.load();
	Status.SACNPackets = SACNPackets.load();
	Status.SendErrors = SendErrors.load();
	{
		FScopeLock Guard(&Lock);
		Status.Universes = Universes.Array();
	}
	Status.Universes.Sort();
	return Status;
}

uint32 FStageDMXNetworkSender::Run()
{
	TArray<TPair<int32, TStaticArray<uint8, 512>>> Outgoing;
	TSet<int32> Wanted;
	while (!bStopping)
	{
		bool bApplyUniverses = false;
		Outgoing.Reset();
		{
			FScopeLock Guard(&Lock);
			for (TPair<int32, TStaticArray<uint8, 512>>& Frame : Pending)
			{
				Outgoing.Emplace(Frame.Key, Frame.Value);
			}
			Pending.Reset();
			if (bUniversesChanged)
			{
				Wanted = Universes;
				bUniversesChanged = false;
				bApplyUniverses = true;
			}
		}

		const double Now = FPlatformTime::Seconds();
		if (bApplyUniverses)
		{
			// A universe that is no longer sent leaves cleanly, so a receiver releases it at once instead of after its timeout.
			for (auto It = Streams.CreateIterator(); It; ++It)
			{
				if (!Wanted.Contains(It.Key()))
				{
					SendSACNTermination(It.Key(), It.Value());
					It.RemoveCurrent();
				}
			}
		}
		for (TPair<int32, TStaticArray<uint8, 512>>& Frame : Outgoing)
		{
			FUniverseStream& Stream = Streams.FindOrAdd(Frame.Key);
			Stream.Slots = Frame.Value;
			SendFrame(Frame.Key, Stream, Now);
		}

		// Keep-alive: unchanged universes again once their interval is up; the earliest next one decides how long to sleep.
		double NextDue = Now + MaxWaitSeconds;
		for (TPair<int32, FUniverseStream>& Stream : Streams)
		{
			const double Due = Stream.Value.LastSentSeconds + Config.KeepAliveSeconds;
			if (Due <= Now)
			{
				SendFrame(Stream.Key, Stream.Value, Now);
				NextDue = FMath::Min(NextDue, Now + Config.KeepAliveSeconds);
			}
			else
			{
				NextDue = FMath::Min(NextDue, Due);
			}
		}

		const uint32 WaitMs = static_cast<uint32>(FMath::Clamp((NextDue - FPlatformTime::Seconds()) * 1000.0, 1.0, MaxWaitSeconds * 1000.0));
		WakeEvent->Wait(WaitMs);
	}

	for (TPair<int32, FUniverseStream>& Stream : Streams)
	{
		SendSACNTermination(Stream.Key, Stream.Value);
	}
	return 0;
}

void FStageDMXNetworkSender::Stop()
{
	bStopping = true;
	if (WakeEvent)
	{
		WakeEvent->Trigger();
	}
}

void FStageDMXNetworkSender::SendFrame(int32 Universe, FUniverseStream& Stream, double NowSeconds)
{
	Stream.LastSentSeconds = NowSeconds;
	const TConstArrayView<uint8> Slots(Stream.Slots.GetData(), 512);

	if (ArtNetDestination.IsValid())
	{
		const int32 PortAddress = StageDMXInput::ToArtNet(Universe, Config.Numbering);
		if (PortAddress != INDEX_NONE)
		{
			Stream.ArtNetSequence = NextArtNetSequence(Stream.ArtNetSequence);
			if (SendTo(StageDMXNet::BuildArtDmx(PortAddress, Stream.ArtNetSequence, Slots), *ArtNetDestination))
			{
				++ArtNetPackets;
			}
		}
	}

	if (Config.Settings.SACN != EStageSACNOutputMode::Off)
	{
		const int32 Wire = StageDMXInput::ToSACN(Universe, Config.Numbering);
		if (Wire != INDEX_NONE)
		{
			const TArray<uint8> Packet = StageDMXNet::BuildSACNData(Wire, Config.Settings.Cid, static_cast<uint8>(Config.Settings.SACNPriority),
				Stream.SACNSequence++, Slots, /*bTerminated*/ false, Config.SourceName);
			if (SACNUnicastDestination.IsValid())
			{
				if (SendTo(Packet, *SACNUnicastDestination))
				{
					++SACNPackets;
				}
			}
			else
			{
				Scratch->SetIp(StageDMXNet::GetSACNMulticastAddress(Wire));
				Scratch->SetPort(Config.SACNPort);
				if (SendTo(Packet, *Scratch))
				{
					++SACNPackets;
				}
			}
		}
	}
}

void FStageDMXNetworkSender::SendSACNTermination(int32 Universe, FUniverseStream& Stream)
{
	const int32 Wire = Config.Settings.SACN != EStageSACNOutputMode::Off ? StageDMXInput::ToSACN(Universe, Config.Numbering) : INDEX_NONE;
	if (Wire == INDEX_NONE)
	{
		return;
	}
	if (!SACNUnicastDestination.IsValid())
	{
		Scratch->SetIp(StageDMXNet::GetSACNMulticastAddress(Wire));
		Scratch->SetPort(Config.SACNPort);
	}
	const FInternetAddr& Destination = SACNUnicastDestination.IsValid() ? *SACNUnicastDestination : *Scratch;
	for (int32 Repeat = 0; Repeat < TerminationPackets; ++Repeat)
	{
		const TArray<uint8> Packet = StageDMXNet::BuildSACNData(Wire, Config.Settings.Cid, static_cast<uint8>(Config.Settings.SACNPriority),
			Stream.SACNSequence++, TConstArrayView<uint8>(Stream.Slots.GetData(), 512), /*bTerminated*/ true, Config.SourceName);
		if (SendTo(Packet, Destination))
		{
			++SACNPackets;
		}
	}
}

bool FStageDMXNetworkSender::SendTo(const TArray<uint8>& Packet, const FInternetAddr& Destination)
{
	int32 Sent = 0;
	if (Socket && !Packet.IsEmpty() && Socket->SendTo(Packet.GetData(), Packet.Num(), Sent, Destination) && Sent == Packet.Num())
	{
		return true;
	}
	++SendErrors;
	return false;
}

void FStageDMXNetworkSender::CloseSocket()
{
	if (Socket)
	{
		Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
	}
}

#undef LOCTEXT_NAMESPACE
