// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/StageCredentialVault.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "IO/StageAtomicFile.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <wincrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace StageCredentialVaultImpl
{
	/** "SCKV" + format version; the protected blob follows. */
	constexpr uint8 Magic[4] = { 'S', 'C', 'K', 'V' };
	constexpr uint8 FormatVersion = 1;
	constexpr int32 HeaderSize = 5;
	/** A DPAPI blob of a MaxSecretLength key is a few KB; anything larger is not ours. */
	constexpr int64 MaxFileBytes = 64 * 1024;
	const TCHAR* FileExtension = TEXT(".key");

	/** Zeroes and releases a byte buffer that held a secret. */
	void Wipe(TArray<uint8>& Bytes)
	{
		if (Bytes.Num() > 0)
		{
			FPlatformMemory::Memzero(Bytes.GetData(), Bytes.Num());
		}
		Bytes.Empty();
	}

	/** Binds the blob to its id: a file renamed to another id does not decrypt. */
	TArray<uint8> MakeEntropy(FName Id)
	{
		const FTCHARToUTF8 Utf8(*(FString(TEXT("StageCraft.Credential.")) + Id.ToString()));
		return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}

#if PLATFORM_WINDOWS
	bool Protect(const TArray<uint8>& Plain, const TArray<uint8>& Entropy, TArray<uint8>& OutBlob)
	{
		DATA_BLOB In{ static_cast<DWORD>(Plain.Num()), const_cast<BYTE*>(Plain.GetData()) };
		DATA_BLOB Extra{ static_cast<DWORD>(Entropy.Num()), const_cast<BYTE*>(Entropy.GetData()) };
		DATA_BLOB Out{ 0, nullptr };
		if (!::CryptProtectData(&In, L"StageCraft credential", &Extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &Out))
		{
			return false;
		}
		OutBlob = TArray<uint8>(Out.pbData, static_cast<int32>(Out.cbData));
		::LocalFree(Out.pbData);
		return true;
	}

	bool Unprotect(const TArray<uint8>& Blob, const TArray<uint8>& Entropy, TArray<uint8>& OutPlain)
	{
		DATA_BLOB In{ static_cast<DWORD>(Blob.Num()), const_cast<BYTE*>(Blob.GetData()) };
		DATA_BLOB Extra{ static_cast<DWORD>(Entropy.Num()), const_cast<BYTE*>(Entropy.GetData()) };
		DATA_BLOB Out{ 0, nullptr };
		if (!::CryptUnprotectData(&In, nullptr, &Extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &Out))
		{
			return false;
		}
		OutPlain = TArray<uint8>(Out.pbData, static_cast<int32>(Out.cbData));
		::SecureZeroMemory(Out.pbData, Out.cbData);
		::LocalFree(Out.pbData);
		return true;
	}
#endif
}

FStageCredentialVault::FStageCredentialVault(FString InDirectory)
	: Directory(MoveTemp(InDirectory))
{
}

FString FStageCredentialVault::GetDefaultDirectory()
{
	// The per-user local application data folder on Windows; other Windows users cannot read it.
	return FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("StageCraft"), TEXT("Credentials"));
}

bool FStageCredentialVault::IsSupported()
{
	return PLATFORM_WINDOWS != 0;
}

bool FStageCredentialVault::IsValidId(FName Id)
{
	const FString Text = Id.ToString();
	if (Id.IsNone() || Text.IsEmpty() || Text.Len() > MaxIdLength)
	{
		return false;
	}
	for (const TCHAR Char : Text)
	{
		if (!FChar::IsAlnum(Char) && Char != TEXT('_') && Char != TEXT('-'))
		{
			return false;
		}
	}
	return true;
}

FString FStageCredentialVault::GetPath(FName Id) const
{
	return FPaths::Combine(Directory, Id.ToString() + StageCredentialVaultImpl::FileExtension);
}

