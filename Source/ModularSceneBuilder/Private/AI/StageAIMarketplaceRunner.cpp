// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/StageAIMarketplaceRunner.h"

#include "AI/StageAICommandParser.h"
#include "AI/StageAICommandSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Marketplace/StageMarketplaceSubsystem.h"
#include "Subsystems/StageEconomySubsystem.h"
#include "Workspace/StageWorkspaceSubsystem.h"
#include "Workspace/StageWorkspaceTypes.h"

#define LOCTEXT_NAMESPACE "StageAIMarketplaceRunner"

namespace StageAIMarketplaceRunner
{
	namespace
	{
		using StageAILanguage::Pick;

		/** Longest listing title quoted in a reply (community text). */
		constexpr int32 MaxTitleLength = 60;

		FStageAIActionOutcome Done(const FText& Message) { return FStageAIActionOutcome::Make(EStageAIOutcome::Succeeded, Message); }
		FStageAIActionOutcome Info(const FText& Message) { return FStageAIActionOutcome::Make(EStageAIOutcome::Info, Message); }
		FStageAIActionOutcome Refused(const FText& Message) { return FStageAIActionOutcome::Make(EStageAIOutcome::Refused, Message); }
		FStageAIActionOutcome Pending(const FText& Message) { return FStageAIActionOutcome::Make(EStageAIOutcome::Pending, Message); }

		FText TitleOf(const FStageMarketplaceListing& Listing)
		{
			FString Title = Listing.Title.Replace(TEXT("\n"), TEXT(" ")).Replace(TEXT("\r"), TEXT(" ")).TrimStartAndEnd();
			return FText::FromString(Title.Len() > MaxTitleLength ? Title.Left(MaxTitleLength - 1).TrimEnd() + TEXT("…") : Title);
		}

		FText DescribeCredits(int64 Credits, EStageAILanguage Language)
		{
			return FText::Format(Pick(Language, LOCTEXT("Credits", "{0} credits"), INVTEXT("{0} կրեդիտ")), FText::AsNumber(Credits));
		}

		/** The assistant and the marketplace of the controller's world, or a refusal saying which is missing. */
		bool GetServices(const AModularPlayerController& Controller, EStageAILanguage Language, UStageAICommandSubsystem*& OutAssistant,
			UStageMarketplaceSubsystem*& OutMarketplace, FStageAIActionOutcome& OutRefusal)
		{
			UWorld* World = Controller.GetWorld();
			const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
			OutAssistant = UWorld::GetSubsystem<UStageAICommandSubsystem>(World);
			OutMarketplace = GameInstance ? GameInstance->GetSubsystem<UStageMarketplaceSubsystem>() : nullptr;
			if (OutAssistant && OutMarketplace)
			{
				return true;
			}
			OutRefusal = Refused(Pick(Language, LOCTEXT("NoMarketplace", "The marketplace is not available here."), INVTEXT("Մարքեթփլեյսն այստեղ հասանելի չէ։")));
			return false;
		}

		FStageAIActionOutcome NotInResults(EStageAILanguage Language)
		{
			return Refused(Pick(Language, LOCTEXT("NotInResults", "That listing is no longer in the marketplace results. Search again and pick it from the new list."),
				INVTEXT("Այդ հայտարարությունն այլևս արդյունքներում չէ։ Որոնեք կրկին և ընտրեք նոր ցանկից։")));
		}

		FStageAIActionOutcome Busy(EStageAILanguage Language)
		{
			return Refused(Pick(Language, LOCTEXT("WaitBusy", "I am still waiting for the marketplace; try again in a moment."),
				INVTEXT("Դեռ սպասում եմ մարքեթփլեյսին, փորձեք մի պահից։")));
		}

