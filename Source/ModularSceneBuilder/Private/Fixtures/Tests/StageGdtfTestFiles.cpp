// Copyright Epic Games, Inc. All Rights Reserved.

#include "Fixtures/Tests/StageGdtfTestFiles.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Fixtures/StageZipReader.h"
#include "Misc/Compression.h"

namespace StageGdtfTestFiles
{
	const TCHAR* const SpotId = TEXT("6C9F1A2B-3D4E-4F50-8A61-7B8C9D0E1F20");

	FString SpotXml()
	{
		return FString(TEXT(R"GDTF(<?xml version="1.0" encoding="UTF-8" standalone="no" ?>
<!-- Self-authored test fixture for StageCraft (ADR 0033). -->
<GDTF DataVersion="1.2">
  <FixtureType Name="Test Spot" ShortName="TSpot" LongName="StageCraft Test Spot" Manufacturer="StageCraft" Description="A spot for tests &amp; nothing else" FixtureTypeID="6C9F1A2B-3D4E-4F50-8A61-7B8C9D0E1F20" Thumbnail="" CanHaveChildren="Yes">
    <AttributeDefinitions>
      <Attributes>
        <Attribute Name="Pan" Pretty="P" PhysicalUnit="Angle"/>
        <Attribute Name="Tilt" Pretty="T" PhysicalUnit="Angle"/>
        <Attribute Name="Dimmer" Pretty="Dim"/>
      </Attributes>
    </AttributeDefinitions>
    <Wheels>
      <Wheel Name="Color1">
        <Slot Name="Open" Color="0.312700,0.329000,100.000000"/>
        <Slot Name="Red" Color="0.640000,0.330000,21.260000"/>
        <Slot Name="Blue" Color='0.150000,0.060000,7.220000'/>
      </Wheel>
      <Wheel Name="Gobo1">
        <Slot Name="Open"/>
        <Slot Name="Dots" MediaFileName="gobo_dots"/>
        <Slot Name="Star" MediaFileName="gobo_star"/>
      </Wheel>
      <Wheel Name="Prism1">
        <Slot Name="Open"/>
        <Slot Name="3-Facet">
          <PrismFacet Color="0.3127,0.3290,100" Rotation="{1,0,0}{0,1,0}{0,0,1}"/>
          <PrismFacet Color="0.3127,0.3290,100" Rotation="{1,0,0}{0,1,0}{0,0,1}"/>
          <PrismFacet Color="0.3127,0.3290,100" Rotation="{1,0,0}{0,1,0}{0,0,1}"/>
        </Slot>
      </Wheel>
    </Wheels>
    <PhysicalDescriptions>
      <Emitters/>
      <Properties>
        <OperatingTemperature High="40" Low="0"/>
        <Weight Value="22.5"/>
        <PowerConsumption Value="470" PowerFactor="1" Connector="Power"/>
        <LegHeight Value="0"/>
      </Properties>
    </PhysicalDescriptions>
)GDTF")
			TEXT(R"GDTF(    <Models>
      <Model Name="Base" Length="0.40" Width="0.30" Height="0.15" PrimitiveType="Base"/>
      <Model Name="Yoke" Length="0.35" Width="0.12" Height="0.40" PrimitiveType="Yoke"/>
      <Model Name="Head" Length="0.25" Width="0.25" Height="0.35" PrimitiveType="Head"/>
    </Models>
    <Geometries>
      <Geometry Name="Base" Model="Base" Position="{1,0,0,0}{0,1,0,0}{0,0,1,0}{0,0,0,1}">
        <Axis Name="Yoke" Model="Yoke" Position="{1,0,0,0}{0,1,0,0.02}{0,0,1,0.15}{0,0,0,1}">
          <Axis Name="Head" Model="Head" Position="{1,0,0,0}{0,1,0,0}{0,0,1,0.25}{0,0,0,1}">
            <Beam Name="Beam" Model="Head" Position="{1,0,0,0}{0,1,0,0}{0,0,1,-0.1}{0,0,0,1}" LampType="LED" PowerConsumption="400" LuminousFlux="18000" ColorTemperature="7000" BeamAngle="20" FieldAngle="30" BeamRadius="0.06" BeamType="Spot"/>
          </Axis>
        </Axis>
      </Geometry>
    </Geometries>
    <DMXModes>
      <DMXMode Name="Standard" Geometry="Base">
        <DMXChannels>
          <DMXChannel DMXBreak="1" Offset="1,2" InitialFunction="Yoke_Pan.Pan.Pan 1" Highlight="None" Geometry="Yoke">
            <LogicalChannel Attribute="Pan" Snap="No" Master="None" MibFade="0" DMXChangeTimeLimit="0">
              <ChannelFunction Name="Pan 1" Attribute="Pan" DMXFrom="0/1" Default="128/1" PhysicalFrom="-270" PhysicalTo="270"/>
            </LogicalChannel>
          </DMXChannel>
          <DMXChannel DMXBreak="1" Offset="3,4" InitialFunction="Head_Tilt.Tilt.Tilt 1" Geometry="Head">
            <LogicalChannel Attribute="Tilt">
              <ChannelFunction Name="Tilt 1" Attribute="Tilt" DMXFrom="0/2" Default="32768/2" PhysicalFrom="-135" PhysicalTo="135"/>
            </LogicalChannel>
          </DMXChannel>
          <DMXChannel DMXBreak="1" Offset="5" InitialFunction="Beam_Dimmer.Dimmer.Dimmer 1" Geometry="Beam">
            <LogicalChannel Attribute="Dimmer" Master="Grand">
              <ChannelFunction Name="Dimmer 1" Attribute="Dimmer" DMXFrom="0/1" Default="0/1" PhysicalFrom="0" PhysicalTo="1"/>
            </LogicalChannel>
          </DMXChannel>
          <DMXChannel DMXBreak="1" Offset="6" InitialFunction="Beam_Shutter1.Shutter1.Open" Geometry="Beam">
            <LogicalChannel Attribute="Shutter1">
              <ChannelFunction Name="Open" Attribute="Shutter1" DMXFrom="0/1" Default="16/1" PhysicalFrom="0" PhysicalTo="1">
                <ChannelSet Name="Closed" DMXFrom="0/1"/>
                <ChannelSet Name="Open" DMXFrom="16/1"/>
              </ChannelFunction>
              <ChannelFunction Name="Strobe" Attribute="Shutter1Strobe" DMXFrom="32/1" Default="32/1" PhysicalFrom="1" PhysicalTo="25"/>
              <ChannelFunction Name="Random" Attribute="Shutter1StrobeRandom" DMXFrom="128/1" Default="128/1" PhysicalFrom="1" PhysicalTo="25"/>
            </LogicalChannel>
          </DMXChannel>
          <DMXChannel DMXBreak="1" Offset="7" Geometry="Beam">
            <LogicalChannel Attribute="Color1">
              <ChannelFunction Name="Color" Attribute="Color1" DMXFrom="0/1" Default="0/1" Wheel="Color1">
                <ChannelSet Name="Open" DMXFrom="0/1" WheelSlotIndex="1"/>
                <ChannelSet Name="Red" DMXFrom="10/1" WheelSlotIndex="2"/>
                <ChannelSet Name="Blue" DMXFrom="20/1" WheelSlotIndex="3"/>
              </ChannelFunction>
              <ChannelFunction Name="Spin" Attribute="Color1WheelSpin" DMXFrom="128/1" PhysicalFrom="-360" PhysicalTo="360"/>
            </LogicalChannel>
          </DMXChannel>
          <DMXChannel DMXBreak="1" Offset="8" Geometry="Beam">
            <LogicalChannel Attribute="Gobo1">
              <ChannelFunction Name="Gobo" Attribute="Gobo1" DMXFrom="0/1" Wheel="Gobo1">
                <ChannelSet Name="Open" DMXFrom="0/1" WheelSlotIndex="1"/>
                <ChannelSet Name="Dots" DMXFrom="10/1" WheelSlotIndex="2"/>
                <ChannelSet Name="Star" DMXFrom="20/1" WheelSlotIndex="3"/>
              </ChannelFunction>
              <ChannelFunction Name="Shake" Attribute="Gobo1SelectShake" DMXFrom="64/1" PhysicalFrom="0" PhysicalTo="10"/>
            </LogicalChannel>
          </DMXChannel>
)GDTF")
			TEXT(R"GDTF(          <DMXChannel DMXBreak="1" Offset="9" Geometry="Beam">
            <LogicalChannel Attribute="Gobo1Pos">
              <ChannelFunction Name="Index" Attribute="Gobo1Pos" DMXFrom="0/1" PhysicalFrom="-180" PhysicalTo="180"/>
              <ChannelFunction Name="Rotate" Attribute="Gobo1PosRotate" DMXFrom="128/1" PhysicalFrom="-720" PhysicalTo="720"/>
            </LogicalChannel>
          </DMXChannel>
          <DMXChannel DMXBreak="1" Offset="10" Geometry="Beam">
            <LogicalChannel Attribute="Prism1">
              <ChannelFunction Name="Prism" Attribute="Prism1" DMXFrom="0/1" Wheel="Prism1">
                <ChannelSet Name="Out" DMXFrom="0/1" WheelSlotIndex="1"/>
                <ChannelSet Name="3-Facet" DMXFrom="128/1" WheelSlotIndex="2"/>
              </ChannelFunction>
            </LogicalChannel>
          </DMXChannel>
          <DMXChannel DMXBreak="1" Offset="11,12" Geometry="Beam">
            <LogicalChannel Attribute="Zoom">
              <ChannelFunction Name="Zoom" Attribute="Zoom" DMXFrom="0/2" Default="0/2" PhysicalFrom="8" PhysicalTo="40"/>
            </LogicalChannel>
          </DMXChannel>
          <DMXChannel DMXBreak="1" Offset="13" Geometry="Beam">
            <LogicalChannel Attribute="Focus1">
              <ChannelFunction Name="Focus" Attribute="Focus1" DMXFrom="0/1" Default="128/1" PhysicalFrom="0" PhysicalTo="1"/>
            </LogicalChannel>
          </DMXChannel>
          <DMXChannel DMXBreak="1" Offset="14" Geometry="Base">
            <LogicalChannel Attribute="Control1">
              <ChannelFunction Name="Nothing" Attribute="NoFeature" DMXFrom="0/1" Default="0/1"/>
              <ChannelFunction Name="Reset" Attribute="Control1" DMXFrom="200/1"/>
            </LogicalChannel>
          </DMXChannel>
          <DMXChannel DMXBreak="1" Offset="15" Geometry="Beam">
            <LogicalChannel Attribute="Blade1A">
              <ChannelFunction Name="Blade 1A" Attribute="Blade1A" DMXFrom="0/1" PhysicalFrom="0" PhysicalTo="100"/>
            </LogicalChannel>
          </DMXChannel>
        </DMXChannels>
        <Relations/>
        <FTMacros/>
      </DMXMode>
      <DMXMode Name="Basic" Geometry="Base">
        <DMXChannels>
          <DMXChannel Offset="1" Geometry="Yoke"><LogicalChannel Attribute="Pan"><ChannelFunction Name="Pan" Attribute="Pan" DMXFrom="0/1" Default="128/1" PhysicalFrom="-270" PhysicalTo="270"/></LogicalChannel></DMXChannel>
          <DMXChannel Offset="2" Geometry="Head"><LogicalChannel Attribute="Tilt"><ChannelFunction Name="Tilt" Attribute="Tilt" DMXFrom="0/1" Default="128/1" PhysicalFrom="-135" PhysicalTo="135"/></LogicalChannel></DMXChannel>
          <DMXChannel Offset="3" Geometry="Beam"><LogicalChannel Attribute="Dimmer"><ChannelFunction Name="Dimmer" Attribute="Dimmer" DMXFrom="0/1"/></LogicalChannel></DMXChannel>
        </DMXChannels>
      </DMXMode>
      <DMXMode Name="Broken" Geometry="Base">
        <DMXChannels>
          <DMXChannel DMXBreak="2" Offset="1" Geometry="Beam"><LogicalChannel Attribute="Dimmer"><ChannelFunction Name="Dimmer" Attribute="Dimmer" DMXFrom="0/1"/></LogicalChannel></DMXChannel>
        </DMXChannels>
      </DMXMode>
    </DMXModes>
    <Revisions><Revision Text="Written for StageCraft tests" Date="2026-11-09T12:00:00"/></Revisions>
  </FixtureType>
</GDTF>
)GDTF"));
	}

	FString BarXml(int32 Pixels)
	{
		FString References;
		for (int32 Pixel = 0; Pixel < Pixels; ++Pixel)
		{
			References += FString::Printf(TEXT("        <GeometryReference Name=\"Pixel %d\" Geometry=\"Pixel\" Model=\"Pixel\" Position=\"{1,0,0,%.3f}{0,1,0,0}{0,0,1,0}{0,0,0,1}\"><Break DMXOffset=\"%d\" DMXBreak=\"1\"/></GeometryReference>\n"),
				Pixel + 1, Pixel * 0.1, 2 + Pixel * 3);
		}
		return FString::Printf(TEXT(R"GDTF(<?xml version="1.0" encoding="UTF-8"?>
<GDTF DataVersion="1.2">
  <FixtureType Name="Test Bar" ShortName="TBar" LongName="StageCraft Test Bar" Manufacturer="StageCraft" Description="" FixtureTypeID="0A1B2C3D-4E5F-4061-8273-94A5B6C7D8E9">
    <AttributeDefinitions/>
    <Wheels/>
    <PhysicalDescriptions>
      <Emitters>
        <Emitter Name="R" Color="0.700,0.300,30"/>
        <Emitter Name="G" Color="0.170,0.700,60"/>
        <Emitter Name="B" Color="0.135,0.040,10"/>
      </Emitters>
      <Properties><Weight Value="4"/><PowerConsumption Value="90"/></Properties>
    </PhysicalDescriptions>
    <Models>
      <Model Name="Body" Length="1.0" Width="0.08" Height="0.1" PrimitiveType="Cube"/>
      <Model Name="Pixel" Length="0.1" Width="0.05" Height="0.01" PrimitiveType="Cube"/>
    </Models>
    <Geometries>
      <Geometry Name="Pixel" Model="Pixel">
        <Beam Name="PixelBeam" LuminousFlux="300" BeamAngle="30" FieldAngle="45" BeamRadius="0.02" BeamType="Wash"/>
      </Geometry>
      <Geometry Name="Body" Model="Body">
%s      </Geometry>
    </Geometries>
    <DMXModes>
      <DMXMode Name="Pixels" Geometry="Body">
        <DMXChannels>
          <DMXChannel Offset="1" Geometry="Body"><LogicalChannel Attribute="Dimmer"><ChannelFunction Name="Dimmer" Attribute="Dimmer" DMXFrom="0/1" Default="255/1"/></LogicalChannel></DMXChannel>
          <DMXChannel DMXBreak="Overwrite" Offset="1" Geometry="PixelBeam"><LogicalChannel Attribute="ColorAdd_R"><ChannelFunction Name="R" Attribute="ColorAdd_R" Emitter="R" DMXFrom="0/1"/></LogicalChannel></DMXChannel>
          <DMXChannel DMXBreak="Overwrite" Offset="2" Geometry="PixelBeam"><LogicalChannel Attribute="ColorAdd_G"><ChannelFunction Name="G" Attribute="ColorAdd_G" Emitter="G" DMXFrom="0/1"/></LogicalChannel></DMXChannel>
          <DMXChannel DMXBreak="Overwrite" Offset="3" Geometry="PixelBeam"><LogicalChannel Attribute="ColorAdd_B"><ChannelFunction Name="B" Attribute="ColorAdd_B" Emitter="B" DMXFrom="0/1"/></LogicalChannel></DMXChannel>
        </DMXChannels>
      </DMXMode>
    </DMXModes>
  </FixtureType>
</GDTF>
)GDTF"), *References);
	}

	TArray<uint8> Utf8(const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	}

	namespace
	{
		void Put16(TArray<uint8>& Out, uint32 Value)
		{
			Out.Add(uint8(Value & 0xFF));
			Out.Add(uint8((Value >> 8) & 0xFF));
		}

		void Put32(TArray<uint8>& Out, uint32 Value)
		{
			Put16(Out, Value & 0xFFFF);
			Put16(Out, Value >> 16);
		}
	}

	TArray<uint8> MakeZip(const TArray<TPair<FString, TArray<uint8>>>& Files, bool bDeflate)
	{
		TArray<uint8> Archive;
		TArray<uint8> Directory;
		for (const TPair<FString, TArray<uint8>>& File : Files)
		{
			TArray<uint8> Stored = File.Value;
			uint16 Method = 0;
			if (bDeflate && !File.Value.IsEmpty())
			{
				int64 Size = File.Value.Num() + File.Value.Num() / 10 + 1024;
				TArray<uint8> Compressed;
				Compressed.SetNumUninitialized(int32(Size));
				if (FCompression::CompressMemory(NAME_Zlib, Compressed.GetData(), Size, File.Value.GetData(), File.Value.Num(), COMPRESS_NoFlags,
					static_cast<uintptr_t>(static_cast<intptr_t>(-15))))
				{
					Compressed.SetNum(int32(Size));
					Stored = MoveTemp(Compressed);
					Method = 8;
				}
			}
			const TArray<uint8> Name = Utf8(File.Key);
			const uint32 Crc = StageZip::Crc32(File.Value);
			const uint32 LocalOffset = uint32(Archive.Num());

			Put32(Archive, 0x04034b50);
			Put16(Archive, 20);
			Put16(Archive, 0x0800); // UTF-8 names.
			Put16(Archive, Method);
			Put16(Archive, 0);
			Put16(Archive, 0x5A21);
			Put32(Archive, Crc);
			Put32(Archive, uint32(Stored.Num()));
			Put32(Archive, uint32(File.Value.Num()));
			Put16(Archive, uint32(Name.Num()));
			Put16(Archive, 0);
			Archive.Append(Name);
			Archive.Append(Stored);

			Put32(Directory, 0x02014b50);
			Put16(Directory, 20);
			Put16(Directory, 20);
			Put16(Directory, 0x0800);
			Put16(Directory, Method);
			Put16(Directory, 0);
			Put16(Directory, 0x5A21);
			Put32(Directory, Crc);
			Put32(Directory, uint32(Stored.Num()));
			Put32(Directory, uint32(File.Value.Num()));
			Put16(Directory, uint32(Name.Num()));
			Put16(Directory, 0);
			Put16(Directory, 0);
			Put16(Directory, 0);
			Put16(Directory, 0);
			Put32(Directory, 0);
			Put32(Directory, LocalOffset);
			Directory.Append(Name);
		}
		const uint32 DirectoryOffset = uint32(Archive.Num());
		Archive.Append(Directory);
		Put32(Archive, 0x06054b50);
		Put16(Archive, 0);
		Put16(Archive, 0);
		Put16(Archive, uint32(Files.Num()));
		Put16(Archive, uint32(Files.Num()));
		Put32(Archive, uint32(Directory.Num()));
		Put32(Archive, DirectoryOffset);
		Put16(Archive, 0);
		return Archive;
	}

	TArray<uint8> MakeGdtf(const FString& Xml, bool bDeflate, bool bWithWheelImages)
	{
		TArray<TPair<FString, TArray<uint8>>> Files;
		Files.Emplace(TEXT("description.xml"), Utf8(Xml));
		if (bWithWheelImages)
		{
			const TArray<uint8> Png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
			Files.Emplace(TEXT("wheels/gobo_dots.png"), Png);
			Files.Emplace(TEXT("wheels/gobo_star.png"), Png);
		}
		return MakeZip(Files, bDeflate);
	}
}

#endif
