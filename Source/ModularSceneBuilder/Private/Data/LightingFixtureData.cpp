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
		if (Data.HasZoom())
		{
			AddChannel(StageCraftTags::Attribute_Zoom, false);
		}
	}
}

ULightingFixtureData::ULightingFixtureData()
{
	ItemType = EStageItemType::Light;
	ActorClass = ALightingFixtureActor::StaticClass();
	CategoryTag = StageCraftTags::Category_Lighting_MovingHead;
	PlacementRules.PlacementMode = EStageItemPlacementMode::Single;
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
	return false;
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
		StageCraftTags::Attribute_Zoom,
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
