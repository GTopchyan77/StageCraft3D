// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/ShowControlTypes.h"

#include "Data/StageParameterTypes.h"
#include "Show/StageFixtureCells.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ShowControlTypes)

bool FStageCueTiming::Equals(const FStageCueTiming& Other) const
{
	constexpr float Tolerance = 1e-4f;
	if (!FMath::IsNearlyEqual(Delay, Other.Delay, Tolerance) || bHasOutTime != Other.bHasOutTime
		|| (bHasOutTime && !FMath::IsNearlyEqual(OutTime, Other.OutTime, Tolerance)) || Trigger != Other.Trigger
		|| !FMath::IsNearlyEqual(WaitTime, Other.WaitTime, Tolerance) || Curve != Other.Curve || FeatureTimes.Num() != Other.FeatureTimes.Num())
	{
		return false;
	}
	for (const TPair<EStagePaletteKind, float>& Pair : FeatureTimes)
	{
		const float* Theirs = Other.FeatureTimes.Find(Pair.Key);
		if (!Theirs || !FMath::IsNearlyEqual(*Theirs, Pair.Value, Tolerance))
		{
			return false;
		}
	}
	return true;
}

namespace StageCueRules
{
	/**
	 * The cue number in whole thousandths. Done in double: near 10000 a float step is about 0.001 itself, so float arithmetic
	 * would let 10000 pass as 9999.999 and turn 1.5 into 1.5000001.
	 */
	int64 ToThousandths(float CueNumber)
	{
		return FMath::RoundToInt64(static_cast<double>(CueNumber) * 1000.0);
	}

	float RoundCueNumber(float CueNumber)
	{
		// Divided, not multiplied by 0.001, so the result is the float nearest the decimal number (1.5 stays exactly 1.5).
		return static_cast<float>(static_cast<double>(ToThousandths(CueNumber)) / 1000.0);
	}

	bool IsValidCueNumber(float CueNumber)
	{
		if (!FMath::IsFinite(CueNumber) || FMath::Abs(CueNumber) > 1.e7f)
		{
			return false;
		}
		const int64 Thousandths = ToThousandths(CueNumber);
		return Thousandths >= ToThousandths(MinCueNumber) && Thousandths <= ToThousandths(MaxCueNumber);
	}

	float SanitizeFadeTime(float FadeTime)
	{
		return FMath::IsFinite(FadeTime) ? FMath::Clamp(FadeTime, 0.f, MaxFadeTime) : 0.f;
	}

	int32 SanitizeTiming(FStageCueTiming& Timing)
	{
		Timing.Delay = SanitizeFadeTime(Timing.Delay);
		Timing.OutTime = Timing.bHasOutTime ? SanitizeFadeTime(Timing.OutTime) : 0.f;
		Timing.WaitTime = SanitizeFadeTime(Timing.WaitTime);
		if (static_cast<uint8>(Timing.Trigger) > static_cast<uint8>(EStageCueTrigger::Wait))
		{
			Timing.Trigger = EStageCueTrigger::Go;
		}
		if (static_cast<uint8>(Timing.Curve) > static_cast<uint8>(EStageCueCurve::SCurve))
		{
			Timing.Curve = EStageCueCurve::Linear;
		}
		const int32 Before = Timing.FeatureTimes.Num();
		for (auto It = Timing.FeatureTimes.CreateIterator(); It; ++It)
		{
			const uint8 Kind = static_cast<uint8>(It.Key());
			if (Kind < static_cast<uint8>(EStagePaletteKind::Intensity) || Kind > static_cast<uint8>(EStagePaletteKind::Focus) || !FMath::IsFinite(It.Value()))
			{
				It.RemoveCurrent();
				continue;
			}
			It.Value() = SanitizeFadeTime(It.Value());
		}
		return Before - Timing.FeatureTimes.Num();
	}

	FText SanitizeLabel(const FText& Label)
	{
		const FString Trimmed = Label.ToString().TrimStartAndEnd();
		if (Trimmed.Len() <= MaxLabelLength)
		{
			// Keeps the original FText (and its localization) when nothing had to change.
			return Trimmed.Len() == Label.ToString().Len() ? Label : FText::FromString(Trimmed);
		}
		return FText::FromString(Trimmed.Left(MaxLabelLength));
	}

