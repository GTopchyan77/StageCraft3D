// Copyright Epic Games, Inc. All Rights Reserved.

#include "Fixtures/StageFixtureLibraryRules.h"

#include "Dom/JsonObject.h"
#include "IO/StageCraftPaths.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#define LOCTEXT_NAMESPACE "StageFixtureLibrary"

namespace StageFixtureLibrary
{
	const TCHAR* const PackageRoot = TEXT("/Temp/StageCraftFixtures/");
	const FName GdtfFormat(TEXT("GDTF"));

	namespace
	{
		const TCHAR* const IndexFormatTag = TEXT("StageCraftFixtureLibrary");
		constexpr int32 IndexVersion = 1;
		constexpr int32 GuidDigits = 32;

		bool IsSlugChar(TCHAR Char)
		{
			return (Char >= TEXT('A') && Char <= TEXT('Z')) || (Char >= TEXT('a') && Char <= TEXT('z')) || (Char >= TEXT('0') && Char <= TEXT('9')) || Char == TEXT('_');
		}

		bool IsHexDigits(FStringView Text)
		{
			if (Text.Len() != GuidDigits)
			{
				return false;
			}
			for (const TCHAR Char : Text)
			{
				if (!FChar::IsHexDigit(Char))
				{
					return false;
				}
			}
			return true;
		}

		FString MakeSlug(const FString& Name)
		{
			FString Slug;
			Slug.Reserve(FMath::Min(Name.Len(), MaxSlugLength));
			bool bPendingSeparator = false;
			for (const TCHAR Char : Name)
			{
				if (IsSlugChar(Char) && Char != TEXT('_'))
				{
					if (bPendingSeparator && !Slug.IsEmpty())
					{
						Slug.AppendChar(TEXT('_'));
					}
					bPendingSeparator = false;
					Slug.AppendChar(Char);
				}
				else
				{
					bPendingSeparator = true;
				}
				if (Slug.Len() >= MaxSlugLength)
				{
					break;
				}
			}
			Slug.LeftInline(MaxSlugLength);
			return Slug.IsEmpty() ? FString(TEXT("Mode")) : Slug;
		}

		FString Clip(const FString& Text)
		{
			return Text.Left(MaxRefTextLength);
		}
	}

	FString GetLibraryDirectory()
	{
		return FPaths::Combine(StageCraftPaths::DataRoot(), TEXT("Fixtures"));
	}

	FString GetIndexPath()
	{
		return FPaths::Combine(GetLibraryDirectory(), TEXT("Fixtures.json"));
	}

	FString GetLibraryFilePath(const FGuid& FixtureTypeId)
	{
		return FPaths::Combine(GetLibraryDirectory(), FixtureTypeId.ToString(EGuidFormats::Digits) + TEXT(".gdtf"));
	}

	bool ParseLibraryFileName(const FString& CleanFileName, FGuid& OutFixtureTypeId)
	{
		if (!FPaths::GetExtension(CleanFileName).Equals(TEXT("gdtf"), ESearchCase::IgnoreCase))
		{
			return false;
		}
		const FString Base = FPaths::GetBaseFilename(CleanFileName);
		return IsHexDigits(Base) && FGuid::ParseExact(Base, EGuidFormats::Digits, OutFixtureTypeId) && OutFixtureTypeId.IsValid();
	}

	TArray<FString> MakeModeSlugs(TConstArrayView<FString> ModeNames)
	{
		TArray<FString> Slugs;
		Slugs.Reserve(ModeNames.Num());
		TSet<FString> Used;
		for (const FString& Name : ModeNames)
		{
			const FString Base = MakeSlug(Name);
			FString Slug = Base;
			for (int32 Suffix = 2; Used.Contains(Slug.ToLower()); ++Suffix)
			{
				const FString Tail = FString::Printf(TEXT("_%d"), Suffix);
				Slug = Base.Left(MaxSlugLength - Tail.Len()) + Tail;
			}
			Used.Add(Slug.ToLower());
			Slugs.Add(MoveTemp(Slug));
		}
		return Slugs;
	}

	FString MakeTypeFolder(const FGuid& FixtureTypeId)
	{
		return FString(PackageRoot) + FixtureTypeId.ToString(EGuidFormats::Digits);
	}

