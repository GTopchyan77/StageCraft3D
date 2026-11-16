// Copyright Epic Games, Inc. All Rights Reserved.

#include "Licence/StageLicenceToken.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// OpenSSL declares a type named UI, which collides with Unreal's; the engine's own OpenSSL users rename it the same way.
#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace StageLicenceTokenPrivate
{
	const TCHAR* const Prefix = TEXT("SC1.");
	constexpr int32 SupportedVersion = 1;
	constexpr int32 Ed25519KeyBytes = 32;
	constexpr int32 Ed25519SignatureBytes = 64;
	constexpr int32 MaxEntitlements = 32;
	constexpr int32 MaxEntitlementLength = 128;
	constexpr int32 MaxAccountIdLength = 64;
	constexpr int32 MaxPlanLength = 32;

	/** Production keys, compiled in so that no file a user can edit adds one (Docs/ADR/0034 §17.3, §17.6). */
	void AddProductionKeys(TArray<FStageLicencePublicKey>& Keys)
	{
		// One line per key from `node Services/StageCraftCloud/scripts/make-licence-key.ts <id>`. None yet: Shipping stays Free until
		// Gevor generates the production pair.
	}

	bool IsKeyId(const FString& Text)
	{
		if (Text.IsEmpty() || Text.Len() > 32)
		{
			return false;
		}
		for (const TCHAR Char : Text)
		{
			if (!FChar::IsAlnum(Char) && Char != TEXT('.') && Char != TEXT('_') && Char != TEXT('-'))
			{
				return false;
			}
		}
		return true;
	}

	bool IsMachineId(const FString& Text)
	{
		if (Text.Len() != 64)
		{
			return false;
		}
		for (const TCHAR Char : Text)
		{
			if (!FChar::IsDigit(Char) && !(Char >= TEXT('a') && Char <= TEXT('f')))
			{
				return false;
			}
		}
		return true;
	}

	bool IsPrintable(const FString& Text, int32 MaxLength)
	{
		if (Text.IsEmpty() || Text.Len() > MaxLength)
		{
			return false;
		}
		for (const TCHAR Char : Text)
		{
			if (Char < 0x20 || Char == 0x7F)
			{
				return false;
			}
		}
		return true;
	}

	/** A JSON number that is a whole, positive Unix time (JSON has no integers; the server writes whole seconds). */
	bool GetUnixSeconds(const FJsonObject& Object, const TCHAR* Field, int64& Out)
	{
		double Value = 0.0;
		if (!Object.TryGetNumberField(Field, Value) || Value <= 0.0 || Value > 253402300799.0 || FMath::Floor(Value) != Value)
		{
			return false;
		}
		Out = static_cast<int64>(Value);
		return true;
	}

	bool ParsePayload(const TArray<uint8>& Json, FStageLicenceClaims& Out, int32& OutVersion)
	{
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Json.GetData()), Json.Num());
		const FString Text(Converted.Length(), Converted.Get());
		TSharedPtr<FJsonObject> Object;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) || !Object.IsValid())
		{
			return false;
		}
		double Version = 0.0;
		if (!Object->TryGetNumberField(TEXT("v"), Version) || FMath::Floor(Version) != Version || Version < 1.0 || Version > 1000.0)
		{
			return false;
		}
		OutVersion = static_cast<int32>(Version);
		FStageLicenceClaims Claims;
		const TArray<TSharedPtr<FJsonValue>>* Entitlements = nullptr;
		if (!Object->TryGetStringField(TEXT("kid"), Claims.KeyId) || !IsKeyId(Claims.KeyId)
			|| !Object->TryGetStringField(TEXT("sub"), Claims.AccountId) || !IsPrintable(Claims.AccountId, MaxAccountIdLength)
			|| !Object->TryGetStringField(TEXT("mid"), Claims.MachineId) || !IsMachineId(Claims.MachineId)
			|| !Object->TryGetStringField(TEXT("plan"), Claims.Plan) || !IsPrintable(Claims.Plan, MaxPlanLength)
			|| !GetUnixSeconds(*Object, TEXT("iat"), Claims.IssuedAt) || !GetUnixSeconds(*Object, TEXT("exp"), Claims.ExpiresAt)
			|| Claims.ExpiresAt <= Claims.IssuedAt
			|| !Object->TryGetArrayField(TEXT("ent"), Entitlements) || Entitlements->Num() > MaxEntitlements)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Entitlements)
		{
			FString Name;
			if (!Value.IsValid() || !Value->TryGetString(Name) || !IsPrintable(Name, MaxEntitlementLength))
			{
				return false;
			}
			Claims.Entitlements.Add(MoveTemp(Name));
		}
		Out = MoveTemp(Claims);
		return true;
	}
}

