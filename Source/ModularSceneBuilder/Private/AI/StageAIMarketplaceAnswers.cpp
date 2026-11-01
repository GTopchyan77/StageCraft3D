// Copyright Epic Games, Inc. All Rights Reserved.

// The assistant's words about the community marketplace (Docs/ADR/0021 §13): search results, listing details, purchase offers and
// why a listing reference found nothing. Pure, like the rest of StageAIAnswers; covered by the StageCraft.AI.Marketplace tests.

#include "AI/StageAICommandParser.h"

#define LOCTEXT_NAMESPACE "StageAIMarketplaceAnswers"

namespace StageAIAnswers
{
	namespace
	{
		using StageAILanguage::Pick;

		/** Longest title and author quoted in a reply; longest description excerpt. Listings are community text. */
		constexpr int32 MaxNameLength = 60;
		constexpr int32 MaxDescriptionLength = 240;

		/** Community text on one line and capped, so a listing cannot stretch or break up a reply. */
		FText OneLine(const FString& Text, int32 MaxLength)
		{
			FString Line = Text.Replace(TEXT("\r"), TEXT(" ")).Replace(TEXT("\n"), TEXT(" ")).Replace(TEXT("\t"), TEXT(" ")).TrimStartAndEnd();
			if (Line.Len() > MaxLength)
			{
				Line = Line.Left(MaxLength - 1).TrimEnd() + TEXT("…");
			}
			return FText::FromString(Line);
		}

		FText DescribeCredits(int64 Credits, EStageAILanguage Language)
		{
			return FText::Format(Pick(Language, LOCTEXT("Credits", "{0} credits"), INVTEXT("{0} կրեդիտ")), FText::AsNumber(Credits));
		}

		FText DescribePrice(const FStageAISceneView::FListing& Entry, EStageAILanguage Language)
		{
			const FStageMarketplacePrice& Price = Entry.Listing.Price;
			switch (Entry.Ownership)
			{
			case EStageOwnershipState::Owned:
				return Pick(Language, LOCTEXT("Owned", "owned"), INVTEXT("ձեռք բերված"));
			case EStageOwnershipState::Free:
				return Pick(Language, LOCTEXT("Free", "free"), INVTEXT("անվճար"));
			case EStageOwnershipState::Locked:
				return Price.HasRealMoneyPrice()
					? Pick(Language, LOCTEXT("RealMoney", "a real-money listing, not sold in this version"), INVTEXT("իրական գնով, այս տարբերակում չի վաճառվում"))
					: FText::Format(Pick(Language, LOCTEXT("Locked", "{0}, spending is frozen"), INVTEXT("{0}, ծախսերը սառեցված են")), DescribeCredits(Price.Credits, Language));
			default:
				return DescribeCredits(Price.Credits, Language);
			}
		}

		/** "2. "Festival Rig" by Ana · lighting rig · festival · 250 credits" */
		FText DescribeLine(const FStageAISceneView::FListing& Entry, int32 Number, EStageAILanguage Language)
		{
			return FText::Format(Pick(Language, LOCTEXT("ResultLine", "{0}. \"{1}\" by {2} · {3} · {4} · {5}"), INVTEXT("{0}. «{1}»՝ {2} · {3} · {4} · {5}")),
				FText::AsNumber(Number), OneLine(Entry.Listing.Title, MaxNameLength), OneLine(Entry.Listing.AuthorName, MaxNameLength),
				DescribeListingKind(Entry.Listing.Kind, Language), DescribeListingCategory(Entry.Listing.Category, Language), DescribePrice(Entry, Language));
		}
	}

	FText DescribeListingKind(EStagePackageKind Kind, EStageAILanguage Language)
	{
		switch (Kind)
		{
		case EStagePackageKind::LightingRig:	return Pick(Language, LOCTEXT("KindRig", "lighting rig"), INVTEXT("լուսային ռիգ"));
		case EStagePackageKind::CueList:		return Pick(Language, LOCTEXT("KindCues", "cue list"), INVTEXT("կյուների ցանկ"));
		default:								return Pick(Language, LOCTEXT("KindStage", "stage design"), INVTEXT("բեմի դիզայն"));
		}
	}

