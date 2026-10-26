// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageDMXNetworkReceiver.h"

#include "Common/UdpSocketBuilder.h"
#include "Common/UdpSocketReceiver.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "IPAddress.h"
#include "ModularSceneBuilder.h"
#include "Serialization/ArrayReader.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

#define LOCTEXT_NAMESPACE "StageDMXNetworkReceiver"

namespace
{
	/** Room for a few hundred full frames per socket while the thread is busy. */
	constexpr int32 ReceiveBufferBytes = 1 << 20;

	/** How long a socket thread waits for data before checking whether it should stop: bounds how long Stop blocks. */
	constexpr double SocketWaitSeconds = 0.1;

	/** At most ten ArtPollReply rounds a second, however many polls arrive: a poll flood must not turn us into a sender. */
	constexpr double MinArtPollReplyIntervalSeconds = 0.1;

	FGuid MakeArtNetSourceId(const FIPv4Endpoint& Sender)
	{
		// Art-Net has no source ID; the sender's address and port are what tells two consoles apart.
		return FGuid(Sender.Address.Value, Sender.Port, 0, 0x41524E54 /* "ARNT" */);
	}
}

// --- FStageDMXInbox ---

FStageDMXInbox::FStageDMXInbox(const FStageDMXInputSettings& InSettings, TFunction<void()> InScheduleDrain)
	: Settings(InSettings)
	, ScheduleDrain(MoveTemp(InScheduleDrain))
{
}

void FStageDMXInbox::HandleArtNetDatagram(const TSharedPtr<FArrayReader, ESPMode::ThreadSafe>& Data, const FIPv4Endpoint& Sender)
{
	if (!Data.IsValid())
	{
		return;
	}

	FStageDMXNetFrame Frame;
	const double Now = FPlatformTime::Seconds();
	switch (StageDMXNet::Parse(TConstArrayView<uint8>(Data->GetData(), Data->Num()), Frame))
	{
	case EStageDMXNetPacket::ArtDmx:
		++ArtNetPackets;
		Frame.Source = MakeArtNetSourceId(Sender);
		HandleFrame(Frame, Now);
		break;
	case EStageDMXNetPacket::ArtPoll:
		ReplyToArtPoll(Sender, Now);
		break;
	case EStageDMXNetPacket::Ignored:
		break;
	default:
		++InvalidPackets; // Including sACN arriving on the Art-Net port: wrong port, not shown.
		break;
	}
}

void FStageDMXInbox::HandleSACNDatagram(const TSharedPtr<FArrayReader, ESPMode::ThreadSafe>& Data, const FIPv4Endpoint& Sender)
{
	if (!Data.IsValid())
	{
		return;
	}

	FStageDMXNetFrame Frame;
	switch (StageDMXNet::Parse(TConstArrayView<uint8>(Data->GetData(), Data->Num()), Frame))
	{
	case EStageDMXNetPacket::SACNData:
		++SACNPackets;
		HandleFrame(Frame, FPlatformTime::Seconds());
		break;
	case EStageDMXNetPacket::Ignored:
		break;
	default:
		++InvalidPackets;
		break;
	}
}

void FStageDMXInbox::HandleFrame(const FStageDMXNetFrame& Frame, double NowSeconds)
{
	const int32 Universe = Frame.Protocol == EStageDMXNetProtocol::ArtNet
		? StageDMXInput::FromArtNet(Frame.WireUniverse, Settings)
		: StageDMXInput::FromSACN(Frame.WireUniverse, Settings);
	if (Universe <= 0)
	{
		++IgnoredFrames;
		return;
	}

	bool bSchedule = false;
	{
		FScopeLock Guard(&Lock);
		if (!Arbiter.Accept(Universe, Frame, NowSeconds))
		{
			++IgnoredFrames;
			return;
		}
		Pending.FindOrAdd(Universe) = Frame.Slots;
		bSchedule = !bDrainScheduled;
		bDrainScheduled = true;
	}

	// Outside the lock: the owner only queues a game-thread task here.
	if (bSchedule && ScheduleDrain)
	{
		ScheduleDrain();
	}
}

