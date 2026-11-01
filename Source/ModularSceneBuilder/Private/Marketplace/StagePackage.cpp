// Copyright Epic Games, Inc. All Rights Reserved.

#include "Marketplace/StagePackage.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Import/StageMeshImportTypes.h"
#include "Memory/MemoryView.h"
#include "Misc/Compression.h"
#include "Misc/SecureHash.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

namespace StagePackageFormat
{
	constexpr uint8 Magic[8] = { 'S', 'T', 'A', 'G', 'E', 'P', 'K', 'G' };

	constexpr uint32 MakeTag(char A, char B, char C, char D)
	{
		return uint32(uint8(A)) | (uint32(uint8(B)) << 8) | (uint32(uint8(C)) << 16) | (uint32(uint8(D)) << 24);
	}

	constexpr uint32 ManifestTag = MakeTag('M', 'N', 'F', 'T');
	constexpr uint32 SceneTag = MakeTag('S', 'C', 'E', 'N');
	constexpr uint32 ThumbnailTag = MakeTag('T', 'H', 'M', 'B');

	enum class ECodec : uint32
	{
		Stored = 0,
		Zlib = 1,
	};

	constexpr int64 FixedHeaderBytes = sizeof(Magic) + sizeof(uint32) + sizeof(uint32);
	constexpr int64 SectionHeaderBytes = sizeof(uint32) + sizeof(uint32) + sizeof(uint64) + sizeof(uint64) + sizeof(FSHAHash::Hash);

	const TCHAR* FormatTag = TEXT("StageCraftPackage");
	const TCHAR* AnonymousAuthor = TEXT("Anonymous");

	namespace Field
	{
		const TCHAR* Format = TEXT("format");
		const TCHAR* Version = TEXT("version");
		const TCHAR* Id = TEXT("id");
		const TCHAR* Title = TEXT("title");
		const TCHAR* Description = TEXT("description");
		const TCHAR* Author = TEXT("author");
		const TCHAR* Kind = TEXT("kind");
		const TCHAR* Category = TEXT("category");
		const TCHAR* Keywords = TEXT("keywords");
		const TCHAR* Price = TEXT("price");
		const TCHAR* Credits = TEXT("credits");
		const TCHAR* RealMoney = TEXT("realMoney");
		const TCHAR* Amount = TEXT("amount");
		const TCHAR* Currency = TEXT("currency");
		const TCHAR* CreatedAt = TEXT("createdAt");
		const TCHAR* SceneVersion = TEXT("sceneVersion");
		const TCHAR* Stats = TEXT("stats");
		const TCHAR* Items = TEXT("items");
		const TCHAR* Fixtures = TEXT("fixtures");
		const TCHAR* Cues = TEXT("cues");
		const TCHAR* RequiredItems = TEXT("requiredItems");
	}

	struct FSectionHeader
	{
		uint32 Tag = 0;
		uint32 Codec = 0;
		uint64 StoredSize = 0;
		uint64 RawSize = 0;
		FSHAHash Hash;
	};

	struct FSectionView
	{
		FSectionHeader Header;
		/** Where the payload starts in the package. */
		int64 Offset = 0;
	};

	/** The raw-size cap of a known section; unknown sections are never inflated. */
	int64 GetMaxRawSize(uint32 Tag)
	{
		switch (Tag)
		{
		case ManifestTag: return StagePackage::MaxManifestBytes;
		case SceneTag: return StagePackage::MaxSceneBytes;
		case ThumbnailTag: return StagePackage::MaxThumbnailBytes;
		default: return 0;
		}
	}

	TArray<uint8> ToUtf8(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}