		FStageAIActionOutcome Search(UStageAICommandSubsystem& Assistant, UStageMarketplaceSubsystem& Marketplace, const FStageAICommand& Command)
		{
			const EStageAILanguage Language = Command.Language;
			TWeakObjectPtr<UStageAICommandSubsystem> WeakAssistant = &Assistant;
			const bool bWaiting = Assistant.AwaitMarketplace(EStageAIMarketplaceEvent::Query, FGuid(), FOnStageAIMarketplaceAnswer::CreateLambda(
				[WeakAssistant, Language](const FGuid&, const FStageMarketplaceResultInfo& Result, const FString&)
				{
					if (UStageAICommandSubsystem* PendingAssistant = WeakAssistant.Get())
					{
						PendingAssistant->FinishPendingAction(Result.IsSuccess()
							? Info(StageAIAnswers::DescribeMarketplaceResults(PendingAssistant->CaptureScene(), Language))
							: Refused(FText::Format(Pick(Language, LOCTEXT("SearchFailed", "The marketplace could not be searched: {0}"), INVTEXT("Մարքեթփլեյսում որոնումը չհաջողվեց՝ {0}")),
								Result.Message)));
					}
				}));
			if (!bWaiting)
			{
				return Busy(Language);
			}
			// The open Marketplace window follows this query too (Docs/ADR/0021 §13.5).
			Marketplace.RequestQuery(Command.MarketplaceQuery);
			return Pending(Pick(Language, LOCTEXT("Searching", "Searching the marketplace…"), INVTEXT("Որոնում եմ մարքեթփլեյսում…")));
		}

		FStageAIActionOutcome Preview(const AModularPlayerController& Controller, UStageAICommandSubsystem& Assistant, UStageMarketplaceSubsystem& Marketplace,
			const FStageAICommand& Command)
		{
			const EStageAILanguage Language = Command.Language;
			const FStageMarketplaceListing* Listing = Marketplace.FindListing(Command.Listing.Id);
			if (!Listing)
			{
				return NotInResults(Language);
			}
			const FText Title = TitleOf(*Listing);

			// The preview is drawn in the Marketplace window: open it first, so it is listening when the verified scene arrives.
			const UGameInstance* GameInstance = Controller.GetGameInstance();
			UStageWorkspaceSubsystem* Workspace = GameInstance ? GameInstance->GetSubsystem<UStageWorkspaceSubsystem>() : nullptr;
			if (!Workspace || !Workspace->OpenPanel(StageCraftTags::Panel_Marketplace))
			{
				return Refused(Pick(Language, LOCTEXT("NoWindow", "The Marketplace window cannot be opened here, so there is nowhere to show the preview."),
					INVTEXT("Marketplace պատուհանն այստեղ չի բացվում, ուստի նախադիտումը ցույց տալու տեղ չկա։")));
			}

			const FGuid ListingId = Listing->ListingId;
			const int32 Items = Listing->ItemCount;
			const int32 Fixtures = Listing->FixtureCount;
			const int32 Cues = Listing->CueCount;
			TWeakObjectPtr<UStageAICommandSubsystem> WeakAssistant = &Assistant;
			const bool bWaiting = Assistant.AwaitMarketplace(EStageAIMarketplaceEvent::Preview, ListingId, FOnStageAIMarketplaceAnswer::CreateLambda(
				[WeakAssistant, ListingId, Title, Items, Fixtures, Cues, Language](const FGuid& PreviewedListing, const FStageMarketplaceResultInfo& Result, const FString&)
				{
					UStageAICommandSubsystem* PendingAssistant = WeakAssistant.Get();
					if (!PendingAssistant)
					{
						return;
					}
					if (PreviewedListing != ListingId)
					{
						PendingAssistant->FinishPendingAction(Info(FText::Format(Pick(Language, LOCTEXT("PreviewReplaced", "Another preview replaced \"{0}\" before it loaded."),
							INVTEXT("Մեկ այլ նախադիտում փոխարինեց «{0}»-ը նախքան այն կբեռնվեր։")), Title)));
						return;
					}
					PendingAssistant->FinishPendingAction(Result.IsSuccess()
						? Done(FText::Format(Pick(Language,
								LOCTEXT("PreviewShown", "Showing \"{0}\" read-only in the Marketplace window: {1} {1}|plural(one=item,other=items), {2} {2}|plural(one=fixture,other=fixtures), {3} {3}|plural(one=cue,other=cues). Drag to orbit, scroll to zoom. Nothing was bought or added to your stage."),
								INVTEXT("«{0}»-ը ցուցադրվում է Marketplace պատուհանում՝ միայն դիտման համար. {1} իր, {2} լուսային սարք, {3} կյու։ Քաշեք՝ պտտելու, անիվով՝ մոտեցնելու համար։ Ոչինչ չի գնվել և չի ավելացվել ձեր բեմին։")),
							Title, FText::AsNumber(Items), FText::AsNumber(Fixtures), FText::AsNumber(Cues)))
						: Refused(FText::Format(Pick(Language, LOCTEXT("PreviewFailed", "\"{0}\" cannot be previewed: {1}"), INVTEXT("«{0}»-ը հնարավոր չէ նախադիտել՝ {1}")),
							Title, Result.Message)));
				}));
			if (!bWaiting)
			{
				return Busy(Language);
			}
			const FStageMarketplaceResultInfo Started = Marketplace.RequestPreview(ListingId);
			if (!Started.IsSuccess())
			{
				Assistant.CancelMarketplaceWait();
				return Refused(FText::Format(Pick(Language, LOCTEXT("PreviewFailed", "\"{0}\" cannot be previewed: {1}"), INVTEXT("«{0}»-ը հնարավոր չէ նախադիտել՝ {1}")),
					Title, Started.Message));
			}
			return Pending(FText::Format(Pick(Language, LOCTEXT("PreviewLoading", "Loading \"{0}\" into the preview…"), INVTEXT("Բեռնում եմ «{0}»-ը նախադիտման համար…")), Title));
		}

