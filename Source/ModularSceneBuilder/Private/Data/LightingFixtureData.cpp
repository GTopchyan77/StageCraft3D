// Copyright Epic Games, Inc. All Rights Reserved.

#include "Data/LightingFixtureData.h"

#include "Actors/LightingFixtureActor.h"
#include "Data/StageParameterTypes.h"
#include "Show/StageColorScience.h"
#include "Show/StageFixtureCells.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(LightingFixtureData)

#define LOCTEXT_NAMESPACE "StageCraftLightingFixtureData"

namespace
{
	/** Gobo index angles and the shutter assembly turn this far either way. */
	constexpr float GoboRotationLimit = 180.f;
	constexpr float BladeRotationLimit = 45.f;

	/** 8-bit DMX gives each wheel slot its own value range only up to 255 slots (Open included). */
	constexpr int32 MaxWheelSlots = 254;

	/** Amber LEDs peak near 590 nm; this is that colour in linear Rec. 709, brightest channel 1. */
	const FLinearColor DefaultAmber(1.f, 0.5f, 0.f);

	bool IsBladeAttribute(const FGameplayTag& Attribute)
	{
		return Attribute == StageCraftTags::Attribute_BladeA || Attribute == StageCraftTags::Attribute_BladeB
			|| Attribute == StageCraftTags::Attribute_BladeC || Attribute == StageCraftTags::Attribute_BladeD
			|| Attribute == StageCraftTags::Attribute_BladeRotation;
	}

	bool HasWhiteEmitter(EStageFixtureColorSystem System)
	{
		return System == EStageFixtureColorSystem::RGBW || System == EStageFixtureColorSystem::RGBWA;
	}

	bool HasAmberEmitter(EStageFixtureColorSystem System)
	{
		return System == EStageFixtureColorSystem::RGBA || System == EStageFixtureColorSystem::RGBWA;
	}

	/** True for a cell control the fixture's cell layout includes. */
	bool SupportsCellChannel(const ULightingFixtureData& Data, int32 Cell, EStageCellChannel Channel)
	{
		if (!Data.HasCells() || Cell < 0 || Cell >= Data.CellCount)
		{
			return false;
		}
		switch (Channel)
		{
		case EStageCellChannel::Dimmer:
			return Data.HasCellDimmer();
		case EStageCellChannel::ColorR:
		case EStageCellChannel::ColorG:
		case EStageCellChannel::ColorB:
			return Data.HasCellColor();
		case EStageCellChannel::ColorW:
			return Data.HasCellColor() && HasWhiteEmitter(Data.ColorSystem);
		case EStageCellChannel::ColorA:
			return Data.HasCellColor() && HasAmberEmitter(Data.ColorSystem);
		default:
			// The composite row is not an attribute of its own.
			return false;
		}
	}

