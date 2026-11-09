// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/LightingFixtureActor.h"
#include "Data/LightingFixtureData.h"
#include "Data/StageParameterTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Fixtures/StageGdtfFixtureBuilder.h"
#include "Fixtures/StageGdtfMapping.h"
#include "Fixtures/StageGdtfParser.h"
#include "Fixtures/Tests/StageGdtfTestFiles.h"
#include "GameFramework/WorldSettings.h"
#include "Math/RandomStream.h"
#include "Misc/AutomationTest.h"
#include "Show/StageDMXFunctions.h"
#include "Show/StageDMXPatching.h"
#include "UObject/Package.h"

namespace StageDMXFunctionTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;
	constexpr int32 UniverseSize = 512;

	FStageDMXFunction Range(int32 From, int32 To, const FGameplayTag& Attribute, float PhysicalFrom, float PhysicalTo, int32 Slot = INDEX_NONE)
	{
		FStageDMXFunction Function;
		Function.DMXFrom = From;
		Function.DMXTo = To;
		Function.Attribute = Attribute;
		Function.PhysicalFrom = PhysicalFrom;
		Function.PhysicalTo = PhysicalTo;
		Function.Slot = Slot;
		return Function;
	}

	FStageFixtureDMXChannel MakeChannel(const FGameplayTag& Attribute, bool b16Bit, int32 DefaultDMX, TArray<FStageDMXFunction> Functions)
	{
		FStageFixtureDMXChannel Channel;
		Channel.Attribute = Attribute;
		Channel.b16Bit = b16Bit;
		Channel.DefaultDMX = DefaultDMX;
		Channel.Functions = MoveTemp(Functions);
		return Channel;
	}

	struct FScopedWorld
	{
		UWorld* World = nullptr;

		FScopedWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("StageDMXFunctionTestWorld"));
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
			World->DestroyWorld(false);
			GEngine->DestroyWorldContext(World);
		}

		ALightingFixtureActor* Spawn(ULightingFixtureData* Data, int32 Address = 1) const
		{
			ALightingFixtureActor* Fixture = World->SpawnActorDeferred<ALightingFixtureActor>(ALightingFixtureActor::StaticClass(), FTransform::Identity);
			Fixture->InitializeFromItemData(Data);
			Fixture->FinishSpawning(FTransform::Identity);
			FStageDMXPatch Patch;
			Patch.Universe = 1;
			Patch.Address = Address;
			Fixture->SetPatch(Patch);
			return Fixture;
		}
	};

	FGameplayTag ResolveReference(const FGameplayTag& ChannelAttribute, bool& bOutInverted)
	{
		bOutInverted = true;
		if (ChannelAttribute == StageCraftTags::Attribute_ColorC) { return StageCraftTags::Attribute_ColorR; }
		if (ChannelAttribute == StageCraftTags::Attribute_ColorM) { return StageCraftTags::Attribute_ColorG; }
		if (ChannelAttribute == StageCraftTags::Attribute_ColorY) { return StageCraftTags::Attribute_ColorB; }
		bOutInverted = false;
		return ChannelAttribute;
	}

	FStageDMXPatchOptions EffectiveOptions(const ALightingFixtureActor& Fixture)
	{
		bool bHasPan = false;
		bool bHasTilt = false;
		Fixture.GetPanTiltChannels(bHasPan, bHasTilt);
		return StageDMXPatchOptions::GetEffective(Fixture.GetPatchOptions(), bHasPan, bHasTilt);
	}

	template <typename TVisit>
	void ForEachReferenceChannel(const ALightingFixtureActor& Fixture, int32 UniverseNum, TVisit Visit)
	{
		const ULightingFixtureData* Data = Fixture.GetFixtureData();
		const FStageDMXPatchOptions Options = EffectiveOptions(Fixture);
		for (const FStageFixtureDMXChannel& Channel : Data->DMXChannels)
		{
			bool bInverted = false;
			FGameplayTag Attribute = ResolveReference(Channel.Attribute, bInverted);
			const FStageDMXChannelSource Source = StageDMXPatchOptions::GetChannelSource(Attribute, Options);
			Attribute = Source.Attribute;
			bInverted ^= Source.bInvert;
			if (!Data->SupportsAttribute(Channel.Attribute) || !Data->SupportsAttribute(Attribute))
			{
				continue;
			}
			const int32 Index = Fixture.GetPatch().Address - 1 + Channel.Channel - 1;
			if (Index < 0 || Index + Channel.GetWidth() > UniverseNum)
			{
				continue;
			}
			Visit(Channel, Attribute, bInverted, Index);
		}
	}

	TArray<uint8> ReferenceWrite(const ALightingFixtureActor& Fixture)
	{
		TArray<uint8> Universe;
		Universe.SetNumZeroed(UniverseSize);
		const ULightingFixtureData* Data = Fixture.GetFixtureData();
		ForEachReferenceChannel(Fixture, Universe.Num(), [&](const FStageFixtureDMXChannel& Channel, const FGameplayTag& Attribute, bool bInverted, int32 Index)
		{
			float Normalized = Data->NormalizeForDMX(Attribute, Fixture.GetAttribute(Attribute));
			Normalized = FMath::Clamp(bInverted ? 1.f - Normalized : Normalized, 0.f, 1.f);
			if (Channel.b16Bit)
			{
				const uint16 Value = static_cast<uint16>(FMath::RoundToInt(Normalized * 65535.f));
				Universe[Index] = static_cast<uint8>(Value >> 8);
				Universe[Index + 1] = static_cast<uint8>(Value & 0xFF);
			}
			else
			{
				Universe[Index] = static_cast<uint8>(FMath::RoundToInt(Normalized * 255.f));
			}
		});
		return Universe;
	}

	TMap<FGameplayTag, float> ReferenceRead(const ALightingFixtureActor& Fixture, const TArray<uint8>& Universe)
	{
		TMap<FGameplayTag, float> Values;
		const ULightingFixtureData* Data = Fixture.GetFixtureData();
		ForEachReferenceChannel(Fixture, Universe.Num(), [&](const FStageFixtureDMXChannel& Channel, const FGameplayTag& Attribute, bool bInverted, int32 Index)
		{
			float Normalized = Channel.b16Bit ? static_cast<float>((Universe[Index] << 8) | Universe[Index + 1]) / 65535.f : static_cast<float>(Universe[Index]) / 255.f;
			if (bInverted)
			{
				Normalized = 1.f - Normalized;
			}
			Values.Add(Attribute, Data->DenormalizeFromDMX(Attribute, Normalized));
		});
		return Values;
	}

	TArray<uint8> Render(const ALightingFixtureActor& Fixture, uint8 Fill = 0)
	{
		TArray<uint8> Universe;
		Universe.Init(Fill, UniverseSize);
		Fixture.WriteDMX(Universe);
		return Universe;
	}

	void SetRandomValues(ALightingFixtureActor& Fixture, FRandomStream& Random)
	{
		const ULightingFixtureData* Data = Fixture.GetFixtureData();
		TMap<FGameplayTag, float> Values;
		for (const FGameplayTag& Attribute : Data->GetSupportedAttributes())
		{
			float Min = 0.f;
			float Max = 1.f;
			Data->GetAttributeRange(Attribute, Min, Max);
			const float Span = FMath::Max(Max - Min, 1.f);
			Values.Add(Attribute, ULightingFixtureData::IsSteppedAttribute(Attribute) ? float(Random.RandRange(int32(Min), int32(Max)))
				: Random.FRandRange(Min - 0.05f * Span, Max + 0.05f * Span));
		}
		Fixture.SetAttributes(Values);
	}

	FStageDMXPatchOptions MakeOptions(int32 Bits)
	{
		FStageDMXPatchOptions Options;
		Options.bInvertPan = (Bits & 1) != 0;
		Options.bInvertTilt = (Bits & 2) != 0;
		Options.bSwapPanTilt = (Bits & 4) != 0;
		return Options;
	}

	ULightingFixtureData* BuildSpotStandard()
	{
		const FStageGdtfParseResult Parsed = StageGdtf::Parse(StageGdtfTestFiles::MakeGdtf(StageGdtfTestFiles::SpotXml()));
		if (!Parsed.IsOk())
		{
			return nullptr;
		}
		TArray<FStageGdtfModeLayout> Layouts;
		for (int32 Mode = 0; Mode < Parsed.Type.Modes.Num(); ++Mode)
		{
			Layouts.Add(StageGdtfMapping::BuildModeLayout(Parsed.Type, Mode));
		}
		const FStageGdtfCapabilities Capabilities = StageGdtfMapping::DeriveCapabilities(Parsed.Type, Layouts);
		ULightingFixtureData* Data = NewObject<ULightingFixtureData>(GetTransientPackage(), NAME_None, RF_Transient);
		StageGdtfFixtureBuilder::Fill(*Data, Parsed.Type, Capabilities, Layouts[0], {}, nullptr);
		return Data;
	}

	TMap<FGameplayTag, float> Values(std::initializer_list<TPair<FGameplayTag, float>> Entries)
	{
		TMap<FGameplayTag, float> Map;
		for (const TPair<FGameplayTag, float>& Entry : Entries)
		{
			Map.Add(Entry.Key, Entry.Value);
		}
		return Map;
	}

	int32 Read16(const TArray<uint8>& Universe, int32 Channel)
	{
		return (Universe[Channel - 1] << 8) | Universe[Channel];
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDMXFunctionsTest, "StageCraft.Fixtures.DMXFunctions", StageDMXFunctionTests::Flags)

