// Copyright Epic Games, Inc. All Rights Reserved.

// Unit tests for professional fixture features (Docs/ADR/0007-lighting-patch-paths-and-pipeline-hardening.md §5):
// photometrics (StagePhotometry) and fixture capabilities, ranges, DMX layout and the stepped gobo encoding
// (ULightingFixtureData). No world: fixture data objects are created transient.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Lighting; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/LightingFixtureData.h"
#include "Data/StageParameterTypes.h"
#include "Misc/AutomationTest.h"
#include "Show/StagePhotometry.h"

namespace StageLightingTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	/** A profile moving head: CTC, a three-gobo wheel, iris and framing shutters, with the matching default DMX layout. */
	ULightingFixtureData* MakeProfile()
	{
		ULightingFixtureData* Data = NewObject<ULightingFixtureData>(GetTransientPackage());
		Data->ColorSystem = EStageFixtureColorSystem::RGBW;
		Data->bHasColorTemperatureControl = true;
		Data->ColorTemperatureRangeK = FVector2D(2700.0, 8000.0);
		Data->GoboWheel.SetNum(3);
		Data->GoboWheel[0].Name = FText::FromString(TEXT("Dots"));
		Data->GoboWheel[0].Transmission = 0.5f;
		Data->bHasIris = true;
		Data->bHasFramingShutters = true;
		Data->BeamAngleMin = 5.f;
		Data->BeamAngleMax = 50.f;
#if WITH_EDITOR
		Data->GenerateDefaultDMXLayout();
#endif
		return Data;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageLightingPhotometryTest, "StageCraft.Lighting.Photometry", StageLightingTests::Flags)

bool FStageLightingPhotometryTest::RunTest(const FString& Parameters)
{
	using namespace StagePhotometry;

	// Solid angle: a hemisphere (180°) is 2 pi; tiny cones approach pi (theta / 2)^2.
	TestNearlyEqual(TEXT("Solid angle of a 179° cone is close to a hemisphere"), GetConeSolidAngle(179.0), 2.0 * UE_DOUBLE_PI * (1.0 - FMath::Cos(FMath::DegreesToRadians(89.5))), 1e-9);
	TestNearlyEqual(TEXT("Small-angle solid angle"), GetConeSolidAngle(2.0), UE_DOUBLE_PI * FMath::Square(FMath::DegreesToRadians(1.0)), 1e-6);

	// A known case: 20 000 lm into a 20° beam -> Omega = 2 pi (1 - cos 10°) = 0.095246 sr -> 209 982 cd; at 10 m 2 100 lx.
	FInputs Inputs;
	Inputs.LuminousFluxLm = 20000.0;
	Inputs.BeamAngleDegrees = 20.0;
	Inputs.Transmission = 1.0;
	Inputs.ThrowCm = 1000.0;
	const FResult Result = Compute(Inputs);
	const double ExpectedCd = 20000.0 / (2.0 * UE_DOUBLE_PI * (1.0 - FMath::Cos(FMath::DegreesToRadians(10.0))));
	TestNearlyEqual(TEXT("Intensity of 20 klm in 20°"), Result.IntensityCd, ExpectedCd, 1e-6);
	TestTrue(TEXT("Has a throw"), Result.bHasThrow);
	TestNearlyEqual(TEXT("Throw in metres"), Result.ThrowMetres, 10.0, 1e-9);
	TestNearlyEqual(TEXT("Illuminance follows the inverse square law"), Result.IlluminanceLux, ExpectedCd / 100.0, 1e-6);
	TestNearlyEqual(TEXT("Beam diameter at 10 m"), Result.BeamDiameterMetres, 2.0 * 10.0 * FMath::Tan(FMath::DegreesToRadians(10.0)), 1e-9);

	// Doubling the distance quarters the illuminance; halving transmission halves intensity.
	FInputs Far = Inputs;
	Far.ThrowCm = 2000.0;
	TestNearlyEqual(TEXT("Double distance, quarter illuminance"), Compute(Far).IlluminanceLux, Result.IlluminanceLux / 4.0, 1e-6);
	FInputs Dim = Inputs;
	Dim.Transmission = 0.5;
	TestNearlyEqual(TEXT("Half transmission, half intensity"), Compute(Dim).IntensityCd, Result.IntensityCd * 0.5, 1e-6);

	// No surface in range: intensity still known, throw-dependent values zero.
	FInputs NoThrow = Inputs;
	NoThrow.ThrowCm = 0.0;
	const FResult Open = Compute(NoThrow);
	TestFalse(TEXT("No throw"), Open.bHasThrow);
	TestEqual(TEXT("No illuminance without a surface"), Open.IlluminanceLux, 0.0);
	TestNearlyEqual(TEXT("Intensity without a surface"), Open.IntensityCd, ExpectedCd, 1e-6);

	// Hostile input never produces NaN or a negative value.
	FInputs Bad;
	Bad.LuminousFluxLm = -5.0;
	Bad.BeamAngleDegrees = std::numeric_limits<double>::quiet_NaN();
	Bad.Transmission = std::numeric_limits<double>::infinity();
	Bad.ThrowCm = -100.0;
	const FResult Sane = Compute(Bad);
	TestTrue(TEXT("Bad input: finite intensity"), FMath::IsFinite(Sane.IntensityCd) && Sane.IntensityCd >= 0.0);
	TestFalse(TEXT("Bad input: no throw"), Sane.bHasThrow);
	TestTrue(TEXT("Bad input: angle clamped"), Sane.BeamAngleDegrees >= MinBeamAngleDegrees && Sane.BeamAngleDegrees <= MaxBeamAngleDegrees);

	// Colour filters: white passes all, pure blue about 7 %, black nothing.
	TestNearlyEqual(TEXT("White transmission"), GetColorTransmission(FLinearColor::White), 1.0, 1e-6);
	TestNearlyEqual(TEXT("Blue transmission"), GetColorTransmission(FLinearColor(0.f, 0.f, 1.f)), 0.0722, 1e-6);
	TestEqual(TEXT("Black transmission"), GetColorTransmission(FLinearColor::Black), 0.0);

	// Framing shutters: open, one blade to the centre (half), opposite blades meeting (closed), two crossed (quarter).
	TestEqual(TEXT("Blades out"), GetShutterOpenFraction(0, 0, 0, 0), 1.0);
	TestNearlyEqual(TEXT("Top blade to centre"), GetShutterOpenFraction(1, 0, 0, 0), 0.5, 1e-9);
	TestEqual(TEXT("Opposite blades meet"), GetShutterOpenFraction(1, 0, 1, 0), 0.0);
	TestNearlyEqual(TEXT("Top and left to centre"), GetShutterOpenFraction(1, 0, 0, 1), 0.25, 1e-9);
	TestEqual(TEXT("Over-inserted blades never go negative"), GetShutterOpenFraction(5, 5, 5, 5), 0.0);

	// Iris: open changes nothing; a closed iris narrows the beam and passes its area, leaving the peak intensity unchanged.
	TestNearlyEqual(TEXT("Open iris"), ApplyIris(30.0, 0.0, 0.1), 30.0, 1e-9);
	const double Narrow = ApplyIris(30.0, 1.0, 0.25);
	TestNearlyEqual(TEXT("Closed iris scales tan(theta/2)"), FMath::Tan(FMath::DegreesToRadians(Narrow * 0.5)), 0.25 * FMath::Tan(FMath::DegreesToRadians(15.0)), 1e-9);
	TestNearlyEqual(TEXT("Closed iris passes its area"), GetIrisTransmission(1.0, 0.25), 0.0625, 1e-9);
	FInputs Wide;
	Wide.LuminousFluxLm = 10000.0;
	Wide.BeamAngleDegrees = 10.0;
	FInputs Irised = Wide;
	Irised.BeamAngleDegrees = ApplyIris(10.0, 1.0, 0.25);
	Irised.Transmission = GetIrisTransmission(1.0, 0.25);
	TestNearlyEqual(TEXT("An iris keeps the peak intensity (narrow beams)"), Compute(Irised).IntensityCd, Compute(Wide).IntensityCd, Compute(Wide).IntensityCd * 0.01);

	// Determinism.
	TestEqual(TEXT("Same inputs, same result"), Compute(Inputs).IntensityCd, Result.IntensityCd);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageLightingFixtureDataTest, "StageCraft.Lighting.FixtureData", StageLightingTests::Flags)

