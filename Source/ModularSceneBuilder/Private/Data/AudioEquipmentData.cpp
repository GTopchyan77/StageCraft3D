// Copyright Epic Games, Inc. All Rights Reserved.

#include "Data/AudioEquipmentData.h"

#include "Actors/AudioEquipmentActor.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(AudioEquipmentData)

#define LOCTEXT_NAMESPACE "StageCraftAudioEquipmentData"

UAudioEquipmentData::UAudioEquipmentData()
{
	ItemType = EStageItemType::Audio;
	ActorClass = AAudioEquipmentActor::StaticClass();
	CategoryTag = StageCraftTags::Category_Audio_LineArray;
	Specs.WeightKg = 60.f;
}

bool UAudioEquipmentData::IsLoudspeaker() const
{
	return AudioKind == EStageAudioKind::LineArrayElement
		|| AudioKind == EStageAudioKind::PointSource
		|| AudioKind == EStageAudioKind::Subwoofer
		|| AudioKind == EStageAudioKind::Monitor;
}

#if WITH_EDITOR

EDataValidationResult UAudioEquipmentData::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);
	const FText AssetName = FText::FromName(GetFName());

	if (IsLoudspeaker() && FrequencyResponse.X >= FrequencyResponse.Y)
	{
		Context.AddError(FText::Format(LOCTEXT("FrequencyRange", "{0}: FrequencyResponse must be (low, high)."), AssetName));
		Result = EDataValidationResult::Invalid;
	}

	if (SupportsSplay() && AudioKind != EStageAudioKind::LineArrayElement && AudioKind != EStageAudioKind::Subwoofer)
	{
		Context.AddWarning(FText::Format(LOCTEXT("SplayOnNonArray", "{0}: MaxSplayAngle is set, but only line array elements and flown subs are splayed."), AssetName));
	}

	return Result;
}

#endif // WITH_EDITOR

#undef LOCTEXT_NAMESPACE