	FText DescribeListingCategory(const FGameplayTag& Category, EStageAILanguage Language)
	{
		struct FCategoryName
		{
			FGameplayTag Tag;
			FText English;
			FText Armenian;
		};
		const FCategoryName Names[] = {
			{ StageCraftTags::Marketplace_Category_Concert, LOCTEXT("Concert", "concert"), INVTEXT("համերգ") },
			{ StageCraftTags::Marketplace_Category_Festival, LOCTEXT("Festival", "festival"), INVTEXT("փառատոն") },
			{ StageCraftTags::Marketplace_Category_Theatre, LOCTEXT("Theatre", "theatre"), INVTEXT("թատրոն") },
			{ StageCraftTags::Marketplace_Category_Corporate, LOCTEXT("Corporate", "corporate"), INVTEXT("կորպորատիվ") },
			{ StageCraftTags::Marketplace_Category_Club, LOCTEXT("Club", "club"), INVTEXT("ակումբ") },
			{ StageCraftTags::Marketplace_Category_Broadcast, LOCTEXT("Broadcast", "broadcast"), INVTEXT("հեռարձակում") },
			{ StageCraftTags::Marketplace_Category_Worship, LOCTEXT("Worship", "worship"), INVTEXT("պաշտամունք") },
		};
		for (const FCategoryName& Name : Names)
		{
			if (Category == Name.Tag)
			{
				return Pick(Language, Name.English, Name.Armenian);
			}
		}
		return Pick(Language, LOCTEXT("OtherCategory", "other"), INVTEXT("այլ"));
	}

	FText DescribeMarketplaceResults(const FStageAISceneView& Scene, EStageAILanguage Language)
	{
		if (Scene.Listings.IsEmpty())
		{
			return Pick(Language, LOCTEXT("NoResults", "Nothing in the marketplace matches. Try fewer words, or \"find stages in the marketplace\" to see everything."),
				INVTEXT("Մարքեթփլեյսում համապատասխան ոչինչ չկա։ Փորձեք քիչ բառերով կամ գրեք «գտիր բեմեր մարքեթփլեյսում»՝ ամեն ինչ տեսնելու համար։"));
		}
		TArray<FText> Lines;
		Lines.Add(FText::Format(Pick(Language, LOCTEXT("ResultCount", "{0} {0}|plural(one=listing matches,other=listings match):"), INVTEXT("Համապատասխանում է {0} հայտարարություն՝")),
			FText::AsNumber(FMath::Max(Scene.ListingMatches, Scene.Listings.Num()))));
		const int32 Shown = FMath::Min(Scene.Listings.Num(), MaxListedListings);
		for (int32 Index = 0; Index < Shown; ++Index)
		{
			Lines.Add(DescribeLine(Scene.Listings[Index], Index + 1, Language));
		}
		if (Scene.Listings.Num() > Shown || Scene.ListingMatches > Scene.Listings.Num())
		{
			Lines.Add(Pick(Language, LOCTEXT("MoreInWindow", "The Marketplace window (Edit › Community Marketplace...) shows them all."),
				INVTEXT("Marketplace պատուհանը (Edit › Community Marketplace...) ցույց է տալիս բոլորը։")));
		}
		Lines.Add(Pick(Language, LOCTEXT("ResultHint", "Say \"details of number 1\", \"preview number 1\" or \"buy number 1\"."),
			INVTEXT("Ասեք «մանրամասներ 1-ինի մասին», «նախադիտիր 1-ինը» կամ «գնիր 1-ինը»։")));
		return FText::Join(FText::FromString(TEXT("\n")), Lines);
	}

	FText DescribeListingDetails(const FStageAISceneView::FListing& Entry, int32 Number, EStageAILanguage Language)
	{
		const FStageMarketplaceListing& Listing = Entry.Listing;
		const FText Contents = FText::Format(Pick(Language,
				LOCTEXT("Contents", "{0} {0}|plural(one=item,other=items), {1} {1}|plural(one=fixture,other=fixtures), {2} {2}|plural(one=cue,other=cues); {3} {3}|plural(one=download,other=downloads); published {4}; {5}."),
				INVTEXT("{0} իր, {1} լուսային սարք, {2} կյու. {3} ներբեռնում. հրապարակվել է {4}. {5}։")),
			FText::AsNumber(Listing.ItemCount), FText::AsNumber(Listing.FixtureCount), FText::AsNumber(Listing.CueCount), FText::AsNumber(Listing.Downloads),
			FText::AsDate(Listing.PublishedAtUtc, EDateTimeStyle::Medium, FText::GetInvariantTimeZone()), FText::AsMemory(uint64(FMath::Max<int64>(Listing.PackageBytes, 0))));
		const FText Price = FText::Format(Pick(Language, LOCTEXT("PriceLine", "Price: {0}."), INVTEXT("Գինը՝ {0}։")), DescribePrice(Entry, Language));
		TArray<FText> Lines = { DescribeLine(Entry, Number, Language), Contents, Price };
		if (!Listing.Description.TrimStartAndEnd().IsEmpty())
		{
			Lines.Add(OneLine(Listing.Description, MaxDescriptionLength));
		}
		return FText::Join(FText::FromString(TEXT("\n")), Lines);
	}