void FStageDMXInbox::Drain(TFunctionRef<void(int32 Universe, TConstArrayView<uint8> Slots)> Visit)
{
	TMap<int32, TStaticArray<uint8, 512>> Frames;
	{
		FScopeLock Guard(&Lock);
		Frames = MoveTemp(Pending);
		Pending.Reset();
		bDrainScheduled = false;
	}

	// Outside the lock, so applying frames to fixtures never holds up the socket threads.
	Frames.KeySort(TLess<int32>());
	for (const TPair<int32, TStaticArray<uint8, 512>>& Frame : Frames)
	{
		Visit(Frame.Key, TConstArrayView<uint8>(Frame.Value.GetData(), Frame.Value.Num()));
	}
}

void FStageDMXInbox::SetArtPollReply(FSocket* InArtNetSocket, const FStageArtPollReplyInfo& Info)
{
	TArray<TArray<uint8>> Replies = Settings.bReplyToArtPoll && InArtNetSocket ? StageDMXNet::BuildArtPollReplies(Info) : TArray<TArray<uint8>>();
	FScopeLock Guard(&Lock);
	ArtNetSocket = InArtNetSocket;
	ArtPollReplies = MoveTemp(Replies);
}

void FStageDMXInbox::ReplyToArtPoll(const FIPv4Endpoint& Sender, double NowSeconds)
{
	FScopeLock Guard(&Lock);
	if (!ArtNetSocket || ArtPollReplies.IsEmpty() || NowSeconds - LastArtPollReplySeconds < MinArtPollReplyIntervalSeconds)
	{
		return;
	}
	LastArtPollReplySeconds = NowSeconds;

	// Straight back to the poller (Art-Net 4 allows a unicast reply); consoles poll from their Art-Net port.
	const TSharedRef<FInternetAddr> Destination = Sender.ToInternetAddr();
	for (const TArray<uint8>& Reply : ArtPollReplies)
	{
		int32 Sent = 0;
		ArtNetSocket->SendTo(Reply.GetData(), Reply.Num(), Sent, *Destination);
	}
}

FStageDMXInputStatus FStageDMXInbox::GetStatus(double NowSeconds) const
{
	FStageDMXInputStatus Status;
	Status.ArtNetPackets = ArtNetPackets.load();
	Status.SACNPackets = SACNPackets.load();
	Status.InvalidPackets = InvalidPackets.load();
	Status.IgnoredFrames = IgnoredFrames.load();

	FScopeLock Guard(&Lock);
	Status.LiveUniverses = Arbiter.GetLiveUniverses(NowSeconds);
	Status.LiveSources = Arbiter.GetLiveSourceCount(NowSeconds);
	return Status;
}

// --- FStageDMXNetworkReceiver ---

FStageDMXNetworkReceiver::FStageDMXNetworkReceiver() = default;

FStageDMXNetworkReceiver::~FStageDMXNetworkReceiver()
{
	Stop();
}