	void BuildDefaultLayout(const ULightingFixtureData& Data, TArray<FStageFixtureDMXChannel>& OutChannels)
	{
		OutChannels.Reset();
		int32 NextChannel = 1;

		auto AddChannel = [&OutChannels, &NextChannel](const FGameplayTag& Attribute, bool b16Bit)
		{
			FStageFixtureDMXChannel& Channel = OutChannels.AddDefaulted_GetRef();
			Channel.Attribute = Attribute;
			Channel.Channel = NextChannel;
			Channel.b16Bit = b16Bit;
			NextChannel += Channel.GetWidth();
		};

		// Order mirrors common moving-head personalities so the layout reads familiar in a console patch.
		if (Data.bHasPanTilt)
		{
			AddChannel(StageCraftTags::Attribute_Pan, true);
			AddChannel(StageCraftTags::Attribute_Tilt, true);
		}
		AddChannel(StageCraftTags::Attribute_Dimmer, true);
		if (Data.bHasShutter)
		{
			AddChannel(StageCraftTags::Attribute_Shutter, false);
		}
		// A gel frame is a physical filter, not a DMX channel; cells carry their own colour below.
		if (Data.IsColorMixing() && !Data.HasCells())
		{
			if (Data.ColorSystem == EStageFixtureColorSystem::CMY)
			{
				AddChannel(StageCraftTags::Attribute_ColorC, false);
				AddChannel(StageCraftTags::Attribute_ColorM, false);
				AddChannel(StageCraftTags::Attribute_ColorY, false);
			}
			else
			{
				for (const FGameplayTag& Attribute : Data.GetColorAttributes())
				{
					AddChannel(Attribute, false);
				}
			}
		}
		if (Data.bHasCTO)
		{
			AddChannel(StageCraftTags::Attribute_CTO, false);
		}
		if (Data.HasColorTemperatureControl())
		{
			AddChannel(StageCraftTags::Attribute_ColorTemp, false);
		}
		if (!Data.ColorWheel.IsEmpty())
		{
			AddChannel(StageCraftTags::Attribute_ColorWheel, false);
		}
		if (!Data.GoboWheel.IsEmpty())
		{
			AddChannel(StageCraftTags::Attribute_Gobo, false);
			AddChannel(StageCraftTags::Attribute_GoboRotation, false);
			if (Data.bGoboSpin)
			{
				AddChannel(StageCraftTags::Attribute_GoboSpin, false);
			}
		}
		if (!Data.StaticGoboWheel.IsEmpty())
		{
			AddChannel(StageCraftTags::Attribute_Gobo2, false);
		}
		if (Data.HasPrisms())
		{
			AddChannel(StageCraftTags::Attribute_Prism, false);
			AddChannel(StageCraftTags::Attribute_PrismRotation, false);
		}
		if (Data.bHasFrost)
		{
			AddChannel(StageCraftTags::Attribute_Frost, false);
		}
		if (Data.bHasIris)
		{
			AddChannel(StageCraftTags::Attribute_Iris, false);
		}
		if (Data.HasZoom())
		{
			AddChannel(StageCraftTags::Attribute_Zoom, false);
		}
		if (Data.HasBlades())
		{
			AddChannel(StageCraftTags::Attribute_BladeA, false);
			AddChannel(StageCraftTags::Attribute_BladeB, false);
			AddChannel(StageCraftTags::Attribute_BladeC, false);
			AddChannel(StageCraftTags::Attribute_BladeD, false);
			AddChannel(StageCraftTags::Attribute_BladeRotation, false);
		}

		// One block per cell, in cell order, as pixel bars lay out their "pixel" modes.
		static constexpr EStageCellChannel CellOrder[] = {
			EStageCellChannel::Dimmer, EStageCellChannel::ColorR, EStageCellChannel::ColorG, EStageCellChannel::ColorB,
			EStageCellChannel::ColorW, EStageCellChannel::ColorA };
		for (int32 Cell = 0; Cell < Data.CellCount; ++Cell)
		{
			for (const EStageCellChannel Channel : CellOrder)
			{
				if (SupportsCellChannel(Data, Cell, Channel))
				{
					AddChannel(StageFixtureCells::GetCellAttribute(Cell, Channel), false);
				}
			}
		}
	}

	/** Display name of a wheel slot, with Open (or Out) for slot 0 and a numbered fallback for unnamed slots. */
	template <typename SlotType>
	FText GetWheelSlotName(const TArray<SlotType>& Wheel, int32 Slot, const FText& EmptyName, const FText& UnnamedFormat)
	{
		if (Slot <= 0 || !Wheel.IsValidIndex(Slot - 1))
		{
			return EmptyName;
		}
		const FText& Name = Wheel[Slot - 1].Name;
		return Name.IsEmpty() ? FText::Format(UnnamedFormat, Slot) : Name;
	}
}

ULightingFixtureData::ULightingFixtureData()
{
	ItemType = EStageItemType::Light;
	ActorClass = ALightingFixtureActor::StaticClass();
	CategoryTag = StageCraftTags::Category_Lighting_MovingHead;
	Specs.WeightKg = 20.f;
	Specs.PowerDrawWatts = 450.f;

	BuildDefaultLayout(*this, DMXChannels);
}

