// Copyright Epic Games, Inc. All Rights Reserved.

#include "Scene/StageSceneStore.h"

#include "Data/BaseItemData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameplayTagContainer.h"
#include "HAL/FileManager.h"
#include "Import/StageMeshImportTypes.h"
#include "Interaction/StageTransformRules.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "IO/StageCraftPaths.h"
#include "Misc/Paths.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "IO/StageAtomicFile.h"
#include "Workspace/StageLayoutStore.h"

namespace StageSceneStore
{
	const TCHAR* Extension = TEXT(".json");
	const TCHAR* FormatTag = TEXT("StageCraftScene");

	namespace Field
	{
		const TCHAR* Format = TEXT("format");
		const TCHAR* Version = TEXT("version");
		const TCHAR* Name = TEXT("name");
		const TCHAR* Level = TEXT("level");
		const TCHAR* SavedAt = TEXT("savedAt");
		const TCHAR* Items = TEXT("items");
		const TCHAR* Id = TEXT("id");
		const TCHAR* Item = TEXT("item");
		const TCHAR* Label = TEXT("label");
		const TCHAR* Location = TEXT("location");
		const TCHAR* Rotation = TEXT("rotation");
		const TCHAR* Scale = TEXT("scale");
		const TCHAR* State = TEXT("state");

		// Version 2: the outdoor environment.
		const TCHAR* Environment = TEXT("environment");
		const TCHAR* SkyEnabled = TEXT("skyEnabled");
		const TCHAR* TimeOfDay = TEXT("timeOfDay");
		const TCHAR* SunElevation = TEXT("sunElevation");
		const TCHAR* SunAzimuth = TEXT("sunAzimuth");
		const TCHAR* CloudCoverage = TEXT("cloudCoverage");
		const TCHAR* Rain = TEXT("rain");
		const TCHAR* Snow = TEXT("snow");
		const TCHAR* Fog = TEXT("fog");

		// Version 5: what lights the stage and the camera look (Docs/ADR/0024 §4). "skyEnabled" is still read from older files.
		const TCHAR* Lighting = TEXT("lighting");
		const TCHAR* Look = TEXT("look");
		const TCHAR* Exposure = TEXT("exposure");
		const TCHAR* Bloom = TEXT("bloom");
		const TCHAR* AmbientOcclusion = TEXT("ambientOcclusion");
		const TCHAR* WhiteBalance = TEXT("whiteBalance");
		const TCHAR* Saturation = TEXT("saturation");
		const TCHAR* Contrast = TEXT("contrast");
		const TCHAR* Vignette = TEXT("vignette");
		const TCHAR* StudioKeyIntensity = TEXT("studioKeyIntensity");
		const TCHAR* StudioKeyColor = TEXT("studioKeyColor");

		// Version 3: the cue list.
		const TCHAR* Cues = TEXT("cues");
		const TCHAR* Number = TEXT("number");
		const TCHAR* Fade = TEXT("fade");
		const TCHAR* Fixtures = TEXT("fixtures");
		const TCHAR* Attributes = TEXT("attributes");

		// Version 4: where a marketplace stage came from.
		const TCHAR* OriginListingId = TEXT("originListingId");

		// Version 6: the programmer, fixture groups and the playing cue (Docs/ADR/0025 §7).
		const TCHAR* Programmer = TEXT("programmer");
		const TCHAR* Groups = TEXT("groups");
		const TCHAR* ActiveCue = TEXT("activeCue");

		// Version 7: palettes and palette references (Docs/ADR/0028 §6).
		const TCHAR* Palettes = TEXT("palettes");
		const TCHAR* Kind = TEXT("kind");
		const TCHAR* Color = TEXT("color");
		const TCHAR* Values = TEXT("values");

		// Version 8: cue timing and tracking (Docs/ADR/0026 §5).
		const TCHAR* Delay = TEXT("delay");
		const TCHAR* Out = TEXT("out");
		const TCHAR* Times = TEXT("times");
		const TCHAR* Trigger = TEXT("trigger");
		const TCHAR* Wait = TEXT("wait");
		const TCHAR* Curve = TEXT("curve");
		const TCHAR* Block = TEXT("block");
		const TCHAR* Playbacks = TEXT("playbacks");
		const TCHAR* Index = TEXT("index");

		// Version 10: effects in cues, stops in cues and the programmer (Docs/ADR/0027 §5).
		const TCHAR* Effects = TEXT("effects");
		const TCHAR* EffectStops = TEXT("effectStops");
		const TCHAR* ProgrammerEffectStops = TEXT("programmerEffectStops");
	}

	// Effects are written and read the same way in cues as in the programmer; both are defined with the version 9 code below.
	TArray<TSharedPtr<FJsonValue>> WriteEffects(const TArray<FStageEffectEntry>& Entries);
	TArray<FStageEffectEntry> ReadEffects(const FJsonObject& Owner, const TCHAR* Name, int32& InOutInvalid);

