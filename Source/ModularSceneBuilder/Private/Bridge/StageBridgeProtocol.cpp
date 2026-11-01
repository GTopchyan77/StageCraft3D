// Copyright Epic Games, Inc. All Rights Reserved.

#include "Bridge/StageBridgeProtocol.h"

#include "Dom/JsonObject.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#define LOCTEXT_NAMESPACE "StageBridgeProtocol"

namespace StageBridgeProtocolImpl
{
	namespace Field
	{
		const TCHAR* Type = TEXT("type");
		const TCHAR* Protocol = TEXT("protocol");
		const TCHAR* Token = TEXT("token");
		const TCHAR* App = TEXT("app");
		const TCHAR* Version = TEXT("version");
		const TCHAR* Client = TEXT("client");
		const TCHAR* Id = TEXT("id");
		const TCHAR* Ok = TEXT("ok");
		const TCHAR* Error = TEXT("error");
		const TCHAR* Format = TEXT("format");
		const TCHAR* Name = TEXT("name");
		const TCHAR* Path = TEXT("path");
		const TCHAR* Sha1 = TEXT("sha1");
		const TCHAR* Bytes = TEXT("bytes");
		const TCHAR* Reason = TEXT("reason");
	}

	FString ToLine(const TSharedRef<FJsonObject>& Object)
	{
		FString Line;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Line);
		FJsonSerializer::Serialize(Object, Writer);
		// The condensed writer escapes control characters, so the newline below is the only one in the line.
		Line.AppendChar(TEXT('\n'));
		return Line;
	}

	FString Hex(const FGuid& Guid)
	{
		return Guid.ToString(EGuidFormats::Digits).ToLower();
	}
}

bool StageBridgeProtocol::FLineReader::Append(TConstArrayView<uint8> Bytes, TArray<FString>& OutLines)
{
	for (const uint8 Byte : Bytes)
	{
		if (Byte != '\n')
		{
			if (Pending.Num() >= MaxLineBytes - 1)
			{
				Pending.Reset();
				return false;
			}
			Pending.Add(Byte);
			continue;
		}

		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Pending.GetData()), Pending.Num());
		FString Line(Converted.Length(), Converted.Get());
		Line.TrimEndInline();
		if (!Line.IsEmpty())
		{
			OutLines.Add(MoveTemp(Line));
		}
		Pending.Reset();
	}
	return true;
}

bool StageBridgeProtocol::ParseClientMessage(const FString& Line, FClientMessage& OutMessage, FString& OutError)
{
	using namespace StageBridgeProtocolImpl;

	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Line);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		OutError = TEXT("not a JSON object");
		return false;
	}

	FString Type;
	if (!Object->TryGetStringField(Field::Type, Type))
	{
		OutError = TEXT("no message type");
		return false;
	}

	OutMessage = FClientMessage();
	if (Type == TEXT("hello"))
	{
		FString App;
		FString AppVersion;
		if (!Object->TryGetNumberField(Field::Protocol, OutMessage.Hello.Protocol) || !Object->TryGetStringField(Field::Token, OutMessage.Hello.Token)
			|| !Object->TryGetStringField(Field::App, App))
		{
			OutError = TEXT("hello needs protocol, token and app");
			return false;
		}
		Object->TryGetStringField(Field::Version, AppVersion);
		OutMessage.Type = EClientMessage::Hello;
		OutMessage.Hello.App = ParseApp(App);
		OutMessage.Hello.AppVersion = SanitizeText(AppVersion, MaxAppVersionLength);
		return true;
	}
	if (Type == TEXT("ack"))
	{
		if (!Object->TryGetStringField(Field::Id, OutMessage.Ack.Id) || OutMessage.Ack.Id.IsEmpty() || OutMessage.Ack.Id.Len() > MaxIdLength
			|| !Object->TryGetBoolField(Field::Ok, OutMessage.Ack.bOk))
		{
			OutError = TEXT("ack needs id and ok");
			return false;
		}
		FString Error;
		Object->TryGetStringField(Field::Error, Error);
		OutMessage.Type = EClientMessage::Ack;
		OutMessage.Ack.Error = SanitizeText(Error, MaxErrorLength);
		return true;
	}

	OutError = FString::Printf(TEXT("unknown message type \"%s\""), *SanitizeText(Type, 32));
	return false;
}

