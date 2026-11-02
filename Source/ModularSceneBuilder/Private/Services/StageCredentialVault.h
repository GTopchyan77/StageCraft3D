// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

enum class EStageVaultResult : uint8
{
	Success,
	NotFound,
	/** The id or the secret is not acceptable (empty, too long, characters outside the id rules). */
	InvalidRequest,
	/** This platform has no per-user protected storage; nothing is ever written in plain text. */
	Unavailable,
	/** The file could not be written, read or unprotected (another user, another id, damage). */
	Failed,
};

/**
 * Per-user storage for third-party API keys (Docs/ADR/0023-ai-tool-providers-and-credential-vault.md §2).
 *
 * Windows: each secret is encrypted with DPAPI (CryptProtectData, current user, no UI) using the id as additional entropy, so a
 * file copied to another id or read by another Windows user does not decrypt. Files live in a per-user folder, are written
 * atomically, and start with a small header. Other platforms: Unavailable; the vault never falls back to plain text.
 *
 * Keys never enter logs, saves or result messages; buffers that held them are zeroed. DPAPI protects against other users and
 * casual file copying, not against code running as the same user: that is the honest limit of a client-side key store.
 *
 * Plain value type with no UObjects; game thread by convention (it does blocking file I/O on small files).
 */
class FStageCredentialVault
{
public:
	static constexpr int32 MaxIdLength = 32;
	static constexpr int32 MaxSecretLength = 4096;

	explicit FStageCredentialVault(FString InDirectory);

	/** %LOCALAPPDATA%\StageCraft\Credentials on Windows. */
	static FString GetDefaultDirectory();

	/** True where secrets can be stored protected (Windows). */
	static bool IsSupported();

	/** Letters, digits, '_' and '-'; 1..MaxIdLength characters. */
	static bool IsValidId(FName Id);

	EStageVaultResult Store(FName Id, FStringView Secret) const;

	/** OutSecret is empty unless Success. The caller zeroes it when done (WipeString). */
	EStageVaultResult Load(FName Id, FString& OutSecret) const;

	bool Contains(FName Id) const;

	/** Success also when there was nothing to remove. */
	EStageVaultResult Remove(FName Id) const;

	/** Overwrites a string's characters before releasing it. */
	static void WipeString(FString& Secret);

	const FString& GetDirectory() const { return Directory; }

private:
	FString GetPath(FName Id) const;

	FString Directory;
};