void ULightingFixtureData::PostLoad()
{
	Super::PostLoad();

	if (bHasFramingShutters_DEPRECATED)
	{
		BeamShaper = EStageBeamShaper::FramingShutters;
		bHasFramingShutters_DEPRECATED = false;
	}
}

TArray<FGameplayTag> ULightingFixtureData::GetColorAttributes() const
{
	TArray<FGameplayTag> Result;
	if (!IsColorMixing() && !bAcceptsGel)
	{
		return Result;
	}
	Result = { StageCraftTags::Attribute_ColorR, StageCraftTags::Attribute_ColorG, StageCraftTags::Attribute_ColorB };
	if (HasWhiteEmitter(ColorSystem))
	{
		Result.Add(StageCraftTags::Attribute_ColorW);
	}
	if (HasAmberEmitter(ColorSystem))
	{
		Result.Add(StageCraftTags::Attribute_ColorA);
	}
	return Result;
}

TArray<FLinearColor> ULightingFixtureData::GetEmitterOutputs() const
{
	TArray<FLinearColor> Defaults = { FLinearColor(1.f, 0.f, 0.f), FLinearColor(0.f, 1.f, 0.f), FLinearColor(0.f, 0.f, 1.f) };
	if (HasWhiteEmitter(ColorSystem))
	{
		Defaults.Add(StageColorScience::KelvinToLinear(WhiteEmitterKelvin));
	}
	if (HasAmberEmitter(ColorSystem))
	{
		Defaults.Add(DefaultAmber);
	}

	// An asset's emitters replace the defaults only as a whole set; a partial set would shift every later channel.
	if (Emitters.Num() != Defaults.Num())
	{
		return Defaults;
	}
	TArray<FLinearColor> Outputs;
	Outputs.Reserve(Emitters.Num());
	for (const FStageColorEmitter& Emitter : Emitters)
	{
		const float Peak = FMath::Max3(Emitter.Color.R, Emitter.Color.G, Emitter.Color.B);
		const FLinearColor Chromaticity = Peak > UE_SMALL_NUMBER ? Emitter.Color / Peak : FLinearColor::Black;
		FLinearColor Output = Chromaticity * FMath::Max(Emitter.RelativeFlux, 0.f);
		Output.A = 1.f;
		Outputs.Add(Output);
	}
	return Outputs;
}

