// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageDMXNetProtocol.h"

namespace
{
	constexpr uint8 ArtNetId[8] = { 'A', 'r', 't', '-', 'N', 'e', 't', 0 };
	constexpr uint16 OpPoll = 0x2000;
	constexpr uint16 OpPollReply = 0x2100;
	constexpr uint16 OpDmx = 0x5000;
	constexpr uint8 ArtNetProtocolVersion = 14;
	constexpr int32 ArtDmxHeaderSize = 18;
	constexpr int32 ArtPollMinSize = 12;

	constexpr uint8 AcnPacketId[12] = { 'A', 'S', 'C', '-', 'E', '1', '.', '1', '7', 0, 0, 0 };
	constexpr uint32 VectorRootE131Data = 0x00000004;
	constexpr uint32 VectorE131DataPacket = 0x00000002;
	constexpr uint8 VectorDmpSetProperty = 0x02;
	constexpr uint8 DmpAddressAndDataType = 0xA1;
	constexpr int32 SACNHeaderSize = 126; // Up to and including the START code.
	constexpr int32 SACNSourceNameSize = 64;
	constexpr uint8 SACNOptionStreamTerminated = 0x40;
	constexpr uint8 SACNMaxPriority = 200;

	/** sACN frames this far behind the last one shown count as out of order (E1.31 §6.7.2). */
	constexpr int32 SACNSequenceWindow = 20;

	uint16 ReadU16BE(TConstArrayView<uint8> Data, int32 Offset)
	{
		return static_cast<uint16>((Data[Offset] << 8) | Data[Offset + 1]);
	}

	uint16 ReadU16LE(TConstArrayView<uint8> Data, int32 Offset)
	{
		return static_cast<uint16>(Data[Offset] | (Data[Offset + 1] << 8));
	}

	uint32 ReadU32BE(TConstArrayView<uint8> Data, int32 Offset)
	{
		return (static_cast<uint32>(Data[Offset]) << 24) | (static_cast<uint32>(Data[Offset + 1]) << 16)
			| (static_cast<uint32>(Data[Offset + 2]) << 8) | static_cast<uint32>(Data[Offset + 3]);
	}

	void WriteU16BE(TArray<uint8>& Data, int32 Offset, uint16 Value)
	{
		Data[Offset] = static_cast<uint8>(Value >> 8);
		Data[Offset + 1] = static_cast<uint8>(Value & 0xFF);
	}

	void WriteU16LE(TArray<uint8>& Data, int32 Offset, uint16 Value)
	{
		Data[Offset] = static_cast<uint8>(Value & 0xFF);
		Data[Offset + 1] = static_cast<uint8>(Value >> 8);
	}

	void WriteU32BE(TArray<uint8>& Data, int32 Offset, uint32 Value)
	{
		WriteU16BE(Data, Offset, static_cast<uint16>(Value >> 16));
		WriteU16BE(Data, Offset + 2, static_cast<uint16>(Value & 0xFFFF));
	}

	bool StartsWith(TConstArrayView<uint8> Data, int32 Offset, TConstArrayView<uint8> Expected)
	{
		return Data.Num() >= Offset + Expected.Num() && FMemory::Memcmp(Data.GetData() + Offset, Expected.GetData(), Expected.Num()) == 0;
	}

	/** ASCII only, NUL-padded, the last byte always NUL: Art-Net and sACN name fields are fixed-size C strings. */
	void WriteName(TArray<uint8>& Data, int32 Offset, int32 FieldSize, FStringView Name)
	{
		for (int32 Index = 0; Index < FieldSize - 1 && Index < Name.Len(); ++Index)
		{
			const TCHAR Char = Name[Index];
			Data[Offset + Index] = (Char >= 0x20 && Char < 0x7F) ? static_cast<uint8>(Char) : static_cast<uint8>('?');
		}
	}