	int32 SanitizeCueList(TArray<FShowCue>& Cues)
	{
		const int32 Before = Cues.Num();
		TArray<FShowCue> Kept;
		Kept.Reserve(Cues.Num());
		for (FShowCue& Cue : Cues)
		{
			if (!IsValidCueNumber(Cue.Number))
			{
				continue;
			}
			Cue.Number = RoundCueNumber(Cue.Number);
			const bool bDuplicate = Kept.ContainsByPredicate([&Cue](const FShowCue& Other)
			{
				return FMath::IsNearlyEqual(Other.Number, Cue.Number, NumberResolution * 0.5f);
			});
			if (bDuplicate)
			{
				continue;
			}
			Cue.FadeTime = SanitizeFadeTime(Cue.FadeTime);
			SanitizeTiming(Cue.Timing);
			Cue.Label = SanitizeLabel(Cue.Label);
			for (FShowCueFixtureState& State : Cue.Fixtures)
			{
				StagePaletteRules::SanitizeReferences(State);
			}
			SanitizeCueEffects(Cue);
			Kept.Add(MoveTemp(Cue));
		}
		// Stable, so the order of equal-looking input never decides which duplicate survived (that was decided above).
		Kept.StableSort([](const FShowCue& A, const FShowCue& B) { return A.Number < B.Number; });
		Cues = MoveTemp(Kept);
		return Before - Cues.Num();
	}

	int32 SanitizeCueEffects(FShowCue& Cue)
	{
		const int32 Dropped = StageEffectRules::SanitizeEntries(Cue.Effects);
		return Dropped + StageEffectRules::SanitizeStops(Cue.EffectStops, Cue.Effects);
	}
}

namespace StageFixtureGroupRules
{
	FText SanitizeName(const FText& Name)
	{
		const FString Trimmed = Name.ToString().TrimStartAndEnd();
		if (Trimmed.Len() <= MaxNameLength)
		{
			return Trimmed.Len() == Name.ToString().Len() ? Name : FText::FromString(Trimmed);
		}
		return FText::FromString(Trimmed.Left(MaxNameLength));
	}

	TArray<int32> SanitizeFixtureIds(TConstArrayView<int32> FixtureIds)
	{
		TArray<int32> Kept;
		TSet<int32> Seen;
		for (const int32 Id : FixtureIds)
		{
			if (Kept.Num() >= MaxFixturesPerGroup)
			{
				break;
			}
			bool bAlreadyKept = false;
			if (Id > 0)
			{
				Seen.Add(Id, &bAlreadyKept);
				if (!bAlreadyKept)
				{
					Kept.Add(Id);
				}
			}
		}
		return Kept;
	}

	int32 SanitizeGroups(TArray<FStageFixtureGroup>& Groups)
	{
		const int32 Before = Groups.Num();
		TArray<FStageFixtureGroup> Kept;
		TSet<FGuid> Seen;
		for (FStageFixtureGroup& Group : Groups)
		{
			bool bDuplicate = false;
			if (!Group.Id.IsValid() || Kept.Num() >= MaxGroups)
			{
				continue;
			}
			Seen.Add(Group.Id, &bDuplicate);
			if (bDuplicate)
			{
				continue;
			}
			Group.Name = SanitizeName(Group.Name);
			if (Group.Name.IsEmpty())
			{
				Group.Name = FText::Format(NSLOCTEXT("StageCraftGroups", "UnnamedGroup", "Group {0}"), FText::AsNumber(Kept.Num() + 1));
			}
			Group.FixtureIds = SanitizeFixtureIds(Group.FixtureIds);
			Kept.Add(MoveTemp(Group));
		}
		Groups = MoveTemp(Kept);
		return Before - Groups.Num();
	}
}

