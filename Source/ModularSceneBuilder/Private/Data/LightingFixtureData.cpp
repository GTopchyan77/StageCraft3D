// Copyright Epic Games, Inc. All Rights Reserved.

#include "Data/LightingFixtureData.h"

#include "Actors/LightingFixtureActor.h"
#include "Data/StageParameterTypes.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(LightingFixtureData)

#define LOCTEXT_NAMESPACE "StageCraftLightingFixtureData"

namespace
{
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
		switch (Data.ColorSystem)
		{
		case EStageFixtureColorSystem::RGBW:
			AddChannel(StageCraftTags::Attribute_ColorR, false);
			AddChannel(StageCraftTags::Attribute_ColorG, false);
			AddChannel(StageCraftTags::Attribute_ColorB, false);
			AddChannel(StageCraftTags::Attribute_ColorW, false);
			break;
		case EStageFixtureColorSystem::RGB:
			AddChannel(StageCraftTags::Attribute_ColorR, false);
			AddChannel(StageCraftTags::Attribute_ColorG, false);
			AddChannel(StageCraftTags::Attribute_ColorB, false);
			break;
		case EStageFixtureColorSystem::CMY:
			AddChannel(StageCraftTags::Attribute_ColorC, false);
			AddChannel(StageCraftTags::Attribute_ColorM, false);
			AddChannel(StageCraftTags::Attribute_ColorY, false);
			break;
		default:
			break;
		}
		if (Data.HasColorTemperatureControl())
		{
			AddChannel(StageCraftTags::Attribute_ColorTemp, false);
		}
		if (!Data.GoboWheel.IsEmpty())
		{
			AddChannel(StageCraftTags::Attribute_Gobo, false);
			AddChannel(StageCraftTags::Attribute_GoboRotation, false);
		}
		if (Data.bHasIris)
		{
			AddChannel(StageCraftTags::Attribute_Iris, false);
		}
		if (Data.HasZoom())
		{
			AddChannel(StageCraftTags::Attribute_Zoom, false);
		}
		if (Data.bHasFramingShutters)
		{
			AddChannel(StageCraftTags::Attribute_BladeA, false);
			AddChannel(StageCraftTags::Attribute_BladeB, false);
			AddChannel(StageCraftTags::Attribute_BladeC, false);
			AddChannel(StageCraftTags::Attribute_BladeD, false);
			AddChannel(StageCraftTags::Attribute_BladeRotation, false);
		}
	}

	/** Gobo index angles and the shutter assembly turn this far either way. */
	constexpr float GoboRotationLimit = 180.f;
	constexpr float BladeRotationLimit = 45.f;

	bool IsBladeAttribute(const FGameplayTag& Attribute)
	{
		return Attribute == StageCraftTags::Attribute_BladeA || Attribute == StageCraftTags::Attribute_BladeB
			|| Attribute == StageCraftTags::Attribute_BladeC || Attribute == StageCraftTags::Attribute_BladeD
			|| Attribute == StageCraftTags::Attribute_BladeRotation;
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
		return ColorSystem != EStageFixtureColorSystem::None;
	}
	if (Attribute == StageCraftTags::Attribute_ColorW)
	{
		return ColorSystem == EStageFixtureColorSystem::RGBW;
	}
	if (Attribute == StageCraftTags::Attribute_ColorC || Attribute == StageCraftTags::Attribute_ColorM || Attribute == StageCraftTags::Attribute_ColorY)
	{
		return ColorSystem == EStageFixtureColorSystem::CMY;
	}
	if (Attribute == StageCraftTags::Attribute_ColorTemp)
	{
		return HasColorTemperatureControl();
	}
	if (Attribute == StageCraftTags::Attribute_Gobo || Attribute == StageCraftTags::Attribute_GoboRotation)
	{
		return !GoboWheel.IsEmpty();
	}
	if (Attribute == StageCraftTags::Attribute_Iris)
	{
		return bHasIris;
	}
	if (IsBladeAttribute(Attribute))
	{
		return bHasFramingShutters;
	}
	return false;
}