bool FStageDMXNetworkReceiver::Start(const FStageDMXReceiverConfig& InConfig, TFunction<void()> ScheduleDrain, FText& OutError)
{
	Stop();
	Config = InConfig;
	Config.Settings = StageDMXInput::Sanitize(InConfig.Settings);
	if (!Config.Settings.bArtNet && !Config.Settings.bSACN)
	{
		OutError = LOCTEXT("NoProtocol", "Neither Art-Net nor sACN is switched on in the DMX input settings.");
		return false;
	}

	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!Sockets)
	{
		OutError = LOCTEXT("NoSockets", "This platform has no network sockets.");
		return false;
	}

	// The address ArtPollReply announces: the chosen card, or the one the OS prefers.
	FIPv4Address BindIp = FIPv4Address::Any;
	if (!Config.Settings.BindAddress.IsEmpty())
	{
		FIPv4Address::Parse(Config.Settings.BindAddress, BindIp);
		LocalIPv4 = BindIp.Value;
	}
	else
	{
		bool bCanBindAll = false;
		uint32 HostIp = 0;
		Sockets->GetLocalHostAddr(*GLog, bCanBindAll)->GetIp(HostIp);
		LocalIPv4 = HostIp;
	}

	if (Config.Settings.bArtNet)
	{
		ArtNetSocket = OpenSocket(TEXT("StageCraft Art-Net"), Config.ArtNetPort, /*bBindToCard*/ true, OutError);
		if (!ArtNetSocket)
		{
			Stop();
			return false;
		}
	}
	if (Config.Settings.bSACN)
	{
		// sACN binds to every card and joins its multicast groups on the chosen one (SetUniverses): on Windows a socket bound to
		// one card's address does not receive multicast.
		SACNSocket = OpenSocket(TEXT("StageCraft sACN"), Config.SACNPort, /*bBindToCard*/ false, OutError);
		if (!SACNSocket)
		{
			Stop();
			return false;
		}
	}

	Inbox = MakeShared<FStageDMXInbox, ESPMode::ThreadSafe>(Config.Settings, MoveTemp(ScheduleDrain));
	const FTimespan WaitTime = FTimespan::FromSeconds(SocketWaitSeconds);
	if (ArtNetSocket)
	{
		ArtNetReceiver = MakeUnique<FUdpSocketReceiver>(ArtNetSocket, WaitTime, TEXT("StageCraft Art-Net In"));
		ArtNetReceiver->OnDataReceived().BindThreadSafeSP(Inbox.ToSharedRef(), &FStageDMXInbox::HandleArtNetDatagram);
		Inbox->SetArtPollReply(ArtNetSocket, MakeArtPollReply({}));
		ArtNetReceiver->Start();
	}
	if (SACNSocket)
	{
		SACNReceiver = MakeUnique<FUdpSocketReceiver>(SACNSocket, WaitTime, TEXT("StageCraft sACN In"));
		SACNReceiver->OnDataReceived().BindThreadSafeSP(Inbox.ToSharedRef(), &FStageDMXInbox::HandleSACNDatagram);
		SACNReceiver->Start();
	}

	TArray<FString> Parts;
	if (ArtNetSocket)
	{
		Parts.Add(FString::Printf(TEXT("Art-Net %d"), Config.ArtNetPort));
	}
	if (SACNSocket)
	{
		Parts.Add(FString::Printf(TEXT("sACN %d"), Config.SACNPort));
	}
	Endpoint = FString::Printf(TEXT("%s on %s"), *FString::Join(Parts, TEXT(", ")),
		Config.Settings.BindAddress.IsEmpty() ? TEXT("every network card") : *Config.Settings.BindAddress);
	UE_LOG(LogStageCraft, Log, TEXT("DMX input: listening, %s (Art-Net %d = universe 1, sACN %d = universe 1)."),
		*Endpoint, Config.Settings.ArtNetFirstPortAddress, Config.Settings.SACNFirstUniverse);
	return true;
}

void FStageDMXNetworkReceiver::Stop()
{
	// Threads first (each joins in its destructor), sockets after: a callback must never run on a destroyed socket.
	if (ArtNetReceiver)
	{
		ArtNetReceiver->Stop();
	}
	if (SACNReceiver)
	{
		SACNReceiver->Stop();
	}
	ArtNetReceiver.Reset();
	SACNReceiver.Reset();

	if (Inbox)
	{
		Inbox->SetArtPollReply(nullptr, FStageArtPollReplyInfo());
		UE_LOG(LogStageCraft, Log, TEXT("DMX input: stopped."));
	}
	JoinedSACNUniverses.Reset();
	CloseSocket(ArtNetSocket);
	CloseSocket(SACNSocket);
	Inbox.Reset();
	Endpoint.Reset();
}

FSocket* FStageDMXNetworkReceiver::OpenSocket(const TCHAR* Name, uint16 Port, bool bBindToCard, FText& OutError) const
{
	FIPv4Address BindIp = FIPv4Address::Any;
	if (bBindToCard && !Config.Settings.BindAddress.IsEmpty())
	{
		FIPv4Address::Parse(Config.Settings.BindAddress, BindIp);
	}

	// Reusable, as other DMX tools on this PC (a visualiser, a node utility) usually hold these ports too: broadcast and
	// multicast frames reach every socket bound to the port.
	FSocket* Socket = FUdpSocketBuilder(Name)
		.AsNonBlocking()
		.AsReusable()
		.BoundToAddress(BindIp)
		.BoundToPort(Port)
		.WithBroadcast()
		.WithReceiveBufferSize(ReceiveBufferBytes)
		.Build();
	if (!Socket)
	{
		OutError = FText::Format(LOCTEXT("PortBusy", "{0} could not listen on port {1}{2}. Another program may hold the port, or the network card is not available."),
			FText::FromString(Name), FText::AsNumber(Port, &FNumberFormattingOptions::DefaultNoGrouping()),
			bBindToCard && !Config.Settings.BindAddress.IsEmpty() ? FText::FromString(TEXT(" of ") + Config.Settings.BindAddress) : FText::GetEmpty());
		UE_LOG(LogStageCraft, Warning, TEXT("DMX input: %s"), *OutError.ToString());
	}
	return Socket;
}