	FSoftObjectPath MakeItemPath(const FGuid& FixtureTypeId, const FString& Slug)
	{
		return FSoftObjectPath(FString::Printf(TEXT("%s/%s.%s"), *MakeTypeFolder(FixtureTypeId), *Slug, *Slug));
	}

	bool IsFixtureItemPath(const FString& PackageName)
	{
		if (!PackageName.StartsWith(PackageRoot, ESearchCase::CaseSensitive))
		{
			return false;
		}
		const FStringView Rest = FStringView(PackageName).RightChop(FCString::Strlen(PackageRoot));
		int32 Slash = INDEX_NONE;
		if (!Rest.FindChar(TEXT('/'), Slash) || !IsHexDigits(Rest.Left(Slash)))
		{
			return false;
		}
		const FStringView Slug = Rest.RightChop(Slash + 1);
		if (Slug.IsEmpty() || Slug.Len() > MaxSlugLength)
		{
			return false;
		}
		for (const TCHAR Char : Slug)
		{
			if (!IsSlugChar(Char))
			{
				return false;
			}
		}
		return true;
	}

	bool ParseFixtureItemPath(const FString& PackageName, FGuid& OutFixtureTypeId)
	{
		OutFixtureTypeId.Invalidate();
		if (!IsFixtureItemPath(PackageName))
		{
			return false;
		}
		const FString Digits = PackageName.Mid(FCString::Strlen(PackageRoot), GuidDigits);
		return FGuid::ParseExact(Digits, EGuidFormats::Digits, OutFixtureTypeId) && OutFixtureTypeId.IsValid();
	}

	FStageFixtureTypeRef MakeTypeRef(const ULightingFixtureData& Fixture)
	{
		FStageFixtureTypeRef Ref;
		if (!Fixture.IsLibraryType())
		{
			return Ref;
		}
		Ref.Id = Fixture.FixtureTypeId;
		Ref.Manufacturer = Clip(Fixture.Specs.Manufacturer.ToString());
		Ref.Name = Clip(Fixture.SourceTypeName);
		Ref.Mode = Clip(Fixture.DMXModeName.ToString());
		return Ref;
	}

	FText DescribeMissingTypes(TConstArrayView<FStageItemSnapshot> Unplaced, int32 MaxListed)
	{
		struct FMissing
		{
			const FStageFixtureTypeRef* Ref = nullptr;
			int32 Count = 0;
		};
		TArray<FMissing> Missing;
		for (const FStageItemSnapshot& Item : Unplaced)
		{
			if (!Item.FixtureType.IsSet())
			{
				continue;
			}
			FMissing* Found = Missing.FindByPredicate([&Item](const FMissing& Entry)
			{
				return Entry.Ref->Id == Item.FixtureType.Id && Entry.Ref->Mode.Equals(Item.FixtureType.Mode, ESearchCase::CaseSensitive);
			});
			if (Found)
			{
				++Found->Count;
			}
			else
			{
				Missing.Add({ &Item.FixtureType, 1 });
			}
		}
		if (Missing.IsEmpty())
		{
			return FText::GetEmpty();
		}

		TArray<FText> Sentences;
		const int32 Listed = FMath::Min(Missing.Num(), FMath::Max(MaxListed, 1));
		for (int32 Index = 0; Index < Listed; ++Index)
		{
			const FStageFixtureTypeRef& Ref = *Missing[Index].Ref;
			const FString Name = FString::Join(TArray<FString>({ Ref.Manufacturer, Ref.Name }).FilterByPredicate([](const FString& Part) { return !Part.IsEmpty(); }), TEXT(" "));
			const FText Type = Ref.Mode.IsEmpty()
				? FText::FromString(Name.IsEmpty() ? Ref.Id.ToString(EGuidFormats::DigitsWithHyphens) : Name)
				: FText::Format(LOCTEXT("TypeWithMode", "{0} ({1})"), FText::FromString(Name.IsEmpty() ? Ref.Id.ToString(EGuidFormats::DigitsWithHyphens) : Name), FText::FromString(Ref.Mode));
			Sentences.Add(Missing[Index].Count == 1
				? FText::Format(LOCTEXT("MissingTypeOne", "{0} is missing. Import its GDTF file to see this fixture."), Type)
				: FText::Format(LOCTEXT("MissingTypeMany", "{0} is missing. Import its GDTF file to see these {1} fixtures."), Type, FText::AsNumber(Missing[Index].Count)));
		}
		if (Missing.Num() > Listed)
		{
			const int32 More = Missing.Num() - Listed;
			Sentences.Add(More == 1 ? LOCTEXT("OneMoreMissingType", "One more fixture type is missing.")
				: FText::Format(LOCTEXT("MoreMissingTypes", "{0} more fixture types are missing."), FText::AsNumber(More)));
		}
		return FText::Join(FText::FromString(TEXT(" ")), Sentences);
	}

