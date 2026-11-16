// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Why a token is not usable (or None). Parsing and signature only; time and machine are judged by StageLicenceToken::Evaluate. */
enum class EStageLicenceTokenError : uint8
{
	None,
	/** Not SC1.<payload>.<signature>, not base64url, not JSON, a field missing or out of its caps. */
	Malformed,
	/** A token version this build does not read. */
	UnsupportedVersion,
	/** Signed with a key id this build does not trust. */
	UnknownKey,
	/** The signature does not match the payload under the named key: edited or forged. */
	BadSignature,
};

/** What a token's signature vouches for (Services/StageCraftCloud/src/licence/token.ts LicencePayload). */
struct FStageLicenceClaims
{
	FString KeyId;
	FString AccountId;
	/** StageLicenceToken::MakeMachineId of the computer it was issued to. */
	FString MachineId;
	FString Plan;
	/** Entitlement names exactly as signed; the subsystem keeps only known StageCraft.Entitlement.Pro.* tags. */
	TArray<FString> Entitlements;
	/** Unix seconds. */
	int64 IssuedAt = 0;
	int64 ExpiresAt = 0;
};

/** One key the client accepts signatures from. */
struct FStageLicencePublicKey
{
	FString KeyId;
	/** Raw Ed25519 public key, 32 bytes. */
	TArray<uint8> Key;
};

/** A verified token's standing at a given time on a given machine. */
enum class EStageLicenceValidity : uint8
{
	Valid,
	Expired,
	/** Issued to another computer (a copied licence file). */
	WrongMachine,
	/** The clock is more than StageLicenceToken::ClockRollbackToleranceSeconds before the newest time this install has seen. */
	ClockRollback,
};

/**
 * StageCraft Pro licence tokens on the client (Docs/ADR/0034 §17.3): SC1.<base64url(JSON payload)>.<base64url(Ed25519 signature)>, the
 * signature over the ASCII bytes "SC1.<base64url(payload)>". Verification uses the engine's OpenSSL (EVP_PKEY_ED25519), never
 * hand-written cryptography.
 *
 * Trust: GetTrustedKeys is the production keys compiled into this binary (never read from config, which a user can edit), plus the
 * public development key outside Shipping. Pure: no world, no files, any thread. Covered by StageCraft.Licence.Token.
 */
namespace StageLicenceToken
{
	/** Longest token accepted; real ones are about 700 characters. */
	constexpr int32 MaxTokenLength = 4096;

	/** A clock this far behind the newest time seen is a rollback; less is ordinary drift or a time-zone change. */
	constexpr int64 ClockRollbackToleranceSeconds = 24 * 60 * 60;

	/** The keys this build trusts (see the namespace comment). */
	MODULARSCENEBUILDER_API TArray<FStageLicencePublicKey> GetTrustedKeys();

	/** Parses Token and verifies its signature against TrustedKeys. OutClaims is set only when the result is None. */
	MODULARSCENEBUILDER_API EStageLicenceTokenError Verify(const FString& Token, TConstArrayView<FStageLicencePublicKey> TrustedKeys, FStageLicenceClaims& OutClaims);

	/** Whether verified Claims hold for MachineId at NowUnix, given the newest Unix time this install has seen (0 when none). */
	MODULARSCENEBUILDER_API EStageLicenceValidity Evaluate(const FStageLicenceClaims& Claims, const FString& MachineId, int64 NowUnix, int64 HighWaterUnix);

	/** Ed25519 verification of Signature (64 bytes) over Message with a raw 32-byte PublicKey. False on any size or library failure. */
	MODULARSCENEBUILDER_API bool VerifyEd25519(TConstArrayView<uint8> Message, TConstArrayView<uint8> Signature, TConstArrayView<uint8> PublicKey);

	/** This computer's id for the licence server: lowercase hex SHA-256 of a fixed salt and InstallId (FPlatformMisc::GetLoginId). */
	MODULARSCENEBUILDER_API FString MakeMachineId(const FString& InstallId);

	/** RFC 4648 §5 base64url without padding; false on any character outside the alphabet or an impossible length. */
	MODULARSCENEBUILDER_API bool DecodeBase64Url(const FString& Text, TArray<uint8>& OutBytes);
}
