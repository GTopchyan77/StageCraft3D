// Copyright Epic Games, Inc. All Rights Reserved.

#include "Fixtures/StageGdtfShareProtocol.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace StageGdtfShare
{
	const TCHAR* const DefaultBaseUrl = TEXT("https://gdtf-share.com/apis/public/");
	const TCHAR* const LoginEndpoint = TEXT("login.php");
	const TCHAR* const ListEndpoint = TEXT("getList.php");
	const TCHAR* const DownloadEndpoint = TEXT("downloadFile.php");
	const TCHAR* const AttributionText = TEXT("Powered by GDTF");

	namespace
	{
		constexpr int32 MaxFileNamePart = 60;
		constexpr int64 MaxUnixSeconds = 253402300799ll;

		FString FromUtf8(TConstArrayView<uint8> Bytes)
		{
			const FUTF8ToTCHAR Text(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
			return FString::ConstructFromPtrSize(Text.Get(), Text.Length());
		}

		TSharedPtr<FJsonObject> ParseObject(TConstArrayView<uint8> Body)
		{
			TSharedPtr<FJsonObject> Root;
			if (Body.IsEmpty() || !FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(FromUtf8(Body)), Root))
			{
				return nullptr;
			}
			return Root;
		}

		FString CleanText(const FString& Text)
		{
			FString Clean;
			Clean.Reserve(FMath::Min(Text.Len(), MaxTextLength));
			for (const TCHAR Char : Text)
			{
				if (Clean.Len() >= MaxTextLength)
				{
					break;
				}
				if (Char >= 32 && Char != 127)
				{
					Clean.AppendChar(Char);
				}
			}
			return Clean.TrimStartAndEnd();
		}

		bool ReadText(const FJsonObject& Object, const TCHAR* Field, FString& OutText)
		{
			const TSharedPtr<FJsonValue> Value = Object.TryGetField(Field);
			if (!Value.IsValid() || Value->Type != EJson::String)
			{
				OutText.Reset();
				return false;
			}
			OutText = CleanText(Value->AsString());
			return true;
		}

		bool ReadInteger(const FJsonObject& Object, const TCHAR* Field, int64 Min, int64 Max, int64& OutValue)
		{
			const TSharedPtr<FJsonValue> Value = Object.TryGetField(Field);
			if (!Value.IsValid())
			{
				return false;
			}
			if (Value->Type == EJson::Number)
			{
				const double Number = Value->AsNumber();
				if (!FMath::IsFinite(Number) || Number != FMath::FloorToDouble(Number) || Number < double(Min) || Number > double(Max))
				{
					return false;
				}
				OutValue = static_cast<int64>(Number);
				return true;
			}
			if (Value->Type == EJson::String)
			{
				const FString Text = Value->AsString().TrimStartAndEnd();
				if (Text.IsEmpty() || Text.Len() > 19 || !Text.IsNumeric() || Text.Contains(TEXT(".")) || Text.StartsWith(TEXT("-")) || Text.StartsWith(TEXT("+")))
				{
					return false;
				}
				const int64 Parsed = FCString::Atoi64(*Text);
				if (Parsed < Min || Parsed > Max)
				{
					return false;
				}
				OutValue = Parsed;
				return true;
			}
			return false;
		}

		EStageGdtfShareUploader ReadUploader(const FJsonObject& Object)
		{
			FString Text;
			ReadText(Object, TEXT("uploader"), Text);
			if (Text.Equals(TEXT("Manuf."), ESearchCase::IgnoreCase) || Text.Equals(TEXT("Manufacturer"), ESearchCase::IgnoreCase))
			{
				return EStageGdtfShareUploader::Manufacturer;
			}
			if (Text.Equals(TEXT("User"), ESearchCase::IgnoreCase))
			{
				return EStageGdtfShareUploader::User;
			}
			return EStageGdtfShareUploader::Unknown;
		}

		void ReadModes(const FJsonObject& Object, TArray<FStageGdtfShareMode>& OutModes)
		{
			const TArray<TSharedPtr<FJsonValue>>* Modes = nullptr;
			if (!Object.TryGetArrayField(TEXT("modes"), Modes))
			{
				return;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Modes)
			{
				if (OutModes.Num() >= MaxModes)
				{
					return;
				}
				const TSharedPtr<FJsonObject>* Mode = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(Mode) || !Mode->IsValid())
				{
					continue;
				}
				FStageGdtfShareMode& Out = OutModes.AddDefaulted_GetRef();
				ReadText(**Mode, TEXT("name"), Out.Name);
				int64 Footprint = 0;
				Out.Footprint = ReadInteger(**Mode, TEXT("dmxfootprint"), 0, 512 * 64, Footprint) ? static_cast<int32>(Footprint) : 0;
			}
		}

		bool IsCookieNameChar(TCHAR Char)
		{
			static const FString Separators(TEXT("()<>@,;:\\\"/[]?={} \t"));
			int32 Unused = 0;
			return Char > 32 && Char < 127 && !Separators.FindChar(Char, Unused);
		}

		bool IsCookieValueChar(TCHAR Char)
		{
			return Char == 0x21 || (Char >= 0x23 && Char <= 0x2B) || (Char >= 0x2D && Char <= 0x3A) || (Char >= 0x3C && Char <= 0x5B) || (Char >= 0x5D && Char <= 0x7E);
		}

		FString CleanFileNamePart(const FString& Text)
		{
			FString Clean;
			Clean.Reserve(Text.Len());
			for (const TCHAR Char : Text)
			{
				const bool bKeep = FChar::IsAlnum(Char) && Char < 128;
				const bool bPunctuation = Char == TEXT(' ') || Char == TEXT('-') || Char == TEXT('_') || Char == TEXT('.') || Char == TEXT('(') || Char == TEXT(')');
				Clean.AppendChar(bKeep || bPunctuation ? Char : TEXT('_'));
			}
			Clean.TrimStartAndEndInline();
			while (Clean.StartsWith(TEXT(".")))
			{
				Clean.RightChopInline(1);
			}
			Clean.LeftInline(MaxFileNamePart);
			Clean.TrimEndInline();
			return Clean;
		}

		bool ContainsWord(const FStageGdtfShareEntry& Entry, const FString& Word)
		{
			return Entry.Manufacturer.Contains(Word) || Entry.Fixture.Contains(Word) || Entry.Revision.Contains(Word) || Entry.Creator.Contains(Word);
		}
	}

	bool IsSafeBaseUrl(const FString& Url)
	{
		const FString Prefix = TEXT("https://");
		if (!Url.StartsWith(Prefix, ESearchCase::IgnoreCase) || !Url.EndsWith(TEXT("/")) || Url.Len() <= Prefix.Len() + 1)
		{
			return false;
		}
		for (const TCHAR Char : Url)
		{
			if (Char <= 32 || Char >= 127 || Char == TEXT('\\') || Char == TEXT('?') || Char == TEXT('#') || Char == TEXT('@'))
			{
				return false;
			}
		}
		return true;
	}

	TArray<uint8> MakeLoginBody(const FString& User, const FString& Password)
	{
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("user"), User);
		Root->SetStringField(TEXT("password"), Password);
		FString Json;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Root, Writer);
		const FTCHARToUTF8 Utf8(*Json);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}

	bool ParseResultFlag(TConstArrayView<uint8> Body, bool& bOutResult, FString& OutError)
	{
		bOutResult = false;
		OutError.Reset();
		const TSharedPtr<FJsonObject> Root = ParseObject(Body);
		const TSharedPtr<FJsonValue> Result = Root.IsValid() ? Root->TryGetField(TEXT("result")) : nullptr;
		if (!Result.IsValid() || Result->Type != EJson::Boolean)
		{
			return false;
		}
		bOutResult = Result->AsBool();
		if (!ReadText(*Root, TEXT("error"), OutError) || OutError.IsEmpty())
		{
			ReadText(*Root, TEXT("notice"), OutError);
		}
		return true;
	}

	bool ParseList(TConstArrayView<uint8> Body, TArray<FStageGdtfShareEntry>& OutEntries, int32& OutSkipped)
	{
		OutEntries.Reset();
		OutSkipped = 0;
		if (Body.Num() > MaxListResponseBytes)
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Root = ParseObject(Body);
		const TSharedPtr<FJsonValue> Result = Root.IsValid() ? Root->TryGetField(TEXT("result")) : nullptr;
		const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
		if (!Result.IsValid() || Result->Type != EJson::Boolean || !Result->AsBool() || !Root->TryGetArrayField(TEXT("list"), List))
		{
			return false;
		}

		TSet<int32> Seen;
		OutEntries.Reserve(FMath::Min(List->Num(), MaxEntries));
		Seen.Reserve(FMath::Min(List->Num(), MaxEntries));
		for (const TSharedPtr<FJsonValue>& Value : *List)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (OutEntries.Num() >= MaxEntries || !Value.IsValid() || !Value->TryGetObject(Object) || !Object->IsValid())
			{
				++OutSkipped;
				continue;
			}
			const FJsonObject& Item = **Object;
			FStageGdtfShareEntry Entry;
			int64 RevisionId = 0;
			if (!ReadInteger(Item, TEXT("rid"), 1, MAX_int32, RevisionId) || !ReadText(Item, TEXT("manufacturer"), Entry.Manufacturer)
				|| !ReadText(Item, TEXT("fixture"), Entry.Fixture) || Entry.Manufacturer.IsEmpty() || Entry.Fixture.IsEmpty())
			{
				++OutSkipped;
				continue;
			}
			Entry.RevisionId = static_cast<int32>(RevisionId);
			bool bAlreadySeen = false;
			Seen.Add(Entry.RevisionId, &bAlreadySeen);
			if (bAlreadySeen)
			{
				++OutSkipped;
				continue;
			}
			ReadText(Item, TEXT("revision"), Entry.Revision);
			ReadText(Item, TEXT("creator"), Entry.Creator);
			ReadText(Item, TEXT("version"), Entry.GdtfVersion);
			Entry.Uploader = ReadUploader(Item);
			double Rating = 0.0;
			if (Item.TryGetNumberField(TEXT("rating"), Rating) && FMath::IsFinite(Rating))
			{
				Entry.Rating = static_cast<float>(FMath::Clamp(Rating, 0.0, 5.0));
			}
			FString Uuid;
			if (ReadText(Item, TEXT("uuid"), Uuid))
			{
				FGuid::Parse(Uuid, Entry.FixtureTypeId);
			}
			int64 Number = 0;
			Entry.FileSizeBytes = ReadInteger(Item, TEXT("filesize"), 0, MAX_int64, Number) ? Number : 0;
			if (ReadInteger(Item, TEXT("lastModified"), 0, MaxUnixSeconds, Number) || ReadInteger(Item, TEXT("creationDate"), 0, MaxUnixSeconds, Number))
			{
				Entry.LastModifiedUtc = FDateTime::FromUnixTimestamp(Number);
			}
			ReadModes(Item, Entry.Modes);
			OutEntries.Add(MoveTemp(Entry));
		}
		return true;
	}

	FString JoinCookies(TConstArrayView<FString> SetCookieValues)
	{
		TArray<FString> Parts;
		for (const FString& Header : SetCookieValues)
		{
			TArray<FString> Split;
			Header.ParseIntoArray(Split, TEXT(","), true);
			Parts.Append(MoveTemp(Split));
		}
		TArray<FString> Pairs;
		for (const FString& Part : Parts)
		{
			FString Pair = Part;
			int32 Semicolon = INDEX_NONE;
			if (Pair.FindChar(TEXT(';'), Semicolon))
			{
				Pair.LeftInline(Semicolon);
			}
			Pair.TrimStartAndEndInline();
			FString Name;
			FString Value;
			if (!Pair.Split(TEXT("="), &Name, &Value) || Name.IsEmpty())
			{
				continue;
			}
			const FString Bare = Value.Len() >= 2 && Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\"")) ? Value.Mid(1, Value.Len() - 2) : Value;
			bool bValid = !Bare.IsEmpty() && !Bare.Equals(TEXT("deleted"), ESearchCase::IgnoreCase);
			for (const TCHAR Char : Name)
			{
				bValid = bValid && IsCookieNameChar(Char);
			}
			for (const TCHAR Char : Bare)
			{
				bValid = bValid && IsCookieValueChar(Char);
			}
			if (!bValid)
			{
				continue;
			}
			Pairs.RemoveAll([&Name](const FString& Kept) { return Kept.StartsWith(Name + TEXT("="), ESearchCase::CaseSensitive); });
			Pairs.Add(Name + TEXT("=") + Value);
		}
		return FString::Join(Pairs, TEXT("; "));
	}

	TArray<int32> Search(TConstArrayView<FStageGdtfShareEntry> Entries, const FString& Text, TOptional<EStageGdtfShareUploader> Uploader, int32 MaxResults)
	{
		TArray<FString> Words;
		Text.ParseIntoArrayWS(Words);
		TArray<int32> Matches;
		for (int32 Index = 0; Index < Entries.Num(); ++Index)
		{
			const FStageGdtfShareEntry& Entry = Entries[Index];
			if (Uploader.IsSet() && Entry.Uploader != Uploader.GetValue())
			{
				continue;
			}
			if (!Words.ContainsByPredicate([&Entry](const FString& Word) { return !ContainsWord(Entry, Word); }))
			{
				Matches.Add(Index);
			}
		}
		Matches.Sort([&Entries](int32 Left, int32 Right)
		{
			const FStageGdtfShareEntry& A = Entries[Left];
			const FStageGdtfShareEntry& B = Entries[Right];
			const int32 ByManufacturer = A.Manufacturer.Compare(B.Manufacturer, ESearchCase::IgnoreCase);
			if (ByManufacturer != 0)
			{
				return ByManufacturer < 0;
			}
			const int32 ByFixture = A.Fixture.Compare(B.Fixture, ESearchCase::IgnoreCase);
			if (ByFixture != 0)
			{
				return ByFixture < 0;
			}
			if (A.LastModifiedUtc != B.LastModifiedUtc)
			{
				return A.LastModifiedUtc > B.LastModifiedUtc;
			}
			return A.RevisionId > B.RevisionId;
		});
		if (MaxResults >= 0 && Matches.Num() > MaxResults)
		{
			Matches.SetNum(MaxResults);
		}
		return Matches;
	}

	FString MakeFileName(const FStageGdtfShareEntry& Entry)
	{
		const FString Manufacturer = CleanFileNamePart(Entry.Manufacturer);
		const FString Fixture = CleanFileNamePart(Entry.Fixture);
		const FString Revision = CleanFileNamePart(Entry.Revision);
		if (Manufacturer.IsEmpty() || Fixture.IsEmpty())
		{
			return FString::Printf(TEXT("GDTF_Share_%d.gdtf"), Entry.RevisionId);
		}
		FString Name = Manufacturer + TEXT("@") + Fixture;
		if (!Revision.IsEmpty())
		{
			Name += TEXT("@") + Revision;
		}
		return Name + TEXT(".gdtf");
	}

	FString MakeLabel(const FStageGdtfShareEntry& Entry)
	{
		const FString Name = Entry.Manufacturer + TEXT(" ") + Entry.Fixture;
		return Entry.Revision.IsEmpty() ? Name : FString::Printf(TEXT("%s (%s)"), *Name, *Entry.Revision);
	}

	bool IsZipFile(TConstArrayView<uint8> Bytes)
	{
		static constexpr uint8 Signature[4] = { 0x50, 0x4B, 0x03, 0x04 };
		return Bytes.Num() >= 22 && FMemory::Memcmp(Bytes.GetData(), Signature, sizeof(Signature)) == 0;
	}
}
