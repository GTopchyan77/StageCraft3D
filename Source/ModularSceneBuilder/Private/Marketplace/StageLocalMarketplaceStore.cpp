// Copyright Epic Games, Inc. All Rights Reserved.

#include "Marketplace/StageLocalMarketplaceStore.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "IO/StageAtomicFile.h"
#include "Marketplace/StageMarketplaceRules.h"
#include "Marketplace/StagePackage.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace StageLocalMarketplace
{
	const TCHAR* DownloadsFile = TEXT("downloads.json");

	/** Loads a file of at most MaxPackageBytes; TooLarge without reading anything bigger. */
	EStageMarketplaceResult LoadPackageFile(const FString& Path, TArray<uint8>& OutBytes)
	{
		const int64 Size = IFileManager::Get().FileSize(*Path);
		if (Size < 0)
		{
			return EStageMarketplaceResult::NotFound;
		}
		if (Size > StagePackage::MaxPackageBytes)
		{
			return EStageMarketplaceResult::TooLarge;
		}
		return FFileHelper::LoadFileToArray(OutBytes, *Path) ? EStageMarketplaceResult::Success : EStageMarketplaceResult::NotFound;
	}
}

FStageLocalMarketplaceStore::FStageLocalMarketplaceStore(FString InDirectory)
	: Directory(MoveTemp(InDirectory))
{
}

FString FStageLocalMarketplaceStore::GetDefaultDirectory()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("StageCraft"), TEXT("Marketplace"), TEXT("Community"));
}

FString FStageLocalMarketplaceStore::GetPackagePath(const FGuid& ListingId) const
{
	return FPaths::Combine(Directory, ListingId.ToString(EGuidFormats::Digits) + StagePackage::FileExtension);
}

EStageMarketplaceResult FStageLocalMarketplaceStore::Query(const FStageMarketplaceQuery& Query, FStageMarketplaceQueryResult& OutResult) const
{
	OutResult = FStageMarketplaceQueryResult();

	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory, FString(TEXT("*")) + StagePackage::FileExtension), /*Files*/ true, /*Directories*/ false);

	const TMap<FGuid, int64> Downloads = LoadDownloads();
	TArray<FStageMarketplaceListing> Listings;
	Listings.Reserve(Files.Num());
	for (const FString& File : Files)
	{
		FGuid FileId;
		if (!FGuid::ParseExact(FPaths::GetBaseFilename(File), EGuidFormats::Digits, FileId))
		{
			continue; // Not a listing this store wrote.
		}

		TArray<uint8> Bytes;
		FStagePackageManifest Manifest;
		bool bHasThumbnail = false;
		EStageMarketplaceResult Result = StageLocalMarketplace::LoadPackageFile(FPaths::Combine(Directory, File), Bytes);
		if (Result == EStageMarketplaceResult::Success)
		{
			Result = StagePackage::ReadManifest(Bytes, Manifest, bHasThumbnail);
		}
		// A renamed copy would show one listing under two ids, and buying one would not own the other.
		if (Result == EStageMarketplaceResult::Success && Manifest.PackageId != FileId)
		{
			Result = EStageMarketplaceResult::Corrupt;
		}
		if (Result != EStageMarketplaceResult::Success)
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Marketplace: skipped %s in %s (%s)."), *File, *Directory, *UEnum::GetValueAsString(Result));
			continue;
		}

		const int64* Count = Downloads.Find(FileId);
		Listings.Add(StagePackage::ToListing(Manifest, bHasThumbnail, Bytes.Num(), Count ? *Count : 0));
	}

	OutResult = StageMarketplaceRules::Apply(MoveTemp(Listings), Query);
	return EStageMarketplaceResult::Success;
}

EStageMarketplaceResult FStageLocalMarketplaceStore::Fetch(const FGuid& ListingId, bool bCountDownload, TArray<uint8>& OutBytes) const
{
	OutBytes.Reset();
	if (!ListingId.IsValid())
	{
		return EStageMarketplaceResult::InvalidRequest;
	}

	const EStageMarketplaceResult Result = StageLocalMarketplace::LoadPackageFile(GetPackagePath(ListingId), OutBytes);
	if (Result == EStageMarketplaceResult::Success && bCountDownload)
	{
		TMap<FGuid, int64> Downloads = LoadDownloads();
		int64& Count = Downloads.FindOrAdd(ListingId);
		Count = Count < MAX_int64 ? Count + 1 : Count;
		// Popularity is a convenience: a failed write must not fail the download.
		if (!SaveDownloads(Downloads))
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Marketplace: could not update %s in %s."), StageLocalMarketplace::DownloadsFile, *Directory);
		}
	}
	return Result;
}

EStageMarketplaceResult FStageLocalMarketplaceStore::Publish(TConstArrayView<uint8> PackageBytes, FStageMarketplaceListing& OutListing) const
{
	OutListing = FStageMarketplaceListing();

	FStagePackage Package;
	const EStageMarketplaceResult Result = StagePackage::Read(PackageBytes, Package);
	if (Result != EStageMarketplaceResult::Success)
	{
		return Result;
	}

	const FString Path = GetPackagePath(Package.Manifest.PackageId);
	if (IFileManager::Get().FileExists(*Path))
	{
		return EStageMarketplaceResult::AlreadyExists;
	}
	IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
	// KeepExisting: another machine publishing the same id to a shared folder between the check and the write loses, not overwrites.
	if (!StageAtomicFile::WriteBytes(Path, TConstArrayView64<uint8>(PackageBytes.GetData(), PackageBytes.Num()), EStageFileWrite::KeepExisting))
	{
		return IFileManager::Get().FileExists(*Path) ? EStageMarketplaceResult::AlreadyExists : EStageMarketplaceResult::WriteFailed;
	}

	OutListing = StagePackage::ToListing(Package.Manifest, !Package.ThumbnailPng.IsEmpty(), PackageBytes.Num(), 0);
	return EStageMarketplaceResult::Success;
}

int64 FStageLocalMarketplaceStore::GetDownloads(const FGuid& ListingId) const
{
	const int64* Count = LoadDownloads().Find(ListingId);
	return Count ? *Count : 0;
}

TMap<FGuid, int64> FStageLocalMarketplaceStore::LoadDownloads() const
{
	TMap<FGuid, int64> Downloads;
	FString Contents;
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Contents, *FPaths::Combine(Directory, StageLocalMarketplace::DownloadsFile))
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Contents), Root) || !Root.IsValid())
	{
		return Downloads; // Missing or unreadable: every listing starts at zero.
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Root->Values)
	{
		FGuid Id;
		double Count = 0.0;
		if (FGuid::Parse(Pair.Key, Id) && Pair.Value.IsValid() && Pair.Value->TryGetNumber(Count) && FMath::IsFinite(Count) && Count >= 0.0)
		{
			Downloads.Add(Id, static_cast<int64>(FMath::Min(Count, 9.0e15)));
		}
	}
	return Downloads;
}

bool FStageLocalMarketplaceStore::SaveDownloads(const TMap<FGuid, int64>& Downloads) const
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	for (const TPair<FGuid, int64>& Pair : Downloads)
	{
		Root->SetNumberField(Pair.Key.ToString(EGuidFormats::Digits), static_cast<double>(Pair.Value));
	}
	FString Contents;
	FJsonSerializer::Serialize(Root, TJsonWriterFactory<TCHAR>::Create(&Contents));
	IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
	return StageAtomicFile::WriteText(FPaths::Combine(Directory, StageLocalMarketplace::DownloadsFile), Contents);
}
