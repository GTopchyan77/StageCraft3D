// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the professional fixture suite (STATE.md #49, Docs/ADR/0020-professional-fixture-suite.md): colour science (additive
// emitters, subtractive filters, CTO, gel matching), the gel library, cell attributes, the new capabilities and their DMX layout,
// the fixture actor's beam (mix, field angle, frost, prism, gels, cells, cues) and the eleven library fixtures.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Lighting; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/LightingFixtureActor.h"
#include "Components/SpotLightComponent.h"
#include "Data/LightingFixtureData.h"
#include "Data/StageParameterTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "Show/StageColorScience.h"
#include "Show/StageFixtureCells.h"
#include "Show/StageGelLibrary.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "UObject/UnrealType.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#include <limits>

namespace StageFixtureSuiteTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	bool NearColor(const FLinearColor& A, const FLinearColor& B, float Tolerance = 0.02f)
	{
		return FMath::IsNearlyEqual(A.R, B.R, Tolerance) && FMath::IsNearlyEqual(A.G, B.G, Tolerance) && FMath::IsNearlyEqual(A.B, B.B, Tolerance);
	}

	FLinearColor Peak(const FLinearColor& Color)
	{
		const float Max = FMath::Max3(Color.R, Color.G, Color.B);
		return Max > 0.f ? Color / Max : FLinearColor::Black;
	}

	ULightingFixtureData* MakeData(EStageFixtureColorSystem System)
	{
		ULightingFixtureData* Data = NewObject<ULightingFixtureData>(GetTransientPackage());
		Data->ColorSystem = System;
		Data->BeamAngleMin = 10.f;
		Data->BeamAngleMax = 40.f;
		return Data;
	}

	void Relayout(ULightingFixtureData& Data)
	{
#if WITH_EDITOR
		Data.GenerateDefaultDMXLayout();
#endif
	}

	/** A game world that has begun play, destroyed with the scope. */
	struct FScopedWorld
	{
		UWorld* World = nullptr;

		FScopedWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageFixtureSuiteTestWorld"));
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

		ALightingFixtureActor* Spawn(ULightingFixtureData* Data) const
		{
			ALightingFixtureActor* Fixture = World->SpawnActorDeferred<ALightingFixtureActor>(ALightingFixtureActor::StaticClass(), FTransform::Identity);
			Fixture->InitializeFromItemData(Data);
			Fixture->FinishSpawning(FTransform::Identity);
			return Fixture;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageColorScienceTest, "StageCraft.Lighting.ColorScience", StageFixtureSuiteTests::Flags)

bool FStageColorScienceTest::RunTest(const FString& Parameters)
{
	using namespace StageFixtureSuiteTests;
	using namespace StageColorScience;
	constexpr float NaN = std::numeric_limits<float>::quiet_NaN();

	// Black bodies and mired shifts: a full CTO takes daylight to tungsten, a full CTB back.
	const FLinearColor Daylight = KelvinToLinear(6500.0);
	TestTrue(TEXT("6500 K is near white"), Daylight.R > 0.9f && Daylight.G > 0.9f && Daylight.B > 0.9f);
	const FLinearColor Tungsten = KelvinToLinear(3200.0);
	TestTrue(*FString::Printf(TEXT("3200 K is warm (%s)"), *Tungsten.ToString()), Tungsten.R >= Tungsten.G && Tungsten.G > Tungsten.B && Tungsten.B < 0.8f);
	TestTrue(TEXT("full CTO: 6500 K -> about 3200 K"), FMath::IsNearlyEqual(ShiftKelvinByMired(6500.0, 159.0), 3200.0, 60.0));
	TestTrue(TEXT("full CTB: 3200 K -> about 6500 K"), FMath::IsNearlyEqual(ShiftKelvinByMired(3200.0, -159.0), 6500.0, 150.0));
	TestTrue(TEXT("absurd shift is clamped"), ShiftKelvinByMired(6500.0, -1.0e6) <= MaxKelvin && ShiftKelvinByMired(6500.0, 1.0e6) >= MinKelvin);
	TestTrue(TEXT("NaN Kelvin falls back to daylight"), NearColor(KelvinToLinear(std::numeric_limits<double>::quiet_NaN()), Daylight));

	// Additive: an RGBW fixture's white emitter is a real white, so RGB alone is about half its output.
	const TArray<FLinearColor> Rgbw = { FLinearColor(1.f, 0.f, 0.f), FLinearColor(0.f, 1.f, 0.f), FLinearColor(0.f, 0.f, 1.f), KelvinToLinear(5600.0) };
	const FMix FullOn = MixAdditive(Rgbw, { 1.f, 1.f, 1.f, 1.f });
	TestTrue(TEXT("everything at full is the rated output"), FMath::IsNearlyEqual(FullOn.LuminousFraction, 1.0, 1e-6));
	const FMix RgbOnly = MixAdditive(Rgbw, { 1.f, 1.f, 1.f, 0.f });
	TestTrue(*FString::Printf(TEXT("RGB white without W is about half (%.2f)"), RgbOnly.LuminousFraction), RgbOnly.LuminousFraction > 0.45 && RgbOnly.LuminousFraction < 0.6);
	TestTrue(TEXT("... and pure white"), NearColor(RgbOnly.Color, FLinearColor::White));
	TestTrue(TEXT("saturated blue carries little light"), MixAdditive(Rgbw, { 0.f, 0.f, 1.f, 0.f }).LuminousFraction < 0.05);
	TestTrue(TEXT("dark is black"), MixAdditive(Rgbw, { 0.f, 0.f, 0.f, 0.f }).LuminousFraction == 0.0);
	TestTrue(TEXT("NaN and missing levels count as off"), FMath::IsNearlyEqual(MixAdditive(Rgbw, { 1.f, NaN }).LuminousFraction, MixAdditive(Rgbw, { 1.f }).LuminousFraction));

	// Subtractive: the lamp's white through the filters; CTO warms and costs light.
	const FMix Open = MixSubtractive(6500.0, FLinearColor::White, 0.0, 159.0);
	TestTrue(TEXT("open white passes everything"), FMath::IsNearlyEqual(Open.LuminousFraction, 1.0, 1e-6));
	const FMix Warm = MixSubtractive(6500.0, FLinearColor::White, 1.0, 159.0);
	TestTrue(TEXT("full CTO is warm"), FMath::IsNearlyEqual(Warm.Color.R, 1.f) && Warm.Color.B < 0.7f);
	TestTrue(TEXT("... and loses light"), Warm.LuminousFraction < Open.LuminousFraction);
	const FMix Red = MixSubtractive(6500.0, FLinearColor(1.f, 0.f, 0.f), 0.0, 159.0);
	TestTrue(TEXT("CMY red"), Red.Color.R > 0.99f && Red.Color.G < 0.01f && Red.LuminousFraction < 0.3);

	// Filters and combined sources.
	TestTrue(TEXT("a clear filter keeps the mix"), NearColor(ApplyFilter(FullOn, FLinearColor::White).Color, FullOn.Color));
	TestTrue(TEXT("a red filter on white is red"), ApplyFilter(Open, FLinearColor(1.f, 0.f, 0.f)).Color.G < 0.01f);
	TestTrue(TEXT("a NaN filter passes nothing"), ApplyFilter(Open, FLinearColor(NaN, NaN, NaN)).LuminousFraction == 0.0);
	FMix RedCell;
	RedCell.Color = FLinearColor(1.f, 0.f, 0.f);
	RedCell.LuminousFraction = 0.2;
	FMix BlueCell;
	BlueCell.Color = FLinearColor(0.f, 0.f, 1.f);
	BlueCell.LuminousFraction = 0.04;
	const FMix Bar = Combine({ RedCell, BlueCell });
	TestTrue(TEXT("two cells mix red and blue"), Bar.Color.R > 0.f && Bar.Color.B > 0.f && Bar.Color.G < 0.01f);
	TestTrue(TEXT("... at their mean output"), FMath::IsNearlyEqual(Bar.LuminousFraction, 0.12, 1e-6));
	TestTrue(TEXT("no sources is dark"), Combine({}).LuminousFraction == 0.0);

	// Matching a colour: the mix of the levels found is the colour asked for, as bright as possible.
	const FLinearColor FullWhite = Rgbw[0] + Rgbw[1] + Rgbw[2] + Rgbw[3];
	const TArray<FLinearColor> Targets = { FullWhite, FLinearColor(1.f, 0.f, 0.f), FullWhite * FLinearColor(1.f, 0.6f, 0.3f), FLinearColor(0.2f, 0.5f, 1.f) };
	for (const FLinearColor& Target : Targets)
	{
		const TArray<float> Levels = MatchAdditive(Rgbw, Target);
		const float Highest = FMath::Max(FMath::Max3(Levels[0], Levels[1], Levels[2]), Levels[3]);
		TestTrue(*FString::Printf(TEXT("match %s: brightest level is 1"), *Target.ToString()), FMath::IsNearlyEqual(Highest, 1.f));
		TestTrue(*FString::Printf(TEXT("match %s: same colour"), *Target.ToString()), NearColor(MixAdditive(Rgbw, Levels).Color, Peak(Target)));
	}
	const TArray<float> White = MatchAdditive(Rgbw, FullWhite);
	TestTrue(TEXT("white uses the white emitter fully"), FMath::IsNearlyEqual(White[3], 1.f));
	TestTrue(TEXT("a pure red does not use white"), MatchAdditive(Rgbw, FLinearColor(1.f, 0.f, 0.f))[3] == 0.f);
	TestTrue(TEXT("black asks for nothing"), MatchAdditive(Rgbw, FLinearColor::Black)[0] == 0.f);
	TestTrue(TEXT("NaN asks for nothing"), MatchAdditive(Rgbw, FLinearColor(NaN, 1.f, 1.f))[1] == 0.f);
	TestEqual(TEXT("one emitter follows the brightness"), MatchAdditive({ FLinearColor::White }, FLinearColor(0.5f, 0.5f, 0.5f))[0], 0.5f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageGelLibraryTest, "StageCraft.Lighting.GelLibrary", StageFixtureSuiteTests::Flags)

bool FStageGelLibraryTest::RunTest(const FString& Parameters)
{
	const auto Row = [](EStageGelBrand Brand, const TCHAR* Number, const TCHAR* Name, FColor Swatch)
	{
		FStageGelRow Gel;
		Gel.Brand = Brand;
		Gel.Number = Number;
		Gel.DisplayName = FText::FromString(Name);
		Gel.Swatch = Swatch;
		return Gel;
	};
	const TArray<FStageGelRow> Rows = {
		Row(EStageGelBrand::Rosco, TEXT("27"), TEXT("Medium Red"), FColor(190, 0, 20)),
		Row(EStageGelBrand::Lee, TEXT("106"), TEXT("Primary Red"), FColor(230, 0, 20)),
		Row(EStageGelBrand::Lee, TEXT("002"), TEXT("Rose Pink"), FColor(255, 64, 160)),
		Row(EStageGelBrand::Rosco, TEXT("02"), TEXT("Bastard Amber"), FColor(255, 190, 150)),
		Row(EStageGelBrand::Lee, TEXT("106"), TEXT("Duplicate"), FColor(255, 0, 0)),
		Row(EStageGelBrand::Lee, TEXT("x1"), TEXT("Bad Number"), FColor(255, 0, 0)),
		Row(EStageGelBrand::Lee, TEXT("999"), TEXT("Black"), FColor(0, 0, 0)),
	};
	const FStageGelLibrary Library = FStageGelLibrary::Build(Rows);
	const TArray<FStageGel>& Gels = Library.GetGels();
	TestEqual(TEXT("duplicates, bad numbers and black swatches are skipped"), Gels.Num(), 4);
	if (Gels.Num() != 4)
	{
		return false;
	}
	TestTrue(TEXT("Lee first, then by number"), Gels[0].GetCode() == TEXT("L002") && Gels[1].GetCode() == TEXT("L106") && Gels[2].GetCode() == TEXT("R02") && Gels[3].GetCode() == TEXT("R27"));
	TestEqual(TEXT("label"), Gels[1].GetLabel().ToString(), FString(TEXT("Lee 106 Primary Red")));
	TestTrue(TEXT("filter peaks at 1"), FMath::IsNearlyEqual(FMath::Max3(Gels[1].Filter.R, Gels[1].Filter.G, Gels[1].Filter.B), 1.f));

	const int32 L106 = 1;
	for (const TCHAR* Query : { TEXT("L106"), TEXT("lee 106"), TEXT("Lee106"), TEXT("LEE-106"), TEXT("106"), TEXT("primary red"), TEXT("Primary Red gel") })
	{
		TestEqual(*FString::Printf(TEXT("'%s' is Lee 106"), Query), Library.Find(Query), L106);
	}
	TestEqual(TEXT("'rosco 2' (leading zeros)"), Library.Find(TEXT("rosco 2")), 2);
	TestEqual(TEXT("'roscolux 27'"), Library.Find(TEXT("roscolux 27")), 3);
	TestEqual(TEXT("'r27'"), Library.Find(TEXT("r27")), 3);
	TestEqual(TEXT("a number both brands have is ambiguous"), Library.Find(TEXT("2")), INDEX_NONE);
	TestEqual(TEXT("a word several names share is ambiguous"), Library.Find(TEXT("red")), INDEX_NONE);
	TestEqual(TEXT("a word one name has"), Library.Find(TEXT("rose")), 0);
	TestEqual(TEXT("empty"), Library.Find(TEXT("  ")), INDEX_NONE);
	TestEqual(TEXT("unknown"), Library.Find(TEXT("lee 9999")), INDEX_NONE);
	TestEqual(TEXT("by code"), Library.FindByCode(TEXT("L002")), 0);

	// The project's library, from DT_StageGels (Scripts/Content/Data/StageGels.csv).
	const FStageGelLibrary& Project = FStageGelLibrary::Get();
	TestTrue(*FString::Printf(TEXT("project library has the swatch books (%d gels)"), Project.GetGels().Num()), Project.GetGels().Num() >= 100);
	TestTrue(TEXT("Lee 106 is in it"), Project.Find(TEXT("lee 106")) != INDEX_NONE);
	TestTrue(TEXT("Rosco 3202 is in it"), Project.Find(TEXT("rosco 3202")) != INDEX_NONE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageFixtureSuiteDataTest, "StageCraft.Lighting.FixtureSuiteData", StageFixtureSuiteTests::Flags)

bool FStageFixtureSuiteDataTest::RunTest(const FString& Parameters)
{
	using namespace StageFixtureSuiteTests;

	// Cell tags: every cell control exists and reads back.
	TestEqual(TEXT("cell tag name"), StageFixtureCells::GetCellAttribute(0, EStageCellChannel::ColorR).ToString(), FString(TEXT("StageCraft.Attribute.Cell.01.ColorR")));
	int32 Mismatches = 0;
	for (int32 Cell = 0; Cell < StageFixtureCells::MaxCells; ++Cell)
	{
		for (int32 Channel = 0; Channel < static_cast<int32>(EStageCellChannel::Count); ++Channel)
		{
			const FGameplayTag Tag = StageFixtureCells::GetCellAttribute(Cell, static_cast<EStageCellChannel>(Channel));
			int32 ParsedCell = INDEX_NONE;
			EStageCellChannel ParsedChannel = EStageCellChannel::Count;
			const bool bRoundTrip = Tag.IsValid() && Tag.MatchesTag(StageCraftTags::Attribute) && StageFixtureCells::ParseCellAttribute(Tag, ParsedCell, ParsedChannel)
				&& ParsedCell == Cell && ParsedChannel == static_cast<EStageCellChannel>(Channel);
			Mismatches += bRoundTrip ? 0 : 1;
		}
	}
	TestEqual(TEXT("all 32 x 7 cell tags round-trip"), Mismatches, 0);
	TestFalse(TEXT("cell 33 does not exist"), StageFixtureCells::GetCellAttribute(StageFixtureCells::MaxCells, EStageCellChannel::Dimmer).IsValid());
	TestFalse(TEXT("the master dimmer is not a cell"), StageFixtureCells::IsCellAttribute(StageCraftTags::Attribute_Dimmer));

	// A 16-cell RGBW pixel bar: colour lives in the cells, one DMX block each after dimmer and shutter.
	ULightingFixtureData* Bar = MakeData(EStageFixtureColorSystem::RGBW);
	Bar->bHasPanTilt = false;
	Bar->BeamAngleMin = Bar->BeamAngleMax = 20.f;
	Bar->CellCount = 16;
	Relayout(*Bar);
	TestFalse(TEXT("bar: no master colour"), Bar->SupportsAttribute(StageCraftTags::Attribute_ColorR));
	TestTrue(TEXT("bar: cell 16 white"), Bar->SupportsAttribute(StageFixtureCells::GetCellAttribute(15, EStageCellChannel::ColorW)));
	TestFalse(TEXT("bar: no cell 17"), Bar->SupportsAttribute(StageFixtureCells::GetCellAttribute(16, EStageCellChannel::ColorR)));
	TestFalse(TEXT("bar: colour cells have no own dimmer"), Bar->SupportsAttribute(StageFixtureCells::GetCellAttribute(0, EStageCellChannel::Dimmer)));
	TestEqual(TEXT("bar: attributes (dimmer, shutter, 64 cell channels)"), Bar->GetSupportedAttributes().Num(), 66);
#if WITH_EDITOR
	TestEqual(TEXT("bar: footprint 2 + 1 + 64"), Bar->GetDMXFootprint(), 67);
#endif

	// A 4-cell white blinder: a dimmer per lamp.
	ULightingFixtureData* Blinder = MakeData(EStageFixtureColorSystem::None);
	Blinder->bHasPanTilt = false;
	Blinder->CellCount = 4;
	Blinder->CellControl = EStageCellControl::Dimmer;
	Relayout(*Blinder);
	TestTrue(TEXT("blinder: cell dimmers"), Blinder->SupportsAttribute(StageFixtureCells::GetCellAttribute(3, EStageCellChannel::Dimmer)));
	TestFalse(TEXT("blinder: no cell colour"), Blinder->SupportsAttribute(StageFixtureCells::GetCellAttribute(0, EStageCellChannel::ColorR)));

	// Wheels, prisms, frost, CTO and amber.
	ULightingFixtureData* Spot = MakeData(EStageFixtureColorSystem::CMY);
	Spot->bHasCTO = true;
	Spot->ColorWheel.SetNum(3);
	Spot->GoboWheel.SetNum(2);
	Spot->bGoboSpin = true;
	Spot->StaticGoboWheel.SetNum(4);
	Spot->Prisms.SetNum(2);
	Spot->bHasFrost = true;
	Spot->MaxRotationRpm = 40.f;
	Relayout(*Spot);
	float Min = 0.f;
	float Max = 0.f;
	TestTrue(TEXT("colour wheel 0..3"), Spot->GetAttributeRange(StageCraftTags::Attribute_ColorWheel, Min, Max) && Min == 0.f && Max == 3.f);
	TestTrue(TEXT("static wheel 0..4"), Spot->GetAttributeRange(StageCraftTags::Attribute_Gobo2, Min, Max) && Max == 4.f);
	TestTrue(TEXT("prism 0..2"), Spot->GetAttributeRange(StageCraftTags::Attribute_Prism, Min, Max) && Max == 2.f);
	TestTrue(TEXT("spin +-40 rpm"), Spot->GetAttributeRange(StageCraftTags::Attribute_GoboSpin, Min, Max) && Min == -40.f && Max == 40.f);
	TestTrue(TEXT("CTO 0..1"), Spot->GetAttributeRange(StageCraftTags::Attribute_CTO, Min, Max) && Max == 1.f);
	TestTrue(TEXT("wheels are stepped"), ULightingFixtureData::IsSteppedAttribute(StageCraftTags::Attribute_Prism) && ULightingFixtureData::IsSteppedAttribute(StageCraftTags::Attribute_ColorWheel));
	TestEqual(TEXT("prism slot 2 survives DMX"), Spot->DenormalizeFromDMX(StageCraftTags::Attribute_Prism, Spot->NormalizeForDMX(StageCraftTags::Attribute_Prism, 2.f)), 2.f);
	TestFalse(TEXT("no CTO without the filter"), MakeData(EStageFixtureColorSystem::RGBW)->SupportsAttribute(StageCraftTags::Attribute_CTO));
	TestTrue(TEXT("RGBWA has amber"), MakeData(EStageFixtureColorSystem::RGBWA)->SupportsAttribute(StageCraftTags::Attribute_ColorA));
	TestEqual(TEXT("RGBWA has five default emitters"), MakeData(EStageFixtureColorSystem::RGBWA)->GetEmitterOutputs().Num(), 5);
#if WITH_EDITOR
	const TArray<FGameplayTag> SpotChannels = [Spot]
	{
		TArray<FGameplayTag> Tags;
		for (const FStageFixtureDMXChannel& Channel : Spot->DMXChannels)
		{
			Tags.Add(Channel.Attribute);
		}
		return Tags;
	}();
	TestTrue(TEXT("spot layout: CMY, CTO, wheels, prism, frost"), SpotChannels.Contains(StageCraftTags::Attribute_ColorC) && SpotChannels.Contains(StageCraftTags::Attribute_CTO)
		&& SpotChannels.Contains(StageCraftTags::Attribute_ColorWheel) && SpotChannels.Contains(StageCraftTags::Attribute_GoboSpin)
		&& SpotChannels.Contains(StageCraftTags::Attribute_Gobo2) && SpotChannels.Contains(StageCraftTags::Attribute_PrismRotation)
		&& SpotChannels.Contains(StageCraftTags::Attribute_Frost));
#endif

	// A gel frame: colour attributes for the inspector and cues, no DMX channels.
	ULightingFixtureData* Fresnel = MakeData(EStageFixtureColorSystem::None);
	Fresnel->bAcceptsGel = true;
	Fresnel->BeamShaper = EStageBeamShaper::Barndoors;
	Relayout(*Fresnel);
	TestTrue(TEXT("gel frame: colour attributes"), Fresnel->HasGelFrame() && Fresnel->SupportsAttribute(StageCraftTags::Attribute_ColorR));
	TestTrue(TEXT("barndoors use the blades"), Fresnel->SupportsAttribute(StageCraftTags::Attribute_BladeA));
	TestFalse(TEXT("gel frame: not on DMX"), Fresnel->DMXChannels.ContainsByPredicate([](const FStageFixtureDMXChannel& Channel) { return Channel.Attribute == StageCraftTags::Attribute_ColorR; }));

	// Assets saved before #49 had a bool for framing shutters.
	ULightingFixtureData* Old = MakeData(EStageFixtureColorSystem::RGBW);
	if (FBoolProperty* Deprecated = FindFProperty<FBoolProperty>(ULightingFixtureData::StaticClass(), TEXT("bHasFramingShutters")))
	{
		Deprecated->SetPropertyValue_InContainer(Old, true);
		Old->PostLoad();
		TestEqual(TEXT("old framing shutters become the beam shaper"), Old->BeamShaper, EStageBeamShaper::FramingShutters);
		TestFalse(TEXT("... and the old flag is cleared"), Deprecated->GetPropertyValue_InContainer(Old));
	}
	else
	{
		AddError(TEXT("bHasFramingShutters_DEPRECATED is missing; old assets would lose their shutters."));
	}

	// Emitters must be a full set, or the defaults stand.
	ULightingFixtureData* Partial = MakeData(EStageFixtureColorSystem::RGBW);
	Partial->Emitters.SetNum(2);
	TestEqual(TEXT("partial emitters fall back to the defaults"), Partial->GetEmitterOutputs().Num(), 4);

#if WITH_EDITOR
	const auto IsInvalid = [](const ULightingFixtureData& Data)
	{
		FDataValidationContext Context;
		return Data.IsDataValid(Context) == EDataValidationResult::Invalid;
	};
	ULightingFixtureData* WhiteColorCells = MakeData(EStageFixtureColorSystem::None);
	WhiteColorCells->CellCount = 4;
	WhiteColorCells->CellControl = EStageCellControl::Color;
	TestTrue(TEXT("colour cells without a colour system are invalid"), IsInvalid(*WhiteColorCells));
	TestTrue(TEXT("a partial emitter set is invalid"), IsInvalid(*Partial));
	TestFalse(TEXT("the bar is valid"), IsInvalid(*Bar));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageFixtureSuiteActorTest, "StageCraft.Lighting.FixtureSuiteActor", StageFixtureSuiteTests::Flags)

bool FStageFixtureSuiteActorTest::RunTest(const FString& Parameters)
{
	using namespace StageFixtureSuiteTests;
	FScopedWorld Scope;

	// An RGBW wash: true mixing, colour matching, field angle and frost.
	ULightingFixtureData* WashData = MakeData(EStageFixtureColorSystem::RGBW);
	WashData->BeamAngleMin = 10.f;
	WashData->BeamAngleMax = 40.f;
	WashData->FieldAngleRatio = 1.5f;
	WashData->bHasFrost = true;
	WashData->FrostFieldMultiplier = 2.f;
	ALightingFixtureActor* Wash = Scope.Spawn(WashData);
	USpotLightComponent* Beam = Wash ? Wash->FindComponentByClass<USpotLightComponent>() : nullptr;
	if (!TestNotNull(TEXT("wash with a beam"), Beam))
	{
		return false;
	}
	TestFalse(TEXT("fixtures never tick"), Wash->PrimaryActorTick.bCanEverTick);

	Wash->SetAttribute(StageCraftTags::Attribute_ColorW, 0.f);
	const double RgbFraction = Wash->GetBeamMix().LuminousFraction;
	Wash->SetAttribute(StageCraftTags::Attribute_ColorW, 1.f);
	TestTrue(*FString::Printf(TEXT("adding W adds light (%.2f -> %.2f)"), RgbFraction, Wash->GetBeamMix().LuminousFraction),
		Wash->GetBeamMix().LuminousFraction > RgbFraction * 1.5);

	const TMap<FGameplayTag, float> White = Wash->MatchColor(FLinearColor::White);
	TestTrue(TEXT("white is every emitter at full"), White.Num() == 4 && White.FindRef(StageCraftTags::Attribute_ColorW) > 0.99f && White.FindRef(StageCraftTags::Attribute_ColorR) > 0.99f);
	Wash->SetAttributes(Wash->MatchColor(FLinearColor(1.f, 0.f, 0.f)));
	TestTrue(TEXT("red is red"), NearColor(Wash->GetBeamMix().Color, FLinearColor(1.f, 0.f, 0.f)));

	Wash->SetAttribute(StageCraftTags::Attribute_Zoom, 20.f);
	TestTrue(*FString::Printf(TEXT("field angle 30: outer cone 15 (%.2f)"), Beam->OuterConeAngle), FMath::IsNearlyEqual(Beam->OuterConeAngle, 15.f, 0.01f));
	TestTrue(*FString::Printf(TEXT("beam 20, field 30: inner cone 5 (%.2f)"), Beam->InnerConeAngle), FMath::IsNearlyEqual(Beam->InnerConeAngle, 5.f, 0.01f));
	const double ClearCandela = Wash->ComputePhotometrics().IntensityCd;
	Wash->SetAttribute(StageCraftTags::Attribute_Frost, 1.f);
	TestTrue(*FString::Printf(TEXT("full frost doubles the field (%.2f)"), Beam->OuterConeAngle), FMath::IsNearlyEqual(Beam->OuterConeAngle, 30.f, 0.01f));
	TestTrue(TEXT("... and lowers the peak"), Wash->ComputePhotometrics().IntensityCd < ClearCandela * 0.5);
	TestNull(TEXT("frost alone projects nothing"), Beam->LightFunctionMaterial.Get());

	// A spot: a prism projects copies in a wider cone; CTO warms the lamp.
	ULightingFixtureData* SpotData = MakeData(EStageFixtureColorSystem::CMY);
	SpotData->bHasCTO = true;
	SpotData->FieldAngleRatio = 1.f;
	FStagePrism& Prism = SpotData->Prisms.AddDefaulted_GetRef();
	Prism.Facets = 3;
	Prism.Spread = 0.6f;
	ALightingFixtureActor* Spot = Scope.Spawn(SpotData);
	USpotLightComponent* SpotBeam = Spot->FindComponentByClass<USpotLightComponent>();
	Spot->SetAttribute(StageCraftTags::Attribute_Zoom, 20.f);
	const float OpenCone = SpotBeam->OuterConeAngle;
	const double OpenCandela = Spot->ComputePhotometrics().IntensityCd;
	Spot->SetAttribute(StageCraftTags::Attribute_Prism, 1.f);
	TestTrue(TEXT("the prism widens the cone"), SpotBeam->OuterConeAngle > OpenCone + 1.f);
	TestNotNull(TEXT("... and projects (light function)"), SpotBeam->LightFunctionMaterial.Get());
	TestTrue(TEXT("each copy gets a third"), Spot->ComputePhotometrics().IntensityCd < OpenCandela * 0.34);
	Spot->SetAttribute(StageCraftTags::Attribute_Prism, 0.f);
	TestTrue(TEXT("prism out: the cone is back"), FMath::IsNearlyEqual(SpotBeam->OuterConeAngle, OpenCone, 0.01f));
	Spot->SetAttribute(StageCraftTags::Attribute_CTO, 1.f);
	TestTrue(TEXT("full CTO warms the beam"), SpotBeam->GetLightColor().B < 0.7f && FMath::IsNearlyEqual(SpotBeam->GetLightColor().R, 1.f, 0.01f));

	// Gels: the picker sets the colour and names it; another colour edit clears the name.
	const FStageGelLibrary& Gels = FStageGelLibrary::Get();
	const int32 L106 = Gels.Find(TEXT("lee 106"));
	if (L106 != INDEX_NONE)
	{
		Spot->SetAttribute(StageCraftTags::Attribute_CTO, 0.f);
		TestTrue(TEXT("gel picker accepted"), IStageParameterInterface::Execute_SetParameterValue(Spot, StageCraftTags::Param_Fixture_Gel, FStageParameterValue::MakeEnum(L106 + 1)));
		TestEqual(TEXT("gel label"), Spot->GetAppliedGel(), FName(TEXT("L106")));
		TestTrue(TEXT("Lee 106 on CMY is red"), Spot->GetBeamMix().Color.G < 0.05f && Spot->GetBeamMix().Color.R > 0.99f);
		Spot->SetAttribute(StageCraftTags::Attribute_ColorG, 1.f);
		TestTrue(TEXT("a colour edit clears the gel label"), Spot->GetAppliedGel().IsNone());
	}
	else
	{
		AddError(TEXT("Lee 106 is not in the project's gel library (DT_StageGels)."));
	}

	// A pixel bar: cells take colour one by one or together, the beam is their mix, and cues record them.
	ULightingFixtureData* BarData = MakeData(EStageFixtureColorSystem::RGBW);
	BarData->bHasPanTilt = false;
	BarData->CellCount = 4;
	Relayout(*BarData);
	ALightingFixtureActor* Bar = Scope.Spawn(BarData);
	TestTrue(TEXT("all cells red"), IStageParameterInterface::Execute_SetParameterValue(Bar, StageCraftTags::Param_Fixture_AllCells, FStageParameterValue::MakeColor(FLinearColor(1.f, 0.f, 0.f))));
	TestTrue(TEXT("cell 3 is red"), Bar->GetCellMix(2).Color.G < 0.01f && Bar->GetCellMix(2).Color.R > 0.99f);
	TestTrue(TEXT("cell 4 blue"), IStageParameterInterface::Execute_SetParameterValue(Bar, StageFixtureCells::GetCellAttribute(3, EStageCellChannel::ColorRGB), FStageParameterValue::MakeColor(FLinearColor(0.f, 0.f, 1.f))));
	TestTrue(TEXT("the bar's light mixes red and blue"), Bar->GetBeamMix().Color.R > 0.f && Bar->GetBeamMix().Color.B > 0.f);
	TestEqual(TEXT("cell 4 blue stored"), Bar->GetAttribute(StageFixtureCells::GetCellAttribute(3, EStageCellChannel::ColorB)), 1.f);

	UShowControlSubsystem* Show = Scope.World->GetSubsystem<UShowControlSubsystem>();
	if (TestNotNull(TEXT("show control"), Show))
	{
		TestTrue(TEXT("store cue"), Show->StoreCue(1.f, FText::FromString(TEXT("Cells")), 0.f));
		const FShowCue* Cue = Show->FindCue(1.f);
		const FShowCueFixtureState* State = Cue ? Cue->Fixtures.FindByPredicate([Bar](const FShowCueFixtureState& Entry) { return Entry.FixtureId == Bar->GetFixtureId(); }) : nullptr;
		TestTrue(TEXT("the cue records the cells"), State && State->Attributes.Contains(StageFixtureCells::GetCellAttribute(3, EStageCellChannel::ColorB)));
		IStageParameterInterface::Execute_SetParameterValue(Bar, StageCraftTags::Param_Fixture_AllCells, FStageParameterValue::MakeColor(FLinearColor(0.f, 1.f, 0.f)));
		Show->GoToCue(1.f);
		TestEqual(TEXT("recalling the cue brings cell 4 back to blue"), Bar->GetAttribute(StageFixtureCells::GetCellAttribute(3, EStageCellChannel::ColorB)), 1.f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageFixtureLibraryAssetsTest, "StageCraft.Lighting.FixtureLibraryAssets", StageFixtureSuiteTests::Flags)

bool FStageFixtureLibraryAssetsTest::RunTest(const FString& Parameters)
{
	// The eleven fixture models of Scripts/Content/create_fixture_library_content.py.
	static const TCHAR* const Names[] = { TEXT("Spot470"), TEXT("WashRGBW"), TEXT("Beam"), TEXT("Hybrid"), TEXT("ProfileMH"), TEXT("ParRGBWA"),
		TEXT("Strobe"), TEXT("Blinder4"), TEXT("PixelBar16"), TEXT("Ellipsoidal26"), TEXT("Fresnel") };
	TMap<FString, const ULightingFixtureData*> Loaded;
	for (const TCHAR* Name : Names)
	{
		const FString Path = FString::Printf(TEXT("/Game/StageCraft/Fixtures/DA_Fixture_%s.DA_Fixture_%s"), Name, Name);
		const ULightingFixtureData* Data = LoadObject<ULightingFixtureData>(nullptr, *Path);
		if (!TestNotNull(*FString::Printf(TEXT("%s loads"), Name), Data))
		{
			continue;
		}
		Loaded.Add(Name, Data);
		TestTrue(*FString::Printf(TEXT("%s: dimensions, weight, power, flux"), Name),
			Data->Specs.HasDimensions() && Data->Specs.WeightKg > 0.f && Data->Specs.PowerDrawWatts > 0.f && Data->LuminousFlux > 0.f);
		TestTrue(*FString::Printf(TEXT("%s: a base mesh and a head"), Name), !Data->Mesh.IsNull() && !Data->HeadMesh.IsNull());
		TestTrue(*FString::Printf(TEXT("%s: DMX footprint 1..512 (%d)"), Name, Data->GetDMXFootprint()), Data->GetDMXFootprint() >= 1 && Data->GetDMXFootprint() <= 512);
#if WITH_EDITOR
		FDataValidationContext Context;
		const EDataValidationResult Result = Data->IsDataValid(Context);
		TestTrue(*FString::Printf(TEXT("%s: valid (%d errors, %d warnings)"), Name, Context.GetNumErrors(), Context.GetNumWarnings()),
			Result != EDataValidationResult::Invalid && Context.GetNumWarnings() == 0);
#endif
	}
	if (Loaded.Num() != UE_ARRAY_COUNT(Names))
	{
		return false;
	}
	TestTrue(TEXT("spot: CMY, CTO, spinning gobos, prism, frost"), Loaded[TEXT("Spot470")]->bHasCTO && Loaded[TEXT("Spot470")]->bGoboSpin
		&& Loaded[TEXT("Spot470")]->HasPrisms() && Loaded[TEXT("Spot470")]->bHasFrost && Loaded[TEXT("Spot470")]->ColorSystem == EStageFixtureColorSystem::CMY);
	TestEqual(TEXT("hybrid kind"), Loaded[TEXT("Hybrid")]->FixtureKind, EStageFixtureKind::MovingHeadHybrid);
	TestEqual(TEXT("moving profile: framing shutters"), Loaded[TEXT("ProfileMH")]->BeamShaper, EStageBeamShaper::FramingShutters);
	TestEqual(TEXT("par: RGBWA"), Loaded[TEXT("ParRGBWA")]->ColorSystem, EStageFixtureColorSystem::RGBWA);
	TestTrue(TEXT("blinder: 4 dimmed cells"), Loaded[TEXT("Blinder4")]->CellCount == 4 && Loaded[TEXT("Blinder4")]->HasCellDimmer());
	TestTrue(TEXT("pixel bar: 16 colour cells"), Loaded[TEXT("PixelBar16")]->CellCount == 16 && Loaded[TEXT("PixelBar16")]->HasCellColor());
	TestTrue(TEXT("fresnel: barndoors, gel frame, a dimmer channel only"), Loaded[TEXT("Fresnel")]->BeamShaper == EStageBeamShaper::Barndoors
		&& Loaded[TEXT("Fresnel")]->HasGelFrame() && Loaded[TEXT("Fresnel")]->GetDMXFootprint() == 1);
	TestTrue(TEXT("ellipsoidal: framing shutters, gobo slot, iris, gel frame"), Loaded[TEXT("Ellipsoidal26")]->BeamShaper == EStageBeamShaper::FramingShutters
		&& !Loaded[TEXT("Ellipsoidal26")]->StaticGoboWheel.IsEmpty() && Loaded[TEXT("Ellipsoidal26")]->bHasIris && Loaded[TEXT("Ellipsoidal26")]->HasGelFrame());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