bool ULightingFixtureData::IsSteppedAttribute(const FGameplayTag& Attribute)
{
	return Attribute == StageCraftTags::Attribute_Gobo;
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

FText ULightingFixtureData::GetGoboSlotName(int32 Slot) const
{
	if (Slot <= 0 || !GoboWheel.IsValidIndex(Slot - 1))
	{
		return LOCTEXT("GoboOpen", "Open");
	}
	const FText& Name = GoboWheel[Slot - 1].Name;
	return Name.IsEmpty() ? FText::Format(LOCTEXT("GoboSlotFormat", "Gobo {0}"), Slot) : Name;
}

UTexture2D* ULightingFixtureData::LoadGoboTexture(int32 Slot) const
{
	// Gobo textures are in the Game bundle, so they are resident once the item is armed or placed; this load is a lookup then.
	return Slot > 0 && GoboWheel.IsValidIndex(Slot - 1) ? GoboWheel[Slot - 1].Texture.LoadSynchronous() : nullptr;
}

float ULightingFixtureData::GetGoboTransmission(int32 Slot) const
{
	return Slot > 0 && GoboWheel.IsValidIndex(Slot - 1) ? FMath::Clamp(GoboWheel[Slot - 1].Transmission, 0.f, 1.f) : 1.f;
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
	// Placed fixtures should be visible immediately, so pre-vis defaults to open white at full.
	if (Attribute == StageCraftTags::Attribute_Dimmer
		|| Attribute == StageCraftTags::Attribute_ColorR
		|| Attribute == StageCraftTags::Attribute_ColorG
		|| Attribute == StageCraftTags::Attribute_ColorB)
	{
		return 1.f;
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
		StageCraftTags::Attribute_ColorTemp,
		StageCraftTags::Attribute_Zoom,
		StageCraftTags::Attribute_Iris,
		StageCraftTags::Attribute_Gobo,
		StageCraftTags::Attribute_GoboRotation,
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

	if (BeamAngleMin > BeamAngleMax)
	{
		Context.AddError(FText::Format(LOCTEXT("BeamRange", "{0}: BeamAngleMin is larger than BeamAngleMax."), AssetName));
		Result = EDataValidationResult::Invalid;
	}

	if (bHasPanTilt && (PanLimits.X > PanLimits.Y || TiltLimits.X > TiltLimits.Y))
	{
		Context.AddError(FText::Format(LOCTEXT("MotionRange", "{0}: Pan/Tilt limits must be (min, max)."), AssetName));
		Result = EDataValidationResult::Invalid;
	}

	if (HasColorTemperatureControl() && ColorTemperatureRangeK.X >= ColorTemperatureRangeK.Y)
	{
		Context.AddError(FText::Format(LOCTEXT("CtcRange", "{0}: ColorTemperatureRangeK must be (warmest, coolest) with warmest below coolest."), AssetName));
		Result = EDataValidationResult::Invalid;
	}

	if (bHasShutter && !(StrobeRateHz.X > 0.0 && StrobeRateHz.X <= StrobeRateHz.Y))
	{
		Context.AddError(FText::Format(LOCTEXT("StrobeRange", "{0}: StrobeRateHz must be (slowest, fastest), both above 0."), AssetName));
		Result = EDataValidationResult::Invalid;
	}

	// 8-bit DMX gives each gobo slot its own value range only up to 255 slots.
	if (GoboWheel.Num() > 254)
	{
		Context.AddError(FText::Format(LOCTEXT("GoboCount", "{0}: a gobo wheel has at most 254 gobos."), AssetName));
		Result = EDataValidationResult::Invalid;
	}
	for (int32 Index = 0; Index < GoboWheel.Num(); ++Index)
	{
		if (GoboWheel[Index].Texture.IsNull())
		{
			Context.AddWarning(FText::Format(LOCTEXT("GoboNoTexture", "{0}: gobo {1} has no texture; it projects like Open."), AssetName, Index + 1));
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
				Context.AddError(FText::Format(LOCTEXT("ChannelOutOfRange", "{0}: DMX channel {1} exceeds 512."), AssetName, Slot));
				Result = EDataValidationResult::Invalid;
			}
			else if (Used[Slot])
			{
				Context.AddError(FText::Format(LOCTEXT("ChannelOverlap", "{0}: DMX channel {1} is used by more than one attribute."), AssetName, Slot));
				Result = EDataValidationResult::Invalid;
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