	/** E1.31 PDU flags (0x7) and length, counted from Offset to the end of the packet. */
	void WriteFlagsAndLength(TArray<uint8>& Data, int32 Offset)
	{
		WriteU16BE(Data, Offset, static_cast<uint16>(0x7000 | ((Data.Num() - Offset) & 0x0FFF)));
	}

	bool HasPduFlags(TConstArrayView<uint8> Data, int32 Offset)
	{
		return (Data[Offset] & 0xF0) == 0x70;
	}

	void CopySlots(TConstArrayView<uint8> Source, FStageDMXNetFrame& OutFrame)
	{
		OutFrame.SlotCount = Source.Num();
		FMemory::Memcpy(OutFrame.Slots.GetData(), Source.GetData(), Source.Num());
	}

	EStageDMXNetPacket ParseArtNet(TConstArrayView<uint8> Packet, FStageDMXNetFrame& OutFrame)
	{
		if (Packet.Num() < ArtPollMinSize)
		{
			return EStageDMXNetPacket::Invalid;
		}

		const uint16 OpCode = ReadU16LE(Packet, 8);
		const uint16 Version = ReadU16BE(Packet, 10);
		if (OpCode == OpPoll)
		{
			return Version >= ArtNetProtocolVersion ? EStageDMXNetPacket::ArtPoll : EStageDMXNetPacket::Invalid;
		}
		if (OpCode != OpDmx)
		{
			return EStageDMXNetPacket::Ignored; // Our own ArtPollReply echoed back, ArtSync, ArtAddress, ...
		}

		if (Packet.Num() < ArtDmxHeaderSize || Version < ArtNetProtocolVersion)
		{
			return EStageDMXNetPacket::Invalid;
		}
		const int32 Length = ReadU16BE(Packet, 16);
		if (Length < 1 || Length > 512 || Packet.Num() < ArtDmxHeaderSize + Length)
		{
			return EStageDMXNetPacket::Invalid;
		}

		OutFrame.Protocol = EStageDMXNetProtocol::ArtNet;
		OutFrame.Sequence = Packet[12];
		// SubUni holds Sub-Net (high nibble) and Universe; Net holds the top 7 bits of the 15-bit port-address.
		OutFrame.WireUniverse = ((Packet[15] & 0x7F) << 8) | Packet[14];
		OutFrame.Priority = 100;
		OutFrame.bTerminated = false;
		CopySlots(Packet.Slice(ArtDmxHeaderSize, Length), OutFrame);
		return EStageDMXNetPacket::ArtDmx;
	}

	EStageDMXNetPacket ParseSACN(TConstArrayView<uint8> Packet, FStageDMXNetFrame& OutFrame)
	{
		// Root layer.
		if (Packet.Num() < 22 || ReadU16BE(Packet, 0) != 0x0010 || ReadU16BE(Packet, 2) != 0x0000 || !HasPduFlags(Packet, 16))
		{
			return EStageDMXNetPacket::Invalid;
		}
		if (ReadU32BE(Packet, 18) != VectorRootE131Data)
		{
			return EStageDMXNetPacket::Ignored; // Extended packets: synchronisation, universe discovery.
		}

		// Framing and DMP layers.
		if (Packet.Num() < SACNHeaderSize || !HasPduFlags(Packet, 38) || ReadU32BE(Packet, 40) != VectorE131DataPacket
			|| !HasPduFlags(Packet, 115) || Packet[117] != VectorDmpSetProperty || Packet[118] != DmpAddressAndDataType
			|| ReadU16BE(Packet, 119) != 0x0000 || ReadU16BE(Packet, 121) != 0x0001)
		{
			return EStageDMXNetPacket::Invalid;
		}

		const uint8 Priority = Packet[108];
		const int32 Universe = ReadU16BE(Packet, 113);
		const int32 PropertyCount = ReadU16BE(Packet, 123); // The START code plus the slots.
		if (Priority > SACNMaxPriority || Universe < 1 || Universe > StageDMXNet::MaxSACNUniverse
			|| PropertyCount < 1 || PropertyCount > 513 || Packet.Num() < SACNHeaderSize - 1 + PropertyCount)
		{
			return EStageDMXNetPacket::Invalid;
		}

		const uint8 Options = Packet[112];
		const bool bTerminated = (Options & SACNOptionStreamTerminated) != 0;
		if (Packet[125] != 0x00 && !bTerminated)
		{
			return EStageDMXNetPacket::Ignored; // Alternate START codes, e.g. 0xDD per-address priority.
		}

		OutFrame.Protocol = EStageDMXNetProtocol::SACN;
		OutFrame.WireUniverse = Universe;
		OutFrame.Priority = Priority;
		OutFrame.Sequence = Packet[111];
		OutFrame.bTerminated = bTerminated;
		OutFrame.Source = FGuid(ReadU32BE(Packet, 22), ReadU32BE(Packet, 26), ReadU32BE(Packet, 30), ReadU32BE(Packet, 34));
		CopySlots(Packet.Slice(SACNHeaderSize, PropertyCount - 1), OutFrame);
		return EStageDMXNetPacket::SACNData;
	}
}