	FString FromUtf8(TConstArrayView<uint8> Bytes)
	{
		const FUTF8ToTCHAR Text(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
		return FString::ConstructFromPtrSize(Text.Get(), Text.Length());
	}

	/** Zlib when it saves space, stored otherwise (a PNG is already compressed). */
	ECodec Compress(TConstArrayView<uint8> Raw, TArray<uint8>& OutStored)
	{
		int64 Bound = 0;
		if (Raw.Num() > 0 && FCompression::CompressMemoryBound(NAME_Zlib, Bound, Raw.Num()) && Bound > 0)
		{
			OutStored.SetNumUninitialized(static_cast<int32>(Bound));
			int64 CompressedSize = Bound;
			if (FCompression::CompressMemory(NAME_Zlib, OutStored.GetData(), CompressedSize, Raw.GetData(), Raw.Num()) && CompressedSize < Raw.Num())
			{
				OutStored.SetNum(static_cast<int32>(CompressedSize));
				return ECodec::Zlib;
			}
		}
		OutStored = TArray<uint8>(Raw.GetData(), Raw.Num());
		return ECodec::Stored;
	}

	/** Reads the container header and section table; checks counts, duplicates, bounds and trailing bytes. Inflates nothing. */
	EStageMarketplaceResult ReadDirectory(TConstArrayView<uint8> Bytes, TArray<FSectionView>& OutSections)
	{
		OutSections.Reset();
		if (Bytes.Num() > StagePackage::MaxPackageBytes)
		{
			return EStageMarketplaceResult::TooLarge;
		}
		if (Bytes.Num() < FixedHeaderBytes)
		{
			return EStageMarketplaceResult::Corrupt;
		}

		FMemoryReaderView Reader(MakeMemoryView(Bytes.GetData(), Bytes.Num()));
		uint8 FileMagic[sizeof(Magic)] = {};
		Reader.Serialize(FileMagic, sizeof(FileMagic));
		if (FMemory::Memcmp(FileMagic, Magic, sizeof(Magic)) != 0)
		{
			return EStageMarketplaceResult::Corrupt;
		}

		uint32 Version = 0;
		uint32 Count = 0;
		Reader << Version;
		Reader << Count;
		if (Reader.IsError())
		{
			return EStageMarketplaceResult::Corrupt;
		}
		if (Version != StagePackage::ContainerVersion)
		{
			return EStageMarketplaceResult::VersionMismatch;
		}
		if (Count == 0 || Count > static_cast<uint32>(StagePackage::MaxSections))
		{
			return EStageMarketplaceResult::Corrupt;
		}

		int64 Offset = FixedHeaderBytes + static_cast<int64>(Count) * SectionHeaderBytes;
		const int64 Total = Bytes.Num();
		for (uint32 Index = 0; Index < Count; ++Index)
		{
			FSectionView Section;
			Reader << Section.Header.Tag;
			Reader << Section.Header.Codec;
			Reader << Section.Header.StoredSize;
			Reader << Section.Header.RawSize;
			Reader.Serialize(Section.Header.Hash.Hash, sizeof(Section.Header.Hash.Hash));
			if (Reader.IsError())
			{
				return EStageMarketplaceResult::Corrupt;
			}
			// Compared as unsigned before any signed arithmetic, so a huge size cannot wrap the bounds check.
			if (Offset > Total || Section.Header.StoredSize > static_cast<uint64>(Total - Offset))
			{
				return EStageMarketplaceResult::Corrupt;
			}
			if (OutSections.ContainsByPredicate([Tag = Section.Header.Tag](const FSectionView& Other) { return Other.Header.Tag == Tag; }))
			{
				return EStageMarketplaceResult::Corrupt;
			}
			Section.Offset = Offset;
			Offset += static_cast<int64>(Section.Header.StoredSize);
			OutSections.Add(Section);
		}
		// Bytes after the last section mean the file is not what its table says.
		return Offset == Total ? EStageMarketplaceResult::Success : EStageMarketplaceResult::Corrupt;
	}

	const FSectionView* FindSection(const TArray<FSectionView>& Sections, uint32 Tag)
	{
		return Sections.FindByPredicate([Tag](const FSectionView& Section) { return Section.Header.Tag == Tag; });
	}

	/** Inflates and verifies one section: size cap before allocating, codec, exact size, hash. */
	EStageMarketplaceResult ReadSection(TConstArrayView<uint8> Bytes, const FSectionView& Section, TArray<uint8>& OutRaw)
	{
		OutRaw.Reset();
		const FSectionHeader& Header = Section.Header;
		if (Header.RawSize > static_cast<uint64>(GetMaxRawSize(Header.Tag)))
		{
			return EStageMarketplaceResult::TooLarge;
		}

		const uint8* Stored = Bytes.GetData() + Section.Offset;
		const int64 StoredSize = static_cast<int64>(Header.StoredSize);
		const int64 RawSize = static_cast<int64>(Header.RawSize);
		switch (static_cast<ECodec>(Header.Codec))
		{
		case ECodec::Stored:
			if (StoredSize != RawSize)
			{
				return EStageMarketplaceResult::Corrupt;
			}
			OutRaw = TArray<uint8>(Stored, static_cast<int32>(RawSize));
			break;
		case ECodec::Zlib:
			OutRaw.SetNumUninitialized(static_cast<int32>(RawSize));
			// The engine's zlib path fails unless exactly RawSize bytes come out.
			if (RawSize == 0 || !FCompression::UncompressMemory(NAME_Zlib, OutRaw.GetData(), RawSize, Stored, StoredSize))
			{
				OutRaw.Reset();
				return EStageMarketplaceResult::Corrupt;
			}
			break;
		default:
			return EStageMarketplaceResult::Corrupt;
		}

		FSHAHash Hash;
		FSHA1::HashBuffer(OutRaw.GetData(), OutRaw.Num(), Hash.Hash);
		if (!(Hash == Header.Hash))
		{
			OutRaw.Reset();
			return EStageMarketplaceResult::HashMismatch;
		}
		return EStageMarketplaceResult::Success;
	}

	/** Removes control characters; keeps line breaks and tabs when bMultiLine. */
	FString CleanText(const FString& Text, bool bMultiLine)
	{
		FString Clean;
		Clean.Reserve(Text.Len());
		for (const TCHAR Char : Text)
		{
			const bool bControl = Char < 32 || Char == 127;
			if (!bControl || (bMultiLine && (Char == TEXT('\n') || Char == TEXT('\t'))))
			{
				Clean.AppendChar(Char);
			}
		}
		return Clean.TrimStartAndEnd();
	}

	bool IsCurrencyCode(const FString& Code)
	{
		if (Code.Len() != 3)
		{
			return false;
		}
		for (const TCHAR Char : Code)
		{
			if (Char < TEXT('A') || Char > TEXT('Z'))
			{
				return false;
			}
		}
		return true;
	}

	bool ReadStringArray(const FJsonObject& Object, const TCHAR* FieldName, TArray<FString>& OutStrings)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Object.TryGetArrayField(FieldName, Array))
		{
			return false;
		}
		OutStrings.Reset(Array->Num());
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			FString String;
			if (!Value.IsValid() || !Value->TryGetString(String))
			{
				return false;
			}
			OutStrings.Add(MoveTemp(String));
		}
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> ToJsonStrings(const TArray<FString>& Strings)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		Array.Reserve(Strings.Num());
		for (const FString& String : Strings)
		{
			Array.Add(MakeShared<FJsonValueString>(String));
		}
		return Array;
	}
}