namespace StagePlaybackRules
{
	int32 SanitizePlaybacks(TArray<FStagePlayback>& Playbacks)
	{
		int32 Dropped = FMath::Max(0, Playbacks.Num() - Count);
		Playbacks.SetNum(Count);
		for (FStagePlayback& Playback : Playbacks)
		{
			Playback.Name = StageCueRules::SanitizeLabel(Playback.Name);
			Dropped += StageCueRules::SanitizeCueList(Playback.Cues);
			if (Playback.Cues.Num() > MaxCuesPerPlayback)
			{
				Dropped += Playback.Cues.Num() - MaxCuesPerPlayback;
				Playback.Cues.SetNum(MaxCuesPerPlayback);
			}
		}
		return Dropped;
	}
}

namespace StagePaletteRules
{
	EStagePaletteKind GetAttributeKind(const FGameplayTag& Attribute)
	{
		using namespace StageCraftTags;
		int32 Cell = 0;
		EStageCellChannel Channel = EStageCellChannel::Count;
		if (StageFixtureCells::ParseCellAttribute(Attribute, Cell, Channel))
		{
			return Channel == EStageCellChannel::Dimmer ? EStagePaletteKind::Intensity : EStagePaletteKind::Color;
		}
		if (Attribute == Attribute_Dimmer)
		{
			return EStagePaletteKind::Intensity;
		}
		if (Attribute == Attribute_Pan || Attribute == Attribute_Tilt)
		{
			return EStagePaletteKind::Position;
		}
		if (Attribute == Attribute_Zoom || Attribute == Attribute_Frost)
		{
			return EStagePaletteKind::Focus;
		}
		const bool bColor = Attribute == Attribute_ColorR || Attribute == Attribute_ColorG || Attribute == Attribute_ColorB
			|| Attribute == Attribute_ColorW || Attribute == Attribute_ColorA || Attribute == Attribute_ColorRGB
			|| Attribute == Attribute_ColorC || Attribute == Attribute_ColorM || Attribute == Attribute_ColorY
			|| Attribute == Attribute_ColorTemp || Attribute == Attribute_CTO || Attribute == Attribute_ColorWheel;
		return bColor ? EStagePaletteKind::Color : EStagePaletteKind::Beam;
	}

	bool AllowsAttribute(EStagePaletteKind Kind, const FGameplayTag& Attribute)
	{
		if (!Attribute.MatchesTag(StageCraftTags::Attribute) || Attribute == StageCraftTags::Attribute || Attribute == StageCraftTags::Attribute_ColorRGB)
		{
			return false;
		}
		int32 Cell = 0;
		EStageCellChannel Channel = EStageCellChannel::Count;
		if (StageFixtureCells::ParseCellAttribute(Attribute, Cell, Channel) && Channel == EStageCellChannel::ColorRGB)
		{
			return false; // A composite row, never stored.
		}
		return Kind == EStagePaletteKind::All || GetAttributeKind(Attribute) == Kind;
	}

	TMap<FGameplayTag, float> FilterValues(EStagePaletteKind Kind, const TMap<FGameplayTag, float>& Values)
	{
		TMap<FGameplayTag, float> Kept;
		for (const TPair<FGameplayTag, float>& Pair : Values)
		{
			if (Kept.Num() >= MaxAttributes)
			{
				break;
			}
			if (FMath::IsFinite(Pair.Value) && AllowsAttribute(Kind, Pair.Key))
			{
				Kept.Add(Pair.Key, Pair.Value);
			}
		}
		return Kept;
	}

	bool HasContent(const FStagePalette& Palette)
	{
		return Palette.bHasColor || !Palette.GlobalValues.IsEmpty() || !Palette.FixtureValues.IsEmpty();
	}

	bool FindPaletteValue(const FStagePalette& Palette, int32 FixtureId, const FGameplayTag& Attribute, const TMap<FGameplayTag, float>* ColorMatch, float& OutValue)
	{
		const FShowCueFixtureState* Own = Palette.FixtureValues.FindByPredicate([FixtureId](const FShowCueFixtureState& State) { return State.FixtureId == FixtureId; });
		const float* Value = Own ? Own->Attributes.Find(Attribute) : nullptr;
		if (!Value)
		{
			Value = Palette.GlobalValues.Find(Attribute);
		}
		if (!Value && Palette.bHasColor && ColorMatch)
		{
			Value = ColorMatch->Find(Attribute);
		}
		if (!Value)
		{
			return false;
		}
		OutValue = *Value;
		return true;
	}