bool FStageLightingFixtureDataTest::RunTest(const FString& Parameters)
{
	ULightingFixtureData* Profile = StageLightingTests::MakeProfile();

	// Capabilities follow the data.
	TestTrue(TEXT("CTC supported"), Profile->SupportsAttribute(StageCraftTags::Attribute_ColorTemp));
	TestTrue(TEXT("Gobo supported"), Profile->SupportsAttribute(StageCraftTags::Attribute_Gobo));
	TestTrue(TEXT("Iris supported"), Profile->SupportsAttribute(StageCraftTags::Attribute_Iris));
	TestTrue(TEXT("Blades supported"), Profile->SupportsAttribute(StageCraftTags::Attribute_BladeC));
	TestTrue(TEXT("Beam shaping"), Profile->HasBeamShaping());

	ULightingFixtureData* Par = NewObject<ULightingFixtureData>(GetTransientPackage());
	Par->ColorSystem = EStageFixtureColorSystem::RGB;
	TestFalse(TEXT("Par: no gobo"), Par->SupportsAttribute(StageCraftTags::Attribute_Gobo));
	TestFalse(TEXT("Par: no CTC unless declared"), Par->SupportsAttribute(StageCraftTags::Attribute_ColorTemp));
	TestFalse(TEXT("Par: no beam shaping"), Par->HasBeamShaping());
	Par->bHasColorTemperatureControl = true;
	Par->ColorSystem = EStageFixtureColorSystem::None;
	TestFalse(TEXT("CTC needs a colour-mixing fixture"), Par->SupportsAttribute(StageCraftTags::Attribute_ColorTemp));

	// Ranges and defaults.
	float Min = 0.f, Max = 0.f;
	TestTrue(TEXT("Gobo range"), Profile->GetAttributeRange(StageCraftTags::Attribute_Gobo, Min, Max));
	TestEqual(TEXT("Gobo range is 0..wheel size"), FVector2f(Min, Max), FVector2f(0.f, 3.f));
	TestTrue(TEXT("CTC range"), Profile->GetAttributeRange(StageCraftTags::Attribute_ColorTemp, Min, Max));
	TestEqual(TEXT("CTC range from data"), FVector2f(Min, Max), FVector2f(2700.f, 8000.f));
	TestEqual(TEXT("CTC default is neutral"), Profile->GetAttributeDefault(StageCraftTags::Attribute_ColorTemp), 6500.f);
	TestEqual(TEXT("Gobo default is open"), Profile->GetAttributeDefault(StageCraftTags::Attribute_Gobo), 0.f);
	TestEqual(TEXT("Iris default is open"), Profile->GetAttributeDefault(StageCraftTags::Attribute_Iris), 0.f);

	// Slots and names.
	TestEqual(TEXT("Slot count includes Open"), Profile->GetGoboSlotCount(), 4);
	TestEqual(TEXT("Slot 0 is Open"), Profile->GetGoboSlotName(0).ToString(), FString(TEXT("Open")));
	TestEqual(TEXT("Named slot"), Profile->GetGoboSlotName(1).ToString(), FString(TEXT("Dots")));
	TestEqual(TEXT("Unnamed slot"), Profile->GetGoboSlotName(2).ToString(), FString(TEXT("Gobo 2")));
	TestEqual(TEXT("Out-of-range slot reads as Open"), Profile->GetGoboSlotName(9).ToString(), FString(TEXT("Open")));
	TestEqual(TEXT("Open transmits all"), Profile->GetGoboTransmission(0), 1.f);
	TestEqual(TEXT("Slot transmission"), Profile->GetGoboTransmission(1), 0.5f);

#if WITH_EDITOR
	// The default layout covers every supported attribute exactly once, without overlaps.
	TSet<FGameplayTag> Mapped;
	TBitArray<> Used(false, 513);
	bool bOverlap = false;
	for (const FStageFixtureDMXChannel& Channel : Profile->DMXChannels)
	{
		Mapped.Add(Channel.Attribute);
		for (int32 Slot = Channel.Channel; Slot < Channel.Channel + Channel.GetWidth(); ++Slot)
		{
			bOverlap |= Used[Slot];
			Used[Slot] = true;
		}
	}
	TestFalse(TEXT("Default layout has no overlaps"), bOverlap);
	for (const FGameplayTag& Attribute : Profile->GetSupportedAttributes())
	{
		TestTrue(FString::Printf(TEXT("Layout maps %s"), *Attribute.ToString()), Mapped.Contains(Attribute));
	}
	// Pan 2 + Tilt 2 + Dimmer 2 + Shutter + RGBW 4 + CTC + Gobo + GoboRot + Iris + Zoom + 4 blades + blade rotation = 21.
	TestEqual(TEXT("Profile footprint"), Profile->GetDMXFootprint(), 21);
#endif

	// Stepped gobo encoding: each of the 4 slots owns a quarter of the range; encode -> decode round-trips every slot.
	const FGameplayTag Gobo = StageCraftTags::Attribute_Gobo;
	for (int32 Slot = 0; Slot < 4; ++Slot)
	{
		const float Normalized = Profile->NormalizeForDMX(Gobo, static_cast<float>(Slot));
		TestNearlyEqual(FString::Printf(TEXT("Slot %d encodes to its share's centre"), Slot), Normalized, (Slot + 0.5f) / 4.f, 1e-6f);
		TestEqual(FString::Printf(TEXT("Slot %d round-trips"), Slot), Profile->DenormalizeFromDMX(Gobo, Normalized), static_cast<float>(Slot));
	}
	TestEqual(TEXT("DMX 0 is Open"), Profile->DenormalizeFromDMX(Gobo, 0.f), 0.f);
	TestEqual(TEXT("DMX 255 is the last gobo"), Profile->DenormalizeFromDMX(Gobo, 1.f), 3.f);
	TestEqual(TEXT("Just below a share boundary"), Profile->DenormalizeFromDMX(Gobo, 0.2499f), 0.f);
	TestEqual(TEXT("At a share boundary"), Profile->DenormalizeFromDMX(Gobo, 0.25f), 1.f);
	TestEqual(TEXT("NaN decodes to Open"), Profile->DenormalizeFromDMX(Gobo, std::numeric_limits<float>::quiet_NaN()), 0.f);

	// Continuous attributes stay linear.
	TestNearlyEqual(TEXT("CTC midpoint encodes to 0.5"), Profile->NormalizeForDMX(StageCraftTags::Attribute_ColorTemp, 5350.f), 0.5f, 1e-6f);
	TestNearlyEqual(TEXT("CTC decode"), Profile->DenormalizeFromDMX(StageCraftTags::Attribute_ColorTemp, 1.f), 8000.f, 1e-3f);
	TestEqual(TEXT("Unsupported attributes encode to 0"), Par->NormalizeForDMX(Gobo, 2.f), 0.f);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