TArray<FStageLicencePublicKey> StageLicenceToken::GetTrustedKeys()
{
	TArray<FStageLicencePublicKey> Keys;
	StageLicenceTokenPrivate::AddProductionKeys(Keys);
#if !UE_BUILD_SHIPPING
	// The development key's private half is public (the cloud server's licence/token.ts), so only non-Shipping builds may trust it.
	Keys.Add({ TEXT("dev-1"), { 0xd0, 0x2d, 0xa7, 0xfb, 0xe4, 0xe1, 0x2f, 0x2d, 0x14, 0x35, 0x12, 0x5b, 0x3c, 0xbf, 0x09, 0x61,
		0xcf, 0xa8, 0xce, 0x42, 0x69, 0xad, 0xf9, 0x10, 0xcb, 0x93, 0x7d, 0xc0, 0xe8, 0x26, 0x19, 0xf0 } });
#endif
	return Keys;
}

bool StageLicenceToken::DecodeBase64Url(const FString& Text, TArray<uint8>& OutBytes)
{
	OutBytes.Reset();
	if (Text.Len() % 4 == 1)
	{
		return false;
	}
	OutBytes.Reserve(Text.Len() * 3 / 4);
	uint32 Buffer = 0;
	int32 Bits = 0;
	for (const TCHAR Char : Text)
	{
		uint32 Value = 0;
		if (Char >= TEXT('A') && Char <= TEXT('Z'))
		{
			Value = Char - TEXT('A');
		}
		else if (Char >= TEXT('a') && Char <= TEXT('z'))
		{
			Value = Char - TEXT('a') + 26;
		}
		else if (Char >= TEXT('0') && Char <= TEXT('9'))
		{
			Value = Char - TEXT('0') + 52;
		}
		else if (Char == TEXT('-'))
		{
			Value = 62;
		}
		else if (Char == TEXT('_'))
		{
			Value = 63;
		}
		else
		{
			OutBytes.Reset();
			return false;
		}
		Buffer = (Buffer << 6) | Value;
		Bits += 6;
		if (Bits >= 8)
		{
			Bits -= 8;
			OutBytes.Add(static_cast<uint8>((Buffer >> Bits) & 0xFF));
		}
	}
	// Leftover bits must be zero, or two different texts would decode to the same bytes.
	if ((Buffer & ((1u << Bits) - 1u)) != 0)
	{
		OutBytes.Reset();
		return false;
	}
	return true;
}

bool StageLicenceToken::VerifyEd25519(TConstArrayView<uint8> Message, TConstArrayView<uint8> Signature, TConstArrayView<uint8> PublicKey)
{
	using namespace StageLicenceTokenPrivate;
	if (Signature.Num() != Ed25519SignatureBytes || PublicKey.Num() != Ed25519KeyBytes)
	{
		return false;
	}
	EVP_PKEY* Key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, PublicKey.GetData(), PublicKey.Num());
	if (!Key)
	{
		return false;
	}
	EVP_MD_CTX* Context = EVP_MD_CTX_new();
	bool bValid = false;
	if (Context && EVP_DigestVerifyInit(Context, nullptr, nullptr, nullptr, Key) == 1)
	{
		bValid = EVP_DigestVerify(Context, Signature.GetData(), Signature.Num(), Message.GetData(), Message.Num()) == 1;
	}
	EVP_MD_CTX_free(Context);
	EVP_PKEY_free(Key);
	return bValid;
}