bool FStageDMXFunctionsTest::RunTest(const FString& Parameters)
{
	using namespace StageDMXFunctionTests;
	using namespace StageCraftTags;
	const float Floor = StageGdtfMapping::StrobeFloor;

	const FStageFixtureDMXChannel Shutter = MakeChannel(Attribute_Shutter, false, 20, {
		Range(0, 15, {}, 0.f, 0.f), Range(16, 31, Attribute_Shutter, 0.f, 0.f), Range(32, 127, Attribute_Shutter, Floor, 1.f), Range(128, 255, {}, 0.f, 0.f) });
	TestTrue(TEXT("Shutter ranges are valid"), StageDMXFunctions::AreValid(Shutter));
	TestEqual(TEXT("Open sends the centre of the open range"), StageDMXFunctions::Encode(Shutter, 0.f), 23);
	TestEqual(TEXT("Slowest strobe at the range start"), StageDMXFunctions::Encode(Shutter, Floor), 32);
	TestEqual(TEXT("Fastest strobe at the range end"), StageDMXFunctions::Encode(Shutter, 1.f), 127);
	TestEqual(TEXT("Half strobe interpolated"), StageDMXFunctions::Encode(Shutter, 0.5f), 32 + FMath::RoundToInt((0.5f - Floor) / (1.f - Floor) * 95.f));
	TestEqual(TEXT("A value just above open goes to the nearest range"), StageDMXFunctions::Encode(Shutter, 0.004f), 23);
	TestEqual(TEXT("Above the top clamps to the fastest strobe"), StageDMXFunctions::Encode(Shutter, 4.f), 127);
	TestEqual(TEXT("Not a number sends the default"), StageDMXFunctions::Encode(Shutter, std::numeric_limits<float>::quiet_NaN()), 20);
	TestEqual(TEXT("Closed reads as open"), StageDMXFunctions::Decode(Shutter, 5).Get(-1.f), 0.f);
	TestEqual(TEXT("Random strobe reads as open"), StageDMXFunctions::Decode(Shutter, 200).Get(-1.f), 0.f);
	TestEqual(TEXT("Open reads as open"), StageDMXFunctions::Decode(Shutter, 16).Get(-1.f), 0.f);
	TestTrue(TEXT("Strobe start reads the floor"), FMath::IsNearlyEqual(StageDMXFunctions::Decode(Shutter, 32).Get(-1.f), Floor));
	TestTrue(TEXT("Strobe end reads 1"), FMath::IsNearlyEqual(StageDMXFunctions::Decode(Shutter, 127).Get(-1.f), 1.f));
	for (int32 Value = 32; Value <= 127; ++Value)
	{
		if (StageDMXFunctions::Encode(Shutter, StageDMXFunctions::Decode(Shutter, Value).Get(-1.f)) != Value)
		{
			AddError(FString::Printf(TEXT("Strobe DMX %d does not survive decode and encode"), Value));
			break;
		}
	}

	const FStageFixtureDMXChannel StrobeOnly = MakeChannel(Attribute_Shutter, false, 7, { Range(0, 9, {}, 0.f, 0.f), Range(10, 255, Attribute_Shutter, Floor, 1.f) });
	TestEqual(TEXT("Open on a channel without an open range sends the default"), StageDMXFunctions::Encode(StrobeOnly, 0.f), 7);
	TestEqual(TEXT("A strobe on it still strobes"), StageDMXFunctions::Encode(StrobeOnly, 1.f), 255);

	const FStageFixtureDMXChannel Gobo = MakeChannel(Attribute_Gobo, false, 0, {
		Range(0, 9, Attribute_Gobo, 0.f, 0.f, 0), Range(10, 19, Attribute_Gobo, 1.f, 1.f, 1), Range(20, 63, Attribute_Gobo, 2.f, 2.f, 2), Range(64, 255, {}, 0.f, 10.f) });
	TestEqual(TEXT("Open slot at its centre"), StageDMXFunctions::Encode(Gobo, 0.f), 4);
	TestEqual(TEXT("Slot 1 at its centre"), StageDMXFunctions::Encode(Gobo, 1.f), 14);
	TestEqual(TEXT("Slot 2 at its centre"), StageDMXFunctions::Encode(Gobo, 2.f), 41);
	TestEqual(TEXT("A fraction goes to the nearest slot"), StageDMXFunctions::Encode(Gobo, 1.4f), 14);
	TestEqual(TEXT("A slot past the wheel clamps to the last"), StageDMXFunctions::Encode(Gobo, 7.f), 41);
	TestEqual(TEXT("Any value in a slot range selects it"), StageDMXFunctions::Decode(Gobo, 19).Get(-1.f), 1.f);
	TestEqual(TEXT("Slot 2 from its first value"), StageDMXFunctions::Decode(Gobo, 20).Get(-1.f), 2.f);
	TestFalse(TEXT("Shake leaves the gobo as it is"), StageDMXFunctions::Decode(Gobo, 100).IsSet());

	const FStageFixtureDMXChannel Pan = MakeChannel(Attribute_Pan, true, 32768, { Range(0, 65535, Attribute_Pan, 270.f, -270.f) });
	TestEqual(TEXT("A falling range: +270 at 0"), StageDMXFunctions::Encode(Pan, 270.f), 0);
	TestEqual(TEXT("A falling range: -270 at 65535"), StageDMXFunctions::Encode(Pan, -270.f), 65535);
	TestEqual(TEXT("A falling range: centre"), StageDMXFunctions::Encode(Pan, 0.f), 32768);
	TestTrue(TEXT("16-bit decode"), FMath::IsNearlyEqual(StageDMXFunctions::Decode(Pan, 65535).Get(0.f), -270.f));
	TestEqual(TEXT("A received value above the resolution is clamped"), StageDMXFunctions::Decode(Pan, 70000).Get(0.f), -270.f);
	TestEqual(TEXT("A negative received value is clamped"), StageDMXFunctions::Decode(Pan, -4).Get(0.f), 270.f);

	const FStageFixtureDMXChannel Zoom = MakeChannel(Attribute_Zoom, false, 0, { Range(0, 99, Attribute_Zoom, 8.f, 20.f), Range(100, 199, {}, 0.f, 0.f), Range(200, 255, Attribute_Zoom, 20.f, 40.f) });
	TestEqual(TEXT("The first range of the attribute that holds the value wins"), StageDMXFunctions::Encode(Zoom, 20.f), 99);
	TestEqual(TEXT("A later range of the attribute"), StageDMXFunctions::Encode(Zoom, 40.f), 255);
	TestEqual(TEXT("Below every range clamps to the nearest"), StageDMXFunctions::Encode(Zoom, 2.f), 0);
	TestTrue(TEXT("A later range decodes"), FMath::IsNearlyEqual(StageDMXFunctions::Decode(Zoom, 200).Get(0.f), 20.f));

	const FStageFixtureDMXChannel Unowned = MakeChannel(Attribute_Iris, false, 99, { Range(0, 255, {}, 0.f, 1.f) });
	TestEqual(TEXT("No range of the attribute sends the default"), StageDMXFunctions::Encode(Unowned, 0.5f), 99);
	TestFalse(TEXT("No range of the attribute never decodes"), StageDMXFunctions::Decode(Unowned, 30).IsSet());
	const FStageFixtureDMXChannel Hostile = MakeChannel(Attribute_Iris, false, 9000, { Range(-50, 9000, Attribute_Iris, 0.f, 1.f) });
	TestEqual(TEXT("A range outside the resolution is clamped"), StageDMXFunctions::Encode(Hostile, 1.f), 255);
	TestEqual(TEXT("A default outside the resolution is clamped"), StageDMXFunctions::Encode(MakeChannel(Attribute_Iris, false, 9000, { Range(0, 255, {}, 0.f, 1.f) }), 1.f), 255);

	TestFalse(TEXT("Hostile ranges are not valid"), StageDMXFunctions::AreValid(Hostile));
	TestFalse(TEXT("Overlapping ranges are not valid"), StageDMXFunctions::AreValid(MakeChannel(Attribute_Iris, false, 0, { Range(0, 100, Attribute_Iris, 0.f, 1.f), Range(100, 255, {}, 0.f, 0.f) })));
	TestFalse(TEXT("Reversed ranges are not valid"), StageDMXFunctions::AreValid(MakeChannel(Attribute_Iris, false, 0, { Range(50, 10, Attribute_Iris, 0.f, 1.f) })));
	TestFalse(TEXT("Non-finite values are not valid"), StageDMXFunctions::AreValid(MakeChannel(Attribute_Iris, false, 0, { Range(0, 255, Attribute_Iris, 0.f, std::numeric_limits<float>::infinity()) })));
	TArray<FStageDMXFunction> Many;
	for (int32 Index = 0; Index <= StageDMXFunctions::MaxFunctions; ++Index)
	{
		Many.Add(Range(Index, Index, Attribute_Iris, 0.f, 1.f));
	}
	TestFalse(TEXT("More than MaxFunctions ranges are not valid"), StageDMXFunctions::AreValid(MakeChannel(Attribute_Iris, true, 0, MoveTemp(Many))));
	TestTrue(TEXT("Pan, zoom and gobo ranges are valid"), StageDMXFunctions::AreValid(Pan) && StageDMXFunctions::AreValid(Zoom) && StageDMXFunctions::AreValid(Gobo));

	TestEqual(TEXT("Strobe Hz on the fixture's span"), StageGdtfFixtureBuilder::StrobeHzToShutter(10.f, FVector2D(1.0, 25.0)), 0.375f);
	TestEqual(TEXT("The slowest strobe is just above open"), StageGdtfFixtureBuilder::StrobeHzToShutter(1.f, FVector2D(1.0, 25.0)), Floor);
	TestEqual(TEXT("Faster than the fixture clamps to 1"), StageGdtfFixtureBuilder::StrobeHzToShutter(60.f, FVector2D(1.0, 25.0)), 1.f);
	TestEqual(TEXT("A single rate is 1"), StageGdtfFixtureBuilder::StrobeHzToShutter(10.f, FVector2D(10.0, 10.0)), 1.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDMXBuiltInsUnchangedTest, "StageCraft.Fixtures.DMXBuiltInsUnchanged", StageDMXFunctionTests::Flags)

bool FStageDMXBuiltInsUnchangedTest::RunTest(const FString& Parameters)
{
	using namespace StageDMXFunctionTests;
	static const TCHAR* const Paths[] = {
		TEXT("/Game/StageCraft/Fixtures/DA_Fixture_Spot470.DA_Fixture_Spot470"), TEXT("/Game/StageCraft/Fixtures/DA_Fixture_WashRGBW.DA_Fixture_WashRGBW"),
		TEXT("/Game/StageCraft/Fixtures/DA_Fixture_Beam.DA_Fixture_Beam"), TEXT("/Game/StageCraft/Fixtures/DA_Fixture_Hybrid.DA_Fixture_Hybrid"),
		TEXT("/Game/StageCraft/Fixtures/DA_Fixture_ProfileMH.DA_Fixture_ProfileMH"), TEXT("/Game/StageCraft/Fixtures/DA_Fixture_ParRGBWA.DA_Fixture_ParRGBWA"),
		TEXT("/Game/StageCraft/Fixtures/DA_Fixture_Strobe.DA_Fixture_Strobe"), TEXT("/Game/StageCraft/Fixtures/DA_Fixture_Blinder4.DA_Fixture_Blinder4"),
		TEXT("/Game/StageCraft/Fixtures/DA_Fixture_PixelBar16.DA_Fixture_PixelBar16"), TEXT("/Game/StageCraft/Fixtures/DA_Fixture_Ellipsoidal26.DA_Fixture_Ellipsoidal26"),
		TEXT("/Game/StageCraft/Fixtures/DA_Fixture_Fresnel.DA_Fixture_Fresnel"), TEXT("/Game/StageCraft/Data/DA_MovingHead_Test.DA_MovingHead_Test"),
		TEXT("/Game/StageCraft/Data/DA_MovingHead_Wash_Test.DA_MovingHead_Wash_Test") };

	FScopedWorld World;
	FRandomStream Random(0x6D3);
	int32 Frames = 0;
	for (const TCHAR* Path : Paths)
	{
		ULightingFixtureData* Data = LoadObject<ULightingFixtureData>(nullptr, Path);
		if (!TestNotNull(*FString::Printf(TEXT("%s loads"), Path), Data))
		{
			continue;
		}
		const FString Name = Data->GetName();
		TestTrue(*FString::Printf(TEXT("%s: every channel has an attribute, no functions and default 0"), *Name),
			!Data->DMXChannels.ContainsByPredicate([](const FStageFixtureDMXChannel& Channel) { return Channel.SendsDefaultWhenUndriven() || Channel.DefaultDMX != 0; }));

		ALightingFixtureActor* Fixture = World.Spawn(Data, 7);
		ALightingFixtureActor* Expected = World.Spawn(Data, 7);
		for (int32 Bits = 0; Bits < 8; ++Bits)
		{
			Fixture->SetPatchOptions(MakeOptions(Bits));
			Expected->SetPatchOptions(MakeOptions(Bits));
			for (int32 Frame = 0; Frame < 4; ++Frame)
			{
				SetRandomValues(*Fixture, Random);
				if (Render(*Fixture) != ReferenceWrite(*Fixture))
				{
					AddError(FString::Printf(TEXT("%s, options %d: WriteDMX differs from the encoding before GD3"), *Name, Bits));
				}

				TArray<uint8> Wire;
				Wire.SetNumUninitialized(UniverseSize);
				for (uint8& Byte : Wire)
				{
					Byte = uint8(Random.RandRange(0, 255));
				}
				const TMap<FGameplayTag, float> Reference = ReferenceRead(*Fixture, Wire);
				Fixture->ReadDMX(Wire);
				Expected->SetAttributes(Reference);
				for (const TPair<FGameplayTag, float>& Entry : Reference)
				{
					const FGameplayTag& Attribute = Entry.Key;
					if (Fixture->GetAttribute(Attribute) != Expected->GetAttribute(Attribute))
					{
						AddError(FString::Printf(TEXT("%s, options %d: ReadDMX sets %s to %f, before GD3 %f"), *Name, Bits, *Attribute.ToString(),
							Fixture->GetAttribute(Attribute), Expected->GetAttribute(Attribute)));
						break;
					}
				}
				++Frames;
			}
		}
	}
	AddInfo(FString::Printf(TEXT("%d frames compared both ways."), Frames));
	TestEqual(TEXT("Every built-in compared"), Frames, int32(UE_ARRAY_COUNT(Paths)) * 8 * 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDMXImportedFixtureTest, "StageCraft.Fixtures.DMXImported", StageDMXFunctionTests::Flags)

bool FStageDMXImportedFixtureTest::RunTest(const FString& Parameters)
{
	using namespace StageDMXFunctionTests;
	using namespace StageCraftTags;

	ULightingFixtureData* Data = BuildSpotStandard();
	if (!TestNotNull(TEXT("The test spot builds"), Data))
	{
		return false;
	}
	auto FindChannel = [Data](int32 Number) { return Data->DMXChannels.FindByPredicate([Number](const FStageFixtureDMXChannel& Channel) { return Channel.Channel == Number; }); };
	const FStageFixtureDMXChannel* ShutterChannel = FindChannel(6);
	TestTrue(TEXT("Shutter: closed, open, strobe and random ranges"), ShutterChannel && ShutterChannel->Functions.Num() == 4 && !ShutterChannel->Functions[0].Attribute.IsValid()
		&& ShutterChannel->Functions[1].Attribute == Attribute_Shutter && !ShutterChannel->Functions[3].Attribute.IsValid());
	TestTrue(TEXT("Every driven channel has valid functions"), !Data->DMXChannels.ContainsByPredicate([](const FStageFixtureDMXChannel& Channel)
		{ return Channel.Attribute.IsValid() && (Channel.Functions.IsEmpty() || !StageDMXFunctions::AreValid(Channel)); }));
	TestTrue(TEXT("Strobe rates from the file"), Data->StrobeRateHz.Equals(FVector2D(1.0, 25.0)));
	const FStageFixtureDMXChannel* PanChannel = FindChannel(1);
	TestTrue(TEXT("Pan default 128/1 byte-mirrored on a 16-bit channel"), PanChannel && PanChannel->DefaultDMX == 0x8080);

	FScopedWorld World;
	ALightingFixtureActor* Fixture = World.Spawn(Data);
	TMap<FGameplayTag, float> Look;
	Look.Add(Attribute_Pan, 0.f);
	Look.Add(Attribute_Tilt, -135.f);
	Look.Add(Attribute_Dimmer, 1.f);
	Look.Add(Attribute_Shutter, 0.f);
	Look.Add(Attribute_ColorWheel, 2.f);
	Look.Add(Attribute_Gobo, 1.f);
	Look.Add(Attribute_Prism, 1.f);
	Look.Add(Attribute_Zoom, 24.f);
	Fixture->SetAttributes(Look);

	TArray<uint8> Universe = Render(*Fixture, 0xEE);
	TestEqual(TEXT("Focus has no control and sends its default"), int32(Universe[12]), 128);
	TestEqual(TEXT("Control sends its default"), int32(Universe[13]), 0);
	TestEqual(TEXT("Past the footprint untouched"), int32(Universe[15]), 0xEE);
	TestEqual(TEXT("Pan centre"), Read16(Universe, 1), 32768);
	TestEqual(TEXT("Tilt at its lowest"), Read16(Universe, 3), 0);
	TestEqual(TEXT("Dimmer full"), int32(Universe[4]), 255);
	TestEqual(TEXT("Shutter open: centre of 16..31"), int32(Universe[5]), 23);
	TestEqual(TEXT("Colour wheel Blue: centre of 20..127"), int32(Universe[6]), 73);
	TestEqual(TEXT("Gobo Dots: centre of 10..19"), int32(Universe[7]), 14);
	TestEqual(TEXT("Prism 3-Facet: centre of 128..255"), int32(Universe[9]), 191);
	TestEqual(TEXT("Zoom 24 of 8..40"), Read16(Universe, 11), 32768);

	const float TenHz = StageGdtfFixtureBuilder::StrobeHzToShutter(10.f, Data->StrobeRateHz);
	Fixture->SetAttributes(Values({ { Attribute_Shutter, TenHz } }));
	TestTrue(TEXT("The fixture strobes at 10 Hz"), FMath::IsNearlyEqual(Fixture->GetStrobeRateHz(), 10.f, 0.01f));
	Universe = Render(*Fixture);
	const float FileHz = 1.f + 24.f * float(Universe[5] - 32) / 95.f;
	TestTrue(*FString::Printf(TEXT("DMX %d is 10 Hz on the real fixture within one step (%.2f Hz)"), Universe[5], FileHz), FMath::Abs(FileHz - 10.f) <= 24.f / 95.f);
	Fixture->SetAttributes(Values({ { Attribute_Shutter, 1.f } }));
	TestEqual(TEXT("Fastest strobe at the end of the strobe range"), int32(Render(*Fixture)[5]), 127);

	ALightingFixtureActor* Receiver = World.Spawn(Data);
	Receiver->SetAttributes(Look);
	Receiver->SetAttributes(Values({ { Attribute_Shutter, 0.5f }, { Attribute_Gobo, 2.f } }));
	TArray<uint8> Wire = Render(*Fixture);
	Wire[5] = 5;
	Wire[7] = 100;
	Wire[6] = 15;
	Wire[9] = 200;
	Wire[13] = 255;
	Wire[0] = 0;
	Wire[1] = 0;
	Receiver->ReadDMX(Wire);
	TestEqual(TEXT("Closed shutter reads as open"), Receiver->GetAttribute(Attribute_Shutter), 0.f);
	TestEqual(TEXT("Gobo shake leaves the gobo"), Receiver->GetAttribute(Attribute_Gobo), 2.f);
	TestEqual(TEXT("Colour wheel Red from inside its range"), Receiver->GetAttribute(Attribute_ColorWheel), 1.f);
	TestEqual(TEXT("Prism from inside its range"), Receiver->GetAttribute(Attribute_Prism), 1.f);
	TestTrue(TEXT("Pan from its 16-bit value"), FMath::IsNearlyEqual(Receiver->GetAttribute(Attribute_Pan), -270.f, 0.01f));
	Wire[5] = 79;
	Receiver->ReadDMX(Wire);
	TestTrue(TEXT("A strobe value reads back"), FMath::IsNearlyEqual(Receiver->GetAttribute(Attribute_Shutter), StageDMXFunctions::Decode(*ShutterChannel, 79).Get(-1.f), 1.e-4f));

	FRandomStream Random(0x5D3);
	for (int32 Bits = 0; Bits < 8; ++Bits)
	{
		Fixture->SetPatchOptions(MakeOptions(Bits));
		Receiver->SetPatchOptions(MakeOptions(Bits));
		for (int32 Frame = 0; Frame < 8; ++Frame)
		{
			SetRandomValues(*Fixture, Random);
			const TArray<uint8> Sent = Render(*Fixture);
			Receiver->ReadDMX(Sent);
			if (Render(*Receiver) != Sent)
			{
				AddError(FString::Printf(TEXT("Options %d: what DMX IN reads does not send the same bytes again"), Bits));
				break;
			}
		}
	}
	Fixture->SetPatchOptions(MakeOptions(1));
	Fixture->SetAttributes(Values({ { Attribute_Pan, 270.f } }));
	TestEqual(TEXT("Inverted pan: +270 sends 0"), Read16(Render(*Fixture), 1), 0);
	return true;
}

#endif
