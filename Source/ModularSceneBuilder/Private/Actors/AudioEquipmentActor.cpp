// Copyright Epic Games, Inc. All Rights Reserved.

#include "Actors/AudioEquipmentActor.h"

#include "Data/AudioEquipmentData.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(AudioEquipmentActor)

#define LOCTEXT_NAMESPACE "StageCraftAudioEquipmentActor"

UAudioEquipmentData* AAudioEquipmentActor::GetAudioData() const
{
	return Cast<UAudioEquipmentData>(ItemData);
}

void AAudioEquipmentActor::ApplyItemData(const UBaseItemData& Data)
{
	Super::ApplyItemData(Data);

	// Restored values are untrusted (Docs/ADR/0013-preferences-key-bindings-and-audit-fixes.md §3.2), and a swapped-in cabinet
	// may allow less splay than the previous one: the inspector's limits apply to every value that arrives here.
	const UAudioEquipmentData* Audio = Cast<UAudioEquipmentData>(&Data);
	const auto Sanitize = [this](float& Value, float Min, float Max, float Default, const TCHAR* Name, bool bReport)
	{
		const float Sanitized = FMath::IsFinite(Value) ? FMath::Clamp(Value, Min, Max) : Default;
		if (Sanitized != Value || !FMath::IsFinite(Value))
		{
			if (bReport)
			{
				ReportStateRepair(FString::Printf(TEXT("%s %s -> %s"), Name, *FString::SanitizeFloat(Value), *FString::SanitizeFloat(Sanitized)));
			}
			Value = Sanitized;
		}
	};
	Sanitize(GainDb, -60.f, 12.f, 0.f, TEXT("gain dB"), /*bReport*/ true);
	Sanitize(DelayMs, 0.f, 1000.f, 0.f, TEXT("delay ms"), /*bReport*/ true);
	// A splay beyond a newly swapped cabinet's maximum is a normal model change, not damaged data; only a non-finite or negative one is.
	const bool bSplayDamaged = !FMath::IsFinite(SplayAngle) || SplayAngle < 0.f;
	Sanitize(SplayAngle, 0.f, Audio ? Audio->MaxSplayAngle : TNumericLimits<float>::Max(), 0.f, TEXT("splay deg"), bSplayDamaged);
}

void AAudioEquipmentActor::GatherParameterSections(TArray<FStageParameterSection>& OutSections) const
{
	Super::GatherParameterSections(OutSections);

	const UAudioEquipmentData* Audio = GetAudioData();
	if (!Audio)
	{
		return;
	}

	FStageParameterSection& Section = OutSections.Emplace_GetRef(StageCraftTags::FeatureGroup_Audio, LOCTEXT("AudioSection", "Audio"));

	if (!Audio->IsLoudspeaker())
	{
		if (Audio->ChannelCount > 0)
		{
			Section.Add(StageCraftTags::Param_Audio_Channels, LOCTEXT("ChannelCount", "Channels"), FStageParameterValue::MakeInteger(Audio->ChannelCount)).ReadOnly();
		}
		return;
	}

	Section.Add(StageCraftTags::Param_Audio_Coverage, LOCTEXT("Coverage", "Coverage"),
		FStageParameterValue::MakeText(FText::Format(LOCTEXT("CoverageFormat", "{0} x {1} deg"), FText::AsNumber(Audio->HorizontalCoverage), FText::AsNumber(Audio->VerticalCoverage)))).ReadOnly();
	Section.Add(StageCraftTags::Param_Audio_Gain, LOCTEXT("Gain", "Gain"), FStageParameterValue::MakeFloat(GainDb)).Range(-60.0, 12.0).Display(1.0, LOCTEXT("Db", "dB"), 0.5);
	Section.Add(StageCraftTags::Param_Audio_Mute, LOCTEXT("Mute", "Mute"), FStageParameterValue::MakeBool(bMuted));
	Section.Add(StageCraftTags::Param_Audio_Delay, LOCTEXT("Delay", "Delay"), FStageParameterValue::MakeFloat(DelayMs)).Range(0.0, 1000.0).Display(1.0, LOCTEXT("Ms", "ms"), 0.1);
	Section.Add(StageCraftTags::Param_Audio_Polarity, LOCTEXT("Polarity", "Polarity Invert"), FStageParameterValue::MakeBool(bPolarityInverted))
		.Abbreviated(LOCTEXT("PolarityShort", "Polarity"));

	if (Audio->SupportsSplay())
	{
		Section.Add(StageCraftTags::Param_Audio_Splay, LOCTEXT("Splay", "Splay"), FStageParameterValue::MakeFloat(SplayAngle)).Range(0.0, Audio->MaxSplayAngle).Display(1.0, LOCTEXT("Deg", "°"), 0.5);
	}
}

bool AAudioEquipmentActor::ReadParameter(const FGameplayTag& ParameterId, FStageParameterValue& OutValue) const
{
	if (ParameterId == StageCraftTags::Param_Audio_Gain)		{ OutValue = FStageParameterValue::MakeFloat(GainDb); return true; }
	if (ParameterId == StageCraftTags::Param_Audio_Mute)		{ OutValue = FStageParameterValue::MakeBool(bMuted); return true; }
	if (ParameterId == StageCraftTags::Param_Audio_Delay)		{ OutValue = FStageParameterValue::MakeFloat(DelayMs); return true; }
	if (ParameterId == StageCraftTags::Param_Audio_Polarity)	{ OutValue = FStageParameterValue::MakeBool(bPolarityInverted); return true; }
	if (ParameterId == StageCraftTags::Param_Audio_Splay)		{ OutValue = FStageParameterValue::MakeFloat(SplayAngle); return true; }
	return Super::ReadParameter(ParameterId, OutValue);
}

bool AAudioEquipmentActor::WriteParameter(const FGameplayTag& ParameterId, const FStageParameterValue& Value)
{
	const UAudioEquipmentData* Audio = GetAudioData();
	const bool bLoudspeaker = Audio && Audio->IsLoudspeaker();
	bool bHandled = false;

	if (bLoudspeaker && Value.Type == EStageParameterType::Float)
	{
		if (ParameterId == StageCraftTags::Param_Audio_Gain)
		{
			GainDb = FMath::Clamp(static_cast<float>(Value.Float), -60.f, 12.f);
			bHandled = true;
		}
		else if (ParameterId == StageCraftTags::Param_Audio_Delay)
		{
			DelayMs = FMath::Clamp(static_cast<float>(Value.Float), 0.f, 1000.f);
			bHandled = true;
		}
		else if (ParameterId == StageCraftTags::Param_Audio_Splay && Audio->SupportsSplay())
		{
			SplayAngle = FMath::Clamp(static_cast<float>(Value.Float), 0.f, Audio->MaxSplayAngle);
			bHandled = true;
		}
	}
	else if (bLoudspeaker && Value.Type == EStageParameterType::Bool)
	{
		if (ParameterId == StageCraftTags::Param_Audio_Mute)
		{
			bMuted = Value.bBool;
			bHandled = true;
		}
		else if (ParameterId == StageCraftTags::Param_Audio_Polarity)
		{
			bPolarityInverted = Value.bBool;
			bHandled = true;
		}
	}

	if (bHandled)
	{
		NotifyParameterChanged(ParameterId);
		return true;
	}
	return Super::WriteParameter(ParameterId, Value);
}

#undef LOCTEXT_NAMESPACE