void StagePackage::NormalizeManifest(FStagePackageManifest& Manifest)
{
	using namespace StagePackageFormat;

	Manifest.Title = CleanText(Manifest.Title, /*bMultiLine*/ false);
	Manifest.Description = CleanText(Manifest.Description, /*bMultiLine*/ true);
	Manifest.AuthorName = CleanText(Manifest.AuthorName, /*bMultiLine*/ false);
	if (Manifest.AuthorName.IsEmpty())
	{
		Manifest.AuthorName = AnonymousAuthor;
	}

	TArray<FString> Keywords;
	Keywords.Reserve(Manifest.Keywords.Num());
	for (const FString& Keyword : Manifest.Keywords)
	{
		FString Clean = CleanText(Keyword, /*bMultiLine*/ false);
		const bool bRepeated = Keywords.ContainsByPredicate([&Clean](const FString& Kept) { return Kept.Equals(Clean, ESearchCase::IgnoreCase); });
		if (!Clean.IsEmpty() && !bRepeated)
		{
			Keywords.Add(MoveTemp(Clean));
		}
	}
	Manifest.Keywords = MoveTemp(Keywords);

	// The root is not a category of its own; a removed or hand-edited category must still land somewhere browsable.
	if (!Manifest.Category.MatchesTag(StageCraftTags::Marketplace_Category) || Manifest.Category == StageCraftTags::Marketplace_Category)
	{
		Manifest.Category = StageCraftTags::Marketplace_Category_Other;
	}

	Manifest.Price.RealMoneyCurrency = Manifest.Price.RealMoneyMinorUnits == 0 ? FString() : Manifest.Price.RealMoneyCurrency.TrimStartAndEnd().ToUpper();
}