bool ULightingFixtureData::SupportsAttribute(FGameplayTag Attribute) const
{
	if (Attribute == StageCraftTags::Attribute_Dimmer)
	{
		return true;
	}
	if (Attribute == StageCraftTags::Attribute_Pan || Attribute == StageCraftTags::Attribute_Tilt)
	{
		return bHasPanTilt;
	}
	if (Attribute == StageCraftTags::Attribute_Zoom)
	{
		return HasZoom();
	}
	if (Attribute == StageCraftTags::Attribute_Shutter)
	{
		return bHasShutter;
	}
	if (Attribute == StageCraftTags::Attribute_ColorR || Attribute == StageCraftTags::Attribute_ColorG || Attribute == StageCraftTags::Attribute_ColorB)
	{
		return (IsColorMixing() || HasGelFrame()) && !HasCells();
	}
	if (Attribute == StageCraftTags::Attribute_ColorW)
	{
		return HasWhiteEmitter(ColorSystem) && !HasCells();
	}
	if (Attribute == StageCraftTags::Attribute_ColorA)
	{
		return HasAmberEmitter(ColorSystem) && !HasCells();
	}
	if (Attribute == StageCraftTags::Attribute_ColorC || Attribute == StageCraftTags::Attribute_ColorM || Attribute == StageCraftTags::Attribute_ColorY)
	{
		return ColorSystem == EStageFixtureColorSystem::CMY && !HasCells();
	}
	if (Attribute == StageCraftTags::Attribute_ColorTemp)
	{
		return HasColorTemperatureControl();
	}
	if (Attribute == StageCraftTags::Attribute_CTO)
	{
		return bHasCTO;
	}
	if (Attribute == StageCraftTags::Attribute_ColorWheel)
	{
		return !ColorWheel.IsEmpty();
	}
	if (Attribute == StageCraftTags::Attribute_Gobo || Attribute == StageCraftTags::Attribute_GoboRotation)
	{
		return !GoboWheel.IsEmpty();
	}
	if (Attribute == StageCraftTags::Attribute_GoboSpin)
	{
		return bGoboSpin && !GoboWheel.IsEmpty();
	}
	if (Attribute == StageCraftTags::Attribute_Gobo2)
	{
		return !StaticGoboWheel.IsEmpty();
	}
	if (Attribute == StageCraftTags::Attribute_Prism || Attribute == StageCraftTags::Attribute_PrismRotation)
	{
		return HasPrisms();
	}
	if (Attribute == StageCraftTags::Attribute_Frost)
	{
		return bHasFrost;
	}
	if (Attribute == StageCraftTags::Attribute_Iris)
	{
		return bHasIris;
	}
	if (IsBladeAttribute(Attribute))
	{
		return HasBlades();
	}

	int32 Cell = INDEX_NONE;
	EStageCellChannel Channel = EStageCellChannel::Count;
	return StageFixtureCells::ParseCellAttribute(Attribute, Cell, Channel) && SupportsCellChannel(*this, Cell, Channel);
}

bool ULightingFixtureData::IsSteppedAttribute(const FGameplayTag& Attribute)
{
	return Attribute == StageCraftTags::Attribute_Gobo || Attribute == StageCraftTags::Attribute_Gobo2
		|| Attribute == StageCraftTags::Attribute_ColorWheel || Attribute == StageCraftTags::Attribute_Prism;
}

float ULightingFixtureData::NormalizeForDMX(const FGameplayTag& Attribute, float Value) const
{
	float Min = 0.f;
	float Max = 1.f;
	if (!GetAttributeRange(Attribute, Min, Max) || !FMath::IsFinite(Value))
	{
		return 0.f;
	}
	if (IsSteppedAttribute(Attribute))
	{
		const int32 SlotCount = FMath::RoundToInt32(Max - Min) + 1;
		const int32 Slot = FMath::Clamp(FMath::RoundToInt32(Value - Min), 0, SlotCount - 1);
		return (Slot + 0.5f) / SlotCount;
	}
	return Max > Min ? FMath::Clamp((Value - Min) / (Max - Min), 0.f, 1.f) : 0.f;
}

float ULightingFixtureData::DenormalizeFromDMX(const FGameplayTag& Attribute, float Normalized) const
{
	float Min = 0.f;
	float Max = 1.f;
	if (!GetAttributeRange(Attribute, Min, Max))
	{
		return 0.f;
	}
	const float Clamped = FMath::IsFinite(Normalized) ? FMath::Clamp(Normalized, 0.f, 1.f) : 0.f;
	if (IsSteppedAttribute(Attribute))
	{
		const int32 SlotCount = FMath::RoundToInt32(Max - Min) + 1;
		return Min + FMath::Min(FMath::FloorToInt32(Clamped * SlotCount), SlotCount - 1);
	}
	return FMath::Lerp(Min, Max, Clamped);
}

FText ULightingFixtureData::GetGoboSlotName(int32 Slot, EStageGoboWheel Wheel) const
{
	return GetWheelSlotName(GetWheel(Wheel), Slot, LOCTEXT("GoboOpen", "Open"), LOCTEXT("GoboSlotFormat", "Gobo {0}"));
}