FStageDMXNetFrame::FStageDMXNetFrame()
{
	FMemory::Memzero(Slots.GetData(), Slots.Num());
}

EStageDMXNetPacket StageDMXNet::Parse(TConstArrayView<uint8> Packet, FStageDMXNetFrame& OutFrame)
{
	if (StartsWith(Packet, 0, ArtNetId))
	{
		return ParseArtNet(Packet, OutFrame);
	}
	if (StartsWith(Packet, 4, AcnPacketId))
	{
		return ParseSACN(Packet, OutFrame);
	}
	return EStageDMXNetPacket::Invalid;
}

TArray<uint8> StageDMXNet::BuildArtDmx(int32 PortAddress, uint8 Sequence, TConstArrayView<uint8> Slots)
{
	// Art-Net asks for an even length of at least 2; the padding slot is zero.
	const int32 Length = FMath::Clamp(Slots.Num() + (Slots.Num() % 2), 2, 512);
	TArray<uint8> Packet;
	Packet.SetNumZeroed(ArtDmxHeaderSize + Length);
	FMemory::Memcpy(Packet.GetData(), ArtNetId, sizeof(ArtNetId));
	WriteU16LE(Packet, 8, OpDmx);
	WriteU16BE(Packet, 10, ArtNetProtocolVersion);
	Packet[12] = Sequence;
	const int32 Address = FMath::Clamp(PortAddress, 0, MaxArtNetPortAddress);
	Packet[14] = static_cast<uint8>(Address & 0xFF);
	Packet[15] = static_cast<uint8>(Address >> 8);
	WriteU16BE(Packet, 16, static_cast<uint16>(Length));
	FMemory::Memcpy(Packet.GetData() + ArtDmxHeaderSize, Slots.GetData(), FMath::Min(Slots.Num(), 512));
	return Packet;
}

TArray<uint8> StageDMXNet::BuildArtPoll()
{
	TArray<uint8> Packet;
	Packet.SetNumZeroed(14);
	FMemory::Memcpy(Packet.GetData(), ArtNetId, sizeof(ArtNetId));
	WriteU16LE(Packet, 8, OpPoll);
	WriteU16BE(Packet, 10, ArtNetProtocolVersion);
	return Packet;
}