EStageMarketplaceResult StagePackage::ValidateManifest(const FStagePackageManifest& Manifest)
{
	const auto InRange = [](int64 Value, int64 Max) { return Value >= 0 && Value <= Max; };

	const bool bTextValid = Manifest.PackageId.IsValid()
		&& Manifest.Title.Len() >= 1 && Manifest.Title.Len() <= MaxTitleLength
		&& Manifest.Description.Len() <= MaxDescriptionLength
		&& Manifest.AuthorName.Len() >= 1 && Manifest.AuthorName.Len() <= MaxAuthorLength
		&& Manifest.Keywords.Num() <= MaxKeywords
		&& !Manifest.Keywords.ContainsByPredicate([](const FString& Keyword) { return Keyword.IsEmpty() || Keyword.Len() > MaxKeywordLength; });

	const bool bCategoryValid = Manifest.Category.MatchesTag(StageCraftTags::Marketplace_Category) && Manifest.Category != StageCraftTags::Marketplace_Category;

	const FStageMarketplacePrice& Price = Manifest.Price;
	const bool bPriceValid = InRange(Price.Credits, MaxCreditsPrice) && InRange(Price.RealMoneyMinorUnits, MaxRealMoneyMinorUnits)
		&& (Price.RealMoneyMinorUnits == 0 ? Price.RealMoneyCurrency.IsEmpty() : StagePackageFormat::IsCurrencyCode(Price.RealMoneyCurrency));

	const bool bStatsValid = Manifest.ItemCount >= 0 && Manifest.ItemCount <= FStageSceneStore::MaxItems
		&& Manifest.CueCount >= 0 && Manifest.CueCount <= FStageSceneStore::MaxCues
		&& Manifest.FixtureCount >= 0 && Manifest.FixtureCount <= Manifest.ItemCount
		&& Manifest.RequiredItems.Num() <= Manifest.ItemCount;

	return bTextValid && bCategoryValid && bPriceValid && bStatsValid ? EStageMarketplaceResult::Success : EStageMarketplaceResult::InvalidRequest;
}

bool StagePackage::IsPortable(const FStageSceneDocument& Scene)
{
	return !Scene.Items.ContainsByPredicate([](const FStageItemSnapshot& Item)
	{
		return StageMeshImport::IsImportedItemPath(Item.Item.ToSoftObjectPath().GetLongPackageName());
	});
}

TArray<FString> StagePackage::CollectRequiredItems(const FStageSceneDocument& Scene)
{
	TSet<FString> Paths;
	Paths.Reserve(Scene.Items.Num());
	for (const FStageItemSnapshot& Item : Scene.Items)
	{
		Paths.Add(Item.Item.ToSoftObjectPath().ToString());
	}
	TArray<FString> Sorted = Paths.Array();
	Sorted.Sort();
	return Sorted;
}

bool StagePackage::IsValidThumbnail(TConstArrayView<uint8> Png)
{
	static constexpr uint8 PngSignature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
	return Png.IsEmpty()
		|| (Png.Num() <= MaxThumbnailBytes && Png.Num() > static_cast<int32>(sizeof(PngSignature)) && FMemory::Memcmp(Png.GetData(), PngSignature, sizeof(PngSignature)) == 0);
}

FString StagePackage::SerializeManifest(const FStagePackageManifest& Manifest)
{
	using namespace StagePackageFormat;

	const TSharedRef<FJsonObject> RealMoney = MakeShared<FJsonObject>();
	RealMoney->SetNumberField(Field::Amount, static_cast<double>(Manifest.Price.RealMoneyMinorUnits));
	RealMoney->SetStringField(Field::Currency, Manifest.Price.RealMoneyCurrency);

	const TSharedRef<FJsonObject> Price = MakeShared<FJsonObject>();
	Price->SetNumberField(Field::Credits, static_cast<double>(Manifest.Price.Credits));
	Price->SetObjectField(Field::RealMoney, RealMoney);

	const TSharedRef<FJsonObject> Stats = MakeShared<FJsonObject>();
	Stats->SetNumberField(Field::Items, Manifest.ItemCount);
	Stats->SetNumberField(Field::Fixtures, Manifest.FixtureCount);
	Stats->SetNumberField(Field::Cues, Manifest.CueCount);

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(Field::Format, FormatTag);
	Root->SetNumberField(Field::Version, ManifestVersion);
	Root->SetStringField(Field::Id, Manifest.PackageId.ToString(EGuidFormats::DigitsWithHyphens));
	Root->SetStringField(Field::Title, Manifest.Title);
	Root->SetStringField(Field::Description, Manifest.Description);
	Root->SetStringField(Field::Author, Manifest.AuthorName);
	Root->SetStringField(Field::Kind, StaticEnum<EStagePackageKind>()->GetNameStringByValue(static_cast<int64>(Manifest.Kind)));
	Root->SetStringField(Field::Category, Manifest.Category.ToString());
	Root->SetArrayField(Field::Keywords, ToJsonStrings(Manifest.Keywords));
	Root->SetObjectField(Field::Price, Price);
	Root->SetStringField(Field::CreatedAt, Manifest.CreatedAtUtc.ToIso8601());
	Root->SetNumberField(Field::SceneVersion, Manifest.SceneVersion);
	Root->SetObjectField(Field::Stats, Stats);
	Root->SetArrayField(Field::RequiredItems, ToJsonStrings(Manifest.RequiredItems));

	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Root, Writer);
	return Json;
}