	int32 SanitizeReferences(FShowCueFixtureState& State)
	{
		const TMap<FGameplayTag, float>& Attributes = State.Attributes;
		const int32 Before = State.PaletteRefs.Num();
		for (auto It = State.PaletteRefs.CreateIterator(); It; ++It)
		{
			if (!It.Value().IsValid() || !Attributes.Contains(It.Key()))
			{
				It.RemoveCurrent();
			}
		}
		return Before - State.PaletteRefs.Num();
	}

	FText SanitizeName(const FText& Name)
	{
		const FString Trimmed = Name.ToString().TrimStartAndEnd();
		if (Trimmed.Len() <= MaxNameLength)
		{
			return Trimmed.Len() == Name.ToString().Len() ? Name : FText::FromString(Trimmed);
		}
		return FText::FromString(Trimmed.Left(MaxNameLength));
	}

	/** Repairs one palette's content in place. Returns how many values and fixture entries were dropped. */
	int32 SanitizeContent(FStagePalette& Palette)
	{
		int32 Dropped = 0;

		// Colour attribute values are never global: the same tag means different things on different colour systems (ADR 0028 §2).
		TMap<FGameplayTag, float> Global = FilterValues(Palette.Kind, Palette.GlobalValues);
		for (auto It = Global.CreateIterator(); It; ++It)
		{
			if (GetAttributeKind(It.Key()) == EStagePaletteKind::Color)
			{
				It.RemoveCurrent();
			}
		}
		Dropped += Palette.GlobalValues.Num() - Global.Num();
		Palette.GlobalValues = MoveTemp(Global);

		const FLinearColor& Color = Palette.Color;
		const bool bColorValid = FMath::IsFinite(Color.R) && FMath::IsFinite(Color.G) && FMath::IsFinite(Color.B);
		if (Palette.bHasColor && (Palette.Kind != EStagePaletteKind::Color || !bColorValid))
		{
			++Dropped;
			Palette.bHasColor = false;
		}
		Palette.Color = Palette.bHasColor
			? FLinearColor(FMath::Clamp(Color.R, 0.f, 1.f), FMath::Clamp(Color.G, 0.f, 1.f), FMath::Clamp(Color.B, 0.f, 1.f), 1.f)
			: FLinearColor::White;

		TArray<FShowCueFixtureState> Fixtures;
		TSet<int32> Seen;
		for (FShowCueFixtureState& Entry : Palette.FixtureValues)
		{
			bool bDuplicate = false;
			if (Entry.FixtureId > 0 && Fixtures.Num() < MaxFixturesPerPalette)
			{
				Seen.Add(Entry.FixtureId, &bDuplicate);
			}
			TMap<FGameplayTag, float> Values = FilterValues(Palette.Kind, Entry.Attributes);
			if (Entry.FixtureId <= 0 || Fixtures.Num() >= MaxFixturesPerPalette || bDuplicate || Values.IsEmpty())
			{
				++Dropped;
				continue;
			}
			Dropped += Entry.Attributes.Num() - Values.Num();
			FShowCueFixtureState& Kept = Fixtures.AddDefaulted_GetRef();
			Kept.FixtureId = Entry.FixtureId;
			Kept.Attributes = MoveTemp(Values);
			// A palette holds values, never references to other palettes.
		}
		Palette.FixtureValues = MoveTemp(Fixtures);
		return Dropped;
	}

	int32 SanitizePalettes(TArray<FStagePalette>& Palettes)
	{
		int32 Dropped = 0;
		TArray<FStagePalette> Kept;
		TSet<FGuid> Seen;
		for (FStagePalette& Palette : Palettes)
		{
			bool bDuplicate = false;
			const bool bKnownKind = static_cast<uint8>(Palette.Kind) <= static_cast<uint8>(EStagePaletteKind::Focus);
			if (!Palette.Id.IsValid() || !bKnownKind || Kept.Num() >= MaxPalettes)
			{
				++Dropped;
				continue;
			}
			Seen.Add(Palette.Id, &bDuplicate);
			if (bDuplicate)
			{
				++Dropped;
				continue;
			}
			Palette.Name = SanitizeName(Palette.Name);
			if (Palette.Name.IsEmpty())
			{
				Palette.Name = FText::Format(NSLOCTEXT("StageCraftPalettes", "UnnamedPalette", "Palette {0}"), FText::AsNumber(Kept.Num() + 1));
			}
			Dropped += SanitizeContent(Palette);
			Kept.Add(MoveTemp(Palette));
		}
		Palettes = MoveTemp(Kept);
		return Dropped;
	}