UTexture2D* ULightingFixtureData::LoadGoboTexture(int32 Slot, EStageGoboWheel Wheel) const
{
	// Gobo textures are in the Game bundle, so they are resident once the item is armed or placed; this load is a lookup then.
	const TArray<FStageGoboSlot>& Slots = GetWheel(Wheel);
	return Slot > 0 && Slots.IsValidIndex(Slot - 1) ? Slots[Slot - 1].Texture.LoadSynchronous() : nullptr;
}

float ULightingFixtureData::GetGoboTransmission(int32 Slot, EStageGoboWheel Wheel) const
{
	const TArray<FStageGoboSlot>& Slots = GetWheel(Wheel);
	return Slot > 0 && Slots.IsValidIndex(Slot - 1) ? FMath::Clamp(Slots[Slot - 1].Transmission, 0.f, 1.f) : 1.f;
}

FText ULightingFixtureData::GetColorWheelSlotName(int32 Slot) const
{
	return GetWheelSlotName(ColorWheel, Slot, LOCTEXT("ColorOpen", "Open"), LOCTEXT("ColorSlotFormat", "Color {0}"));
}

FLinearColor ULightingFixtureData::GetColorWheelFilter(int32 Slot) const
{
	if (Slot <= 0 || !ColorWheel.IsValidIndex(Slot - 1))
	{
		return FLinearColor::White;
	}
	const FLinearColor& Filter = ColorWheel[Slot - 1].Filter;
	return FLinearColor(FMath::Clamp(Filter.R, 0.f, 1.f), FMath::Clamp(Filter.G, 0.f, 1.f), FMath::Clamp(Filter.B, 0.f, 1.f));
}

FText ULightingFixtureData::GetPrismSlotName(int32 Slot) const
{
	return GetWheelSlotName(Prisms, Slot, LOCTEXT("PrismOut", "Out"), LOCTEXT("PrismSlotFormat", "Prism {0}"));
}

const FStagePrism* ULightingFixtureData::GetPrism(int32 Slot) const
{
	return Slot > 0 && Prisms.IsValidIndex(Slot - 1) ? &Prisms[Slot - 1] : nullptr;
}

bool ULightingFixtureData::GetAttributeRange(FGameplayTag Attribute, float& OutMin, float& OutMax) const
{
	OutMin = 0.f;
	OutMax = 1.f;

	if (!SupportsAttribute(Attribute))
	{
		return false;
	}
	if (Attribute == StageCraftTags::Attribute_Pan)
	{
		OutMin = PanLimits.X;
		OutMax = PanLimits.Y;
	}
	else if (Attribute == StageCraftTags::Attribute_Tilt)
	{
		OutMin = TiltLimits.X;
		OutMax = TiltLimits.Y;
	}
	else if (Attribute == StageCraftTags::Attribute_Zoom)
	{
		OutMin = BeamAngleMin;
		OutMax = BeamAngleMax;
	}
	else if (Attribute == StageCraftTags::Attribute_ColorTemp)
	{
		OutMin = ColorTemperatureRangeK.X;
		OutMax = ColorTemperatureRangeK.Y;
	}
	else if (Attribute == StageCraftTags::Attribute_Gobo)
	{
		OutMax = static_cast<float>(GoboWheel.Num());
	}
	else if (Attribute == StageCraftTags::Attribute_Gobo2)
	{
		OutMax = static_cast<float>(StaticGoboWheel.Num());
	}
	else if (Attribute == StageCraftTags::Attribute_ColorWheel)
	{
		OutMax = static_cast<float>(ColorWheel.Num());
	}
	else if (Attribute == StageCraftTags::Attribute_Prism)
	{
		OutMax = static_cast<float>(Prisms.Num());
	}
	else if (Attribute == StageCraftTags::Attribute_GoboSpin || Attribute == StageCraftTags::Attribute_PrismRotation)
	{
		OutMin = -MaxRotationRpm;
		OutMax = MaxRotationRpm;
	}
	else if (Attribute == StageCraftTags::Attribute_GoboRotation)
	{
		OutMin = -GoboRotationLimit;
		OutMax = GoboRotationLimit;
	}
	else if (Attribute == StageCraftTags::Attribute_BladeRotation)
	{
		OutMin = -BladeRotationLimit;
		OutMax = BladeRotationLimit;
	}
	return true;
}

