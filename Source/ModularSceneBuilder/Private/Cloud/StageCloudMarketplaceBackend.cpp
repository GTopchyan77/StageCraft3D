// Copyright Epic Games, Inc. All Rights Reserved.

#include "Cloud/StageCloudMarketplaceBackend.h"

#include "Cloud/StageCloudJson.h"
#include "Cloud/StageCloudSubsystem.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Marketplace/StagePackage.h"
#include "ModularSceneBuilder.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageCloudMarketplaceBackend)

#define LOCTEXT_NAMESPACE "StageCloudMarketplaceBackend"

namespace StageCloudMarketplace
{
	EStageMarketplaceResult Failure(const FStageCloudResponse& Response)
	{
		const FStageCloudError Error = StageCloudJson::ParseError(Response);
		if (Response.bConnected)
		{
			UE_LOG(LogStageCraft, Log, TEXT("Cloud marketplace: HTTP %d %s."), Error.Status, *Error.Code);
		}
		return StageCloudJson::ToMarketplaceResult(Error);
	}
}

UStageCloudSubsystem* UStageCloudMarketplaceBackend::GetCloud() const
{
	const UGameInstance* GameInstance = GetTypedOuter<UGameInstance>();
	return GameInstance ? GameInstance->GetSubsystem<UStageCloudSubsystem>() : nullptr;
}

bool UStageCloudMarketplaceBackend::UsesCloud() const
{
	const UStageCloudSubsystem* Cloud = GetCloud();
	return Cloud && Cloud->IsOnline();
}

UStageMarketplaceBackend& UStageCloudMarketplaceBackend::GetLocal()
{
	if (!Local)
	{
		Local = NewObject<UStageLocalMarketplaceBackend>(this);
	}
	return *Local;
}

FText UStageCloudMarketplaceBackend::GetDisplayName() const
{
	return UsesCloud() ? LOCTEXT("Cloud", "StageCraft Cloud") : (Local ? Local->GetDisplayName() : LOCTEXT("Community", "Community folder"));
}

void UStageCloudMarketplaceBackend::Query(const FStageMarketplaceQuery& Query, FOnStageMarketplaceQueried OnQueried)
{
	UStageCloudSubsystem* Cloud = GetCloud();
	if (!Cloud || !Cloud->IsOnline())
	{
		GetLocal().Query(Query, MoveTemp(OnQueried));
		return;
	}
	// Browsing needs no account; signed in, the server may also show the user's own listings awaiting review.
	Cloud->SendJson(TEXT("GET"), StageCloudJson::BuildListingsPath(Query), FString(), Cloud->IsSignedIn(),
		FOnStageCloudResponse::CreateLambda([OnQueried](const FStageCloudResponse& Response)
		{
			FStageMarketplaceQueryResult Page;
			int32 Dropped = 0;
			if (!Response.IsSuccess())
			{
				OnQueried.ExecuteIfBound(StageCloudMarketplace::Failure(Response), Page);
				return;
			}
			if (!StageCloudJson::ParseListingsPage(Response.GetBodyAsString(), Page, Dropped))
			{
				OnQueried.ExecuteIfBound(EStageMarketplaceResult::BackendUnavailable, FStageMarketplaceQueryResult());
				return;
			}
			UE_CLOG(Dropped > 0, LogStageCraft, Warning, TEXT("Cloud marketplace: %d listings in the answer were not usable and are not shown."), Dropped);
			OnQueried.ExecuteIfBound(EStageMarketplaceResult::Success, Page);
		}));
}

