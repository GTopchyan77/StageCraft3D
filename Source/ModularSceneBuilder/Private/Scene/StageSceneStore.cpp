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
	}

	/** One fixture's attribute values: {"id": fixtureId, "attributes": {"<tag>": value}} (cues and the programmer). */
	TSharedPtr<FJsonValue> WriteFixtureState(const FShowCueFixtureState& State)
	{
		// Tags by name: a name stays readable and survives tag table changes; unknown ones are dropped on load.
		const TSharedRef<FJsonObject> Attributes = MakeShared<FJsonObject>();
		for (const TPair<FGameplayTag, float>& Pair : State.Attributes)
		{
			Attributes->SetNumberField(Pair.Key.ToString(), Pair.Value);
		}
		const TSharedRef<FJsonObject> Fixture = MakeShared<FJsonObject>();
		Fixture->SetNumberField(Field::Id, State.FixtureId);
		Fixture->SetObjectField(Field::Attributes, Attributes);
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
			Object->SetArrayField(Field::Fixtures, Fixtures);
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

		int32 Read = 0;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Attributes)->Values)
		{
			double Value = 0.0;
			// ErrorIfNotFound = false: a tag this build does not know (removed attribute, hand edit) is skipped, not fatal.
			const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*Pair.Key), /*ErrorIfNotFound*/ false);
			if (Read >= FStageSceneStore::MaxAttributesPerFixture || !Tag.IsValid() || !Pair.Value.IsValid() || !Pair.Value->TryGetNumber(Value) || !FMath::IsFinite(Value))
			{
				++InOutInvalid;
				continue;
			}
			OutState.Attributes.Add(Tag, static_cast<float>(Value));
			++Read;
		}
		return true;
	}

	/** One cue. False drops it (bad number); bad fixtures inside it are dropped and counted. */
	bool ReadCue(const FJsonObject& Object, FShowCue& OutCue, int32& InOutInvalid)
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
		return true;
	}

	/**
	 * The cue list of a version 3 file. Tolerant like ReadEnvironment: what cannot be read is dropped and counted, the rest loads,
	 * and the list is repaired by the same rules as every other cue entry (duplicates, order).
	 */
	TArray<FShowCue> ReadCues(const FJsonObject& Root, int32& InOutInvalid)
	{
		TArray<FShowCue> Cues;
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Root.TryGetArrayField(Field::Cues, Array))
		{
			return Cues;
		}
		Cues.Reserve(FMath::Min(Array->Num(), FStageSceneStore::MaxCues));
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FShowCue Cue;
			if (Cues.Num() >= FStageSceneStore::MaxCues || !Value.IsValid() || !Value->TryGetObject(Object) || !Object->IsValid()
				|| !ReadCue(**Object, Cue, InOutInvalid))
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
	Root->SetArrayField(Field::Groups, WriteGroups(Document.Groups));
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
		OutDocument.Cues = ReadCues(*Root, OutReport.InvalidCueEntries);
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
