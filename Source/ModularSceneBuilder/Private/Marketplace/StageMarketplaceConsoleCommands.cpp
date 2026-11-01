// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for the community marketplace (Docs/ADR/0021-community-marketplace.md §9), until the Marketplace
// panel exists (Phase B). They call UStageMarketplaceSubsystem exactly as the panel will, so publishing, browsing and acquiring
// can be verified from logs; outcomes are logged by the subsystem when they arrive. Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "Marketplace/StageMarketplaceSubsystem.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "UI/StageMarketplaceWidget.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/SWindow.h"

namespace StageMarketplaceCommands
{
	UStageMarketplaceSubsystem* GetMarketplace(UWorld* World)
	{
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		UStageMarketplaceSubsystem* Marketplace = GameInstance ? GameInstance->GetSubsystem<UStageMarketplaceSubsystem>() : nullptr;
		if (!Marketplace)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Marketplace: no game instance (start PIE or -game first)."));
		}
		return Marketplace;
	}

	FString PriceText(const FStageMarketplacePrice& Price)
	{
		if (Price.HasRealMoneyPrice())
		{
			return FString::Printf(TEXT("%lld %s (minor units)"), Price.RealMoneyMinorUnits, *Price.RealMoneyCurrency);
		}
		return Price.Credits > 0 ? FString::Printf(TEXT("%lld credits"), Price.Credits) : FString(TEXT("free"));
	}

	void Query(const TArray<FString>& Args, UWorld* World)
	{
		if (UStageMarketplaceSubsystem* Marketplace = GetMarketplace(World))
		{
			FStageMarketplaceQuery Query;
			Query.SearchText = FString::Join(Args, TEXT(" "));
			Marketplace->RequestQuery(Query);
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Marketplace.Query \"%s\" sent; StageCraft.Marketplace.Status lists the answer."), *Query.SearchText);
		}
	}

	void Status(const TArray<FString>& Args, UWorld* World)
	{
		const UStageMarketplaceSubsystem* Marketplace = GetMarketplace(World);
		if (!Marketplace)
		{
			return;
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Marketplace.Status: %s, %d of %d listings%s%s"), *Marketplace->GetBackendName().ToString(),
			Marketplace->GetListings().Num(), Marketplace->GetTotalMatches(), Marketplace->IsQueryInFlight() ? TEXT(", query running") : TEXT(""),
			Marketplace->IsPublishing() ? TEXT(", publishing") : TEXT(""));
		int32 Index = 1;
		for (const FStageMarketplaceListing& Listing : Marketplace->GetListings())
		{
			UE_LOG(LogStageCraft, Display, TEXT("  %d. \"%s\" by %s  %s  %s  %d items, %d fixtures, %d cues  %lld downloads  %s  [%s]"), Index++, *Listing.Title,
				*Listing.AuthorName, *UEnum::GetDisplayValueAsText(Listing.Kind).ToString(), *Listing.Category.ToString(), Listing.ItemCount, Listing.FixtureCount,
				Listing.CueCount, Listing.Downloads, *PriceText(Listing.Price), *UEnum::GetDisplayValueAsText(Marketplace->GetListingOwnership(Listing.ListingId)).ToString());
		}
	}

	/** <listing number from Status, or id> */
	void Get(const TArray<FString>& Args, UWorld* World)
	{
		UStageMarketplaceSubsystem* Marketplace = GetMarketplace(World);
		if (!Marketplace || Args.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Marketplace.Get <listing number from Status | listing id>"));
			return;
		}
		FGuid ListingId;
		if (!FGuid::Parse(Args[0], ListingId))
		{
			const int32 Number = FCString::Atoi(*Args[0]);
			if (Marketplace->GetListings().IsValidIndex(Number - 1))
			{
				ListingId = Marketplace->GetListings()[Number - 1].ListingId;
			}
		}
		const FStageMarketplaceResultInfo Result = Marketplace->RequestAcquire(ListingId);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Marketplace.Get %s: %s %s"), *Args[0], *UEnum::GetValueAsString(Result.Code), *Result.Message.ToString());
	}

	/** <listing number from Status | id>: loads the read-only preview; an open Marketplace panel shows it (ADR 0021 §12.3). */
	void Preview(const TArray<FString>& Args, UWorld* World)
	{
		UStageMarketplaceSubsystem* Marketplace = GetMarketplace(World);
		if (!Marketplace || Args.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Marketplace.Preview <listing number from Status | listing id>"));
			return;
		}
		FGuid ListingId;
		if (!FGuid::Parse(Args[0], ListingId) && Marketplace->GetListings().IsValidIndex(FCString::Atoi(*Args[0]) - 1))
		{
			ListingId = Marketplace->GetListings()[FCString::Atoi(*Args[0]) - 1].ListingId;
		}
		const FStageMarketplaceResultInfo Result = Marketplace->RequestPreview(ListingId);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Marketplace.Preview %s: %s %s"), *Args[0], *UEnum::GetValueAsString(Result.Code), *Result.Message.ToString());
	}

	/** Saves the window that hosts the Marketplace panel (a floating window by default) as Saved/Screenshots/Marketplace_<time>.png. */
	void Screenshot(const TArray<FString>& Args, UWorld* World)
	{
		for (TObjectIterator<UStageMarketplaceWidget> It; It; ++It)
		{
			const TSharedPtr<SWidget> Widget = It->GetCachedWidget();
			const TSharedPtr<SWindow> Window = Widget.IsValid() ? FSlateApplication::Get().FindWidgetWindow(Widget.ToSharedRef()) : nullptr;
			if (!Window.IsValid())
			{
				continue;
			}
			TArray<FColor> Pixels;
			FIntVector Size;
			if (!FSlateApplication::Get().TakeScreenshot(Window.ToSharedRef(), Pixels, Size) || Pixels.IsEmpty())
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Marketplace.Screenshot: capture failed."));
				return;
			}
			for (FColor& Pixel : Pixels)
			{
				Pixel.A = 255;
			}
			TArray64<uint8> Png;
			FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
			const FString File = FPaths::ProjectSavedDir() / TEXT("Screenshots") / FString::Printf(TEXT("Marketplace_%s.png"), *FDateTime::Now().ToString(TEXT("%H%M%S")));
			const bool bSaved = FFileHelper::SaveArrayToFile(Png, *File);
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Marketplace.Screenshot: window \"%s\" %dx%d, %s %s."), *Window->GetTitle().ToString(), Size.X, Size.Y,
				bSaved ? TEXT("saved") : TEXT("NOT saved"), *File);
			return;
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Marketplace.Screenshot: the Marketplace window is not open."));
	}

	/** <StageDesign|LightingRig|CueList> <credits> <saved scene name...>; the scene name is also the title. */
	void Publish(const TArray<FString>& Args, UWorld* World)
	{
		UStageMarketplaceSubsystem* Marketplace = GetMarketplace(World);
		const int64 KindValue = Args.Num() > 0 ? StaticEnum<EStagePackageKind>()->GetValueByNameString(Args[0]) : INDEX_NONE;
		if (!Marketplace || Args.Num() < 3 || KindValue == INDEX_NONE)
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Marketplace.Publish <StageDesign|LightingRig|CueList> <credits> <saved scene name>"));
			return;
		}
		FStageMarketplacePublishRequest Request;
		Request.Kind = static_cast<EStagePackageKind>(KindValue);
		Request.Price.Credits = FCString::Atoi64(*Args[1]);
		Request.SceneName = FString::Join(TArrayView<const FString>(Args).RightChop(2), TEXT(" "));
		Request.Title = Request.SceneName;
		Request.Description = TEXT("Published from the console.");
		Request.Category = StageCraftTags::Marketplace_Category_Concert;
		const FStageMarketplaceResultInfo Result = Marketplace->RequestPublish(Request);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Marketplace.Publish \"%s\": %s %s"), *Request.SceneName, *UEnum::GetValueAsString(Result.Code), *Result.Message.ToString());
	}

	FAutoConsoleCommandWithWorldAndArgs QueryCommand(TEXT("StageCraft.Marketplace.Query"), TEXT("[search words] Browses the marketplace (most popular first)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Query));
	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.Marketplace.Status"), TEXT("Logs the listings of the last query and their ownership."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
	FAutoConsoleCommandWithWorldAndArgs GetCommand(TEXT("StageCraft.Marketplace.Get"), TEXT("<number | id> Acquires a listing (buys it with credits if paid) and imports it as a scene."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Get));
	FAutoConsoleCommandWithWorldAndArgs PreviewCommand(TEXT("StageCraft.Marketplace.Preview"), TEXT("<number | id> Loads the read-only 3D preview (shown in an open Marketplace panel)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Preview));
	FAutoConsoleCommandWithWorldAndArgs ScreenshotCommand(TEXT("StageCraft.Marketplace.Screenshot"), TEXT("Saves the Marketplace window to Saved/Screenshots."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Screenshot));
	FAutoConsoleCommandWithWorldAndArgs PublishCommand(TEXT("StageCraft.Marketplace.Publish"), TEXT("<StageDesign|LightingRig|CueList> <credits> <saved scene name> Publishes a saved scene."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Publish));
}

#endif // !UE_BUILD_SHIPPING