	/** Effect ids (stops) as strings. */
	TArray<TSharedPtr<FJsonValue>> WriteEffectStops(const TArray<FGuid>& Stops)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		Array.Reserve(Stops.Num());
		for (const FGuid& Stop : Stops)
		{
			Array.Add(MakeShared<FJsonValueString>(Stop.ToString(EGuidFormats::DigitsWithHyphens)));
		}
		return Array;
	}

	/** The stops in Name (a version 10 file): unreadable ids dropped and counted, then repaired against Effects (SanitizeStops). */
	TArray<FGuid> ReadEffectStops(const FJsonObject& Owner, const TCHAR* Name, TConstArrayView<FStageEffectEntry> Effects, int32& InOutInvalid)
	{
		TArray<FGuid> Stops;
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Owner.TryGetArrayField(Name, Array))
		{
			return Stops;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			FString Text;
			FGuid Id;
			if (Stops.Num() >= StageEffectRules::MaxEffects || !Value.IsValid() || !Value->TryGetString(Text) || !FGuid::Parse(Text, Id) || !Id.IsValid())
			{
				++InOutInvalid;
				continue;
			}
			Stops.Add(Id);
		}
		InOutInvalid += StageEffectRules::SanitizeStops(Stops, Effects);
		return Stops;
	}

	/** Cue triggers and curves by name in files, so a reordered enum never changes what a file means. Index = enum value. */
	const TCHAR* const CueTriggerNames[] = { TEXT("Go"), TEXT("Follow"), TEXT("Wait") };
	static_assert(UE_ARRAY_COUNT(CueTriggerNames) == static_cast<int32>(EStageCueTrigger::Wait) + 1, "Name every cue trigger.");
	const TCHAR* const CueCurveNames[] = { TEXT("Linear"), TEXT("SCurve") };
	static_assert(UE_ARRAY_COUNT(CueCurveNames) == static_cast<int32>(EStageCueCurve::SCurve) + 1, "Name every cue curve.");

	/** Index of Name in Names; INDEX_NONE for a name this build does not know. */
	int32 FindName(TConstArrayView<const TCHAR*> Names, const FString& Name)
	{
		for (int32 Index = 0; Index < Names.Num(); ++Index)
		{
			if (Name == Names[Index])
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	/** Palette kinds by name in files, so a reordered enum never changes what a file means. Index = EStagePaletteKind value. */
	const TCHAR* const PaletteKindNames[] = { TEXT("All"), TEXT("Intensity"), TEXT("Position"), TEXT("Color"), TEXT("Beam"), TEXT("Focus") };
	static_assert(UE_ARRAY_COUNT(PaletteKindNames) == static_cast<int32>(EStagePaletteKind::Focus) + 1, "Name every palette kind.");

	/** The palette kind named Name in a file; false for a name this build does not know. */
	bool ParsePaletteKind(const FString& Name, EStagePaletteKind& OutKind)
	{
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(PaletteKindNames); ++Index)
		{
			if (Name == PaletteKindNames[Index])
			{
				OutKind = static_cast<EStagePaletteKind>(Index);
				return true;
			}
		}
		return false;
	}


	/** {"<tag>": value} for an attribute map. */
	TSharedRef<FJsonObject> WriteAttributeValues(const TMap<FGameplayTag, float>& Values)
	{
		// Tags by name: a name stays readable and survives tag table changes; unknown ones are dropped on load.
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		for (const TPair<FGameplayTag, float>& Pair : Values)
		{
			Object->SetNumberField(Pair.Key.ToString(), Pair.Value);
		}
		return Object;
	}

	/** Reads {"<tag>": value} into OutValues; unknown tags and non-finite values are dropped and counted, at most MaxAttributesPerFixture. */
	void ReadAttributeValues(const FJsonObject& Object, TMap<FGameplayTag, float>& OutValues, int32& InOutInvalid)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object.Values)
		{
			double Value = 0.0;
			// ErrorIfNotFound = false: a tag this build does not know (removed attribute, hand edit) is skipped, not fatal.
			const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*Pair.Key), /*ErrorIfNotFound*/ false);
			if (OutValues.Num() >= FStageSceneStore::MaxAttributesPerFixture || !Tag.IsValid() || !Pair.Value.IsValid() || !Pair.Value->TryGetNumber(Value) || !FMath::IsFinite(Value))
			{
				++InOutInvalid;
				continue;
			}
			OutValues.Add(Tag, static_cast<float>(Value));
		}
	}

	/**
	 * One fixture's attribute values: {"id": fixtureId, "attributes": {"<tag>": value}, "palettes": {"<tag>": id}} (cues, the
	 * programmer and per-fixture palette values). "palettes" only when the fixture follows any.
	 */
	TSharedPtr<FJsonValue> WriteFixtureState(const FShowCueFixtureState& State)
	{
		const TSharedRef<FJsonObject> Fixture = MakeShared<FJsonObject>();
		Fixture->SetNumberField(Field::Id, State.FixtureId);
		Fixture->SetObjectField(Field::Attributes, WriteAttributeValues(State.Attributes));
		if (!State.PaletteRefs.IsEmpty())
		{
			const TSharedRef<FJsonObject> References = MakeShared<FJsonObject>();
			for (const TPair<FGameplayTag, FGuid>& Pair : State.PaletteRefs)
			{
				References->SetStringField(Pair.Key.ToString(), Pair.Value.ToString(EGuidFormats::DigitsWithHyphens));
			}
			Fixture->SetObjectField(Field::Palettes, References);
		}
		return MakeShared<FJsonValueObject>(Fixture);
	}

	TArray<TSharedPtr<FJsonValue>> WriteCues(const TArray<FShowCue>& Cues)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		Array.Reserve(Cues.Num());
		for (const FShowCue& Cue : Cues)
		{
			TArray<TSharedPtr<FJsonValue>> Fixtures;
			Fixtures.Reserve(Cue.Fixtures.Num());
			for (const FShowCueFixtureState& State : Cue.Fixtures)
			{
				Fixtures.Add(WriteFixtureState(State));
			}

			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetNumberField(Field::Number, Cue.Number);
			Object->SetStringField(Field::Label, Cue.Label.ToString());
			Object->SetNumberField(Field::Fade, Cue.FadeTime);
			const FStageCueTiming& Timing = Cue.Timing;
			Object->SetNumberField(Field::Delay, Timing.Delay);
			if (Timing.bHasOutTime)
			{
				Object->SetNumberField(Field::Out, Timing.OutTime);
			}
			if (!Timing.FeatureTimes.IsEmpty())
			{
				const TSharedRef<FJsonObject> Times = MakeShared<FJsonObject>();
				for (const TPair<EStagePaletteKind, float>& Pair : Timing.FeatureTimes)
				{
					Times->SetNumberField(PaletteKindNames[static_cast<int32>(Pair.Key)], Pair.Value);
				}
				Object->SetObjectField(Field::Times, Times);
			}
			Object->SetStringField(Field::Trigger, CueTriggerNames[static_cast<int32>(Timing.Trigger)]);
			Object->SetNumberField(Field::Wait, Timing.WaitTime);
			Object->SetStringField(Field::Curve, CueCurveNames[static_cast<int32>(Timing.Curve)]);
			Object->SetBoolField(Field::Block, Cue.bBlock);
			Object->SetArrayField(Field::Fixtures, Fixtures);
			if (!Cue.Effects.IsEmpty())
			{
				Object->SetArrayField(Field::Effects, WriteEffects(Cue.Effects));
			}
			if (!Cue.EffectStops.IsEmpty())
			{
				Object->SetArrayField(Field::EffectStops, WriteEffectStops(Cue.EffectStops));
			}
			Array.Add(MakeShared<FJsonValueObject>(Object));
		}
		return Array;
	}

	/** One fixture of a cue. Invalid attributes are dropped and counted; false drops the whole fixture. */
	bool ReadCueFixture(const FJsonObject& Object, FShowCueFixtureState& OutState, int32& InOutInvalid)
	{
		int32 FixtureId = 0;
		const TSharedPtr<FJsonObject>* Attributes = nullptr;
		if (!Object.TryGetNumberField(Field::Id, FixtureId) || FixtureId <= 0 || !Object.TryGetObjectField(Field::Attributes, Attributes) || !Attributes->IsValid())
		{
			return false;
		}
		OutState.FixtureId = FixtureId;
		ReadAttributeValues(**Attributes, OutState.Attributes, InOutInvalid);

		// Version 7: references to palettes. One without a valid tag, id or a value of its own is dropped and counted.
		const TSharedPtr<FJsonObject>* References = nullptr;
		if (Object.TryGetObjectField(Field::Palettes, References) && References->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*References)->Values)
			{
				const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*Pair.Key), /*ErrorIfNotFound*/ false);
				FString Id;
				FGuid PaletteId;
				if (!Tag.IsValid() || !OutState.Attributes.Contains(Tag) || !Pair.Value.IsValid() || !Pair.Value->TryGetString(Id)
					|| !FGuid::Parse(Id, PaletteId) || !PaletteId.IsValid())
				{
					++InOutInvalid;
					continue;
				}
				OutState.PaletteRefs.Add(Tag, PaletteId);
			}
		}
		return true;
	}

	/** A finite time within StageCueRules' range from Object's Field; false (counted) when present but not one. */
	bool ReadTime(const FJsonObject& Object, const TCHAR* Name, float& OutTime, int32& InOutInvalid)
	{
		const TSharedPtr<FJsonValue> Value = Object.TryGetField(Name);
		double Number = 0.0;
		if (!Value.IsValid())
		{
			return false;
		}
		if (!Value->TryGetNumber(Number) || !FMath::IsFinite(Number) || Number < 0.0 || Number > StageCueRules::MaxFadeTime)
		{
			++InOutInvalid;
			return false;
		}
		OutTime = static_cast<float>(Number);
		return true;
	}

	/** A version 8 cue's timing and block. What cannot be read is dropped and counted; the cue keeps the default for it. */
	void ReadCueTiming(const FJsonObject& Object, FShowCue& OutCue, int32& InOutInvalid)
	{
		FStageCueTiming& Timing = OutCue.Timing;
		ReadTime(Object, Field::Delay, Timing.Delay, InOutInvalid);
		Timing.bHasOutTime = ReadTime(Object, Field::Out, Timing.OutTime, InOutInvalid);
		ReadTime(Object, Field::Wait, Timing.WaitTime, InOutInvalid);
		const TSharedPtr<FJsonObject>* Times = nullptr;
		if (Object.TryGetObjectField(Field::Times, Times) && Times->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Times)->Values)
			{
				EStagePaletteKind Kind = EStagePaletteKind::All;
				float Time = 0.f;
				if (!ParsePaletteKind(Pair.Key, Kind) || Kind == EStagePaletteKind::All)
				{
					++InOutInvalid;
					continue;
				}
				if (ReadTime(**Times, *Pair.Key, Time, InOutInvalid))
				{
					Timing.FeatureTimes.Add(Kind, Time);
				}
			}
		}
		FString Name;
		if (Object.TryGetStringField(Field::Trigger, Name))
		{
			const int32 Trigger = FindName(CueTriggerNames, Name);
			InOutInvalid += Trigger == INDEX_NONE ? 1 : 0;
			Timing.Trigger = Trigger == INDEX_NONE ? EStageCueTrigger::Go : static_cast<EStageCueTrigger>(Trigger);
		}
		if (Object.TryGetStringField(Field::Curve, Name))
		{
			const int32 Curve = FindName(CueCurveNames, Name);
			InOutInvalid += Curve == INDEX_NONE ? 1 : 0;
			Timing.Curve = Curve == INDEX_NONE ? EStageCueCurve::Linear : static_cast<EStageCueCurve>(Curve);
		}
		bool bBlock = false;
		const TSharedPtr<FJsonValue> BlockValue = Object.TryGetField(Field::Block);
		if (BlockValue.IsValid())
		{
			// A real boolean only: TryGetBool would read "yes" as true.
			if (BlockValue->Type == EJson::Boolean && BlockValue->TryGetBool(bBlock))
			{
				OutCue.bBlock = bBlock;
			}
			else
			{
				++InOutInvalid;
			}
		}
	}

	/** One cue. False drops it (bad number); bad fixtures inside it are dropped and counted. */
	bool ReadCue(const FJsonObject& Object, FShowCue& OutCue, int32 Version, int32& InOutInvalid)
	{
		double Number = 0.0;
		if (!Object.TryGetNumberField(Field::Number, Number) || !StageCueRules::IsValidCueNumber(static_cast<float>(Number)))
		{
			return false;
		}
		OutCue.Number = StageCueRules::RoundCueNumber(static_cast<float>(Number));

		FString Label;
		Object.TryGetStringField(Field::Label, Label);
		OutCue.Label = StageCueRules::SanitizeLabel(FText::FromString(Label));

		double Fade = 0.0;
		OutCue.FadeTime = Object.TryGetNumberField(Field::Fade, Fade) ? StageCueRules::SanitizeFadeTime(static_cast<float>(Fade)) : 0.f;

		// Migration 7 -> 8: cues were full snapshots of every fixture, so they play as blocked cues (Docs/ADR/0026 §3).
		if (Version >= 8)
		{
			ReadCueTiming(Object, OutCue, InOutInvalid);
		}
		else
		{
			OutCue.bBlock = true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Fixtures = nullptr;
		if (Object.TryGetArrayField(Field::Fixtures, Fixtures))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Fixtures)
			{
				const TSharedPtr<FJsonObject>* FixtureObject = nullptr;
				FShowCueFixtureState State;
				if (OutCue.Fixtures.Num() >= FStageSceneStore::MaxFixturesPerCue || !Value.IsValid() || !Value->TryGetObject(FixtureObject)
					|| !FixtureObject->IsValid() || !ReadCueFixture(**FixtureObject, State, InOutInvalid))
				{
					++InOutInvalid;
					continue;
				}
				OutCue.Fixtures.Add(MoveTemp(State));
			}
		}

		// Migration 9 -> 10: older cues hold no effects (Docs/ADR/0027 §5).
		if (Version >= 10)
		{
			OutCue.Effects = ReadEffects(Object, Field::Effects, InOutInvalid);
			OutCue.EffectStops = ReadEffectStops(Object, Field::EffectStops, OutCue.Effects, InOutInvalid);
		}
		return true;
	}

	/**
	 * The cue list of a version 3 file. Tolerant like ReadEnvironment: what cannot be read is dropped and counted, the rest loads,
	 * and the list is repaired by the same rules as every other cue entry (duplicates, order).
	 */
	TArray<FShowCue> ReadCues(const FJsonObject& Root, int32 Version, int32 MaxCues, int32& InOutInvalid)
	{
		TArray<FShowCue> Cues;
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Root.TryGetArrayField(Field::Cues, Array))
		{
			return Cues;
		}
		Cues.Reserve(FMath::Min(Array->Num(), MaxCues));
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FShowCue Cue;
			if (Cues.Num() >= MaxCues || !Value.IsValid() || !Value->TryGetObject(Object) || !Object->IsValid()
				|| !ReadCue(**Object, Cue, Version, InOutInvalid))
			{
				++InOutInvalid;
				continue;
			}
			Cues.Add(MoveTemp(Cue));
		}
		InOutInvalid += StageCueRules::SanitizeCueList(Cues);
		return Cues;
	}

	TArray<TSharedPtr<FJsonValue>> WriteGroups(const TArray<FStageFixtureGroup>& Groups)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (const FStageFixtureGroup& Group : Groups)
		{
			TArray<TSharedPtr<FJsonValue>> Fixtures;
			for (const int32 FixtureId : Group.FixtureIds)
			{
				Fixtures.Add(MakeShared<FJsonValueNumber>(FixtureId));
			}
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(Field::Id, Group.Id.ToString(EGuidFormats::DigitsWithHyphens));
			Object->SetStringField(Field::Name, Group.Name.ToString());
			Object->SetArrayField(Field::Fixtures, Fixtures);
			Array.Add(MakeShared<FJsonValueObject>(Object));
		}
		return Array;
	}

	/** The programmer of a version 6 file: one entry per fixture, as a cue's fixtures. What cannot be read is dropped and counted. */
	TArray<FShowCueFixtureState> ReadProgrammer(const FJsonObject& Root, int32& InOutInvalid)
	{
		TArray<FShowCueFixtureState> Entries;
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Root.TryGetArrayField(Field::Programmer, Array))
		{
			return Entries;
		}
		TSet<int32> Seen;
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FShowCueFixtureState State;
			bool bDuplicate = false;
			if (Entries.Num() >= FStageSceneStore::MaxFixturesPerCue || !Value.IsValid() || !Value->TryGetObject(Object) || !Object->IsValid()
				|| !ReadCueFixture(**Object, State, InOutInvalid))
			{
				++InOutInvalid;
				continue;
			}
			Seen.Add(State.FixtureId, &bDuplicate);
			if (bDuplicate)
			{
				++InOutInvalid;
				continue;
			}
			Entries.Add(MoveTemp(State));
		}
		return Entries;
	}

	/** The fixture groups of a version 6 file, repaired by StageFixtureGroupRules. */
	TArray<FStageFixtureGroup> ReadGroups(const FJsonObject& Root, int32& InOutInvalid)
	{
		TArray<FStageFixtureGroup> Groups;
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Root.TryGetArrayField(Field::Groups, Array))
		{
			return Groups;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FString Id;
			FStageFixtureGroup Group;
			if (Groups.Num() >= FStageSceneStore::MaxGroups || !Value.IsValid() || !Value->TryGetObject(Object) || !Object->IsValid()
				|| !(*Object)->TryGetStringField(Field::Id, Id) || !FGuid::Parse(Id, Group.Id) || !Group.Id.IsValid())
			{
				++InOutInvalid;
				continue;
			}
			FString Name;
			(*Object)->TryGetStringField(Field::Name, Name);
			Group.Name = FText::FromString(Name);
			const TArray<TSharedPtr<FJsonValue>>* Fixtures = nullptr;
			if ((*Object)->TryGetArrayField(Field::Fixtures, Fixtures))
			{
				for (const TSharedPtr<FJsonValue>& Fixture : *Fixtures)
				{
					double FixtureId = 0.0;
					if (Group.FixtureIds.Num() < StageFixtureGroupRules::MaxFixturesPerGroup && Fixture.IsValid() && Fixture->TryGetNumber(FixtureId)
						&& FMath::IsFinite(FixtureId) && FixtureId >= 1.0 && FixtureId <= double(MAX_int32))
					{
						Group.FixtureIds.Add(static_cast<int32>(FixtureId));
					}
					else
					{
						++InOutInvalid;
					}
				}
			}
			Groups.Add(MoveTemp(Group));
		}
		InOutInvalid += StageFixtureGroupRules::SanitizeGroups(Groups);
		return Groups;
	}

	/** The playbacks that hold anything (Docs/ADR/0026 §6): {"index", "name", "cues"}, cues as in the main list. */
	TArray<TSharedPtr<FJsonValue>> WritePlaybacks(const TArray<FStagePlayback>& Playbacks)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (int32 Index = 0; Index < Playbacks.Num(); ++Index)
		{
			const FStagePlayback& Playback = Playbacks[Index];
			if (Playback.Cues.IsEmpty() && Playback.Name.IsEmpty())
			{
				continue;
			}
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetNumberField(Field::Index, Index);
			Object->SetStringField(Field::Name, Playback.Name.ToString());
			Object->SetArrayField(Field::Cues, WriteCues(Playback.Cues));
			Array.Add(MakeShared<FJsonValueObject>(Object));
		}
		return Array;
	}

	/** The playbacks of a version 8 file: one entry per used playback, by index. Bad entries are dropped and counted. */
	TArray<FStagePlayback> ReadPlaybacks(const FJsonObject& Root, int32 Version, int32& InOutInvalid)
	{
		TArray<FStagePlayback> Playbacks;
		Playbacks.SetNum(StagePlaybackRules::Count);
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Root.TryGetArrayField(Field::Playbacks, Array))
		{
			return Playbacks;
		}
		TSet<int32> Seen;
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			double Index = -1.0;
			bool bDuplicate = false;
			if (!Value.IsValid() || !Value->TryGetObject(Object) || !Object->IsValid() || !(*Object)->TryGetNumberField(Field::Index, Index)
				|| Index < 0.0 || Index >= StagePlaybackRules::Count || Index != FMath::FloorToDouble(Index))
			{
				++InOutInvalid;
				continue;
			}
			Seen.Add(static_cast<int32>(Index), &bDuplicate);
			if (bDuplicate)
			{
				++InOutInvalid;
				continue;
			}
			FStagePlayback& Playback = Playbacks[static_cast<int32>(Index)];
			FString Name;
			(*Object)->TryGetStringField(Field::Name, Name);
			Playback.Name = FText::FromString(Name);
			Playback.Cues = ReadCues(**Object, Version, StagePlaybackRules::MaxCuesPerPlayback, InOutInvalid);
		}
		InOutInvalid += StagePlaybackRules::SanitizePlaybacks(Playbacks);
		return Playbacks;
	}

	TArray<TSharedPtr<FJsonValue>> WritePalettes(const TArray<FStagePalette>& Palettes)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (const FStagePalette& Palette : Palettes)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(Field::Id, Palette.Id.ToString(EGuidFormats::DigitsWithHyphens));
			Object->SetStringField(Field::Name, Palette.Name.ToString());
			Object->SetStringField(Field::Kind, PaletteKindNames[static_cast<int32>(Palette.Kind)]);
			if (Palette.bHasColor)
			{
				TArray<TSharedPtr<FJsonValue>> Color;
				Color.Add(MakeShared<FJsonValueNumber>(Palette.Color.R));
				Color.Add(MakeShared<FJsonValueNumber>(Palette.Color.G));
				Color.Add(MakeShared<FJsonValueNumber>(Palette.Color.B));
				Object->SetArrayField(Field::Color, Color);
			}
			Object->SetObjectField(Field::Values, WriteAttributeValues(Palette.GlobalValues));
			TArray<TSharedPtr<FJsonValue>> Fixtures;
			for (const FShowCueFixtureState& Entry : Palette.FixtureValues)
			{
				Fixtures.Add(WriteFixtureState(Entry));
			}
			Object->SetArrayField(Field::Fixtures, Fixtures);
			Array.Add(MakeShared<FJsonValueObject>(Object));
		}
		return Array;
	}

	/** One palette. False drops it (no valid id or an unknown kind); bad values inside are dropped and counted. */
	bool ReadPalette(const FJsonObject& Object, FStagePalette& OutPalette, int32& InOutInvalid)
	{
		FString Id;
		FString Kind;
		if (!Object.TryGetStringField(Field::Id, Id) || !FGuid::Parse(Id, OutPalette.Id) || !OutPalette.Id.IsValid()
			|| !Object.TryGetStringField(Field::Kind, Kind) || !ParsePaletteKind(Kind, OutPalette.Kind))
		{
			return false;
		}

		FString Name;
		Object.TryGetStringField(Field::Name, Name);
		OutPalette.Name = FText::FromString(Name);

		const TArray<TSharedPtr<FJsonValue>>* Color = nullptr;
		if (Object.TryGetArrayField(Field::Color, Color))
		{
			double Channels[3] = { 0.0, 0.0, 0.0 };
			bool bValid = Color->Num() == 3;
			for (int32 Index = 0; bValid && Index < 3; ++Index)
			{
				bValid = (*Color)[Index].IsValid() && (*Color)[Index]->TryGetNumber(Channels[Index]) && FMath::IsFinite(Channels[Index]);
			}
			if (bValid)
			{
				OutPalette.bHasColor = true;
				OutPalette.Color = FLinearColor(static_cast<float>(Channels[0]), static_cast<float>(Channels[1]), static_cast<float>(Channels[2]));
			}
			else
			{
				++InOutInvalid;
			}
		}

		const TSharedPtr<FJsonObject>* Values = nullptr;
		if (Object.TryGetObjectField(Field::Values, Values) && Values->IsValid())
		{
			ReadAttributeValues(**Values, OutPalette.GlobalValues, InOutInvalid);
		}

		const TArray<TSharedPtr<FJsonValue>>* Fixtures = nullptr;
		if (Object.TryGetArrayField(Field::Fixtures, Fixtures))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Fixtures)
			{
				const TSharedPtr<FJsonObject>* FixtureObject = nullptr;
				FShowCueFixtureState Entry;
				if (OutPalette.FixtureValues.Num() >= StagePaletteRules::MaxFixturesPerPalette || !Value.IsValid() || !Value->TryGetObject(FixtureObject)
					|| !FixtureObject->IsValid() || !ReadCueFixture(**FixtureObject, Entry, InOutInvalid))
				{
					++InOutInvalid;
					continue;
				}
				OutPalette.FixtureValues.Add(MoveTemp(Entry));
			}
		}
		return true;
	}

	/** The palettes of a version 7 file, repaired by StagePaletteRules (wrong kinds, duplicates, bounds). */
	TArray<FStagePalette> ReadPalettes(const FJsonObject& Root, int32& InOutInvalid)
	{
		TArray<FStagePalette> Palettes;
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Root.TryGetArrayField(Field::Palettes, Array))
		{
			return Palettes;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FStagePalette Palette;
			if (Palettes.Num() >= StagePaletteRules::MaxPalettes || !Value.IsValid() || !Value->TryGetObject(Object) || !Object->IsValid()
				|| !ReadPalette(**Object, Palette, InOutInvalid))
			{
				++InOutInvalid;
				continue;
			}
			Palettes.Add(MoveTemp(Palette));
		}
		InOutInvalid += StagePaletteRules::SanitizePalettes(Palettes);
		return Palettes;
	}

	// --- Version 9: effects (Docs/ADR/0027-effects-engine.md §4) ---

	namespace EffectField
	{
		const TCHAR* ProgrammerEffects = TEXT("programmerEffects");
		const TCHAR* Target = TEXT("target");
		const TCHAR* Waveform = TEXT("waveform");
		const TCHAR* Attribute = TEXT("attribute");
		const TCHAR* Axis = TEXT("axis");
		const TCHAR* ColorMode = TEXT("colorMode");
		const TCHAR* ColorA = TEXT("colorA");
		const TCHAR* ColorB = TEXT("colorB");
		const TCHAR* Low = TEXT("low");
		const TCHAR* High = TEXT("high");
		const TCHAR* Size = TEXT("size");
		const TCHAR* Rate = TEXT("rate");
		const TCHAR* Duty = TEXT("duty");
		const TCHAR* Phase = TEXT("phase");
		const TCHAR* Spread = TEXT("spread");
		const TCHAR* Wings = TEXT("wings");
		const TCHAR* Blocks = TEXT("blocks");
		const TCHAR* Direction = TEXT("direction");
		const TCHAR* SizeFan = TEXT("sizeFan");
	}

	/** Effect settings by name in files, so a reordered enum never changes what a file means. Index = enum value. */
	const TCHAR* const EffectTargetNames[] = { TEXT("Intensity"), TEXT("Position"), TEXT("Color"), TEXT("Beam") };
	static_assert(UE_ARRAY_COUNT(EffectTargetNames) == static_cast<int32>(EStageEffectTarget::Beam) + 1, "Name every effect target.");
	const TCHAR* const EffectWaveformNames[] = { TEXT("Sine"), TEXT("Cosine"), TEXT("Square"), TEXT("RampUp"), TEXT("RampDown"), TEXT("Triangle"),
		TEXT("Random"), TEXT("Circle"), TEXT("Figure8") };
	static_assert(UE_ARRAY_COUNT(EffectWaveformNames) == static_cast<int32>(EStageEffectWaveform::Figure8) + 1, "Name every waveform.");
	const TCHAR* const EffectAxisNames[] = { TEXT("Pan"), TEXT("Tilt"), TEXT("Both") };
	static_assert(UE_ARRAY_COUNT(EffectAxisNames) == static_cast<int32>(EStageEffectAxis::Both) + 1, "Name every effect axis.");
	const TCHAR* const EffectColorModeNames[] = { TEXT("Crossfade"), TEXT("Rainbow") };
	static_assert(UE_ARRAY_COUNT(EffectColorModeNames) == static_cast<int32>(EStageEffectColorMode::Rainbow) + 1, "Name every colour mode.");
	const TCHAR* const EffectDirectionNames[] = { TEXT("Forward"), TEXT("Reverse"), TEXT("Bounce") };
	static_assert(UE_ARRAY_COUNT(EffectDirectionNames) == static_cast<int32>(EStageEffectDirection::Bounce) + 1, "Name every direction.");
	const TCHAR* const FanModeNames[] = { TEXT("Off"), TEXT("Linear"), TEXT("V"), TEXT("InvertedV") };
	static_assert(UE_ARRAY_COUNT(FanModeNames) == static_cast<int32>(EStageFanMode::InvertedV) + 1, "Name every fan mode.");

	TArray<TSharedPtr<FJsonValue>> WriteColor(const FLinearColor& Color)
	{
		TArray<TSharedPtr<FJsonValue>> Channels;
		Channels.Add(MakeShared<FJsonValueNumber>(Color.R));
		Channels.Add(MakeShared<FJsonValueNumber>(Color.G));
		Channels.Add(MakeShared<FJsonValueNumber>(Color.B));
		return Channels;
	}

	TArray<TSharedPtr<FJsonValue>> WriteEffects(const TArray<FStageEffectEntry>& Entries)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (const FStageEffectEntry& Entry : Entries)
		{
			const FStageEffect& Effect = Entry.Effect;
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(Field::Id, Entry.Id.ToString(EGuidFormats::DigitsWithHyphens));
			Object->SetStringField(Field::Name, Effect.Name.ToString());
			TArray<TSharedPtr<FJsonValue>> Fixtures;
			for (const int32 FixtureId : Entry.FixtureIds)
			{
				Fixtures.Add(MakeShared<FJsonValueNumber>(FixtureId));
			}
			Object->SetArrayField(Field::Fixtures, Fixtures);
			Object->SetStringField(EffectField::Target, EffectTargetNames[static_cast<int32>(Effect.Target)]);
			Object->SetStringField(EffectField::Waveform, EffectWaveformNames[static_cast<int32>(Effect.Waveform)]);
			if (Effect.Attribute.IsValid())
			{
				Object->SetStringField(EffectField::Attribute, Effect.Attribute.ToString());
			}
			Object->SetStringField(EffectField::Axis, EffectAxisNames[static_cast<int32>(Effect.Axis)]);
			Object->SetStringField(EffectField::ColorMode, EffectColorModeNames[static_cast<int32>(Effect.ColorMode)]);
			Object->SetArrayField(EffectField::ColorA, WriteColor(Effect.ColorA));
			Object->SetArrayField(EffectField::ColorB, WriteColor(Effect.ColorB));
			Object->SetNumberField(EffectField::Low, Effect.Low);
			Object->SetNumberField(EffectField::High, Effect.High);
			Object->SetNumberField(EffectField::Size, Effect.Size);
			Object->SetNumberField(EffectField::Rate, Effect.RateHz);
			Object->SetNumberField(EffectField::Duty, Effect.Duty);
			Object->SetNumberField(EffectField::Phase, Effect.Phase);
			Object->SetNumberField(EffectField::Spread, Effect.Spread);
			Object->SetNumberField(EffectField::Wings, Effect.Wings);
			Object->SetNumberField(EffectField::Blocks, Effect.Blocks);
			Object->SetStringField(EffectField::Direction, EffectDirectionNames[static_cast<int32>(Effect.Direction)]);
			Object->SetStringField(EffectField::SizeFan, FanModeNames[static_cast<int32>(Effect.SizeFan)]);
			Array.Add(MakeShared<FJsonValueObject>(Object));
		}
		return Array;
	}

	/** An enum field by name: absent keeps the default; an unknown name keeps it too and is counted. */
	template <typename EnumType>
	void ReadEffectEnum(const FJsonObject& Object, const TCHAR* Name, TConstArrayView<const TCHAR*> Names, EnumType& InOutValue, int32& InOutInvalid)
	{
		FString Text;
		if (!Object.TryGetStringField(Name, Text))
		{
			return;
		}
		const int32 Index = FindName(Names, Text);
		if (Index == INDEX_NONE)
		{
			++InOutInvalid;
			return;
		}
		InOutValue = static_cast<EnumType>(Index);
	}

	/** A number field: absent keeps the default; one that is not a finite number keeps it too and is counted (ranges: SanitizeEffect). */
	template <typename NumberType>
	void ReadEffectNumber(const FJsonObject& Object, const TCHAR* Name, NumberType& InOutValue, int32& InOutInvalid)
	{
		const TSharedPtr<FJsonValue> Value = Object.TryGetField(Name);
		if (!Value.IsValid())
		{
			return;
		}
		double Number = 0.0;
		if (!Value->TryGetNumber(Number) || !FMath::IsFinite(Number) || FMath::Abs(Number) > 1.0e6)
		{
			++InOutInvalid;
			return;
		}
		InOutValue = static_cast<NumberType>(Number);
	}

	void ReadEffectColor(const FJsonObject& Object, const TCHAR* Name, FLinearColor& InOutColor, int32& InOutInvalid)
	{
		const TArray<TSharedPtr<FJsonValue>>* Channels = nullptr;
		if (!Object.TryGetArrayField(Name, Channels))
		{
			return;
		}
		double Values[3] = { 0.0, 0.0, 0.0 };
		bool bValid = Channels->Num() == 3;
		for (int32 Index = 0; bValid && Index < 3; ++Index)
		{
			bValid = (*Channels)[Index].IsValid() && (*Channels)[Index]->TryGetNumber(Values[Index]) && FMath::IsFinite(Values[Index]);
		}
		if (!bValid)
		{
			++InOutInvalid;
			return;
		}
		InOutColor = FLinearColor(static_cast<float>(Values[0]), static_cast<float>(Values[1]), static_cast<float>(Values[2]));
	}

	/** One effect entry. False drops it (no valid id); bad settings inside fall back to their defaults and are counted. */
	bool ReadEffect(const FJsonObject& Object, FStageEffectEntry& OutEntry, int32& InOutInvalid)
	{
		FString Id;
		if (!Object.TryGetStringField(Field::Id, Id) || !FGuid::Parse(Id, OutEntry.Id) || !OutEntry.Id.IsValid())
		{
			return false;
		}
		FStageEffect& Effect = OutEntry.Effect;
		FString Name;
		Object.TryGetStringField(Field::Name, Name);
		Effect.Name = FText::FromString(Name);

		const TArray<TSharedPtr<FJsonValue>>* Fixtures = nullptr;
		if (Object.TryGetArrayField(Field::Fixtures, Fixtures))
		{
			for (const TSharedPtr<FJsonValue>& Fixture : *Fixtures)
			{
				double FixtureId = 0.0;
				if (OutEntry.FixtureIds.Num() >= StageEffectRules::MaxFixturesPerEffect || !Fixture.IsValid() || !Fixture->TryGetNumber(FixtureId)
					|| !FMath::IsFinite(FixtureId) || FixtureId < 1.0 || FixtureId > static_cast<double>(MAX_int32))
				{
					++InOutInvalid;
					continue;
				}
				OutEntry.FixtureIds.Add(static_cast<int32>(FixtureId));
			}
		}

		ReadEffectEnum(Object, EffectField::Target, EffectTargetNames, Effect.Target, InOutInvalid);
		ReadEffectEnum(Object, EffectField::Waveform, EffectWaveformNames, Effect.Waveform, InOutInvalid);
		ReadEffectEnum(Object, EffectField::Axis, EffectAxisNames, Effect.Axis, InOutInvalid);
		ReadEffectEnum(Object, EffectField::ColorMode, EffectColorModeNames, Effect.ColorMode, InOutInvalid);
		ReadEffectEnum(Object, EffectField::Direction, EffectDirectionNames, Effect.Direction, InOutInvalid);
		ReadEffectEnum(Object, EffectField::SizeFan, FanModeNames, Effect.SizeFan, InOutInvalid);
		FString Attribute;
		if (Object.TryGetStringField(EffectField::Attribute, Attribute))
		{
			Effect.Attribute = FGameplayTag::RequestGameplayTag(FName(*Attribute), /*ErrorIfNotFound=*/ false);
			if (!StageEffectRules::IsBeamAttribute(Effect.Attribute))
			{
				++InOutInvalid; // SanitizeEffect gives a Beam effect Zoom instead.
			}
		}
		ReadEffectColor(Object, EffectField::ColorA, Effect.ColorA, InOutInvalid);
		ReadEffectColor(Object, EffectField::ColorB, Effect.ColorB, InOutInvalid);
		ReadEffectNumber(Object, EffectField::Low, Effect.Low, InOutInvalid);
		ReadEffectNumber(Object, EffectField::High, Effect.High, InOutInvalid);
		ReadEffectNumber(Object, EffectField::Size, Effect.Size, InOutInvalid);
		ReadEffectNumber(Object, EffectField::Rate, Effect.RateHz, InOutInvalid);
		ReadEffectNumber(Object, EffectField::Duty, Effect.Duty, InOutInvalid);
		ReadEffectNumber(Object, EffectField::Phase, Effect.Phase, InOutInvalid);
		ReadEffectNumber(Object, EffectField::Spread, Effect.Spread, InOutInvalid);
		ReadEffectNumber(Object, EffectField::Wings, Effect.Wings, InOutInvalid);
		ReadEffectNumber(Object, EffectField::Blocks, Effect.Blocks, InOutInvalid);
		return true;
	}

	/** The effects in Name (a version 9 file), repaired by StageEffectRules (duplicates, bounds, ranges). */
	TArray<FStageEffectEntry> ReadEffects(const FJsonObject& Owner, const TCHAR* Name, int32& InOutInvalid)
	{
		TArray<FStageEffectEntry> Entries;
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Owner.TryGetArrayField(Name, Array))
		{
			return Entries;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FStageEffectEntry Entry;
			if (Entries.Num() >= StageEffectRules::MaxEffects || !Value.IsValid() || !Value->TryGetObject(Object) || !Object->IsValid()
				|| !ReadEffect(**Object, Entry, InOutInvalid))
			{
				++InOutInvalid;
				continue;
			}
			Entries.Add(MoveTemp(Entry));
		}
		InOutInvalid += StageEffectRules::SanitizeEntries(Entries);
		return Entries;
	}

	TSharedRef<FJsonObject> WriteEnvironment(const FStageEnvironmentSettings& Settings)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(Field::Lighting, StaticEnum<EStageLightingSource>()->GetNameStringByValue(int64(Settings.Lighting)));
		Object->SetNumberField(Field::TimeOfDay, Settings.TimeOfDay);
		Object->SetNumberField(Field::SunElevation, Settings.SunElevation);
		Object->SetNumberField(Field::SunAzimuth, Settings.SunAzimuth);
		Object->SetNumberField(Field::CloudCoverage, Settings.CloudCoverage);
		Object->SetNumberField(Field::Rain, Settings.Rain);
		Object->SetNumberField(Field::Snow, Settings.Snow);
		Object->SetNumberField(Field::Fog, Settings.Fog);

		const FStageEnvironmentLook& Look = Settings.Look;
		const TSharedRef<FJsonObject> LookObject = MakeShared<FJsonObject>();
		LookObject->SetNumberField(Field::Exposure, Look.ExposureCompensation);
		LookObject->SetNumberField(Field::Bloom, Look.BloomIntensity);
		LookObject->SetNumberField(Field::AmbientOcclusion, Look.AmbientOcclusionIntensity);
		LookObject->SetNumberField(Field::WhiteBalance, Look.WhiteBalanceTemp);
		LookObject->SetNumberField(Field::Saturation, Look.Saturation);
		LookObject->SetNumberField(Field::Contrast, Look.Contrast);
		LookObject->SetNumberField(Field::Vignette, Look.VignetteIntensity);
		LookObject->SetNumberField(Field::StudioKeyIntensity, Look.StudioKeyIntensity);
		LookObject->SetArrayField(Field::StudioKeyColor, {
			MakeShared<FJsonValueNumber>(Look.StudioKeyColor.R), MakeShared<FJsonValueNumber>(Look.StudioKeyColor.G), MakeShared<FJsonValueNumber>(Look.StudioKeyColor.B) });
		Object->SetObjectField(Field::Look, LookObject);
		return Object;
	}

	/**
	 * The environment of a version 2 file. A missing object or field keeps the default for that value (a hand-edited file may
	 * leave some out), and the result is sanitized like every environment write, so a file can never put the sky out of range.
	 */
	FStageEnvironmentSettings ReadEnvironment(const FJsonObject& Root)
	{
		FStageEnvironmentSettings Settings;
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Root.TryGetObjectField(Field::Environment, Object) || !Object->IsValid())
		{
			return Settings;
		}

		const FJsonObject& Environment = **Object;
		// Version 5 names the lighting source; older files only had the outdoor sky switch. An unknown name is the level's lighting.
		FString LightingName;
		bool bSkyEnabled = false;
		if (Environment.TryGetStringField(Field::Lighting, LightingName))
		{
			const int64 Source = StaticEnum<EStageLightingSource>()->GetValueByNameString(LightingName, EGetByNameFlags::None);
			Settings.Lighting = Source == INDEX_NONE ? EStageLightingSource::Level : EStageLightingSource(Source);
		}
		else if (Environment.TryGetBoolField(Field::SkyEnabled, bSkyEnabled))
		{
			Settings.Lighting = bSkyEnabled ? EStageLightingSource::OutdoorSky : EStageLightingSource::Level;
		}
		Environment.TryGetNumberField(Field::TimeOfDay, Settings.TimeOfDay);
		Environment.TryGetNumberField(Field::SunElevation, Settings.SunElevation);
		Environment.TryGetNumberField(Field::SunAzimuth, Settings.SunAzimuth);
		Environment.TryGetNumberField(Field::CloudCoverage, Settings.CloudCoverage);
		Environment.TryGetNumberField(Field::Rain, Settings.Rain);
		Environment.TryGetNumberField(Field::Snow, Settings.Snow);
		Environment.TryGetNumberField(Field::Fog, Settings.Fog);

		const TSharedPtr<FJsonObject>* LookObject = nullptr;
		if (Environment.TryGetObjectField(Field::Look, LookObject) && LookObject->IsValid())
		{
			const FJsonObject& Look = **LookObject;
			Look.TryGetNumberField(Field::Exposure, Settings.Look.ExposureCompensation);
			Look.TryGetNumberField(Field::Bloom, Settings.Look.BloomIntensity);
			Look.TryGetNumberField(Field::AmbientOcclusion, Settings.Look.AmbientOcclusionIntensity);
			Look.TryGetNumberField(Field::WhiteBalance, Settings.Look.WhiteBalanceTemp);
			Look.TryGetNumberField(Field::Saturation, Settings.Look.Saturation);
			Look.TryGetNumberField(Field::Contrast, Settings.Look.Contrast);
			Look.TryGetNumberField(Field::Vignette, Settings.Look.VignetteIntensity);
			Look.TryGetNumberField(Field::StudioKeyIntensity, Settings.Look.StudioKeyIntensity);
			const TArray<TSharedPtr<FJsonValue>>* Color = nullptr;
			if (Look.TryGetArrayField(Field::StudioKeyColor, Color) && Color->Num() == 3)
			{
				double Channels[3] = { 1.0, 1.0, 1.0 };
				for (int32 Index = 0; Index < 3; ++Index)
				{
					(*Color)[Index]->TryGetNumber(Channels[Index]);
				}
				Settings.Look.StudioKeyColor = FLinearColor(float(Channels[0]), float(Channels[1]), float(Channels[2]), 1.f);
			}
		}
		return StageEnvironment::Sanitize(Settings);
	}

	TArray<TSharedPtr<FJsonValue>> ToJsonArray(std::initializer_list<double> Values)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (const double Value : Values)
		{
			Array.Add(MakeShared<FJsonValueNumber>(Value));
		}
		return Array;
	}

	/** Exactly Count finite numbers, or false. */
	bool ReadNumbers(const FJsonObject& Object, const TCHAR* FieldName, int32 Count, double* OutValues)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Object.TryGetArrayField(FieldName, Array) || Array->Num() != Count)
		{
			return false;
		}
		for (int32 Index = 0; Index < Count; ++Index)
		{
			double Value = 0.0;
			if (!(*Array)[Index].IsValid() || !(*Array)[Index]->TryGetNumber(Value) || !FMath::IsFinite(Value))
			{
				return false;
			}
			OutValues[Index] = Value;
		}
		return true;
	}

	/** One item record, validated. False drops the item. */
	bool ParseItem(const FJsonObject& Object, FStageItemSnapshot& OutItem)
	{
		FString IdString;
		FString ItemPath;
		if (!Object.TryGetStringField(Field::Id, IdString) || !FGuid::Parse(IdString, OutItem.InstanceId) || !OutItem.InstanceId.IsValid())
		{
			return false;
		}

		if (!Object.TryGetStringField(Field::Item, ItemPath))
		{
			return false;
		}
		// Only catalog paths: content items live under /Game, imported models under the import root. Membership in the catalog is
		// checked when the scene is applied.
		const FSoftObjectPath Path(ItemPath);
		const FString PackageName = Path.GetLongPackageName();
		if (!Path.IsValid() || !(PackageName.StartsWith(TEXT("/Game/")) || StageMeshImport::IsImportedItemPath(PackageName)))
		{
			return false;
		}
		OutItem.Item = TSoftObjectPtr<UBaseItemData>(Path);

		double Location[3];
		double Rotation[4];
		double Scale[3];
		if (!ReadNumbers(Object, Field::Location, 3, Location) || !ReadNumbers(Object, Field::Rotation, 4, Rotation) || !ReadNumbers(Object, Field::Scale, 3, Scale))
		{
			return false;
		}

		const FVector LocationVector(Location[0], Location[1], Location[2]);
		if (LocationVector.GetAbsMax() > FStageSceneStore::MaxCoordinate)
		{
			return false;
		}

		FQuat Quat(Rotation[0], Rotation[1], Rotation[2], Rotation[3]);
		if (Quat.SizeSquared() < UE_KINDA_SMALL_NUMBER)
		{
			return false;
		}
		Quat.Normalize();

		OutItem.Transform = FTransform(Quat, LocationVector, StageTransformRules::ClampScale(FVector(Scale[0], Scale[1], Scale[2])));

		FString Label;
		Object.TryGetStringField(Field::Label, Label);
		OutItem.DisplayName = FText::FromString(Label.Left(FStageSceneStore::MaxLabelLength));

		FString State;
		if (Object.TryGetStringField(Field::State, State) && !State.IsEmpty())
		{
			// Base64 inflates by 4/3; refuse before decoding anything oversized.
			if (State.Len() > (FStageSceneStore::MaxStateBytes / 3 + 1) * 4 || !FBase64::Decode(State, OutItem.SavedProperties))
			{
				return false;
			}
		}
		return true;
	}
}

