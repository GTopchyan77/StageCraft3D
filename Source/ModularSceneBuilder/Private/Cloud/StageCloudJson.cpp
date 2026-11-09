// Copyright Epic Games, Inc. All Rights Reserved.

#include "Cloud/StageCloudJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Marketplace/StagePackage.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FString FStageCloudResponse::GetBodyAsString() const
{
	if (Body.IsEmpty())
	{
		return FString();
	}
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Body.GetData()), Body.Num());
	return FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());
}

namespace StageCloudJson
{
	namespace
	{
		constexpr int32 MaxErrorCodeLength = 64;
		constexpr int32 MaxErrorMessageLength = 300;
		constexpr int32 MaxIdLength = 64;
		constexpr int32 MaxNameLength = 80;
		constexpr int32 MaxUrlLength = 2048;
		constexpr int32 MaxPageListings = 1000;

		TSharedPtr<FJsonObject> ParseObject(const FString& Body)
		{
			TSharedPtr<FJsonObject> Root;
			if (Body.IsEmpty() || !FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Body), Root))
			{
				return nullptr;
			}
			return Root;
		}

		FString Serialize(const TSharedRef<FJsonObject>& Object)
		{
			FString Out;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
			FJsonSerializer::Serialize(Object, Writer);
			return Out;
		}

		/** Server text for display: control characters become spaces, trimmed, capped. */
		FString Clean(const FString& Text, int32 MaxLength)
		{
			FString Out;
			Out.Reserve(FMath::Min(Text.Len(), MaxLength));
			for (const TCHAR Character : Text)
			{
				Out.AppendChar(FChar::IsControl(Character) ? TEXT(' ') : Character);
				if (Out.Len() > MaxLength * 2)
				{
					break;
				}
			}
			return Out.TrimStartAndEnd().Left(MaxLength);
		}

		/** A whole number in 0..Max; false for anything else (fractions, negatives, NaN, out of range). */
		bool ReadCount(const FJsonObject& Object, const TCHAR* Field, int64 Max, int64& Out)
		{
			double Value = 0.0;
			if (!Object.TryGetNumberField(Field, Value) || !FMath::IsFinite(Value) || Value < 0.0 || Value > double(Max) || FMath::Frac(Value) != 0.0)
			{
				return false;
			}
			Out = int64(Value);
			return true;
		}

		bool IsCurrencyCode(const FString& Code)
		{
			return Code.Len() == 3 && FChar::IsUpper(Code[0]) && FChar::IsUpper(Code[1]) && FChar::IsUpper(Code[2]);
		}

		bool IsLocalHost(const FString& HostAndRest)
		{
			for (const TCHAR* Host : { TEXT("localhost"), TEXT("127.0.0.1"), TEXT("[::1]") })
			{
				const int32 Length = FCString::Strlen(Host);
				if (HostAndRest.StartsWith(Host, ESearchCase::IgnoreCase)
					&& (HostAndRest.Len() == Length || HostAndRest[Length] == TEXT(':') || HostAndRest[Length] == TEXT('/')))
				{
					return true;
				}
			}
			return false;
		}
	}

	FString GuidToWire(const FGuid& Guid)
	{
		return Guid.ToString(EGuidFormats::DigitsWithHyphensLower);
	}

	FString BuildListingsPath(const FStageMarketplaceQuery& Query)
	{
		TArray<FString> Parameters;
		const FString Search = Query.SearchText.TrimStartAndEnd().Left(FStageMarketplaceQuery::MaxSearchTextLength);
		if (!Search.IsEmpty())
		{
			Parameters.Add(TEXT("searchText=") + FGenericPlatformHttp::UrlEncode(Search));
		}
		TArray<FString> Kinds;
		for (const EStagePackageKind Kind : Query.Kinds)
		{
			Kinds.AddUnique(StaticEnum<EStagePackageKind>()->GetNameStringByValue(int64(Kind)));
		}
		if (!Kinds.IsEmpty())
		{
			Parameters.Add(TEXT("kinds=") + FGenericPlatformHttp::UrlEncode(FString::Join(Kinds, TEXT(","))));
		}
		if (Query.Category.IsValid())
		{
			Parameters.Add(TEXT("category=") + FGenericPlatformHttp::UrlEncode(Query.Category.ToString()));
		}
		Parameters.Add(TEXT("priceFilter=") + StaticEnum<EStageMarketplacePriceFilter>()->GetNameStringByValue(int64(Query.PriceFilter)));
		if (Query.MaxCredits > 0)
		{
			Parameters.Add(FString::Printf(TEXT("maxCredits=%lld"), Query.MaxCredits));
		}
		Parameters.Add(TEXT("sort=") + StaticEnum<EStageMarketplaceSort>()->GetNameStringByValue(int64(Query.Sort)));
		Parameters.Add(FString::Printf(TEXT("offset=%d"), FMath::Max(0, Query.Offset)));
		Parameters.Add(FString::Printf(TEXT("limit=%d"), FMath::Clamp(Query.Limit, 1, FStageMarketplaceQuery::MaxPageSize)));
		return TEXT("/v1/listings?") + FString::Join(Parameters, TEXT("&"));
	}

	FString BuildOrderBody(const FGuid& ListingId, const FGuid& IdempotencyKey)
	{
		TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("listingId"), GuidToWire(ListingId));
		Body->SetStringField(TEXT("idempotencyKey"), GuidToWire(IdempotencyKey));
		return Serialize(Body);
	}

	FString BuildDeviceTokenBody(const FString& DeviceCode)
	{
		TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("deviceCode"), DeviceCode);
		return Serialize(Body);
	}

	bool ParseListing(const FJsonObject& Object, FStageMarketplaceListing& Out)
	{
		FString Id;
		FGuid ListingId;
		FString Title;
		if (!Object.TryGetStringField(TEXT("listingId"), Id) || !FGuid::Parse(Id, ListingId) || !ListingId.IsValid()
			|| !Object.TryGetStringField(TEXT("title"), Title) || Clean(Title, StagePackage::MaxTitleLength).IsEmpty())
		{
			return false;
		}
		FString KindName;
		const int64 KindValue = Object.TryGetStringField(TEXT("kind"), KindName) ? StaticEnum<EStagePackageKind>()->GetValueByNameString(KindName) : INDEX_NONE;
		if (KindValue == INDEX_NONE)
		{
			return false;
		}

		// The price decides what the economy asks for: anything odd refuses the whole listing rather than guessing.
		const TSharedPtr<FJsonObject>* Price = nullptr;
		int64 Credits = 0;
		int64 RealMoney = 0;
		FString Currency;
		if (!Object.TryGetObjectField(TEXT("price"), Price) || !Price || !ReadCount(**Price, TEXT("credits"), StagePackage::MaxCreditsPrice, Credits)
			|| !ReadCount(**Price, TEXT("realMoneyMinorUnits"), int64(1) << 40, RealMoney))
		{
			return false;
		}
		(*Price)->TryGetStringField(TEXT("realMoneyCurrency"), Currency);
		if (RealMoney > 0 && !IsCurrencyCode(Currency))
		{
			return false;
		}

		FStageMarketplaceListing Listing;
		Listing.ListingId = ListingId;
		Listing.Title = Clean(Title, StagePackage::MaxTitleLength);
		FString Text;
		Listing.Description = Object.TryGetStringField(TEXT("description"), Text) ? Clean(Text, StagePackage::MaxDescriptionLength) : FString();
		Listing.AuthorName = Object.TryGetStringField(TEXT("authorName"), Text) ? Clean(Text, MaxNameLength) : FString();
		Listing.Kind = EStagePackageKind(KindValue);
		FString CategoryName;
		const FGameplayTag Category = Object.TryGetStringField(TEXT("category"), CategoryName) && CategoryName.Len() <= 128
			? FGameplayTag::RequestGameplayTag(FName(*CategoryName), /*ErrorIfNotFound*/ false) : FGameplayTag();
		Listing.Category = Category.IsValid() && Category.MatchesTag(StageCraftTags::Marketplace_Category) && Category != StageCraftTags::Marketplace_Category
			? Category : FGameplayTag(StageCraftTags::Marketplace_Category_Other);
		const TArray<TSharedPtr<FJsonValue>>* Keywords = nullptr;
		if (Object.TryGetArrayField(TEXT("keywords"), Keywords) && Keywords)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Keywords)
			{
				FString Keyword;
				if (Listing.Keywords.Num() < StagePackage::MaxKeywords && Value.IsValid() && Value->TryGetString(Keyword))
				{
					Keyword = Clean(Keyword, StagePackage::MaxKeywordLength);
					if (!Keyword.IsEmpty())
					{
						Listing.Keywords.Add(Keyword);
					}
				}
			}
		}
		Listing.Price.Credits = Credits;
		Listing.Price.RealMoneyMinorUnits = RealMoney;
		Listing.Price.RealMoneyCurrency = RealMoney > 0 ? Currency : FString();
		int64 Count = 0;
		Listing.Downloads = ReadCount(Object, TEXT("downloads"), MAX_int32, Count) ? Count : 0;
		Listing.ItemCount = ReadCount(Object, TEXT("itemCount"), MAX_int32, Count) ? int32(Count) : 0;
		Listing.FixtureCount = ReadCount(Object, TEXT("fixtureCount"), MAX_int32, Count) ? int32(Count) : 0;
		Listing.CueCount = ReadCount(Object, TEXT("cueCount"), MAX_int32, Count) ? int32(Count) : 0;
		Listing.PackageBytes = ReadCount(Object, TEXT("packageBytes"), StagePackage::MaxPackageBytes, Count) ? Count : 0;
		FString Published;
		if (!Object.TryGetStringField(TEXT("publishedAtUtc"), Published) || !FDateTime::ParseIso8601(*Published, Listing.PublishedAtUtc))
		{
			Listing.PublishedAtUtc = FDateTime();
		}
		bool bFlag = false;
		Listing.bHasThumbnail = Object.TryGetBoolField(TEXT("hasThumbnail"), bFlag) && bFlag;
		bFlag = false;
		Listing.bAllowsExport = Object.TryGetBoolField(TEXT("allowsExport"), bFlag) && bFlag;
		Out = MoveTemp(Listing);
		return true;
	}

	bool ParseListingsPage(const FString& Body, FStageMarketplaceQueryResult& Out, int32& OutDropped)
	{
		OutDropped = 0;
		const TSharedPtr<FJsonObject> Root = ParseObject(Body);
		const TArray<TSharedPtr<FJsonValue>>* Listings = nullptr;
		int64 Total = 0;
		if (!Root.IsValid() || !Root->TryGetArrayField(TEXT("listings"), Listings) || !Listings || !ReadCount(*Root, TEXT("totalMatches"), MAX_int32, Total))
		{
			return false;
		}
		FStageMarketplaceQueryResult Page;
		for (const TSharedPtr<FJsonValue>& Value : *Listings)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FStageMarketplaceListing Listing;
			if (Page.Listings.Num() < MaxPageListings && Value.IsValid() && Value->TryGetObject(Object) && Object && ParseListing(**Object, Listing))
			{
				Page.Listings.Add(MoveTemp(Listing));
			}
			else
			{
				++OutDropped;
			}
		}
		// The total can never be below what this page shows.
		Page.TotalMatches = FMath::Max(int32(Total), Page.Listings.Num());
		Out = MoveTemp(Page);
		return true;
	}

	FStageCloudError ParseError(const FStageCloudResponse& Response)
	{
		FStageCloudError Error;
		Error.Status = Response.bConnected ? Response.Status : 0;
		if (!Response.bConnected)
		{
			return Error;
		}
		const TSharedPtr<FJsonObject> Root = ParseObject(Response.GetBodyAsString());
		const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
		if (Root.IsValid() && Root->TryGetObjectField(TEXT("error"), ErrorObject) && ErrorObject)
		{
			FString Text;
			if ((*ErrorObject)->TryGetStringField(TEXT("code"), Text))
			{
				Error.Code = Clean(Text, MaxErrorCodeLength);
			}
			if ((*ErrorObject)->TryGetStringField(TEXT("message"), Text))
			{
				Error.Message = Clean(Text, MaxErrorMessageLength);
			}
		}
		return Error;
	}

	EStageMarketplaceResult ToMarketplaceResult(const FStageCloudError& Error)
	{
		struct FMapping
		{
			const TCHAR* Code;
			EStageMarketplaceResult Result;
		};
		static const FMapping Mappings[] = {
			{ TEXT("InvalidRequest"), EStageMarketplaceResult::InvalidRequest },
			{ TEXT("NotFound"), EStageMarketplaceResult::NotFound },
			{ TEXT("Corrupt"), EStageMarketplaceResult::Corrupt },
			{ TEXT("VersionMismatch"), EStageMarketplaceResult::VersionMismatch },
			{ TEXT("TooLarge"), EStageMarketplaceResult::TooLarge },
			{ TEXT("HashMismatch"), EStageMarketplaceResult::HashMismatch },
			{ TEXT("AlreadyExists"), EStageMarketplaceResult::AlreadyExists },
			{ TEXT("NotPortable"), EStageMarketplaceResult::NotPortable },
			{ TEXT("NotOwned"), EStageMarketplaceResult::NotOwned },
			{ TEXT("NotOriginal"), EStageMarketplaceResult::NotOriginal },
			{ TEXT("NotYours"), EStageMarketplaceResult::NotYours },
			{ TEXT("Unauthenticated"), EStageMarketplaceResult::SignInRequired },
			{ TEXT("RateLimited"), EStageMarketplaceResult::Busy },
			{ TEXT("PurchasePending"), EStageMarketplaceResult::InUse },
			{ TEXT("SellerNotOnboarded"), EStageMarketplaceResult::ServerRequired },
			{ TEXT("CurrencyNotSupported"), EStageMarketplaceResult::InvalidRequest },
		};
		for (const FMapping& Mapping : Mappings)
		{
			if (Error.Code.Equals(Mapping.Code, ESearchCase::CaseSensitive))
			{
				return Mapping.Result;
			}
		}
		return EStageMarketplaceResult::BackendUnavailable;
	}

	bool ParseDeviceCode(const FString& Body, FStageCloudDeviceCode& Out)
	{
		const TSharedPtr<FJsonObject> Root = ParseObject(Body);
		FStageCloudDeviceCode Code;
		int64 ExpiresIn = 0;
		int64 Interval = 0;
		if (!Root.IsValid() || !Root->TryGetStringField(TEXT("deviceCode"), Code.DeviceCode) || !Root->TryGetStringField(TEXT("userCode"), Code.UserCode)
			|| !Root->TryGetStringField(TEXT("verificationUri"), Code.VerificationUri) || !ReadCount(*Root, TEXT("expiresIn"), 3600, ExpiresIn)
			|| !ReadCount(*Root, TEXT("interval"), 60, Interval))
		{
			return false;
		}
		Root->TryGetStringField(TEXT("verificationUriComplete"), Code.VerificationUriComplete);
		if (Code.DeviceCode.IsEmpty() || Code.DeviceCode.Len() > 128 || Code.UserCode.IsEmpty() || Code.UserCode.Len() > 16 || Clean(Code.UserCode, 16) != Code.UserCode
			|| !IsSafeBrowserUrl(Code.VerificationUri) || (!Code.VerificationUriComplete.IsEmpty() && !IsSafeBrowserUrl(Code.VerificationUriComplete)) || ExpiresIn == 0)
		{
			return false;
		}
		Code.ExpiresInSeconds = int32(ExpiresIn);
		Code.IntervalSeconds = FMath::Max(1, int32(Interval));
		Out = MoveTemp(Code);
		return true;
	}

	FStageCloudDevicePollResult ParseDevicePoll(const FStageCloudResponse& Response)
	{
		FStageCloudDevicePollResult Result;
		if (Response.IsSuccess())
		{
			const TSharedPtr<FJsonObject> Root = ParseObject(Response.GetBodyAsString());
			FString Token;
			if (Root.IsValid() && Root->TryGetStringField(TEXT("sessionToken"), Token) && Token.Len() >= 16 && Token.Len() <= 512
				&& !Token.Contains(TEXT(" ")) && Clean(Token, 512) == Token)
			{
				Result.Outcome = EStageCloudDevicePoll::SignedIn;
				Result.SessionToken = MoveTemp(Token);
			}
			return Result;
		}
		const FStageCloudError Error = ParseError(Response);
		if (Error.Code == TEXT("authorization_pending"))
		{
			Result.Outcome = EStageCloudDevicePoll::Pending;
		}
		else if (Error.Code == TEXT("slow_down"))
		{
			Result.Outcome = EStageCloudDevicePoll::SlowDown;
		}
		else if (Error.Code == TEXT("expired_token") || Error.Code == TEXT("invalid_grant"))
		{
			Result.Outcome = EStageCloudDevicePoll::Expired;
		}
		else if (Error.Code == TEXT("access_denied"))
		{
			Result.Outcome = EStageCloudDevicePoll::Denied;
		}
		return Result;
	}

	bool ParseAccount(const FString& Body, FStageCloudAccount& Out)
	{
		const TSharedPtr<FJsonObject> Root = ParseObject(Body);
		FStageCloudAccount Account;
		if (!Root.IsValid() || !Root->TryGetStringField(TEXT("accountId"), Account.AccountId) || Account.AccountId.IsEmpty() || Account.AccountId.Len() > MaxIdLength)
		{
			return false;
		}
		FString Name;
		Account.DisplayName = Root->TryGetStringField(TEXT("displayName"), Name) ? Clean(Name, MaxNameLength) : FString();
		const TArray<TSharedPtr<FJsonValue>>* Roles = nullptr;
		if (Root->TryGetArrayField(TEXT("roles"), Roles) && Roles)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Roles)
			{
				FString Role;
				if (Account.Roles.Num() < 16 && Value.IsValid() && Value->TryGetString(Role) && Role.Len() <= 32)
				{
					Account.Roles.AddUnique(Role);
				}
			}
		}
		const TSharedPtr<FJsonObject>* Seller = nullptr;
		if (Root->TryGetObjectField(TEXT("seller"), Seller) && Seller)
		{
			FString Status;
			if ((*Seller)->TryGetStringField(TEXT("status"), Status) && (Status == TEXT("none") || Status == TEXT("pending") || Status == TEXT("enabled")))
			{
				Account.SellerStatus = Status;
			}
			bool bEnabled = false;
			Account.bPayoutsEnabled = (*Seller)->TryGetBoolField(TEXT("payoutsEnabled"), bEnabled) && bEnabled;
		}
		Out = MoveTemp(Account);
		return true;
	}

	EStageCloudOrderState ParseOrderState(const FString& Name)
	{
		static const TPair<const TCHAR*, EStageCloudOrderState> States[] = {
			{ TEXT("Created"), EStageCloudOrderState::Created },
			{ TEXT("CheckoutOpen"), EStageCloudOrderState::CheckoutOpen },
			{ TEXT("Paid"), EStageCloudOrderState::Paid },
			{ TEXT("PartiallyRefunded"), EStageCloudOrderState::PartiallyRefunded },
			{ TEXT("Refunded"), EStageCloudOrderState::Refunded },
			{ TEXT("Expired"), EStageCloudOrderState::Expired },
			{ TEXT("Failed"), EStageCloudOrderState::Failed },
			{ TEXT("Disputed"), EStageCloudOrderState::Disputed },
			{ TEXT("OnHold"), EStageCloudOrderState::OnHold },
		};
		for (const TPair<const TCHAR*, EStageCloudOrderState>& State : States)
		{
			if (Name.Equals(State.Key, ESearchCase::CaseSensitive))
			{
				return State.Value;
			}
		}
		return EStageCloudOrderState::Unknown;
	}

	bool ParseOrder(const FString& Body, FStageCloudOrder& Out)
	{
		const TSharedPtr<FJsonObject> Root = ParseObject(Body);
		FStageCloudOrder Order;
		FString ListingId;
		FString State;
		int64 Amount = 0;
		if (!Root.IsValid() || !Root->TryGetStringField(TEXT("orderId"), Order.OrderId) || Order.OrderId.IsEmpty() || Order.OrderId.Len() > MaxIdLength
			|| !Root->TryGetStringField(TEXT("listingId"), ListingId) || !FGuid::Parse(ListingId, Order.ListingId)
			|| !Root->TryGetStringField(TEXT("state"), State) || !ReadCount(*Root, TEXT("amountMinor"), int64(1) << 40, Amount))
		{
			return false;
		}
		// The order id goes into a URL path: only characters that need no encoding.
		for (const TCHAR Character : Order.OrderId)
		{
			if (!FChar::IsAlnum(Character) && Character != TEXT('-') && Character != TEXT('_'))
			{
				return false;
			}
		}
		Order.State = ParseOrderState(State);
		Order.AmountMinor = Amount;
		Root->TryGetStringField(TEXT("currency"), Order.Currency);
		if (Amount > 0 && !IsCurrencyCode(Order.Currency))
		{
			return false;
		}
		FString Checkout;
		if (Order.State == EStageCloudOrderState::CheckoutOpen && Root->TryGetStringField(TEXT("checkoutUrl"), Checkout) && IsSafeBrowserUrl(Checkout))
		{
			Order.CheckoutUrl = Checkout;
		}
		Out = MoveTemp(Order);
		return true;
	}

	bool ParseEntitlements(const FString& Body, TArray<FGuid>& Out)
	{
		const TSharedPtr<FJsonObject> Root = ParseObject(Body);
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!Root.IsValid() || !Root->TryGetArrayField(TEXT("entitlements"), Entries) || !Entries)
		{
			return false;
		}
		TArray<FGuid> Owned;
		for (const TSharedPtr<FJsonValue>& Value : *Entries)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			FString Id;
			FGuid ListingId;
			if (Value.IsValid() && Value->TryGetObject(Entry) && Entry && (*Entry)->TryGetStringField(TEXT("listingId"), Id) && FGuid::Parse(Id, ListingId) && ListingId.IsValid())
			{
				Owned.AddUnique(ListingId);
			}
		}
		Out = MoveTemp(Owned);
		return true;
	}

	bool ParsePackageUrl(const FString& Body, FString& OutUrl)
	{
		const TSharedPtr<FJsonObject> Root = ParseObject(Body);
		FString Url;
		if (!Root.IsValid() || !Root->TryGetStringField(TEXT("url"), Url) || !IsSafeBrowserUrl(Url))
		{
			return false;
		}
		OutUrl = MoveTemp(Url);
		return true;
	}

	bool ParseTicketId(const FString& Body, FString& OutTicketId)
	{
		const TSharedPtr<FJsonObject> Root = ParseObject(Body);
		FString Id;
		if (!Root.IsValid() || !Root->TryGetStringField(TEXT("ticketId"), Id) || Id.IsEmpty() || Id.Len() > MaxIdLength || Clean(Id, MaxIdLength) != Id)
		{
			return false;
		}
		OutTicketId = MoveTemp(Id);
		return true;
	}

	bool IsSafeBrowserUrl(const FString& Url)
	{
		if (Url.IsEmpty() || Url.Len() > MaxUrlLength)
		{
			return false;
		}
		for (const TCHAR Character : Url)
		{
			if (FChar::IsControl(Character) || FChar::IsWhitespace(Character) || Character == TEXT('\\') || Character > 0x7E)
			{
				return false;
			}
		}
		FString Rest;
		if (Url.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase))
		{
			Rest = Url.Mid(8);
		}
		else if (Url.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase))
		{
			Rest = Url.Mid(7);
			if (!IsLocalHost(Rest))
			{
				return false;
			}
		}
		else
		{
			return false;
		}
		// No user:password@ in the authority, and a host is required.
		int32 AuthorityEnd = Rest.Len();
		for (int32 Index = 0; Index < Rest.Len(); ++Index)
		{
			if (Rest[Index] == TEXT('/') || Rest[Index] == TEXT('?') || Rest[Index] == TEXT('#'))
			{
				AuthorityEnd = Index;
				break;
			}
		}
		const FString Authority = Rest.Left(AuthorityEnd);
		return !Authority.IsEmpty() && !Authority.Contains(TEXT("@")) && Authority[0] != TEXT(':');
	}

	bool NormalizeBaseUrl(const FString& Url, FString& OutBaseUrl)
	{
		FString Base = Url.TrimStartAndEnd();
		while (Base.EndsWith(TEXT("/")))
		{
			Base.LeftChopInline(1);
		}
		if (!IsSafeBrowserUrl(Base) || Base.Contains(TEXT("?")) || Base.Contains(TEXT("#")))
		{
			return false;
		}
		OutBaseUrl = MoveTemp(Base);
		return true;
	}
}