EStageMarketplaceResult StagePackage::ParseManifest(const FString& Json, FStagePackageManifest& OutManifest)
{
	using namespace StagePackageFormat;

	OutManifest = FStagePackageManifest();

	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Json), Root) || !Root.IsValid())
	{
		return EStageMarketplaceResult::Corrupt;
	}

	FString Format;
	int32 Version = 0;
	if (!Root->TryGetStringField(Field::Format, Format) || Format != FormatTag || !Root->TryGetNumberField(Field::Version, Version) || Version < 1)
	{
		return EStageMarketplaceResult::Corrupt;
	}
	if (Version > ManifestVersion)
	{
		return EStageMarketplaceResult::VersionMismatch;
	}

	FString Id;
	FString Kind;
	FString Category;
	const TSharedPtr<FJsonObject>* Price = nullptr;
	const TSharedPtr<FJsonObject>* Stats = nullptr;
	if (!Root->TryGetStringField(Field::Id, Id) || !FGuid::Parse(Id, OutManifest.PackageId)
		|| !Root->TryGetStringField(Field::Title, OutManifest.Title)
		|| !Root->TryGetStringField(Field::Kind, Kind)
		|| !Root->TryGetObjectField(Field::Price, Price) || !Price->IsValid()
		|| !Root->TryGetObjectField(Field::Stats, Stats) || !Stats->IsValid())
	{
		return EStageMarketplaceResult::Corrupt;
	}

	const int64 KindValue = StaticEnum<EStagePackageKind>()->GetValueByNameString(Kind);
	if (KindValue == INDEX_NONE)
	{
		return EStageMarketplaceResult::Corrupt;
	}
	OutManifest.Kind = static_cast<EStagePackageKind>(KindValue);

	Root->TryGetStringField(Field::Description, OutManifest.Description);
	Root->TryGetStringField(Field::Author, OutManifest.AuthorName);
	if (Root->TryGetStringField(Field::Category, Category))
	{
		// ErrorIfNotFound = false: a category this build does not know becomes Other in NormalizeManifest.
		OutManifest.Category = FGameplayTag::RequestGameplayTag(FName(*Category), /*ErrorIfNotFound*/ false);
	}
	if (Root->HasField(Field::Keywords) && !ReadStringArray(*Root, Field::Keywords, OutManifest.Keywords))
	{
		return EStageMarketplaceResult::Corrupt;
	}
	if (!Root->HasField(Field::RequiredItems) || !ReadStringArray(*Root, Field::RequiredItems, OutManifest.RequiredItems))
	{
		return EStageMarketplaceResult::Corrupt;
	}

	if (!(*Price)->TryGetNumberField(Field::Credits, OutManifest.Price.Credits))
	{
		return EStageMarketplaceResult::Corrupt;
	}
	const TSharedPtr<FJsonObject>* RealMoney = nullptr;
	if ((*Price)->TryGetObjectField(Field::RealMoney, RealMoney) && RealMoney->IsValid())
	{
		(*RealMoney)->TryGetNumberField(Field::Amount, OutManifest.Price.RealMoneyMinorUnits);
		(*RealMoney)->TryGetStringField(Field::Currency, OutManifest.Price.RealMoneyCurrency);
	}

	if (!(*Stats)->TryGetNumberField(Field::Items, OutManifest.ItemCount) || !(*Stats)->TryGetNumberField(Field::Fixtures, OutManifest.FixtureCount)
		|| !(*Stats)->TryGetNumberField(Field::Cues, OutManifest.CueCount))
	{
		return EStageMarketplaceResult::Corrupt;
	}

	FString CreatedAt;
	if (Root->TryGetStringField(Field::CreatedAt, CreatedAt))
	{
		FDateTime::ParseIso8601(*CreatedAt, OutManifest.CreatedAtUtc);
	}
	Root->TryGetNumberField(Field::SceneVersion, OutManifest.SceneVersion);

	NormalizeManifest(OutManifest);
	return ValidateManifest(OutManifest) == EStageMarketplaceResult::Success ? EStageMarketplaceResult::Success : EStageMarketplaceResult::Corrupt;
}