		FStageAIActionOutcome Acquire(AModularPlayerController& Controller, UStageAICommandSubsystem& Assistant, UStageMarketplaceSubsystem& Marketplace,
			const FStageAICommand& Command)
		{
			const EStageAILanguage Language = Command.Language;
			const FStageMarketplaceListing* Found = Marketplace.FindListing(Command.Listing.Id);
			if (!Found)
			{
				return NotInResults(Language);
			}
			// A copy: the results can change while the acquisition runs.
			const FStageMarketplaceListing Listing = *Found;
			const FText Title = TitleOf(Listing);
			if (Marketplace.IsAcquiring(Listing.ListingId))
			{
				return Refused(FText::Format(Pick(Language, LOCTEXT("AlreadyAcquiring", "\"{0}\" is already being added."), INVTEXT("«{0}»-ն արդեն ավելացվում է։")), Title));
			}

			const UGameInstance* GameInstance = Controller.GetGameInstance();
			const UStageEconomySubsystem* Economy = GameInstance ? GameInstance->GetSubsystem<UStageEconomySubsystem>() : nullptr;
			const EStageOwnershipState Ownership = Marketplace.GetListingOwnership(Listing.ListingId);
			const bool bPaid = Ownership == EStageOwnershipState::Purchasable || Ownership == EStageOwnershipState::Locked;
			if (bPaid)
			{
				// The economy's own reason first (frozen profile, real money without a server, too few credits): read-only, nothing is spent.
				const FStageEconomyResultInfo Allowed = Economy ? Economy->CanPurchaseListing(Listing.ListingId)
					: FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, Pick(Language, LOCTEXT("NoEconomy", "the shop is not available"), INVTEXT("խանութը հասանելի չէ")));
				if (Ownership == EStageOwnershipState::Locked || !Allowed.IsSuccess())
				{
					const FText Reason = Allowed.IsSuccess() ? Pick(Language, LOCTEXT("NotForSale", "it is not for sale here"), INVTEXT("այստեղ չի վաճառվում")) : Allowed.Message;
					return Refused(FText::Format(Pick(Language, LOCTEXT("CannotBuy", "I can't buy \"{0}\": {1}"), INVTEXT("Չեմ կարող գնել «{0}»-ը՝ {1}")), Title, Reason));
				}
				// Never spent on the assistant's own reading: the user's next prompt confirms it (Docs/ADR/0021 §13.3).
				if (!Command.ConfirmedCredits.IsSet())
				{
					FStageAIActionOutcome Offer = Info(StageAIAnswers::DescribePurchaseOffer(Listing, Assistant.CaptureScene().Credits, Language));
					Offer.PurchaseOffer = FStageAIPurchaseOffer{ Listing.ListingId, Listing.Price.Credits, Listing.Title, Language };
					return Offer;
				}
				if (*Command.ConfirmedCredits != Listing.Price.Credits)
				{
					return Refused(FText::Format(Pick(Language, LOCTEXT("PriceChanged", "The price of \"{0}\" is now {1}, not the {2} you agreed to, so nothing was bought. Say \"buy it\" to see the new offer."),
							INVTEXT("«{0}»-ի գինն այժմ {1} է, ոչ թե ձեր համաձայնած {2}-ը, ուստի ոչինչ չգնվեց։ Ասեք «գնիր այն»՝ նոր առաջարկը տեսնելու համար։")),
						Title, DescribeCredits(Listing.Price.Credits, Language), DescribeCredits(*Command.ConfirmedCredits, Language)));
				}
			}