EStageLicenceTokenError StageLicenceToken::Verify(const FString& Token, TConstArrayView<FStageLicencePublicKey> TrustedKeys, FStageLicenceClaims& OutClaims)
{
	using namespace StageLicenceTokenPrivate;
	if (Token.Len() > MaxTokenLength || !Token.StartsWith(Prefix, ESearchCase::CaseSensitive))
	{
		return EStageLicenceTokenError::Malformed;
	}
	int32 Dot = INDEX_NONE;
	if (!Token.FindLastChar(TEXT('.'), Dot) || Dot <= FCString::Strlen(Prefix))
	{
		return EStageLicenceTokenError::Malformed;
	}
	const FString SigningInput = Token.Left(Dot);
	TArray<uint8> Payload;
	TArray<uint8> Signature;
	if (!DecodeBase64Url(SigningInput.Mid(FCString::Strlen(Prefix)), Payload) || !DecodeBase64Url(Token.Mid(Dot + 1), Signature))
	{
		return EStageLicenceTokenError::Malformed;
	}

	FStageLicenceClaims Claims;
	int32 Version = 0;
	if (!ParsePayload(Payload, Claims, Version))
	{
		return EStageLicenceTokenError::Malformed;
	}
	if (Version != SupportedVersion)
	{
		return EStageLicenceTokenError::UnsupportedVersion;
	}
	const FStageLicencePublicKey* Key = TrustedKeys.FindByPredicate([&Claims](const FStageLicencePublicKey& Candidate) { return Candidate.KeyId == Claims.KeyId; });
	if (!Key)
	{
		return EStageLicenceTokenError::UnknownKey;
	}
	// The signing input is pure ASCII (prefix and base64url), so its TCHARs narrow to the exact bytes the server signed.
	TArray<uint8> Message;
	Message.Reserve(SigningInput.Len());
	for (const TCHAR Char : SigningInput)
	{
		Message.Add(static_cast<uint8>(Char));
	}
	if (!VerifyEd25519(Message, Signature, Key->Key))
	{
		return EStageLicenceTokenError::BadSignature;
	}
	OutClaims = MoveTemp(Claims);
	return EStageLicenceTokenError::None;
}

EStageLicenceValidity StageLicenceToken::Evaluate(const FStageLicenceClaims& Claims, const FString& MachineId, int64 NowUnix, int64 HighWaterUnix)
{
	if (Claims.MachineId != MachineId)
	{
		return EStageLicenceValidity::WrongMachine;
	}
	// Setting the clock back would otherwise stretch a 14-day licence forever while offline.
	if ((HighWaterUnix > 0 && NowUnix + ClockRollbackToleranceSeconds < HighWaterUnix) || NowUnix + ClockRollbackToleranceSeconds < Claims.IssuedAt)
	{
		return EStageLicenceValidity::ClockRollback;
	}
	if (NowUnix >= Claims.ExpiresAt)
	{
		return EStageLicenceValidity::Expired;
	}
	return EStageLicenceValidity::Valid;
}

FString StageLicenceToken::MakeMachineId(const FString& InstallId)
{
	const FTCHARToUTF8 Utf8(*(FString(TEXT("StageCraft.Licence.Machine.v1:")) + InstallId));
	uint8 Digest[EVP_MAX_MD_SIZE];
	unsigned int DigestLength = 0;
	if (EVP_Digest(Utf8.Get(), Utf8.Length(), Digest, &DigestLength, EVP_sha256(), nullptr) != 1 || DigestLength != 32)
	{
		return FString();
	}
	return BytesToHexLower(Digest, DigestLength);
}