	TArray<float> FindCuesUsing(const TArray<FShowCue>& Cues, const FGuid& PaletteId)
	{
		TArray<float> Numbers;
		for (const FShowCue& Cue : Cues)
		{
			const bool bUses = Cue.Fixtures.ContainsByPredicate([&PaletteId](const FShowCueFixtureState& State)
			{
				for (const TPair<FGameplayTag, FGuid>& Ref : State.PaletteRefs)
				{
					if (Ref.Value == PaletteId)
					{
						return true;
					}
				}
				return false;
			});
			if (bUses)
			{
				Numbers.Add(Cue.Number);
			}
		}
		Numbers.Sort();
		return Numbers;
	}

	TArray<FStagePalette> MakeStarterPalettes()
	{
		using namespace StageCraftTags;
		// Fixed ids ("SCPALETTE" + index): every stage has the same starter palettes, so cues copied between stages still find them.
		int32 Index = 0;
		TArray<FStagePalette> Palettes;
		const auto Add = [&Palettes, &Index](const FText& Name, EStagePaletteKind Kind) -> FStagePalette&
		{
			FStagePalette& Palette = Palettes.AddDefaulted_GetRef();
			Palette.Id = FGuid(0x53435041, 0x4C455454, 0x45000000, static_cast<uint32>(++Index));
			Palette.Name = Name;
			Palette.Kind = Kind;
			return Palette;
		};
		const auto AddColor = [&Add](const FText& Name, const FLinearColor& Color)
		{
			FStagePalette& Palette = Add(Name, EStagePaletteKind::Color);
			Palette.bHasColor = true;
			Palette.Color = Color;
		};
		AddColor(NSLOCTEXT("StageCraftPalettes", "White", "White"), FLinearColor(1.f, 1.f, 1.f));
		AddColor(NSLOCTEXT("StageCraftPalettes", "Red", "Red"), FLinearColor(1.f, 0.f, 0.f));
		AddColor(NSLOCTEXT("StageCraftPalettes", "Green", "Green"), FLinearColor(0.f, 1.f, 0.f));
		AddColor(NSLOCTEXT("StageCraftPalettes", "Blue", "Blue"), FLinearColor(0.f, 0.f, 1.f));
		AddColor(NSLOCTEXT("StageCraftPalettes", "Amber", "Amber"), FLinearColor(1.f, 0.5f, 0.f));
		AddColor(NSLOCTEXT("StageCraftPalettes", "Magenta", "Magenta"), FLinearColor(1.f, 0.f, 1.f));
		AddColor(NSLOCTEXT("StageCraftPalettes", "Cyan", "Cyan"), FLinearColor(0.f, 1.f, 1.f));
		AddColor(NSLOCTEXT("StageCraftPalettes", "UV", "UV"), FLinearColor(0.3f, 0.f, 1.f));

		FStagePalette& Home = Add(NSLOCTEXT("StageCraftPalettes", "Home", "Home"), EStagePaletteKind::Position);
		Home.GlobalValues.Add(Attribute_Pan, 0.f);
		Home.GlobalValues.Add(Attribute_Tilt, 0.f);

		FStagePalette& Open = Add(NSLOCTEXT("StageCraftPalettes", "Open", "Open"), EStagePaletteKind::Beam);
		Open.GlobalValues.Add(Attribute_Shutter, 0.f);
		Open.GlobalValues.Add(Attribute_Gobo, 0.f);
		Open.GlobalValues.Add(Attribute_Gobo2, 0.f);
		Open.GlobalValues.Add(Attribute_Prism, 0.f);
		Open.GlobalValues.Add(Attribute_Iris, 0.f);
		return Palettes;
	}
}