			// Free, owned, or a confirmed purchase: the panel's own GET / BUY request (economy validate, settle, re-validate, commit).
			const int64 Price = bPaid ? Listing.Price.Credits : 0;
			TWeakObjectPtr<UStageAICommandSubsystem> WeakAssistant = &Assistant;
			const bool bWaiting = Assistant.AwaitMarketplace(EStageAIMarketplaceEvent::Acquire, Listing.ListingId, FOnStageAIMarketplaceAnswer::CreateLambda(
				[WeakAssistant, Title, Price, Language](const FGuid&, const FStageMarketplaceResultInfo& Result, const FString& SceneName)
				{
					UStageAICommandSubsystem* PendingAssistant = WeakAssistant.Get();
					if (!PendingAssistant)
					{
						return;
					}
					if (!Result.IsSuccess())
					{
						PendingAssistant->FinishPendingAction(Refused(FText::Format(Pick(Language, LOCTEXT("AcquireFailed", "\"{0}\" was not added: {1}"),
							INVTEXT("«{0}»-ը չավելացվեց՝ {1}")), Title, Result.Message)));
						return;
					}
					const FText Scene = FText::FromString(SceneName);
					PendingAssistant->FinishPendingAction(Done(Price > 0
						? FText::Format(Pick(Language,
								LOCTEXT("Bought", "Bought \"{0}\" for {1} ({2} left) and added it to your scenes as \"{3}\". Open it with File › Open; your current stage stays as it is until you do."),
								INVTEXT("«{0}»-ը գնվեց {1}-ով (մնաց {2}) և ավելացվեց ձեր տեսարաններին «{3}» անունով։ Բացեք այն File › Open-ով. ձեր ներկա բեմը մնում է անփոփոխ, մինչև դա անեք։")),
							Title, DescribeCredits(Price, Language), DescribeCredits(PendingAssistant->CaptureScene().Credits, Language), Scene)
						: FText::Format(Pick(Language,
								LOCTEXT("Added", "Added \"{0}\" to your scenes as \"{1}\". Open it with File › Open; your current stage stays as it is until you do."),
								INVTEXT("«{0}»-ը ավելացվեց ձեր տեսարաններին «{1}» անունով։ Բացեք այն File › Open-ով. ձեր ներկա բեմը մնում է անփոփոխ, մինչև դա անեք։")),
							Title, Scene)));
				}));
			if (!bWaiting)
			{
				return Busy(Language);
			}
			bool bStarted = false;
			const TArray<FStageEconomyResultInfo> Refusals = Controller.RunCapturingRejections([&]() { bStarted = Controller.RequestMarketplaceAcquire(Listing.ListingId); });
			if (!bStarted)
			{
				Assistant.CancelMarketplaceWait();
				const FText Reason = Refusals.IsEmpty() || Refusals[0].Message.IsEmpty()
					? Pick(Language, LOCTEXT("AcquireRefused", "the request was refused"), INVTEXT("հարցումը մերժվեց"))
					: Refusals[0].Message;
				return Refused(FText::Format(Pick(Language, LOCTEXT("AcquireFailed", "\"{0}\" was not added: {1}"), INVTEXT("«{0}»-ը չավելացվեց՝ {1}")), Title, Reason));
			}
			return Pending(FText::Format(Price > 0
				? Pick(Language, LOCTEXT("Buying", "Buying \"{0}\" for {1}…"), INVTEXT("Գնում եմ «{0}»-ը {1}-ով…"))
				: Pick(Language, LOCTEXT("Getting", "Getting \"{0}\"…"), INVTEXT("Ստանում եմ «{0}»-ը…")), Title, DescribeCredits(Price, Language)));
		}
	}

	FStageAIActionOutcome Run(AModularPlayerController& Controller, const FStageAICommand& Command)
	{
		UStageAICommandSubsystem* Assistant = nullptr;
		UStageMarketplaceSubsystem* Marketplace = nullptr;
		FStageAIActionOutcome Refusal;
		if (!GetServices(Controller, Command.Language, Assistant, Marketplace, Refusal))
		{
			return Refusal;
		}
		switch (Command.Intent)
		{
		case EStageAIIntent::SearchMarketplace:	return Search(*Assistant, *Marketplace, Command);
		case EStageAIIntent::PreviewListing:	return Preview(Controller, *Assistant, *Marketplace, Command);
		case EStageAIIntent::AcquireListing:	return Acquire(Controller, *Assistant, *Marketplace, Command);
		default:
			return Refused(Pick(Command.Language, LOCTEXT("NotMarketplace", "That is not a marketplace request."), INVTEXT("Դա մարքեթփլեյսի հարցում չէ։")));
		}
	}
}

#undef LOCTEXT_NAMESPACE