TArray<TArray<uint8>> StageDMXNet::BuildArtPollReplies(const FStageArtPollReplyInfo& Info)
{
	// Group the port-addresses by Net + Sub-Net (bits 4..14), at most four ports per reply.
	TArray<int32> Addresses;
	for (const int32 Address : Info.PortAddresses)
	{
		if (Address >= 0 && Address <= MaxArtNetPortAddress)
		{
			Addresses.AddUnique(Address);
		}
	}
	Addresses.Sort();

	TArray<TArray<int32>> Groups;
	for (const int32 Address : Addresses)
	{
		if (Groups.IsEmpty() || Groups.Last().Num() == 4 || (Groups.Last()[0] >> 4) != (Address >> 4))
		{
			Groups.AddDefaulted();
		}
		Groups.Last().Add(Address);
	}
	if (Groups.IsEmpty())
	{
		Groups.AddDefaulted();
	}

	TArray<TArray<uint8>> Replies;
	for (int32 GroupIndex = 0; GroupIndex < Groups.Num(); ++GroupIndex)
	{
		const TArray<int32>& Ports = Groups[GroupIndex];
		TArray<uint8>& Packet = Replies.AddDefaulted_GetRef();
		Packet.SetNumZeroed(ArtPollReplySize);
		FMemory::Memcpy(Packet.GetData(), ArtNetId, sizeof(ArtNetId));
		WriteU16LE(Packet, 8, OpPollReply);
		for (int32 Byte = 0; Byte < 4; ++Byte)
		{
			Packet[10 + Byte] = static_cast<uint8>(Info.IPv4 >> (24 - 8 * Byte));
			Packet[207 + Byte] = Packet[10 + Byte]; // BindIp: every reply comes from the same root device.
		}
		WriteU16LE(Packet, 14, ArtNetPort);
		const int32 NetSubNet = Ports.IsEmpty() ? 0 : Ports[0] >> 4;
		Packet[18] = static_cast<uint8>((NetSubNet >> 4) & 0x7F); // NetSwitch
		Packet[19] = static_cast<uint8>(NetSubNet & 0x0F);       // SubSwitch
		Packet[20] = 0x00;                                       // OemHi
		Packet[21] = 0xFF;                                       // Oem: OemUnknown, for unregistered products
		Packet[23] = 0xD0;                                       // Status1: indicators normal, port-addresses set locally (the settings)
		WriteName(Packet, 26, 18, Info.ShortName);
		WriteName(Packet, 44, 64, Info.LongName);
		WriteName(Packet, 108, 64, FString::Printf(TEXT("#0001 [%04d] StageCraft OK"), GroupIndex));
		WriteU16BE(Packet, 172, static_cast<uint16>(Ports.Num()));
		for (int32 Port = 0; Port < Ports.Num(); ++Port)
		{
			Packet[174 + Port] = 0x80;                                 // PortTypes: can output DMX512 from the network
			Packet[182 + Port] = Info.bReceiving ? 0x80 : 0x00;        // GoodOutputA: data is being output
			Packet[190 + Port] = static_cast<uint8>(Ports[Port] & 0x0F); // SwOut
		}
		Packet[200] = 0x00;                                      // Style: StNode
		Packet[211] = static_cast<uint8>(GroupIndex + 1);        // BindIndex, 1-based
		Packet[212] = 0x08;                                      // Status2: 15-bit port-addresses
	}
	return Replies;
}