	FText DescribePurchaseOffer(const FStageMarketplaceListing& Listing, int64 Credits, EStageAILanguage Language)
	{
		return FText::Format(Pick(Language, LOCTEXT("Offer", "Buy \"{0}\" by {1} for {2}? You have {3}. Say yes to buy it, or no to cancel."),
				INVTEXT("Գնե՞լ «{0}»-ը ({1}) {2}-ով։ Դուք ունեք {3}։ Ասեք «այո»՝ գնելու, կամ «ոչ»՝ չեղարկելու համար։")),
			OneLine(Listing.Title, MaxNameLength), OneLine(Listing.AuthorName, MaxNameLength), DescribeCredits(Listing.Price.Credits, Language), DescribeCredits(Credits, Language));
	}

	FText DescribeListingNotFound(const FStageAISceneView& Scene, const FStageAIListingRef& Ref, const StageAICommandParser::FStageAIListingMatch& Match,
		EStageAILanguage Language)
	{
		if (Scene.Listings.IsEmpty())
		{
			return Pick(Language, LOCTEXT("NoResultsYet", "There are no marketplace results to pick from yet. Search first, for example \"find stages in the marketplace\"."),
				INVTEXT("Դեռ ընտրելու արդյունքներ չկան։ Նախ որոնեք, օրինակ՝ «գտիր բեմեր մարքեթփլեյսում»։"));
		}
		if (!Match.Ambiguous.IsEmpty())
		{
			TArray<FText> Candidates;
			for (const int32 Index : Match.Ambiguous)
			{
				if (Scene.Listings.IsValidIndex(Index) && Candidates.Num() < MaxListedListings)
				{
					Candidates.Add(FText::Format(INVTEXT("{0}. \"{1}\""), FText::AsNumber(Index + 1), OneLine(Scene.Listings[Index].Listing.Title, MaxNameLength)));
				}
			}
			return FText::Format(Pick(Language, LOCTEXT("Ambiguous", "Several listings match \"{0}\": {1}. Say which number."),
					INVTEXT("«{0}»-ին համապատասխանում են մի քանիսը՝ {1}։ Ասեք, թե որ համարը։")),
				OneLine(Ref.Title, MaxNameLength), FText::Join(LOCTEXT("Separator", ", "), Candidates));
		}
		if (Ref.Number > 0)
		{
			return FText::Format(Pick(Language, LOCTEXT("NoNumber", "There is no number {0}: the results have {1} {1}|plural(one=listing,other=listings)."),
					INVTEXT("{0} համար չկա. արդյունքներում կա {1} հայտարարություն։")),
				FText::AsNumber(Ref.Number), FText::AsNumber(Scene.Listings.Num()));
		}
		if (!Ref.Title.IsEmpty())
		{
			return FText::Format(Pick(Language, LOCTEXT("NoTitle", "No listing in the results matches \"{0}\". Search for it first: \"find {0} in the marketplace\"."),
					INVTEXT("Արդյունքներում «{0}»-ին համապատասխանող հայտարարություն չկա։ Նախ որոնեք այն՝ «գտիր {0} մարքեթփլեյսում»։")),
				OneLine(Ref.Title, MaxNameLength));
		}
		return Pick(Language, LOCTEXT("WhichListing", "Which listing? Say its number in the results (for example \"preview number 2\") or its title."),
			INVTEXT("Ո՞ր հայտարարությունը։ Ասեք դրա համարը արդյունքներում (օրինակ՝ «նախադիտիր 2-րդը») կամ անվանումը։"));
	}
}

#undef LOCTEXT_NAMESPACE