FStageSceneStore::FStageSceneStore(FString InDirectory)
	: Directory(MoveTemp(InDirectory))
{
}

FString FStageSceneStore::GetDefaultDirectory()
{
	return FPaths::Combine(StageCraftPaths::DataRoot(), TEXT("Scenes"));
}

bool FStageSceneStore::IsValidSceneName(const FString& Name)
{
	// One naming rule for every user file the app writes, so users learn it once.
	return FStageLayoutStore::IsValidLayoutName(Name);
}

FString FStageSceneStore::GetScenePath(const FString& Name) const
{
	return FPaths::Combine(Directory, Name + StageSceneStore::Extension);
}

TArray<FString> FStageSceneStore::ListScenes() const
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory, FString(TEXT("*")) + StageSceneStore::Extension), true, false);

	TArray<FString> Names;
	Names.Reserve(Files.Num());
	for (const FString& File : Files)
	{
		const FString Name = FPaths::GetBaseFilename(File);
		if (IsValidSceneName(Name))
		{
			Names.Add(Name);
		}
	}
	Names.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::IgnoreCase) < 0; });
	return Names;
}

bool FStageSceneStore::SceneExists(const FString& Name) const
{
	return IsValidSceneName(Name) && IFileManager::Get().FileExists(*GetScenePath(Name));
}