bool StageBridgeProtocol::IsTokenMatch(const FString& Expected, const FString& Presented)
{
	if (Expected.IsEmpty() || Expected.Len() != Presented.Len())
	{
		return false;
	}
	uint32 Difference = 0;
	for (int32 Index = 0; Index < Expected.Len(); ++Index)
	{
		Difference |= static_cast<uint32>(Expected[Index] ^ Presented[Index]);
	}
	return Difference == 0;
}

FString StageBridgeProtocol::MakeToken()
{
	return StageBridgeProtocolImpl::Hex(FGuid::NewGuid()) + StageBridgeProtocolImpl::Hex(FGuid::NewGuid());
}

FString StageBridgeProtocol::MakeAssetId()
{
	return StageBridgeProtocolImpl::Hex(FGuid::NewGuid());
}

FString StageBridgeProtocol::MakeWelcome(int32 ClientId)
{
	using namespace StageBridgeProtocolImpl;
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(Field::Type, TEXT("welcome"));
	Object->SetNumberField(Field::Protocol, Version);
	Object->SetNumberField(Field::Client, ClientId);
	return ToLine(Object);
}

FString StageBridgeProtocol::MakeAsset(const FAsset& Asset)
{
	using namespace StageBridgeProtocolImpl;
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(Field::Type, TEXT("asset"));
	Object->SetStringField(Field::Id, Asset.Id);
	Object->SetStringField(Field::Format, Asset.Format);
	Object->SetStringField(Field::Name, Asset.Name);
	Object->SetStringField(Field::Path, Asset.Path);
	Object->SetStringField(Field::Sha1, Asset.Sha1);
	Object->SetNumberField(Field::Bytes, static_cast<double>(Asset.Bytes));
	return ToLine(Object);
}

FString StageBridgeProtocol::MakeBye(const FString& Reason)
{
	using namespace StageBridgeProtocolImpl;
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(Field::Type, TEXT("bye"));
	Object->SetStringField(Field::Reason, Reason);
	return ToLine(Object);
}

const TCHAR* StageBridgeProtocol::GetAppKey(EStageBridgeApp App)
{
	switch (App)
	{
	case EStageBridgeApp::Blender:	return TEXT("blender");
	case EStageBridgeApp::Maya:		return TEXT("maya");
	case EStageBridgeApp::Max:		return TEXT("max");
	case EStageBridgeApp::Cinema4D:	return TEXT("cinema4d");
	default:						return TEXT("");
	}
}

FText StageBridgeProtocol::GetAppDisplayName(EStageBridgeApp App)
{
	switch (App)
	{
	case EStageBridgeApp::Blender:	return LOCTEXT("Blender", "Blender");
	case EStageBridgeApp::Maya:		return LOCTEXT("Maya", "Maya");
	case EStageBridgeApp::Max:		return LOCTEXT("Max", "3ds Max");
	case EStageBridgeApp::Cinema4D:	return LOCTEXT("Cinema4D", "Cinema 4D");
	default:						return LOCTEXT("Unknown", "Unknown app");
	}
}

EStageBridgeApp StageBridgeProtocol::ParseApp(const FString& Key)
{
	for (const EStageBridgeApp App : { EStageBridgeApp::Blender, EStageBridgeApp::Maya, EStageBridgeApp::Max, EStageBridgeApp::Cinema4D })
	{
		if (Key.Equals(GetAppKey(App), ESearchCase::IgnoreCase))
		{
			return App;
		}
	}
	return EStageBridgeApp::Unknown;
}

bool StageBridgeProtocol::ImportsGlb(EStageBridgeApp App)
{
	return App == EStageBridgeApp::Blender || App == EStageBridgeApp::Max || App == EStageBridgeApp::Cinema4D;
}

FString StageBridgeProtocol::SanitizeText(const FString& Text, int32 MaxLength)
{
	FString Clean;
	Clean.Reserve(FMath::Min(Text.Len(), MaxLength));
	for (const TCHAR Character : Text)
	{
		if (Clean.Len() >= MaxLength)
		{
			break;
		}
		if (Character >= 0x20 && Character != 0x7F && !(Character >= 0x80 && Character < 0xA0))
		{
			Clean.AppendChar(Character);
		}
	}
	return Clean.TrimStartAndEnd();
}

#undef LOCTEXT_NAMESPACE
