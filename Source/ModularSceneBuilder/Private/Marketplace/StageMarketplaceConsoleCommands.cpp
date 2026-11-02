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
#include "Bridge/StageBridgeSubsystem.h"
#include "Marketplace/StageMarketplaceSubsystem.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Player/ModularPlayerController.h"
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

	/** A listing id, or a number from Status. Invalid when neither. */
	FGuid ParseListing(const UStageMarketplaceSubsystem& Marketplace, const FString& Arg)
	{
		FGuid ListingId;
		if (!FGuid::Parse(Arg, ListingId))
		{
			const int32 Number = FCString::Atoi(*Arg);
			if (Marketplace.GetListings().IsValidIndex(Number - 1))
			{
				ListingId = Marketplace.GetListings()[Number - 1].ListingId;
			}
		}
		return ListingId;
	}

	/** <listing number from Status | id>: the panel's UNPUBLISH (ADR 0021 §15). */
	void Unpublish(const TArray<FString>& Args, UWorld* World)
	{
		UStageMarketplaceSubsystem* Marketplace = GetMarketplace(World);
		if (!Marketplace || Args.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Marketplace.Unpublish <listing number from Status | listing id>"));
			return;
		}
		const FStageMarketplaceResultInfo Result = Marketplace->RequestUnpublish(ParseListing(*Marketplace, Args[0]));
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Marketplace.Unpublish %s: %s %s"), *Args[0], *UEnum::GetValueAsString(Result.Code), *Result.Message.ToString());
	}

	/** <listing number from Status | id>: the panel's REMOVE FROM LIBRARY (ADR 0021 §15). */
	void RemoveFromLibrary(const TArray<FString>& Args, UWorld* World)
	{
		UStageMarketplaceSubsystem* Marketplace = GetMarketplace(World);
		if (!Marketplace || Args.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Marketplace.RemoveFromLibrary <listing number from Status | listing id>"));
			return;
		}
		const FStageMarketplaceResultInfo Result = Marketplace->RequestRemoveFromLibrary(ParseListing(*Marketplace, Args[0]));
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Marketplace.RemoveFromLibrary %s: %s %s"), *Args[0], *UEnum::GetValueAsString(Result.Code), *Result.Message.ToString());
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
	void PublishWith(const TArray<FString>& Args, UWorld* World, bool bAllowExport)
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
		Request.bAllowExport = bAllowExport;
		const FStageMarketplaceResultInfo Result = Marketplace->RequestPublish(Request);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Marketplace.Publish \"%s\" (export %s): %s %s"), *Request.SceneName, bAllowExport ? TEXT("allowed") : TEXT("kept in StageCraft"),
			*UEnum::GetValueAsString(Result.Code), *Result.Message.ToString());
	}

	void Publish(const TArray<FString>& Args, UWorld* World) { PublishWith(Args, World, /*bAllowExport*/ false); }
	void PublishExportable(const TArray<FString>& Args, UWorld* World) { PublishWith(Args, World, /*bAllowExport*/ true); }

	FGuid ResolveListing(const UStageMarketplaceSubsystem& Marketplace, const FString& Arg)
	{
		FGuid ListingId;
		if (!FGuid::Parse(Arg, ListingId) && Marketplace.GetListings().IsValidIndex(FCString::Atoi(*Arg) - 1))
		{
			ListingId = Marketplace.GetListings()[FCString::Atoi(*Arg) - 1].ListingId;
		}
		return ListingId;
	}

	/**
	 * <listing number | id> <open | glb <absolute path> | app <client id | first> | tool <tool id>>: places an acquire order through
	 * the player controller, at the listing's current price, exactly as the acquire dialog does (ADR 0022 §11.2).
	 */
	void Order(const TArray<FString>& Args, UWorld* World)
	{
		UStageMarketplaceSubsystem* Marketplace = GetMarketplace(World);
		AModularPlayerController* Controller = World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
		const FGuid ListingId = Marketplace && Args.Num() >= 2 ? ResolveListing(*Marketplace, Args[0]) : FGuid();
		const FStageMarketplaceListing* Listing = Marketplace ? Marketplace->FindListing(ListingId) : nullptr;
		if (!Controller || !Listing)
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Marketplace.Order <listing number | id> <open | glb <path> | app <client id | first> | tool <tool id>>"));
			return;
		}

		FStageAcquireOrder Order;
		Order.ListingId = ListingId;
		Order.ExpectedPrice = Listing->Price;
		const FString Kind = Args[1].ToLower();
		const FString Value = Args.Num() > 2 ? Args[2] : FString();
		if (Kind == TEXT("glb"))
		{
			Order.Target.Kind = EStageDispatchKind::SaveFile;
			Order.Target.FilePath = Value;
		}
		else if (Kind == TEXT("app"))
		{
			Order.Target.Kind = EStageDispatchKind::BridgeApp;
			const UStageBridgeSubsystem* Bridge = World->GetGameInstance()->GetSubsystem<UStageBridgeSubsystem>();
			Order.Target.BridgeClientId = Value == TEXT("first") && Bridge && !Bridge->GetClients().IsEmpty() ? Bridge->GetClients()[0].ClientId : FCString::Atoi(*Value);
		}
		else if (Kind == TEXT("tool"))
		{
			Order.Target.Kind = EStageDispatchKind::AITool;
			Order.Target.ProviderId = FName(*Value);
		}
		const TArray<FStageEconomyResultInfo> Refusals = Controller->RunCapturingRejections([Controller, &Order]() { Controller->RequestAcquireAndDispatch(Order); });
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Marketplace.Order \"%s\" -> %s: %s"), *Listing->Title, *Kind,
			Refusals.IsEmpty() ? TEXT("started") : *FString::Printf(TEXT("refused: %s"), *Refusals[0].Message.ToString()));
	}

	/** <listing number | id>: opens the acquire dialog for it in an open Marketplace panel. */
	void Dialog(const TArray<FString>& Args, UWorld* World)
	{
		UStageMarketplaceSubsystem* Marketplace = GetMarketplace(World);
		const FGuid ListingId = Marketplace && !Args.IsEmpty() ? ResolveListing(*Marketplace, Args[0]) : FGuid();
		for (TObjectIterator<UStageMarketplaceWidget> It; It && ListingId.IsValid(); ++It)
		{
			if (It->IsConstructed())
			{
				It->OpenAcquireDialog(ListingId);
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Marketplace.Dialog: opened for %s."), *ListingId.ToString());
				return;
			}
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Marketplace.Dialog: needs an open Marketplace panel and a listing number from Status."));
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
	FAutoConsoleCommandWithWorldAndArgs PublishExportableCommand(TEXT("StageCraft.Marketplace.PublishExportable"),
		TEXT("<StageDesign|LightingRig|CueList> <credits> <saved scene name> Publishes a saved scene whose buyers may export it (ADR 0022 §11.1)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&PublishExportable));
	FAutoConsoleCommandWithWorldAndArgs OrderCommand(TEXT("StageCraft.Marketplace.Order"),
		TEXT("<number | id> <open | glb <path> | app <client id | first> | tool <tool id>> Gets a listing and sends it to a target, like the acquire dialog."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Order));
	FAutoConsoleCommandWithWorldAndArgs UnpublishCommand(TEXT("StageCraft.Marketplace.Unpublish"), TEXT("<number | id> Unpublishes a listing published from this install."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Unpublish));
	FAutoConsoleCommandWithWorldAndArgs RemoveFromLibraryCommand(TEXT("StageCraft.Marketplace.RemoveFromLibrary"), TEXT("<number | id> Deletes the library copy of an acquired listing (ownership stays)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&RemoveFromLibrary));
	FAutoConsoleCommandWithWorldAndArgs DialogCommand(TEXT("StageCraft.Marketplace.Dialog"), TEXT("<number | id> Opens the acquire dialog in an open Marketplace panel."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Dialog));
}

#endif // !UE_BUILD_SHIPPING