EStageSceneResult FStageSceneStore::Save(const FStageSceneDocument& Document) const
{
	if (!IsValidSceneName(Document.Name))
	{
		return EStageSceneResult::InvalidName;
	}
	IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
	return StageAtomicFile::WriteText(GetScenePath(Document.Name), Serialize(Document)) ? EStageSceneResult::Success : EStageSceneResult::WriteFailed;
}

EStageSceneResult FStageSceneStore::Load(const FString& Name, FStageSceneDocument& OutDocument, FStageSceneParseReport& OutReport) const
{
	if (!IsValidSceneName(Name))
	{
		return EStageSceneResult::InvalidName;
	}

	const FString Path = GetScenePath(Name);
	if (!IFileManager::Get().FileExists(*Path))
	{
		return EStageSceneResult::NotFound;
	}

	FString Contents;
	if (!FFileHelper::LoadFileToString(Contents, *Path))
	{
		return EStageSceneResult::Corrupt;
	}

	const EStageSceneResult Result = Parse(Contents, OutDocument, OutReport);
	OutDocument.Name = Name;
	return Result;
}

EStageSceneResult FStageSceneStore::Delete(const FString& Name) const
{
	if (!IsValidSceneName(Name))
	{
		return EStageSceneResult::InvalidName;
	}
	const FString Path = GetScenePath(Name);
	if (!IFileManager::Get().FileExists(*Path))
	{
		return EStageSceneResult::NotFound;
	}
	return IFileManager::Get().Delete(*Path, /*RequireExists*/ true, /*EvenReadOnly*/ false, /*Quiet*/ true) ? EStageSceneResult::Success : EStageSceneResult::WriteFailed;
}