	FString WriteIndex(TConstArrayView<FIndexEntry> Entries)
	{
		TArray<TSharedPtr<FJsonValue>> Fixtures;
		Fixtures.Reserve(Entries.Num());
		for (const FIndexEntry& Entry : Entries)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("id"), Entry.FixtureTypeId.ToString(EGuidFormats::DigitsWithHyphens));
			Object->SetStringField(TEXT("sourceFile"), Entry.SourceFileName);
			Object->SetStringField(TEXT("importedUtc"), Entry.ImportedUtc.ToIso8601());
			if (Entry.Kind.IsSet())
			{
				Object->SetStringField(TEXT("kind"), StaticEnum<EStageFixtureKind>()->GetNameStringByValue(static_cast<int64>(Entry.Kind.GetValue())));
			}
			Fixtures.Add(MakeShared<FJsonValueObject>(Object));
		}

		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("format"), IndexFormatTag);
		Root->SetNumberField(TEXT("version"), IndexVersion);
		Root->SetArrayField(TEXT("fixtures"), Fixtures);

		FString Json;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		FJsonSerializer::Serialize(Root, Writer);
		return Json;
	}

	bool ReadIndex(const FString& Json, TArray<FIndexEntry>& OutEntries, int32& OutSkipped)
	{
		OutEntries.Reset();
		OutSkipped = 0;
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FString Format;
		int32 Version = 0;
		const TArray<TSharedPtr<FJsonValue>>* Fixtures = nullptr;
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid() || !Root->TryGetStringField(TEXT("format"), Format) || Format != IndexFormatTag
			|| !Root->TryGetNumberField(TEXT("version"), Version) || Version != IndexVersion || !Root->TryGetArrayField(TEXT("fixtures"), Fixtures))
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Fixtures)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FIndexEntry Entry;
			FString Id;
			if (OutEntries.Num() >= MaxTypes || !Value.IsValid() || !Value->TryGetObject(Object) || !(*Object)->TryGetStringField(TEXT("id"), Id)
				|| !FGuid::Parse(Id, Entry.FixtureTypeId) || !Entry.FixtureTypeId.IsValid())
			{
				++OutSkipped;
				continue;
			}
			(*Object)->TryGetStringField(TEXT("sourceFile"), Entry.SourceFileName);
			Entry.SourceFileName = FPaths::GetCleanFilename(Entry.SourceFileName).Left(MaxRefTextLength);
			FString Imported;
			if ((*Object)->TryGetStringField(TEXT("importedUtc"), Imported))
			{
				FDateTime::ParseIso8601(*Imported, Entry.ImportedUtc);
			}
			FString Kind;
			if ((*Object)->TryGetStringField(TEXT("kind"), Kind))
			{
				const int64 KindValue = StaticEnum<EStageFixtureKind>()->GetValueByNameString(Kind, EGetByNameFlags::CaseSensitive);
				if (KindValue == INDEX_NONE)
				{
					++OutSkipped;
					continue;
				}
				Entry.Kind = static_cast<EStageFixtureKind>(KindValue);
			}
			if (OutEntries.ContainsByPredicate([&Entry](const FIndexEntry& Existing) { return Existing.FixtureTypeId == Entry.FixtureTypeId; }))
			{
				++OutSkipped;
				continue;
			}
			OutEntries.Add(MoveTemp(Entry));
		}
		return true;
	}
}

#undef LOCTEXT_NAMESPACE