float ULightingFixtureData::GetAttributeDefault(FGameplayTag Attribute) const
{
	if (Attribute == StageCraftTags::Attribute_Zoom)
	{
		return BeamAngleMax;
	}
	if (Attribute == StageCraftTags::Attribute_Pan)
	{
		return FMath::Clamp(0.f, PanLimits.X, PanLimits.Y);
	}
	if (Attribute == StageCraftTags::Attribute_Tilt)
	{
		return FMath::Clamp(0.f, TiltLimits.X, TiltLimits.Y);
	}
	if (Attribute == StageCraftTags::Attribute_ColorTemp)
	{
		// Neutral: the light's temperature white point, so CTC starts out not tinting the mix.
		return FMath::Clamp(6500.f, static_cast<float>(ColorTemperatureRangeK.X), static_cast<float>(ColorTemperatureRangeK.Y));
	}
	// Placed fixtures should be visible immediately, so pre-vis defaults to open white at full: every white-making emitter on
	// (an RGBW fixture's rated output needs its W; amber tints, so it stays off), and a gel frame starts clear.
	if (Attribute == StageCraftTags::Attribute_Dimmer
		|| Attribute == StageCraftTags::Attribute_ColorR
		|| Attribute == StageCraftTags::Attribute_ColorG
		|| Attribute == StageCraftTags::Attribute_ColorB
		|| Attribute == StageCraftTags::Attribute_ColorW)
	{
		return 1.f;
	}
	int32 Cell = INDEX_NONE;
	EStageCellChannel Channel = EStageCellChannel::Count;
	if (StageFixtureCells::ParseCellAttribute(Attribute, Cell, Channel))
	{
		return Channel == EStageCellChannel::ColorA ? 0.f : 1.f;
	}
	return 0.f;
}

TArray<FGameplayTag> ULightingFixtureData::GetSupportedAttributes() const
{
	// C/M/Y are DMX encodings of R/G/B, not attributes of their own.
	static const FGameplayTag Ordered[] = {
		StageCraftTags::Attribute_Dimmer,
		StageCraftTags::Attribute_Shutter,
		StageCraftTags::Attribute_Pan,
		StageCraftTags::Attribute_Tilt,
		StageCraftTags::Attribute_ColorR,
		StageCraftTags::Attribute_ColorG,
		StageCraftTags::Attribute_ColorB,
		StageCraftTags::Attribute_ColorW,
		StageCraftTags::Attribute_ColorA,
		StageCraftTags::Attribute_ColorTemp,
		StageCraftTags::Attribute_CTO,
		StageCraftTags::Attribute_ColorWheel,
		StageCraftTags::Attribute_Zoom,
		StageCraftTags::Attribute_Frost,
		StageCraftTags::Attribute_Iris,
		StageCraftTags::Attribute_Gobo,
		StageCraftTags::Attribute_GoboRotation,
		StageCraftTags::Attribute_GoboSpin,
		StageCraftTags::Attribute_Gobo2,
		StageCraftTags::Attribute_Prism,
		StageCraftTags::Attribute_PrismRotation,
		StageCraftTags::Attribute_BladeA,
		StageCraftTags::Attribute_BladeB,
		StageCraftTags::Attribute_BladeC,
		StageCraftTags::Attribute_BladeD,
		StageCraftTags::Attribute_BladeRotation,
	};

	TArray<FGameplayTag> Result;
	for (const FGameplayTag& Attribute : Ordered)
	{
		if (SupportsAttribute(Attribute))
		{
			Result.Add(Attribute);
		}
	}
	for (int32 Cell = 0; Cell < CellCount && Cell < StageFixtureCells::MaxCells; ++Cell)
	{
		for (int32 Channel = 0; Channel < static_cast<int32>(EStageCellChannel::ColorRGB); ++Channel)
		{
			if (SupportsCellChannel(*this, Cell, static_cast<EStageCellChannel>(Channel)))
			{
				Result.Add(StageFixtureCells::GetCellAttribute(Cell, static_cast<EStageCellChannel>(Channel)));
			}
		}
	}
	return Result;
}