TArray<uint8> StageDMXNet::BuildSACNData(int32 Universe, const FGuid& Cid, uint8 Priority, uint8 Sequence,
	TConstArrayView<uint8> Slots, bool bTerminated, FStringView SourceName)
{
	const int32 SlotCount = FMath::Clamp(Slots.Num(), 0, 512);
	TArray<uint8> Packet;
	Packet.SetNumZeroed(SACNHeaderSize + SlotCount);

	WriteU16BE(Packet, 0, 0x0010);
	FMemory::Memcpy(Packet.GetData() + 4, AcnPacketId, sizeof(AcnPacketId));
	WriteFlagsAndLength(Packet, 16);
	WriteU32BE(Packet, 18, VectorRootE131Data);
	WriteU32BE(Packet, 22, Cid.A);
	WriteU32BE(Packet, 26, Cid.B);
	WriteU32BE(Packet, 30, Cid.C);
	WriteU32BE(Packet, 34, Cid.D);

	WriteFlagsAndLength(Packet, 38);
	WriteU32BE(Packet, 40, VectorE131DataPacket);
	WriteName(Packet, 44, SACNSourceNameSize, SourceName);
	Packet[108] = FMath::Min(Priority, SACNMaxPriority);
	Packet[111] = Sequence;
	Packet[112] = bTerminated ? SACNOptionStreamTerminated : 0x00;
	WriteU16BE(Packet, 113, static_cast<uint16>(FMath::Clamp(Universe, 1, MaxSACNUniverse)));

	WriteFlagsAndLength(Packet, 115);
	Packet[117] = VectorDmpSetProperty;
	Packet[118] = DmpAddressAndDataType;
	WriteU16BE(Packet, 119, 0x0000);
	WriteU16BE(Packet, 121, 0x0001);
	WriteU16BE(Packet, 123, static_cast<uint16>(SlotCount + 1));
	Packet[125] = 0x00;
	FMemory::Memcpy(Packet.GetData() + SACNHeaderSize, Slots.GetData(), SlotCount);
	return Packet;
}

uint32 StageDMXNet::GetSACNMulticastAddress(int32 Universe)
{
	const uint32 Clamped = static_cast<uint32>(FMath::Clamp(Universe, 1, MaxSACNUniverse));
	return (239u << 24) | (255u << 16) | (((Clamped >> 8) & 0xFF) << 8) | (Clamped & 0xFF);
}

// --- FStageDMXSourceArbiter ---

bool FStageDMXSourceArbiter::IsAlive(const FCurrentSource& Source, double NowSeconds)
{
	return NowSeconds - Source.LastSeen <= SourceTimeoutSeconds;
}

bool FStageDMXSourceArbiter::Accept(int32 Universe, const FStageDMXNetFrame& Frame, double NowSeconds)
{
	FCurrentSource* CurrentSource = Current.Find(Universe);
	if (Frame.bTerminated)
	{
		if (CurrentSource && CurrentSource->Source == Frame.Source)
		{
			Current.Remove(Universe); // The next sender takes over at once, without waiting for the timeout.
		}
		return false;
	}

	if (CurrentSource && IsAlive(*CurrentSource, NowSeconds))
	{
		if (CurrentSource->Source == Frame.Source)
		{
			if (Frame.Protocol == EStageDMXNetProtocol::SACN && CurrentSource->Protocol == EStageDMXNetProtocol::SACN)
			{
				const int32 Behind = static_cast<int8>(static_cast<uint8>(Frame.Sequence - CurrentSource->Sequence));
				if (Behind <= 0 && Behind > -SACNSequenceWindow)
				{
					return false; // A duplicate or a late frame: showing it would step the look backwards.
				}
			}
		}
		else if (Frame.Priority <= CurrentSource->Priority)
		{
			return false;
		}
	}

	FCurrentSource& Updated = Current.FindOrAdd(Universe);
	Updated.Source = Frame.Source;
	Updated.Priority = Frame.Priority;
	Updated.Sequence = Frame.Sequence;
	Updated.Protocol = Frame.Protocol;
	Updated.LastSeen = NowSeconds;
	return true;
}

TArray<int32> FStageDMXSourceArbiter::GetLiveUniverses(double NowSeconds) const
{
	TArray<int32> Universes;
	for (const TPair<int32, FCurrentSource>& Pair : Current)
	{
		if (IsAlive(Pair.Value, NowSeconds))
		{
			Universes.Add(Pair.Key);
		}
	}
	Universes.Sort();
	return Universes;
}

int32 FStageDMXSourceArbiter::GetLiveSourceCount(double NowSeconds) const
{
	TSet<FGuid> Sources;
	for (const TPair<int32, FCurrentSource>& Pair : Current)
	{
		if (IsAlive(Pair.Value, NowSeconds))
		{
			Sources.Add(Pair.Value.Source);
		}
	}
	return Sources.Num();
}