void UStageCloudMarketplaceBackend::FetchPackage(const FGuid& ListingId, EStageMarketplaceFetchPurpose Purpose, FOnStageMarketplacePackageFetched OnFetched)
{
	UStageCloudSubsystem* Cloud = GetCloud();
	if (!Cloud || !Cloud->IsOnline())
	{
		GetLocal().FetchPackage(ListingId, Purpose, MoveTemp(OnFetched));
		return;
	}
	if (!Cloud->IsSignedIn())
	{
		OnFetched.ExecuteIfBound(EStageMarketplaceResult::SignInRequired, nullptr);
		return;
	}
	// Two steps: the server checks the entitlement and hands out a short-lived address; the bytes come from there without the session.
	const FString Path = FString::Printf(TEXT("/v1/listings/%s/package?purpose=%s&redirect=false"), *StageCloudJson::GuidToWire(ListingId),
		Purpose == EStageMarketplaceFetchPurpose::Preview ? TEXT("preview") : TEXT("acquire"));
	TWeakObjectPtr<UStageCloudSubsystem> WeakCloud = Cloud;
	const float Timeout = DownloadTimeoutSeconds;
	Cloud->SendJson(TEXT("GET"), Path, FString(), /*bAuthenticated*/ true,
		FOnStageCloudResponse::CreateLambda([WeakCloud, OnFetched, Timeout](const FStageCloudResponse& Response)
		{
			FString Url;
			if (!Response.IsSuccess())
			{
				OnFetched.ExecuteIfBound(StageCloudMarketplace::Failure(Response), nullptr);
				return;
			}
			UStageCloudSubsystem* Cloud = WeakCloud.Get();
			if (!Cloud || !StageCloudJson::ParsePackageUrl(Response.GetBodyAsString(), Url))
			{
				OnFetched.ExecuteIfBound(EStageMarketplaceResult::BackendUnavailable, nullptr);
				return;
			}
			Cloud->Download(Url, Timeout, FOnStageCloudResponse::CreateLambda([OnFetched](const FStageCloudResponse& Download)
			{
				if (!Download.IsSuccess())
				{
					OnFetched.ExecuteIfBound(EStageMarketplaceResult::BackendUnavailable, nullptr);
					return;
				}
				if (Download.Body.Num() > StagePackage::MaxPackageBytes)
				{
					OnFetched.ExecuteIfBound(EStageMarketplaceResult::TooLarge, nullptr);
					return;
				}
				OnFetched.ExecuteIfBound(EStageMarketplaceResult::Success, MakeShared<const TArray<uint8>, ESPMode::ThreadSafe>(Download.Body));
			}));
		}));
}

void UStageCloudMarketplaceBackend::Publish(const FStageMarketplacePackageBytes& PackageBytes, FOnStageMarketplacePublished OnPublished)
{
	UStageCloudSubsystem* Cloud = GetCloud();
	if (!Cloud || !Cloud->IsOnline())
	{
		GetLocal().Publish(PackageBytes, MoveTemp(OnPublished));
		return;
	}
	if (!Cloud->IsSignedIn() || !PackageBytes.IsValid())
	{
		OnPublished.ExecuteIfBound(PackageBytes.IsValid() ? EStageMarketplaceResult::SignInRequired : EStageMarketplaceResult::InvalidRequest, FStageMarketplaceListing());
		return;
	}
	Cloud->Send(TEXT("POST"), TEXT("/v1/listings"), *PackageBytes, TEXT("application/octet-stream"), /*bAuthenticated*/ true, DownloadTimeoutSeconds,
		FOnStageCloudResponse::CreateLambda([OnPublished](const FStageCloudResponse& Response)
		{
			FStageMarketplaceListing Listing;
			if (!Response.IsSuccess())
			{
				OnPublished.ExecuteIfBound(StageCloudMarketplace::Failure(Response), Listing);
				return;
			}
			TSharedPtr<FJsonObject> Root;
			const bool bRead = FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Response.GetBodyAsString()), Root) && Root.IsValid()
				&& StageCloudJson::ParseListing(*Root, Listing);
			OnPublished.ExecuteIfBound(bRead ? EStageMarketplaceResult::Success : EStageMarketplaceResult::BackendUnavailable, Listing);
		}));
}

void UStageCloudMarketplaceBackend::Unpublish(const FGuid& ListingId, FOnStageMarketplaceUnpublished OnUnpublished)
{
	UStageCloudSubsystem* Cloud = GetCloud();
	if (!Cloud || !Cloud->IsOnline())
	{
		GetLocal().Unpublish(ListingId, MoveTemp(OnUnpublished));
		return;
	}
	if (!Cloud->IsSignedIn())
	{
		OnUnpublished.ExecuteIfBound(EStageMarketplaceResult::SignInRequired);
		return;
	}
	Cloud->SendJson(TEXT("DELETE"), FString::Printf(TEXT("/v1/listings/%s"), *StageCloudJson::GuidToWire(ListingId)), FString(), /*bAuthenticated*/ true,
		FOnStageCloudResponse::CreateLambda([OnUnpublished](const FStageCloudResponse& Response)
		{
			OnUnpublished.ExecuteIfBound(Response.IsSuccess() ? EStageMarketplaceResult::Success : StageCloudMarketplace::Failure(Response));
		}));
}

#undef LOCTEXT_NAMESPACE