EStageMarketplaceResult StagePackage::Write(const FStagePackage& Package, TArray<uint8>& OutBytes)
{
	using namespace StagePackageFormat;

	OutBytes.Reset();
	const FStagePackageManifest& Manifest = Package.Manifest;
	if (ValidateManifest(Manifest) != EStageMarketplaceResult::Success || !IsValidThumbnail(Package.ThumbnailPng)
		|| Manifest.ItemCount != Package.Scene.Items.Num() || Manifest.CueCount != Package.Scene.Cues.Num()
		|| Manifest.RequiredItems != CollectRequiredItems(Package.Scene))
	{
		return EStageMarketplaceResult::InvalidRequest;
	}
	if (!IsPortable(Package.Scene))
	{
		return EStageMarketplaceResult::NotPortable;
	}

	struct FPendingSection
	{
		uint32 Tag = 0;
		TArray<uint8> Raw;
	};
	TArray<FPendingSection, TInlineAllocator<3>> Sections;
	Sections.Add({ ManifestTag, ToUtf8(SerializeManifest(Manifest)) });
	Sections.Add({ SceneTag, ToUtf8(FStageSceneStore::Serialize(Package.Scene)) });
	if (!Package.ThumbnailPng.IsEmpty())
	{
		Sections.Add({ ThumbnailTag, Package.ThumbnailPng });
	}

	TArray<FSectionHeader> Headers;
	TArray<TArray<uint8>> Payloads;
	for (const FPendingSection& Section : Sections)
	{
		if (Section.Raw.Num() > GetMaxRawSize(Section.Tag))
		{
			return EStageMarketplaceResult::TooLarge;
		}
		FSectionHeader& Header = Headers.AddDefaulted_GetRef();
		Header.Tag = Section.Tag;
		Header.RawSize = Section.Raw.Num();
		FSHA1::HashBuffer(Section.Raw.GetData(), Section.Raw.Num(), Header.Hash.Hash);
		Header.Codec = static_cast<uint32>(Compress(Section.Raw, Payloads.AddDefaulted_GetRef()));
		Header.StoredSize = Payloads.Last().Num();
	}

	FMemoryWriter Writer(OutBytes);
	Writer.Serialize(const_cast<uint8*>(Magic), sizeof(Magic));
	uint32 Version = ContainerVersion;
	uint32 Count = Headers.Num();
	Writer << Version;
	Writer << Count;
	for (FSectionHeader& Header : Headers)
	{
		Writer << Header.Tag;
		Writer << Header.Codec;
		Writer << Header.StoredSize;
		Writer << Header.RawSize;
		Writer.Serialize(Header.Hash.Hash, sizeof(Header.Hash.Hash));
	}
	for (TArray<uint8>& Payload : Payloads)
	{
		Writer.Serialize(Payload.GetData(), Payload.Num());
	}

	if (OutBytes.Num() > MaxPackageBytes)
	{
		OutBytes.Reset();
		return EStageMarketplaceResult::TooLarge;
	}
	return EStageMarketplaceResult::Success;
}

EStageMarketplaceResult StagePackage::ReadManifest(TConstArrayView<uint8> Bytes, FStagePackageManifest& OutManifest, bool& bOutHasThumbnail)
{
	using namespace StagePackageFormat;

	OutManifest = FStagePackageManifest();
	bOutHasThumbnail = false;

	TArray<FSectionView> Sections;
	EStageMarketplaceResult Result = ReadDirectory(Bytes, Sections);
	if (Result != EStageMarketplaceResult::Success)
	{
		return Result;
	}
	const FSectionView* Manifest = FindSection(Sections, ManifestTag);
	if (!Manifest || !FindSection(Sections, SceneTag))
	{
		return EStageMarketplaceResult::Corrupt;
	}

	TArray<uint8> Raw;
	Result = ReadSection(Bytes, *Manifest, Raw);
	if (Result != EStageMarketplaceResult::Success)
	{
		return Result;
	}
	Result = ParseManifest(FromUtf8(Raw), OutManifest);
	const FSectionView* Thumbnail = FindSection(Sections, ThumbnailTag);
	bOutHasThumbnail = Thumbnail && Thumbnail->Header.RawSize > 0;
	return Result;
}