bool FStageSceneStore::BackupScene(const FString& Name) const
{
	if (!SceneExists(Name))
	{
		return false;
	}
	const FString Path = GetScenePath(Name);
	return IFileManager::Get().Copy(*(Path + TEXT(".bak")), *Path, /*bReplace*/ true, /*bEvenIfReadOnly*/ true) == COPY_OK;
}

void FStageSceneStore::AppendUnplacedItems(FStageSceneDocument& Document, TConstArrayView<FStageItemSnapshot> Unplaced)
{
	TSet<FGuid> PlacedIds;
	PlacedIds.Reserve(Document.Items.Num());
	for (const FStageItemSnapshot& Item : Document.Items)
	{
		PlacedIds.Add(Item.InstanceId);
	}
	for (const FStageItemSnapshot& Item : Unplaced)
	{
		bool bAlreadyThere = false;
		PlacedIds.Add(Item.InstanceId, &bAlreadyThere);
		if (!bAlreadyThere)
		{
			Document.Items.Add(Item);
		}
	}
}

FString FStageSceneStore::Serialize(const FStageSceneDocument& Document)
{
	using namespace StageSceneStore;

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(Document.Items.Num());
	for (const FStageItemSnapshot& Item : Document.Items)
	{
		const FVector Location = Item.Transform.GetLocation();
		const FQuat Rotation = Item.Transform.GetRotation();
		const FVector Scale = Item.Transform.GetScale3D();

		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(Field::Id, Item.InstanceId.ToString(EGuidFormats::DigitsWithHyphens));
		Object->SetStringField(Field::Item, Item.Item.ToSoftObjectPath().ToString());
		Object->SetStringField(Field::Label, Item.DisplayName.ToString());
		Object->SetArrayField(Field::Location, ToJsonArray({ Location.X, Location.Y, Location.Z }));
		// A quaternion round-trips exactly; Euler angles would not near the poles.
		Object->SetArrayField(Field::Rotation, ToJsonArray({ Rotation.X, Rotation.Y, Rotation.Z, Rotation.W }));
		Object->SetArrayField(Field::Scale, ToJsonArray({ Scale.X, Scale.Y, Scale.Z }));
		Object->SetStringField(Field::State, FBase64::Encode(Item.SavedProperties));
		Items.Add(MakeShared<FJsonValueObject>(Object));
	}

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(Field::Format, FormatTag);
	Root->SetNumberField(Field::Version, FormatVersion);
	Root->SetStringField(Field::Name, Document.Name);
	Root->SetStringField(Field::Level, Document.LevelName);
	Root->SetStringField(Field::SavedAt, Document.SavedAtUtc.ToIso8601());
	Root->SetArrayField(Field::Items, Items);
	Root->SetObjectField(Field::Environment, WriteEnvironment(Document.Environment));
	Root->SetArrayField(Field::Cues, WriteCues(Document.Cues));
	TArray<TSharedPtr<FJsonValue>> Programmer;
	for (const FShowCueFixtureState& State : Document.Programmer)
	{
		Programmer.Add(WriteFixtureState(State));
	}
	Root->SetArrayField(Field::Programmer, Programmer);
	Root->SetArrayField(EffectField::ProgrammerEffects, WriteEffects(Document.ProgrammerEffects));
	Root->SetArrayField(Field::ProgrammerEffectStops, WriteEffectStops(Document.ProgrammerEffectStops));
	Root->SetArrayField(Field::Groups, WriteGroups(Document.Groups));
	Root->SetArrayField(Field::Palettes, WritePalettes(Document.Palettes));
	Root->SetArrayField(Field::Playbacks, WritePlaybacks(Document.Playbacks));
	if (Document.ActiveCueNumber.IsSet())
	{
		Root->SetNumberField(Field::ActiveCue, Document.ActiveCueNumber.GetValue());
	}
	// Only when set: the user's own stages stay free of it.
	if (Document.OriginListingId.IsValid())
	{
		Root->SetStringField(Field::OriginListingId, Document.OriginListingId.ToString(EGuidFormats::DigitsWithHyphens));
	}

	FString Contents;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Contents);
	FJsonSerializer::Serialize(Root, Writer);
	return Contents;
}