EStageVaultResult FStageCredentialVault::Store(FName Id, FStringView Secret) const
{
	using namespace StageCredentialVaultImpl;

	if (!IsValidId(Id) || Secret.IsEmpty() || Secret.Len() > MaxSecretLength)
	{
		return EStageVaultResult::InvalidRequest;
	}
#if PLATFORM_WINDOWS
	TArray<uint8> Plain;
	{
		const FTCHARToUTF8 Utf8(Secret.GetData(), Secret.Len());
		Plain.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		// The converter's own buffer held the key too.
		FPlatformMemory::Memzero(const_cast<ANSICHAR*>(Utf8.Get()), Utf8.Length());
	}

	TArray<uint8> Blob;
	const bool bProtected = Protect(Plain, MakeEntropy(Id), Blob);
	Wipe(Plain);
	if (!bProtected)
	{
		return EStageVaultResult::Failed;
	}

	TArray<uint8> File;
	File.Reserve(HeaderSize + Blob.Num());
	File.Append(Magic, UE_ARRAY_COUNT(Magic));
	File.Add(FormatVersion);
	File.Append(Blob);

	IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
	return StageAtomicFile::WriteBytes(GetPath(Id), TConstArrayView64<uint8>(File.GetData(), File.Num())) ? EStageVaultResult::Success : EStageVaultResult::Failed;
#else
	return EStageVaultResult::Unavailable;
#endif
}

EStageVaultResult FStageCredentialVault::Load(FName Id, FString& OutSecret) const
{
	using namespace StageCredentialVaultImpl;

	WipeString(OutSecret);
	if (!IsValidId(Id))
	{
		return EStageVaultResult::InvalidRequest;
	}
#if PLATFORM_WINDOWS
	const FString Path = GetPath(Id);
	const int64 Size = IFileManager::Get().FileSize(*Path);
	if (Size < 0)
	{
		return EStageVaultResult::NotFound;
	}
	TArray<uint8> File;
	if (Size <= HeaderSize || Size > MaxFileBytes || !FFileHelper::LoadFileToArray(File, *Path, FILEREAD_Silent)
		|| FMemory::Memcmp(File.GetData(), Magic, UE_ARRAY_COUNT(Magic)) != 0 || File[4] != FormatVersion)
	{
		return EStageVaultResult::Failed;
	}

	const TArray<uint8> Blob(File.GetData() + HeaderSize, File.Num() - HeaderSize);
	TArray<uint8> Plain;
	if (!Unprotect(Blob, MakeEntropy(Id), Plain))
	{
		return EStageVaultResult::Failed;
	}
	{
		const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Plain.GetData()), Plain.Num());
		OutSecret = FString::ConstructFromPtrSize(Text.Get(), Text.Length());
		FPlatformMemory::Memzero(const_cast<TCHAR*>(Text.Get()), Text.Length() * sizeof(TCHAR));
	}
	Wipe(Plain);
	if (OutSecret.IsEmpty() || OutSecret.Len() > MaxSecretLength)
	{
		WipeString(OutSecret);
		return EStageVaultResult::Failed;
	}
	return EStageVaultResult::Success;
#else
	return EStageVaultResult::Unavailable;
#endif
}

bool FStageCredentialVault::Contains(FName Id) const
{
	return IsValidId(Id) && IFileManager::Get().FileExists(*GetPath(Id));
}

EStageVaultResult FStageCredentialVault::Remove(FName Id) const
{
	if (!IsValidId(Id))
	{
		return EStageVaultResult::InvalidRequest;
	}
	const FString Path = GetPath(Id);
	if (!IFileManager::Get().FileExists(*Path))
	{
		return EStageVaultResult::Success;
	}
	return IFileManager::Get().Delete(*Path, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true) ? EStageVaultResult::Success : EStageVaultResult::Failed;
}

void FStageCredentialVault::WipeString(FString& Secret)
{
	TArray<TCHAR, FString::AllocatorType>& Chars = Secret.GetCharArray();
	if (Chars.Num() > 0)
	{
		FPlatformMemory::Memzero(Chars.GetData(), Chars.Num() * sizeof(TCHAR));
	}
	Secret.Empty();
}
