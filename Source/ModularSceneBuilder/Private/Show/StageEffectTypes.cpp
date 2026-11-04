// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageEffectTypes.h"

#include "Data/StageParameterTypes.h"
#include "Show/ShowControlTypes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageEffectTypes)

#define LOCTEXT_NAMESPACE "StageEffects"

bool FStageEffect::Equals(const FStageEffect& Other) const
{
	return Name.ToString().Equals(Other.Name.ToString(), ESearchCase::CaseSensitive) && Target == Other.Target && Waveform == Other.Waveform
		&& Attribute == Other.Attribute && Axis == Other.Axis && ColorMode == Other.ColorMode && ColorA == Other.ColorA && ColorB == Other.ColorB
		&& Low == Other.Low && High == Other.High && Size == Other.Size && RateHz == Other.RateHz && Duty == Other.Duty && Phase == Other.Phase
		&& Spread == Other.Spread && Wings == Other.Wings && Blocks == Other.Blocks && Direction == Other.Direction && SizeFan == Other.SizeFan;
}

namespace StageEffectRules
{
	namespace
	{
		float Finite(float Value, float Fallback)
		{
			return FMath::IsFinite(Value) ? Value : Fallback;
		}

		FLinearColor SanitizeColor(const FLinearColor& Color, const FLinearColor& Fallback)
		{
			if (!FMath::IsFinite(Color.R) || !FMath::IsFinite(Color.G) || !FMath::IsFinite(Color.B))
			{
				return Fallback;
			}
			return FLinearColor(FMath::Clamp(Color.R, 0.f, 1.f), FMath::Clamp(Color.G, 0.f, 1.f), FMath::Clamp(Color.B, 0.f, 1.f), 1.f);
		}

		template <typename EnumType>
		EnumType SanitizeEnum(EnumType Value, EnumType Last, EnumType Fallback)
		{
			return static_cast<uint8>(Value) <= static_cast<uint8>(Last) ? Value : Fallback;
		}
	}

	float HzToBPM(float Hz)
	{
		return Hz * 60.f;
	}

	float BPMToHz(float BPM)
	{
		return BPM / 60.f;
	}

	bool IsBeamAttribute(const FGameplayTag& Attribute)
	{
		return StagePaletteRules::AllowsAttribute(EStagePaletteKind::Beam, Attribute) || StagePaletteRules::AllowsAttribute(EStagePaletteKind::Focus, Attribute);
	}

	FStageEffect SanitizeEffect(const FStageEffect& Effect)
	{
		const FStageEffect Defaults;
		FStageEffect Out = Effect;
		Out.Name = StagePaletteRules::SanitizeName(Effect.Name);
		Out.Target = SanitizeEnum(Effect.Target, EStageEffectTarget::Beam, Defaults.Target);
		Out.Waveform = SanitizeEnum(Effect.Waveform, EStageEffectWaveform::Figure8, Defaults.Waveform);
		Out.Axis = SanitizeEnum(Effect.Axis, EStageEffectAxis::Both, Defaults.Axis);
		Out.ColorMode = SanitizeEnum(Effect.ColorMode, EStageEffectColorMode::Rainbow, Defaults.ColorMode);
		Out.Direction = SanitizeEnum(Effect.Direction, EStageEffectDirection::Bounce, Defaults.Direction);
		Out.SizeFan = SanitizeEnum(Effect.SizeFan, EStageFanMode::InvertedV, Defaults.SizeFan);
		Out.ColorA = SanitizeColor(Effect.ColorA, Defaults.ColorA);
		Out.ColorB = SanitizeColor(Effect.ColorB, Defaults.ColorB);
		Out.Low = FMath::Clamp(Finite(Effect.Low, Defaults.Low), 0.f, 1.f);
		Out.High = FMath::Clamp(Finite(Effect.High, Defaults.High), 0.f, 1.f);
		Out.Size = FMath::Clamp(Finite(Effect.Size, Defaults.Size), 0.f, MaxSizeDegrees);
		Out.RateHz = FMath::Clamp(Finite(Effect.RateHz, Defaults.RateHz), 0.f, MaxRateHz);
		Out.Duty = FMath::Clamp(Finite(Effect.Duty, Defaults.Duty), 0.f, 1.f);
		Out.Phase = FMath::Clamp(Finite(Effect.Phase, 0.f), -MaxPhaseDegrees, MaxPhaseDegrees);
		Out.Spread = FMath::Clamp(Finite(Effect.Spread, 0.f), -MaxPhaseDegrees, MaxPhaseDegrees);
		Out.Wings = FMath::Clamp(Effect.Wings, 1, MaxWings);
		Out.Blocks = FMath::Clamp(Effect.Blocks, 1, MaxBlocks);
		if (Out.Target == EStageEffectTarget::Beam && !IsBeamAttribute(Out.Attribute))
		{
			Out.Attribute = StageCraftTags::Attribute_Zoom;
		}
		if (Out.Target != EStageEffectTarget::Beam)
		{
			Out.Attribute = FGameplayTag();
		}
		return Out;
	}