EStageSceneResult FStageSceneStore::Parse(const FString& Contents, FStageSceneDocument& OutDocument, FStageSceneParseReport& OutReport)
{
	using namespace StageSceneStore;

	OutDocument = FStageSceneDocument();
	OutReport = FStageSceneParseReport();

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Contents);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return EStageSceneResult::Corrupt;
	}

	FString Format;
	int32 Version = 0;
	if (!Root->TryGetStringField(Field::Format, Format) || Format != FormatTag || !Root->TryGetNumberField(Field::Version, Version))
	{
		return EStageSceneResult::Corrupt;
	}
	// A newer file may hold data this build would silently drop on the next save, so it is refused rather than half-read.
	if (Version < MinReadableVersion || Version > FormatVersion)
	{
		return EStageSceneResult::VersionMismatch;
	}

	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!Root->TryGetArrayField(Field::Items, Items) || Items->Num() > MaxItems)
	{
		return EStageSceneResult::Corrupt;
	}

	Root->TryGetStringField(Field::Name, OutDocument.Name);
	Root->TryGetStringField(Field::Level, OutDocument.LevelName);
	FString SavedAt;
	if (Root->TryGetStringField(Field::SavedAt, SavedAt))
	{
		FDateTime::ParseIso8601(*SavedAt, OutDocument.SavedAtUtc);
	}

	// Migration 1 -> 2: a version 1 stage had no saved environment; it opens with the default one (sky off, the indoor stage).
	OutDocument.Environment = Version >= 2 ? ReadEnvironment(*Root) : FStageEnvironmentSettings();
	// Migration 2 -> 3: older stages had no saved cues; they open with an empty cue list.
	if (Version >= 3)
	{
		OutDocument.Cues = ReadCues(*Root, Version, MaxCues, OutReport.InvalidCueEntries);
	}
	// Migration 5 -> 6: older stages have an empty programmer, no groups and no cue playing.
	if (Version >= 6)
	{
		OutDocument.Programmer = ReadProgrammer(*Root, OutReport.InvalidShowEntries);
		OutDocument.Groups = ReadGroups(*Root, OutReport.InvalidShowEntries);
		double ActiveCue = 0.0;
		if (Root->TryGetNumberField(Field::ActiveCue, ActiveCue))
		{
			const float Number = static_cast<float>(ActiveCue);
			if (StageCueRules::IsValidCueNumber(Number))
			{
				OutDocument.ActiveCueNumber = StageCueRules::RoundCueNumber(Number);
			}
			else
			{
				++OutReport.InvalidShowEntries;
			}
		}
	}
	// Migration 8 -> 9: older stages have no effects running.
	if (Version >= 9)
	{
		OutDocument.ProgrammerEffects = ReadEffects(*Root, EffectField::ProgrammerEffects, OutReport.InvalidShowEntries);
	}
	// Migration 9 -> 10: no stops in the programmer.
	if (Version >= 10)
	{
		OutDocument.ProgrammerEffectStops = ReadEffectStops(*Root, Field::ProgrammerEffectStops, OutDocument.ProgrammerEffects, OutReport.InvalidShowEntries);
	}
	// Migration 7 -> 8: older stages have empty playbacks.
	if (Version >= 8)
	{
		OutDocument.Playbacks = ReadPlaybacks(*Root, Version, OutReport.InvalidCueEntries);
	}
	else
	{
		OutDocument.Playbacks.SetNum(StagePlaybackRules::Count);
	}
	// Migration 6 -> 7: older stages (and every starter stage) get the starter palettes; a version 7 file has exactly its own.
	OutDocument.Palettes = Version >= 7 ? ReadPalettes(*Root, OutReport.InvalidShowEntries) : StagePaletteRules::MakeStarterPalettes();
	// Migration 3 -> 4: older stages are the user's own. Deny by default: an origin that is there but unreadable is not "own".
	FString Origin;
	if (Version >= 4 && Root->TryGetStringField(Field::OriginListingId, Origin)
		&& (!FGuid::Parse(Origin, OutDocument.OriginListingId) || !OutDocument.OriginListingId.IsValid()))
	{
		return EStageSceneResult::Corrupt;
	}

	TSet<FGuid> SeenIds;
	SeenIds.Reserve(Items->Num());
	OutDocument.Items.Reserve(Items->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Items)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		FStageItemSnapshot Item;
		if (!Value.IsValid() || !Value->TryGetObject(Object) || !Object->IsValid() || !ParseItem(**Object, Item))
		{
			++OutReport.InvalidItems;
			continue;
		}

		// Two items with one id would make undo and later saves ambiguous; the later copy gets its own identity.
		bool bAlreadySeen = false;
		SeenIds.Add(Item.InstanceId, &bAlreadySeen);
		if (bAlreadySeen)
		{
			Item.InstanceId = FGuid::NewGuid();
			SeenIds.Add(Item.InstanceId);
			++OutReport.RepairedIds;
		}
		OutDocument.Items.Add(MoveTemp(Item));
	}
	return EStageSceneResult::Success;
}