void FStageDMXNetworkReceiver::CloseSocket(FSocket*& Socket) const
{
	if (Socket)
	{
		Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
	}
}

void FStageDMXNetworkReceiver::Drain(TFunctionRef<void(int32 Universe, TConstArrayView<uint8> Slots)> Visit)
{
	if (Inbox)
	{
		Inbox->Drain(Visit);
	}
}

void FStageDMXNetworkReceiver::SetUniverses(TConstArrayView<int32> Universes)
{
	if (!Inbox)
	{
		return;
	}

	if (SACNSocket)
	{
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		TSharedRef<FInternetAddr> Interface = Sockets->CreateInternetAddr();
		Interface->SetIp(LocalIPv4);
		const bool bOnCard = !Config.Settings.BindAddress.IsEmpty();

		TSet<int32> Wanted;
		for (const int32 Universe : Universes)
		{
			const int32 Wire = StageDMXInput::ToSACN(Universe, Config.Settings);
			if (Wire != INDEX_NONE)
			{
				Wanted.Add(Wire);
			}
		}

		const auto MakeGroup = [Sockets](int32 Wire)
		{
			TSharedRef<FInternetAddr> Group = Sockets->CreateInternetAddr();
			Group->SetIp(StageDMXNet::GetSACNMulticastAddress(Wire));
			return Group;
		};
		for (const int32 Wire : JoinedSACNUniverses.Difference(Wanted))
		{
			if (bOnCard)
			{
				SACNSocket->LeaveMulticastGroup(*MakeGroup(Wire), *Interface);
			}
			else
			{
				SACNSocket->LeaveMulticastGroup(*MakeGroup(Wire));
			}
		}
		for (const int32 Wire : Wanted.Difference(JoinedSACNUniverses))
		{
			const bool bJoined = bOnCard ? SACNSocket->JoinMulticastGroup(*MakeGroup(Wire), *Interface) : SACNSocket->JoinMulticastGroup(*MakeGroup(Wire));
			if (!bJoined)
			{
				UE_LOG(LogStageCraft, Warning, TEXT("DMX input: could not join the sACN multicast group of universe %d; unicast sACN still arrives."), Wire);
			}
		}
		JoinedSACNUniverses = MoveTemp(Wanted);
	}

	if (ArtNetSocket)
	{
		Inbox->SetArtPollReply(ArtNetSocket, MakeArtPollReply(Universes));
	}
}

FStageArtPollReplyInfo FStageDMXNetworkReceiver::MakeArtPollReply(TConstArrayView<int32> Universes) const
{
	FStageArtPollReplyInfo Info;
	Info.IPv4 = LocalIPv4;
	Info.ShortName = Config.NodeName;
	Info.LongName = Config.NodeName + TEXT(" stage visualiser");
	Info.bReceiving = true;
	for (const int32 Universe : Universes)
	{
		const int32 PortAddress = StageDMXInput::ToArtNet(Universe, Config.Settings);
		if (PortAddress != INDEX_NONE)
		{
			Info.PortAddresses.Add(PortAddress);
		}
	}
	return Info;
}

FStageDMXInputStatus FStageDMXNetworkReceiver::GetStatus() const
{
	FStageDMXInputStatus Status = Inbox ? Inbox->GetStatus(FPlatformTime::Seconds()) : FStageDMXInputStatus();
	Status.State = Inbox ? EStageDMXInputState::Listening : EStageDMXInputState::Off;
	Status.Endpoint = Endpoint;
	return Status;
}

#undef LOCTEXT_NAMESPACE