	int32 SanitizeEntries(TArray<FStageEffectEntry>& Entries)
	{
		int32 Dropped = 0;
		TSet<FGuid> SeenIds;
		TArray<FStageEffectEntry> Kept;
		Kept.Reserve(FMath::Min(Entries.Num(), MaxEffects));
		for (FStageEffectEntry& Entry : Entries)
		{
			bool bIdSeen = false;
			if (!Entry.Id.IsValid() || Kept.Num() >= MaxEffects)
			{
				++Dropped;
				continue;
			}
			SeenIds.Add(Entry.Id, &bIdSeen);
			if (bIdSeen)
			{
				++Dropped;
				continue;
			}
			TArray<int32> FixtureIds;
			TSet<int32> SeenFixtures;
			for (const int32 FixtureId : Entry.FixtureIds)
			{
				bool bFixtureSeen = false;
				if (FixtureId > 0 && FixtureIds.Num() < MaxFixturesPerEffect)
				{
					SeenFixtures.Add(FixtureId, &bFixtureSeen);
				}
				if (FixtureId <= 0 || bFixtureSeen || FixtureIds.Num() >= MaxFixturesPerEffect)
				{
					++Dropped;
					continue;
				}
				FixtureIds.Add(FixtureId);
			}
			if (FixtureIds.IsEmpty())
			{
				++Dropped;
				continue;
			}
			Entry.FixtureIds = MoveTemp(FixtureIds);
			Entry.Effect = SanitizeEffect(Entry.Effect);
			Kept.Add(MoveTemp(Entry));
		}
		Entries = MoveTemp(Kept);
		return Dropped;
	}

	TArray<FStageEffect> MakeQuickEffects()
	{
		TArray<FStageEffect> Effects;
		const auto Add = [&Effects](const FText& Name, EStageEffectTarget Target, EStageEffectWaveform Waveform, float RateHz, float Spread) -> FStageEffect&
		{
			FStageEffect& Effect = Effects.AddDefaulted_GetRef();
			Effect.Name = Name;
			Effect.Target = Target;
			Effect.Waveform = Waveform;
			Effect.RateHz = RateHz;
			Effect.Spread = Spread;
			return Effect;
		};
		Add(LOCTEXT("Circle", "Circle"), EStageEffectTarget::Position, EStageEffectWaveform::Circle, 0.25f, 0.f).Size = 25.f;
		Add(LOCTEXT("Figure8", "Figure 8"), EStageEffectTarget::Position, EStageEffectWaveform::Figure8, 0.2f, 0.f).Size = 25.f;
		FStageEffect& PanSweep = Add(LOCTEXT("PanSweep", "Pan Sweep"), EStageEffectTarget::Position, EStageEffectWaveform::Sine, 0.2f, 180.f);
		PanSweep.Axis = EStageEffectAxis::Pan;
		PanSweep.Size = 45.f;
		FStageEffect& TiltWave = Add(LOCTEXT("TiltWave", "Tilt Wave"), EStageEffectTarget::Position, EStageEffectWaveform::Sine, 0.3f, 360.f);
		TiltWave.Axis = EStageEffectAxis::Tilt;
		TiltWave.Size = 20.f;
		Add(LOCTEXT("DimmerSine", "Dimmer Sine"), EStageEffectTarget::Intensity, EStageEffectWaveform::Sine, 0.5f, 360.f);
		Add(LOCTEXT("DimmerChase", "Dimmer Chase"), EStageEffectTarget::Intensity, EStageEffectWaveform::Square, 1.f, 360.f).Duty = 0.25f;
		Add(LOCTEXT("Strobe", "Strobe"), EStageEffectTarget::Intensity, EStageEffectWaveform::Square, 10.f, 0.f).Duty = 0.2f;
		Add(LOCTEXT("Sparkle", "Random Sparkle"), EStageEffectTarget::Intensity, EStageEffectWaveform::Random, 4.f, 360.f);
		Add(LOCTEXT("Rainbow", "Rainbow"), EStageEffectTarget::Color, EStageEffectWaveform::RampUp, 0.1f, 360.f).ColorMode = EStageEffectColorMode::Rainbow;
		Add(LOCTEXT("ColorFade", "Colour Fade"), EStageEffectTarget::Color, EStageEffectWaveform::Sine, 0.25f, 180.f);
		FStageEffect& Iris = Add(LOCTEXT("IrisPulse", "Iris Pulse"), EStageEffectTarget::Beam, EStageEffectWaveform::Sine, 0.5f, 0.f);
		Iris.Attribute = StageCraftTags::Attribute_Iris;
		Iris.High = 0.8f;
		FStageEffect& Zoom = Add(LOCTEXT("ZoomBreathe", "Zoom Breathe"), EStageEffectTarget::Beam, EStageEffectWaveform::Triangle, 0.2f, 90.f);
		Zoom.Attribute = StageCraftTags::Attribute_Zoom;
		return Effects;
	}
}

#undef LOCTEXT_NAMESPACE