EStageMarketplaceResult StagePackage::Read(TConstArrayView<uint8> Bytes, FStagePackage& OutPackage)
{
	using namespace StagePackageFormat;

	OutPackage = FStagePackage();

	TArray<FSectionView> Sections;
	EStageMarketplaceResult Result = ReadDirectory(Bytes, Sections);
	if (Result != EStageMarketplaceResult::Success)
	{
		return Result;
	}
	const FSectionView* ManifestSection = FindSection(Sections, ManifestTag);
	const FSectionView* SceneSection = FindSection(Sections, SceneTag);
	if (!ManifestSection || !SceneSection)
	{
		return EStageMarketplaceResult::Corrupt;
	}

	TArray<uint8> Raw;
	Result = ReadSection(Bytes, *ManifestSection, Raw);
	if (Result == EStageMarketplaceResult::Success)
	{
		Result = ParseManifest(FromUtf8(Raw), OutPackage.Manifest);
	}
	if (Result != EStageMarketplaceResult::Success)
	{
		return Result;
	}

	Result = ReadSection(Bytes, *SceneSection, Raw);
	if (Result != EStageMarketplaceResult::Success)
	{
		return Result;
	}
	FStageSceneParseReport Report;
	switch (FStageSceneStore::Parse(FromUtf8(Raw), OutPackage.Scene, Report))
	{
	case EStageSceneResult::Success:
		break;
	case EStageSceneResult::VersionMismatch:
		return EStageMarketplaceResult::VersionMismatch;
	default:
		return EStageMarketplaceResult::Corrupt;
	}

	// The writer never produces a scene the parser has to repair, so any repair means the package was not written by StageCraft
	// as it claims. The counts must match what a browser was shown.
	FStagePackageManifest& Manifest = OutPackage.Manifest;
	if (Report.InvalidItems > 0 || Report.RepairedIds > 0 || Report.InvalidCueEntries > 0
		|| Manifest.ItemCount != OutPackage.Scene.Items.Num() || Manifest.CueCount != OutPackage.Scene.Cues.Num())
	{
		return EStageMarketplaceResult::Corrupt;
	}
	if (!IsPortable(OutPackage.Scene))
	{
		return EStageMarketplaceResult::NotPortable;
	}
	Manifest.RequiredItems = CollectRequiredItems(OutPackage.Scene);

	if (const FSectionView* Thumbnail = FindSection(Sections, ThumbnailTag))
	{
		Result = ReadSection(Bytes, *Thumbnail, OutPackage.ThumbnailPng);
		if (Result != EStageMarketplaceResult::Success)
		{
			return Result;
		}
		if (!IsValidThumbnail(OutPackage.ThumbnailPng))
		{
			return EStageMarketplaceResult::Corrupt;
		}
	}
	return EStageMarketplaceResult::Success;
}

FStageMarketplaceListing StagePackage::ToListing(const FStagePackageManifest& Manifest, bool bHasThumbnail, int64 PackageBytes, int64 Downloads)
{
	FStageMarketplaceListing Listing;
	Listing.ListingId = Manifest.PackageId;
	Listing.Title = Manifest.Title;
	Listing.Description = Manifest.Description;
	Listing.AuthorName = Manifest.AuthorName;
	Listing.Kind = Manifest.Kind;
	Listing.Category = Manifest.Category;
	Listing.Keywords = Manifest.Keywords;
	Listing.Price = Manifest.Price;
	Listing.Downloads = FMath::Max<int64>(Downloads, 0);
	Listing.PublishedAtUtc = Manifest.CreatedAtUtc;
	Listing.ItemCount = Manifest.ItemCount;
	Listing.FixtureCount = Manifest.FixtureCount;
	Listing.CueCount = Manifest.CueCount;
	Listing.PackageBytes = PackageBytes;
	Listing.bHasThumbnail = bHasThumbnail;
	return Listing;
}