int32 ULightingFixtureData::GetDMXFootprint() const
{
	int32 Footprint = 0;
	for (const FStageFixtureDMXChannel& Channel : DMXChannels)
	{
		Footprint = FMath::Max(Footprint, Channel.Channel + Channel.GetWidth() - 1);
	}
	return Footprint;
}

#if WITH_EDITOR

void ULightingFixtureData::GenerateDefaultDMXLayout()
{
	Modify();
	BuildDefaultLayout(*this, DMXChannels);
}

EDataValidationResult ULightingFixtureData::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);
	const FText AssetName = FText::FromName(GetFName());
	auto AddError = [&Context, &Result](const FText& Message)
	{
		Context.AddError(Message);
		Result = EDataValidationResult::Invalid;
	};

	if (BeamAngleMin > BeamAngleMax)
	{
		AddError(FText::Format(LOCTEXT("BeamRange", "{0}: BeamAngleMin is larger than BeamAngleMax."), AssetName));
	}

	if (bHasPanTilt && (PanLimits.X > PanLimits.Y || TiltLimits.X > TiltLimits.Y))
	{
		AddError(FText::Format(LOCTEXT("MotionRange", "{0}: Pan/Tilt limits must be (min, max)."), AssetName));
	}

	if (HasColorTemperatureControl() && ColorTemperatureRangeK.X >= ColorTemperatureRangeK.Y)
	{
		AddError(FText::Format(LOCTEXT("CtcRange", "{0}: ColorTemperatureRangeK must be (warmest, coolest) with warmest below coolest."), AssetName));
	}

	if (bHasShutter && !(StrobeRateHz.X > 0.0 && StrobeRateHz.X <= StrobeRateHz.Y))
	{
		AddError(FText::Format(LOCTEXT("StrobeRange", "{0}: StrobeRateHz must be (slowest, fastest), both above 0."), AssetName));
	}

	// Colour.
	if (IsAdditive() && !Emitters.IsEmpty() && Emitters.Num() != GetColorAttributes().Num())
	{
		AddError(FText::Format(LOCTEXT("EmitterCount", "{0}: {1} needs {2} emitters (R, G, B, then W, then A), or none for the defaults; it has {3}."),
			AssetName, UEnum::GetDisplayValueAsText(ColorSystem), GetColorAttributes().Num(), Emitters.Num()));
	}
	if (bAcceptsGel && IsColorMixing())
	{
		Context.AddWarning(FText::Format(LOCTEXT("GelOnMixer", "{0}: bAcceptsGel is ignored on a colour-mixing fixture; gels are matched by its colour mixing instead."), AssetName));
	}
	if (bHasCTO && ColorSystem != EStageFixtureColorSystem::CMY && ColorSystem != EStageFixtureColorSystem::None)
	{
		Context.AddWarning(FText::Format(LOCTEXT("CtoOnLed", "{0}: a CTO filter on an additive fixture; LED fixtures usually use CTC (bHasColorTemperatureControl)."), AssetName));
	}

	// Wheels.
	const auto CheckWheelCount = [&AddError, &AssetName](int32 Count, const FText& WheelName)
	{
		if (Count > MaxWheelSlots)
		{
			AddError(FText::Format(LOCTEXT("WheelCount", "{0}: the {1} has {2} slots; DMX allows at most {3}."), AssetName, WheelName, Count, MaxWheelSlots));
		}
	};
	CheckWheelCount(GoboWheel.Num(), LOCTEXT("RotatingWheel", "rotating gobo wheel"));
	CheckWheelCount(StaticGoboWheel.Num(), LOCTEXT("StaticWheel", "static gobo wheel"));
	CheckWheelCount(ColorWheel.Num(), LOCTEXT("ColorWheelName", "colour wheel"));
	CheckWheelCount(Prisms.Num(), LOCTEXT("PrismWheel", "prism wheel"));
	for (const TArray<FStageGoboSlot>* Wheel : { &GoboWheel, &StaticGoboWheel })
	{
		for (int32 Index = 0; Index < Wheel->Num(); ++Index)
		{
			if ((*Wheel)[Index].Texture.IsNull())
			{
				Context.AddWarning(FText::Format(LOCTEXT("GoboNoTexture", "{0}: gobo {1} has no texture; it projects like Open."), AssetName, Index + 1));
			}
		}
	}
	if (bGoboSpin && GoboWheel.IsEmpty())
	{
		Context.AddWarning(FText::Format(LOCTEXT("SpinNoWheel", "{0}: bGoboSpin without a rotating gobo wheel has no effect."), AssetName));
	}

	// Cells.
	if (HasCells() && CellControl != EStageCellControl::Dimmer && !IsColorMixing())
	{
		AddError(FText::Format(LOCTEXT("CellColorNoSystem", "{0}: colour cells need a colour system; use CellControl Dimmer for white cells."), AssetName));
	}
	if (HasCells() && (Body.CellStripSizeCm.X <= 0.0 || Body.CellStripSizeCm.Y <= 0.0))
	{
		Context.AddWarning(FText::Format(LOCTEXT("CellsNoStrip", "{0}: {1} cells but no Body.CellStripSizeCm; the cells will not be drawn."), AssetName, CellCount));
	}

	// Body against the stated dimensions: a part larger than the whole is a typo in one of them.
	if (Specs.HasDimensions())
	{
		const FVector Overall = Specs.DimensionsCm * 1.01;
		const auto Exceeds = [&Overall](const FVector& Size) { return Size.X > Overall.X || Size.Y > Overall.Y || Size.Z > Overall.Z; };
		if (Exceeds(Body.BaseSizeCm) || Exceeds(Body.YokeSizeCm) || Exceeds(Body.HeadSizeCm))
		{
			Context.AddWarning(FText::Format(LOCTEXT("BodyTooLarge", "{0}: a Body part is larger than Specs.DimensionsCm."), AssetName));
		}
	}

	if (Mesh.IsNull() && HeadMesh.IsNull())
	{
		Context.AddWarning(FText::Format(LOCTEXT("NoFixtureMesh", "{0}: Neither Mesh nor HeadMesh is set; only the light itself will be visible."), AssetName));
	}

	// Channels must not overlap, otherwise two attributes fight over one DMX slot.
	TBitArray<> Used(false, 513);
	for (const FStageFixtureDMXChannel& Channel : DMXChannels)
	{
		if (!SupportsAttribute(Channel.Attribute))
		{
			Context.AddWarning(FText::Format(LOCTEXT("UnsupportedChannel", "{0}: DMX channel {1} maps {2}, which this fixture's capabilities do not support. It will be ignored."),
				AssetName, Channel.Channel, FText::FromName(Channel.Attribute.GetTagName())));
		}

		for (int32 Slot = Channel.Channel; Slot < Channel.Channel + Channel.GetWidth(); ++Slot)
		{
			if (Slot > 512)
			{
				AddError(FText::Format(LOCTEXT("ChannelOutOfRange", "{0}: DMX channel {1} exceeds 512."), AssetName, Slot));
			}
			else if (Used[Slot])
			{
				AddError(FText::Format(LOCTEXT("ChannelOverlap", "{0}: DMX channel {1} is used by more than one attribute."), AssetName, Slot));
			}
			else
			{
				Used[Slot] = true;
			}
		}
	}

	return Result;
}

#endif // WITH_EDITOR

#undef LOCTEXT_NAMESPACE
