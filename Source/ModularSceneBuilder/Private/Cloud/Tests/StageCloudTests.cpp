// Copyright Epic Games, Inc. All Rights Reserved.

// Tests of the StageCraft Cloud client (Docs/ADR/0032 §4–§7, STATE.md #77): the wire format and its validation, device sign-in and the
// session, the cloud marketplace and store backends (real-money checkout, idempotent re-orders, refusals, cancel, retries), and sending
// support reports. A fake server answers through UStageCloudSubsystem's test transport: no network, no browser, no real money.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Cloud; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Cloud/StageCloudCommerceBackend.h"
#include "Cloud/StageCloudJson.h"
#include "Cloud/StageCloudMarketplaceBackend.h"
#include "Cloud/StageCloudSubsystem.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Marketplace/StageLocalMarketplaceStore.h"
#include "Marketplace/StageMarketplaceRules.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Services/StageCredentialVault.h"
#include "Support/StageSupportSubsystem.h"
#include "Tests/StageTestFixtures.h"
#include "Tests/StageTestGameInstance.h"

namespace StageCloudTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;
	const TCHAR* const Server = TEXT("https://cloud.test");
	const TCHAR* const Token = TEXT("scs_session_token_0123456789abcdef");

	FStageCloudResponse Answer(int32 Status, const FString& Json)
	{
		FStageCloudResponse Response;
		Response.bConnected = true;
		Response.Status = Status;
		const FTCHARToUTF8 Utf8(*Json, Json.Len());
		Response.Body = TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		return Response;
	}

	FStageCloudResponse ErrorAnswer(int32 Status, const TCHAR* Code, const TCHAR* Message = TEXT("refused"))
	{
		return Answer(Status, FString::Printf(TEXT("{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}"), Code, Message));
	}

	FString ListingJson(const FGuid& Id, const TCHAR* PriceJson = TEXT("{\"credits\":0,\"realMoneyMinorUnits\":1500,\"realMoneyCurrency\":\"USD\"}"))
	{
		return FString::Printf(TEXT("{\"listingId\":\"%s\",\"title\":\"Arena Rig\",\"description\":\"Big.\",\"authorName\":\"Seller\",\"kind\":\"LightingRig\",")
			TEXT("\"category\":\"StageCraft.Marketplace.Category.Concert\",\"keywords\":[\"arena\"],\"price\":%s,\"downloads\":3,")
			TEXT("\"publishedAtUtc\":\"2026-11-07T10:00:00.000Z\",\"itemCount\":12,\"fixtureCount\":8,\"cueCount\":2,\"packageBytes\":2048,")
			TEXT("\"hasThumbnail\":true,\"allowsExport\":false}"), *StageCloudJson::GuidToWire(Id), PriceJson);
	}

	FString OrderJson(const FGuid& ListingId, const TCHAR* State, const TCHAR* CheckoutUrl = nullptr)
	{
		return FString::Printf(TEXT("{\"orderId\":\"ord-1\",\"listingId\":\"%s\",\"state\":\"%s\",\"checkoutUrl\":%s,\"amountMinor\":1500,\"currency\":\"USD\"}"),
			*StageCloudJson::GuidToWire(ListingId), State, CheckoutUrl ? *FString::Printf(TEXT("\"%s\""), CheckoutUrl) : TEXT("null"));
	}

	/** A fake StageCraft Cloud: every request is recorded and answered by Handler (now, or later through Held). */
	struct FFakeServer
	{
		TArray<FStageCloudHttpRequest> Requests;
		TFunction<TOptional<FStageCloudResponse>(const FStageCloudHttpRequest&)> Handler;
		TArray<TPair<FStageCloudResponse, FOnStageCloudResponse>> Held;

		UStageCloudSubsystem::FTestTransport Transport()
		{
			return [this](const FStageCloudHttpRequest& Request, FOnStageCloudResponse OnDone)
			{
				Requests.Add(Request);
				TOptional<FStageCloudResponse> Response = Handler ? Handler(Request) : TOptional<FStageCloudResponse>();
				if (Response.IsSet())
				{
					OnDone.ExecuteIfBound(*Response);
				}
				else
				{
					Held.Emplace(FStageCloudResponse(), MoveTemp(OnDone));
				}
			};
		}

		int32 Count(const TCHAR* PathPart) const
		{
			return Requests.FilterByPredicate([PathPart](const FStageCloudHttpRequest& Request) { return Request.Url.Contains(PathPart); }).Num();
		}

		FString BodyOf(const FStageCloudHttpRequest& Request) const
		{
			const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
			return FString::ConstructFromPtrSize(Text.Get(), Text.Length());
		}
	};

	/** Signs Cloud in through the device flow against Server. */
	bool SignIn(FStageTestGameInstance& Game, UStageCloudSubsystem& Cloud, FFakeServer& Remote)
	{
		const TSharedRef<int32> Polls = MakeShared<int32>(0);
		Remote.Handler = [Polls](const FStageCloudHttpRequest& Request) -> TOptional<FStageCloudResponse>
		{
			if (Request.Url.EndsWith(TEXT("/v1/auth/device")))
			{
				return Answer(200, TEXT("{\"deviceCode\":\"scd_device\",\"userCode\":\"WXYZ-1234\",\"verificationUri\":\"https://cloud.test/auth/verify\",")
					TEXT("\"verificationUriComplete\":\"https://cloud.test/auth/verify?user_code=WXYZ-1234\",\"expiresIn\":600,\"interval\":1}"));
			}
			if (Request.Url.EndsWith(TEXT("/v1/auth/device/token")))
			{
				return ++(*Polls) < 2 ? ErrorAnswer(400, TEXT("authorization_pending"))
					: Answer(200, FString::Printf(TEXT("{\"sessionToken\":\"%s\",\"expiresAt\":\"2026-12-07T00:00:00Z\",\"accountId\":\"acc-1\"}"), Token));
			}
			if (Request.Url.EndsWith(TEXT("/v1/me")))
			{
				return Answer(200, TEXT("{\"accountId\":\"acc-1\",\"displayName\":\"Gevor\",\"email\":null,\"roles\":[\"admin\"],")
					TEXT("\"seller\":{\"status\":\"enabled\",\"payoutMethod\":\"manual\",\"payoutsEnabled\":true,\"country\":\"AM\"}}"));
			}
			return ErrorAnswer(404, TEXT("NotFound"));
		};
		FText Error;
		return Cloud.StartSignIn(Error) && Game.Pump([&Cloud]() { return Cloud.IsSignedIn() && !Cloud.GetAccount().DisplayName.IsEmpty(); }, 10.0);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageCloudJsonTest, "StageCraft.Cloud.Json", StageCloudTests::Flags)

bool FStageCloudJsonTest::RunTest(const FString& Parameters)
{
	using namespace StageCloudTests;
	const FGuid Id(0x11111111, 0x22222222, 0x33333333, 0x44444444);

	// Listings: validated and capped like a manifest; anything odd about the price refuses the listing.
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(ListingJson(Id)), Object);
	FStageMarketplaceListing Listing;
	if (!TestTrue(TEXT("Listing reads"), Object.IsValid() && StageCloudJson::ParseListing(*Object, Listing)))
	{
		return false;
	}
	TestTrue(TEXT("Id"), Listing.ListingId == Id);
	TestTrue(TEXT("Kind"), Listing.Kind == EStagePackageKind::LightingRig);
	TestTrue(TEXT("Category"), Listing.Category == StageCraftTags::Marketplace_Category_Concert);
	TestEqual(TEXT("Real-money price"), Listing.Price.RealMoneyMinorUnits, int64(1500));
	TestEqual(TEXT("Currency"), Listing.Price.RealMoneyCurrency, FString(TEXT("USD")));
	TestTrue(TEXT("Published"), Listing.PublishedAtUtc == FDateTime(2026, 11, 7, 10, 0, 0));
	TestTrue(TEXT("Thumbnail"), Listing.bHasThumbnail);
	const auto Reads = [](const FString& Json)
	{
		TSharedPtr<FJsonObject> Parsed;
		FStageMarketplaceListing Out;
		return FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Json), Parsed) && Parsed.IsValid() && StageCloudJson::ParseListing(*Parsed, Out);
	};
	TestFalse(TEXT("Negative price"), Reads(ListingJson(Id, TEXT("{\"credits\":-1,\"realMoneyMinorUnits\":0}"))));
	TestFalse(TEXT("Fractional price"), Reads(ListingJson(Id, TEXT("{\"credits\":1.5,\"realMoneyMinorUnits\":0}"))));
	TestFalse(TEXT("Credits over the cap"), Reads(ListingJson(Id, TEXT("{\"credits\":2000000,\"realMoneyMinorUnits\":0}"))));
	TestFalse(TEXT("Money without a currency"), Reads(ListingJson(Id, TEXT("{\"credits\":0,\"realMoneyMinorUnits\":100,\"realMoneyCurrency\":\"usd\"}"))));
	TestFalse(TEXT("No price"), Reads(ListingJson(Id, TEXT("7"))));
	TestFalse(TEXT("Bad id"), Reads(ListingJson(FGuid())));
	TestFalse(TEXT("Unknown kind"), Reads(ListingJson(Id).Replace(TEXT("LightingRig"), TEXT("Spaceship"))));
	TestTrue(TEXT("Free listing"), Reads(ListingJson(Id, TEXT("{\"credits\":0,\"realMoneyMinorUnits\":0,\"realMoneyCurrency\":\"\"}"))));
	FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(ListingJson(Id).Replace(TEXT("StageCraft.Marketplace.Category.Concert"), TEXT("StageCraft.Category.Truss"))), Object);
	StageCloudJson::ParseListing(*Object, Listing);
	TestTrue(TEXT("A tag outside the marketplace categories reads as Other"), Listing.Category == StageCraftTags::Marketplace_Category_Other);

	FStageMarketplaceQueryResult Page;
	int32 Dropped = 0;
	TestTrue(TEXT("Page reads"), StageCloudJson::ParseListingsPage(FString::Printf(TEXT("{\"listings\":[%s,{\"listingId\":\"x\"},5],\"totalMatches\":0}"), *ListingJson(Id)), Page, Dropped));
	TestEqual(TEXT("Good listing kept"), Page.Listings.Num(), 1);
	TestEqual(TEXT("Bad ones counted"), Dropped, 2);
	TestEqual(TEXT("Total never below the page"), Page.TotalMatches, 1);
	TestFalse(TEXT("Not a page"), StageCloudJson::ParseListingsPage(TEXT("[]"), Page, Dropped));

	// Requests.
	FStageMarketplaceQuery Query;
	Query.SearchText = TEXT("big arena & co");
	Query.Kinds = { EStagePackageKind::LightingRig, EStagePackageKind::CueList, EStagePackageKind::LightingRig };
	Query.Category = StageCraftTags::Marketplace_Category_Concert;
	Query.PriceFilter = EStageMarketplacePriceFilter::Paid;
	Query.Sort = EStageMarketplaceSort::Newest;
	Query.Offset = -4;
	Query.Limit = 900;
	const FString Path = StageCloudJson::BuildListingsPath(Query);
	TestTrue(TEXT("Search encoded"), Path.Contains(TEXT("searchText=big%20arena%20%26%20co")));
	TestTrue(TEXT("Kinds once each"), Path.Contains(TEXT("kinds=LightingRig%2CCueList")));
	TestTrue(TEXT("Category"), Path.Contains(TEXT("category=StageCraft.Marketplace.Category.Concert")));
	TestTrue(TEXT("Sort and filter names"), Path.Contains(TEXT("priceFilter=Paid")) && Path.Contains(TEXT("sort=Newest")));
	TestTrue(TEXT("Paging clamped"), Path.Contains(TEXT("offset=0")) && Path.Contains(TEXT("limit=200")));
	TestTrue(TEXT("Order body"), StageCloudJson::BuildOrderBody(Id, FGuid(1, 2, 3, 4)).Contains(TEXT("\"idempotencyKey\":\"00000001-0000-0002-0000-000300000004\"")));

	// Errors and what they mean.
	const FStageCloudError Error = StageCloudJson::ParseError(ErrorAnswer(403, TEXT("NotOwned"), TEXT("buy it first")));
	TestEqual(TEXT("Code"), Error.Code, FString(TEXT("NotOwned")));
	TestEqual(TEXT("Message"), Error.Message, FString(TEXT("buy it first")));
	TestTrue(TEXT("NotOwned"), StageCloudJson::ToMarketplaceResult(Error) == EStageMarketplaceResult::NotOwned);
	TestTrue(TEXT("Sign in"), StageCloudJson::ToMarketplaceResult(StageCloudJson::ParseError(ErrorAnswer(401, TEXT("Unauthenticated")))) == EStageMarketplaceResult::SignInRequired);
	TestTrue(TEXT("Unknown code denies"), StageCloudJson::ToMarketplaceResult(StageCloudJson::ParseError(ErrorAnswer(418, TEXT("Teapot")))) == EStageMarketplaceResult::BackendUnavailable);
	TestTrue(TEXT("Case matters"), StageCloudJson::ToMarketplaceResult(StageCloudJson::ParseError(ErrorAnswer(403, TEXT("notowned")))) == EStageMarketplaceResult::BackendUnavailable);
	TestEqual(TEXT("No connection is status 0"), StageCloudJson::ParseError(FStageCloudResponse()).Status, 0);
	TestEqual(TEXT("Huge messages capped"), StageCloudJson::ParseError(ErrorAnswer(500, TEXT("Internal"), *FString::ChrN(5000, TEXT('m')))).Message.Len(), 300);

	// Device flow.
	FStageCloudDeviceCode Code;
	TestTrue(TEXT("Device code"), StageCloudJson::ParseDeviceCode(TEXT("{\"deviceCode\":\"d\",\"userCode\":\"AB-12\",\"verificationUri\":\"https://x.test/v\",\"expiresIn\":600,\"interval\":5}"), Code));
	TestFalse(TEXT("Device page must be safe"), StageCloudJson::ParseDeviceCode(TEXT("{\"deviceCode\":\"d\",\"userCode\":\"AB\",\"verificationUri\":\"javascript:alert(1)\",\"expiresIn\":600,\"interval\":5}"), Code));
	TestFalse(TEXT("No interval"), StageCloudJson::ParseDeviceCode(TEXT("{\"deviceCode\":\"d\",\"userCode\":\"AB\",\"verificationUri\":\"https://x.test/v\",\"expiresIn\":600}"), Code));
	TestTrue(TEXT("Pending"), StageCloudJson::ParseDevicePoll(ErrorAnswer(400, TEXT("authorization_pending"))).Outcome == EStageCloudDevicePoll::Pending);
	TestTrue(TEXT("Slow down"), StageCloudJson::ParseDevicePoll(ErrorAnswer(400, TEXT("slow_down"))).Outcome == EStageCloudDevicePoll::SlowDown);
	TestTrue(TEXT("Expired"), StageCloudJson::ParseDevicePoll(ErrorAnswer(400, TEXT("expired_token"))).Outcome == EStageCloudDevicePoll::Expired);
	TestTrue(TEXT("Denied"), StageCloudJson::ParseDevicePoll(ErrorAnswer(400, TEXT("access_denied"))).Outcome == EStageCloudDevicePoll::Denied);
	TestTrue(TEXT("Signed in"), StageCloudJson::ParseDevicePoll(Answer(200, FString::Printf(TEXT("{\"sessionToken\":\"%s\"}"), Token))).Outcome == EStageCloudDevicePoll::SignedIn);
	TestTrue(TEXT("Short token refused"), StageCloudJson::ParseDevicePoll(Answer(200, TEXT("{\"sessionToken\":\"abc\"}"))).Outcome == EStageCloudDevicePoll::Failed);

	// Account, orders, entitlements, downloads, tickets.
	FStageCloudAccount Account;
	TestTrue(TEXT("Account"), StageCloudJson::ParseAccount(TEXT("{\"accountId\":\"a1\",\"displayName\":\"G\",\"roles\":[\"admin\"],\"seller\":{\"status\":\"pending\",\"payoutsEnabled\":false}}"), Account));
	TestTrue(TEXT("Admin"), Account.IsAdmin());
	TestEqual(TEXT("Seller status"), Account.SellerStatus, FString(TEXT("pending")));
	FStageCloudOrder Order;
	TestTrue(TEXT("Order"), StageCloudJson::ParseOrder(OrderJson(Id, TEXT("CheckoutOpen"), TEXT("https://checkout.stripe.com/c/pay/cs_test_1")), Order));
	TestTrue(TEXT("Checkout open"), Order.State == EStageCloudOrderState::CheckoutOpen && Order.CheckoutUrl.StartsWith(TEXT("https://checkout.stripe.com")));
	TestTrue(TEXT("Unsafe checkout dropped"), StageCloudJson::ParseOrder(OrderJson(Id, TEXT("CheckoutOpen"), TEXT("http://evil.test/pay")), Order) && Order.CheckoutUrl.IsEmpty());
	TestTrue(TEXT("Paid"), StageCloudJson::ParseOrder(OrderJson(Id, TEXT("Paid")), Order) && Order.IsPaid() && Order.IsSettled());
	TestTrue(TEXT("Unknown state"), StageCloudJson::ParseOrder(OrderJson(Id, TEXT("Teleported")), Order) && Order.State == EStageCloudOrderState::Unknown && !Order.IsSettled());
	TestFalse(TEXT("Order id must be URL-safe"), StageCloudJson::ParseOrder(OrderJson(Id, TEXT("Paid")).Replace(TEXT("ord-1"), TEXT("../admin")), Order));
	TArray<FGuid> Owned;
	TestTrue(TEXT("Entitlements"), StageCloudJson::ParseEntitlements(FString::Printf(TEXT("{\"entitlements\":[{\"listingId\":\"%s\"},{\"listingId\":\"x\"}]}"), *StageCloudJson::GuidToWire(Id)), Owned));
	TestEqual(TEXT("Only valid ids"), Owned.Num(), 1);
	FString Url;
	TestTrue(TEXT("Package url"), StageCloudJson::ParsePackageUrl(TEXT("{\"url\":\"https://bucket.s3.amazonaws.com/p?X-Amz-Signature=abc\"}"), Url));
	TestFalse(TEXT("Package url over http"), StageCloudJson::ParsePackageUrl(TEXT("{\"url\":\"http://bucket.example.com/p\"}"), Url));
	FString Ticket;
	TestTrue(TEXT("Ticket"), StageCloudJson::ParseTicketId(TEXT("{\"ticketId\":\"5b0c\",\"status\":\"open\"}"), Ticket) && Ticket == TEXT("5b0c"));

	// Addresses.
	for (const TCHAR* Safe : { TEXT("https://a.test"), TEXT("https://a.test:8443/x?y=1#z"), TEXT("http://127.0.0.1:8787/auth"), TEXT("http://localhost/x") })
	{
		TestTrue(FString::Printf(TEXT("Safe: %s"), Safe), StageCloudJson::IsSafeBrowserUrl(Safe));
	}
	for (const TCHAR* Unsafe : { TEXT(""), TEXT("http://a.test"), TEXT("file:///C:/x"), TEXT("javascript:alert(1)"), TEXT("https://user:pw@a.test"),
		TEXT("https://"), TEXT("https:///x"), TEXT("https://a.test/x y"), TEXT("https://a.test\\x"), TEXT("http://localhost.evil.test/") })
	{
		TestFalse(FString::Printf(TEXT("Unsafe: '%s'"), Unsafe), StageCloudJson::IsSafeBrowserUrl(Unsafe));
	}
	FString Base;
	TestTrue(TEXT("Base trimmed"), StageCloudJson::NormalizeBaseUrl(TEXT(" https://cloud.test/ "), Base) && Base == TEXT("https://cloud.test"));
	TestFalse(TEXT("Base with a query"), StageCloudJson::NormalizeBaseUrl(TEXT("https://cloud.test/?a=1"), Base));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageCloudSignInTest, "StageCraft.Cloud.SignIn", StageCloudTests::Flags)

bool FStageCloudSignInTest::RunTest(const FString& Parameters)
{
	using namespace StageCloudTests;

	FStageTestGameInstance Game;
	UStageCloudSubsystem* Cloud = Game.Subsystem<UStageCloudSubsystem>();
	if (!TestTrue(TEXT("Game instance"), Game.IsValid()) || !TestNotNull(TEXT("Cloud subsystem"), Cloud))
	{
		return false;
	}
	TestTrue(TEXT("Offline without a server address"), Cloud->GetState() == EStageCloudState::Offline);
	FText Error;
	TestFalse(TEXT("No sign-in offline"), Cloud->StartSignIn(Error));
	bool bAnswered = false;
	Cloud->SendJson(TEXT("GET"), TEXT("/v1/me"), FString(), false, FOnStageCloudResponse::CreateLambda([&bAnswered](const FStageCloudResponse& Response) { bAnswered = !Response.bConnected; }));
	TestTrue(TEXT("Offline requests answer at once, unsent"), bAnswered);

	FFakeServer Fake;
	Cloud->SetTransportForTesting(Fake.Transport());
	Cloud->SetBaseUrlForTesting(TEXT("http://cloud.test"));
	TestTrue(TEXT("Plain http to another host is refused"), Cloud->GetState() == EStageCloudState::Offline);
	Cloud->SetBaseUrlForTesting(Server);
	TestTrue(TEXT("Signed out"), Cloud->GetState() == EStageCloudState::SignedOut);
	if (!TestTrue(TEXT("Device sign-in completes"), SignIn(Game, *Cloud, Fake)))
	{
		return false;
	}
	TestEqual(TEXT("Account read"), Cloud->GetAccount().DisplayName, FString(TEXT("Gevor")));
	TestTrue(TEXT("The verification page was opened"), Cloud->GetOpenedUrlsForTesting().Contains(TEXT("https://cloud.test/auth/verify?user_code=WXYZ-1234")));
	TestTrue(TEXT("Polled until approved"), Fake.Count(TEXT("/v1/auth/device/token")) >= 2);
	const FStageCloudHttpRequest* Me = Fake.Requests.FindByPredicate([](const FStageCloudHttpRequest& Request) { return Request.Url.EndsWith(TEXT("/v1/me")); });
	TestTrue(TEXT("Session sent as a bearer token"), Me && Me->Headers.FindRef(TEXT("Authorization")) == FString(TEXT("Bearer ")) + Token);
	const FStageCloudHttpRequest* Start = Fake.Requests.FindByPredicate([](const FStageCloudHttpRequest& Request) { return Request.Url.EndsWith(TEXT("/v1/auth/device")); });
	TestTrue(TEXT("No session on unauthenticated calls"), Start && !Start->Headers.Contains(TEXT("Authorization")));
	TestTrue(TEXT("The token is a secret for support reports"), Cloud->GetSecretsToRedact().Contains(Token));

	// Stored in the vault, bound to this server: a new start of the same server is signed in; another server is not.
	if (FStageCredentialVault::IsSupported())
	{
		TestTrue(TEXT("Session in the vault"), FStageCredentialVault(FStageCredentialVault::GetDefaultDirectory()).Contains(TEXT("StageCraftCloud")));
		Cloud->SetBaseUrlForTesting(TEXT("https://other.test"));
		TestTrue(TEXT("Not signed in on another server"), Cloud->GetState() == EStageCloudState::SignedOut);
		Cloud->SetBaseUrlForTesting(Server);
		TestTrue(TEXT("Signed in again from the vault"), Cloud->IsSignedIn());
		Game.Pump([&Cloud]() { return !Cloud->GetAccount().DisplayName.IsEmpty(); });
	}

	// Only this server's paths: no absolute or escaping paths carry the session.
	int32 Before = Fake.Requests.Num();
	bool bRefused = false;
	Cloud->SendJson(TEXT("GET"), TEXT("https://evil.test/v1/me"), FString(), true, FOnStageCloudResponse::CreateLambda([&bRefused](const FStageCloudResponse& R) { bRefused = !R.bConnected; }));
	TestTrue(TEXT("Absolute address refused"), bRefused && Fake.Requests.Num() == Before);
	bRefused = false;
	Cloud->SendJson(TEXT("GET"), TEXT("/v1/../admin"), FString(), true, FOnStageCloudResponse::CreateLambda([&bRefused](const FStageCloudResponse& R) { bRefused = !R.bConnected; }));
	TestTrue(TEXT("Escaping path refused"), bRefused && Fake.Requests.Num() == Before);
	Fake.Handler = [](const FStageCloudHttpRequest&) { return TOptional<FStageCloudResponse>(Answer(200, TEXT("{}"))); };
	Cloud->Download(TEXT("https://bucket.test/p"), 5.f, FOnStageCloudResponse());
	TestFalse(TEXT("Downloads never carry the session"), Fake.Requests.Last().Headers.Contains(TEXT("Authorization")));

	// A 401 ends the session; signing out revokes it on the server and clears the vault.
	Fake.Handler = [](const FStageCloudHttpRequest&) { return TOptional<FStageCloudResponse>(ErrorAnswer(401, TEXT("Unauthenticated"))); };
	Cloud->RefreshAccount();
	TestTrue(TEXT("401 signs out"), Cloud->GetState() == EStageCloudState::SignedOut && Cloud->GetSecretsToRedact().IsEmpty());
	TestTrue(TEXT("Signs in again"), SignIn(Game, *Cloud, Fake));
	Before = Fake.Count(TEXT("/v1/auth/logout"));
	Cloud->SignOut();
	TestEqual(TEXT("Logout sent"), Fake.Count(TEXT("/v1/auth/logout")), Before + 1);
	TestTrue(TEXT("Signed out"), Cloud->GetState() == EStageCloudState::SignedOut);
	if (FStageCredentialVault::IsSupported())
	{
		TestFalse(TEXT("Vault cleared"), FStageCredentialVault(FStageCredentialVault::GetDefaultDirectory()).Contains(TEXT("StageCraftCloud")));
	}

	// Declined and expired codes end the sign-in; slow_down slows the polls; cancelling stops them.
	Fake.Handler = [](const FStageCloudHttpRequest& Request) -> TOptional<FStageCloudResponse>
	{
		return Request.Url.EndsWith(TEXT("/v1/auth/device"))
			? Answer(200, TEXT("{\"deviceCode\":\"d2\",\"userCode\":\"AAAA\",\"verificationUri\":\"https://cloud.test/auth/verify\",\"expiresIn\":600,\"interval\":1}"))
			: ErrorAnswer(400, TEXT("access_denied"));
	};
	TestTrue(TEXT("Sign-in starts"), Cloud->StartSignIn(Error));
	TestFalse(TEXT("Only one at a time"), Cloud->StartSignIn(Error));
	TestTrue(TEXT("Declined"), Game.Pump([&Cloud]() { return Cloud->GetState() == EStageCloudState::SignedOut; }, 5.0));
	Fake.Handler = [](const FStageCloudHttpRequest& Request) -> TOptional<FStageCloudResponse>
	{
		return Request.Url.EndsWith(TEXT("/v1/auth/device"))
			? Answer(200, TEXT("{\"deviceCode\":\"d3\",\"userCode\":\"BBBB\",\"verificationUri\":\"https://cloud.test/auth/verify\",\"expiresIn\":600,\"interval\":1}"))
			: ErrorAnswer(400, TEXT("slow_down"));
	};
	TestTrue(TEXT("Sign-in starts again"), Cloud->StartSignIn(Error));
	Game.Pump([&Cloud]() { return Cloud->GetDeviceCode().IntervalSeconds > 1; }, 5.0);
	TestEqual(TEXT("slow_down adds 5 seconds"), Cloud->GetDeviceCode().IntervalSeconds, 6);
	Before = Fake.Count(TEXT("/v1/auth/device/token"));
	Cloud->CancelSignIn();
	Game.PumpFor(1.5);
	TestEqual(TEXT("No polls after cancel"), Fake.Count(TEXT("/v1/auth/device/token")), Before);
	TestTrue(TEXT("Cancelled"), Cloud->GetState() == EStageCloudState::SignedOut);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageCloudMarketplaceTest, "StageCraft.Cloud.Marketplace", StageCloudTests::Flags)

bool FStageCloudMarketplaceTest::RunTest(const FString& Parameters)
{
	using namespace StageCloudTests;

	FStageTestGameInstance Game;
	UStageCloudSubsystem* Cloud = Game.Subsystem<UStageCloudSubsystem>();
	if (!TestTrue(TEXT("Game instance"), Game.IsValid()) || !TestNotNull(TEXT("Cloud"), Cloud))
	{
		return false;
	}
	UStageCloudMarketplaceBackend* Backend = NewObject<UStageCloudMarketplaceBackend>(&Game.Get());

	// Offline: the community folder, exactly as before.
	const FGuid LocalId = StageTestFixtures::PublishListing(TEXT("Folder Stage"), 0, false);
	EStageMarketplaceResult QueryResult = EStageMarketplaceResult::BackendUnavailable;
	FStageMarketplaceQueryResult Page;
	Backend->Query(FStageMarketplaceQuery(), FOnStageMarketplaceQueried::CreateLambda([&](EStageMarketplaceResult Result, const FStageMarketplaceQueryResult& Found)
	{
		QueryResult = Result;
		Page = Found;
	}));
	Game.Pump([&Page]() { return Page.Listings.Num() > 0; });
	TestFalse(TEXT("Offline uses the folder"), Backend->UsesCloud());
	TestTrue(TEXT("Folder listing found"), QueryResult == EStageMarketplaceResult::Success && Page.Listings.ContainsByPredicate([&LocalId](const FStageMarketplaceListing& L) { return L.ListingId == LocalId; }));

	// Online: the server's listings.
	FFakeServer Fake;
	Cloud->SetTransportForTesting(Fake.Transport());
	Cloud->SetBaseUrlForTesting(Server);
	const FGuid Id = FGuid::NewGuid();
	Fake.Handler = [&Id](const FStageCloudHttpRequest& Request) -> TOptional<FStageCloudResponse>
	{
		return Request.Url.Contains(TEXT("/v1/listings?")) ? Answer(200, FString::Printf(TEXT("{\"listings\":[%s],\"totalMatches\":40}"), *ListingJson(Id)))
			: ErrorAnswer(404, TEXT("NotFound"));
	};
	Page = FStageMarketplaceQueryResult();
	Backend->Query(FStageMarketplaceQuery(), FOnStageMarketplaceQueried::CreateLambda([&](EStageMarketplaceResult Result, const FStageMarketplaceQueryResult& Found)
	{
		QueryResult = Result;
		Page = Found;
	}));
	TestTrue(TEXT("Cloud listing"), QueryResult == EStageMarketplaceResult::Success && Page.Listings.Num() == 1 && Page.Listings[0].ListingId == Id);
	TestEqual(TEXT("Total"), Page.TotalMatches, 40);
	TestTrue(TEXT("Browsing needs no account"), !Fake.Requests.Last().Headers.Contains(TEXT("Authorization")));
	Fake.Handler = [](const FStageCloudHttpRequest&) { return TOptional<FStageCloudResponse>(Answer(200, TEXT("<html>"))); };
	Backend->Query(FStageMarketplaceQuery(), FOnStageMarketplaceQueried::CreateLambda([&](EStageMarketplaceResult Result, const FStageMarketplaceQueryResult&) { QueryResult = Result; }));
	TestTrue(TEXT("Unreadable page is unavailable"), QueryResult == EStageMarketplaceResult::BackendUnavailable);

	// Downloads need an account; the package comes from the short-lived address without the session.
	EStageMarketplaceResult FetchResult = EStageMarketplaceResult::Success;
	FStageMarketplacePackageBytes Bytes;
	const auto Fetch = [&]()
	{
		FetchResult = EStageMarketplaceResult::Busy;
		Bytes.Reset();
		Backend->FetchPackage(Id, EStageMarketplaceFetchPurpose::Acquire, FOnStageMarketplacePackageFetched::CreateLambda([&](EStageMarketplaceResult Result, const FStageMarketplacePackageBytes& Got)
		{
			FetchResult = Result;
			Bytes = Got;
		}));
	};
	Fetch();
	TestTrue(TEXT("Sign in first"), FetchResult == EStageMarketplaceResult::SignInRequired);
	if (!TestTrue(TEXT("Signed in"), SignIn(Game, *Cloud, Fake)))
	{
		return false;
	}
	Fake.Handler = [](const FStageCloudHttpRequest& Request) -> TOptional<FStageCloudResponse>
	{
		if (Request.Url.Contains(TEXT("/package?purpose=acquire&redirect=false")))
		{
			return Answer(200, TEXT("{\"url\":\"https://bucket.test/pkg?sig=1\",\"expiresAt\":\"2026-11-07T10:05:00Z\"}"));
		}
		return Request.Url == TEXT("https://bucket.test/pkg?sig=1") ? Answer(200, TEXT("STAGEPKG-bytes")) : ErrorAnswer(404, TEXT("NotFound"));
	};
	Fetch();
	TestTrue(TEXT("Downloaded"), FetchResult == EStageMarketplaceResult::Success && Bytes.IsValid() && Bytes->Num() == 14);
	TestFalse(TEXT("No session sent to the storage"), Fake.Requests.Last().Headers.Contains(TEXT("Authorization")));
	Fake.Handler = [](const FStageCloudHttpRequest&) { return TOptional<FStageCloudResponse>(ErrorAnswer(403, TEXT("NotOwned"))); };
	Fetch();
	TestTrue(TEXT("Not owned"), FetchResult == EStageMarketplaceResult::NotOwned && !Bytes.IsValid());
	Fake.Handler = [](const FStageCloudHttpRequest&) { return TOptional<FStageCloudResponse>(Answer(200, TEXT("{\"url\":\"http://bucket.test/pkg\"}"))); };
	Fetch();
	TestTrue(TEXT("An address that is not safe is not fetched"), FetchResult == EStageMarketplaceResult::BackendUnavailable);

	// Publishing and unpublishing go to the server, which decides.
	EStageMarketplaceResult PublishResult = EStageMarketplaceResult::Busy;
	FStageMarketplaceListing Published;
	Fake.Handler = [&Id](const FStageCloudHttpRequest& Request) { return TOptional<FStageCloudResponse>(Answer(201, ListingJson(Id))); };
	Backend->Publish(MakeShared<const TArray<uint8>, ESPMode::ThreadSafe>(TArray<uint8>{ 1, 2, 3 }),
		FOnStageMarketplacePublished::CreateLambda([&](EStageMarketplaceResult Result, const FStageMarketplaceListing& Listing) { PublishResult = Result; Published = Listing; }));
	TestTrue(TEXT("Published"), PublishResult == EStageMarketplaceResult::Success && Published.ListingId == Id);
	TestEqual(TEXT("Package sent as octet-stream"), Fake.Requests.Last().Headers.FindRef(TEXT("Content-Type")), FString(TEXT("application/octet-stream")));
	Fake.Handler = [](const FStageCloudHttpRequest&) { return TOptional<FStageCloudResponse>(ErrorAnswer(422, TEXT("Corrupt"))); };
	Backend->Publish(MakeShared<const TArray<uint8>, ESPMode::ThreadSafe>(TArray<uint8>{ 1 }),
		FOnStageMarketplacePublished::CreateLambda([&](EStageMarketplaceResult Result, const FStageMarketplaceListing&) { PublishResult = Result; }));
	TestTrue(TEXT("Server re-validation refuses"), PublishResult == EStageMarketplaceResult::Corrupt);
	EStageMarketplaceResult UnpublishResult = EStageMarketplaceResult::Busy;
	Fake.Handler = [](const FStageCloudHttpRequest& Request) { return TOptional<FStageCloudResponse>(ErrorAnswer(403, TEXT("NotYours"))); };
	Backend->Unpublish(Id, FOnStageMarketplaceUnpublished::CreateLambda([&](EStageMarketplaceResult Result) { UnpublishResult = Result; }));
	TestTrue(TEXT("Only the author unpublishes"), UnpublishResult == EStageMarketplaceResult::NotYours);
	TestEqual(TEXT("DELETE"), Fake.Requests.Last().Verb, FString(TEXT("DELETE")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageCloudCheckoutTest, "StageCraft.Cloud.Checkout", StageCloudTests::Flags)

bool FStageCloudCheckoutTest::RunTest(const FString& Parameters)
{
	using namespace StageCloudTests;

	FStageTestGameInstance Game;
	UStageCloudSubsystem* Cloud = Game.Subsystem<UStageCloudSubsystem>();
	UStageMarketplaceSubsystem* Marketplace = Game.Subsystem<UStageMarketplaceSubsystem>();
	if (!TestTrue(TEXT("Game instance"), Game.IsValid()) || !TestNotNull(TEXT("Cloud"), Cloud) || !TestNotNull(TEXT("Marketplace"), Marketplace))
	{
		return false;
	}

	// A real-money listing and a credits listing in the marketplace's visible listings (the folder backend of the test harness).
	FStagePackage Package;
	Package.Scene = StageTestFixtures::MakeScene(TEXT("Paid Rig"));
	Package.Manifest.PackageId = FGuid::NewGuid();
	Package.Manifest.Title = TEXT("Paid Rig");
	Package.Manifest.Description = TEXT("Real money.");
	Package.Manifest.AuthorName = TEXT("Seller");
	Package.Manifest.Category = StageCraftTags::Marketplace_Category_Concert;
	Package.Manifest.Price.RealMoneyMinorUnits = 1500;
	Package.Manifest.Price.RealMoneyCurrency = TEXT("USD");
	Package.Manifest.CreatedAtUtc = FDateTime::UtcNow();
	Package.Manifest.ItemCount = Package.Scene.Items.Num();
	Package.Manifest.RequiredItems = StagePackage::CollectRequiredItems(Package.Scene);
	TArray<uint8> PackageBytes;
	FStageMarketplaceListing Stored;
	const bool bStored = StagePackage::Write(Package, PackageBytes) == EStageMarketplaceResult::Success
		&& FStageLocalMarketplaceStore(FStageLocalMarketplaceStore::GetDefaultDirectory()).Publish(PackageBytes, Stored) == EStageMarketplaceResult::Success;
	const FGuid PaidId = Package.Manifest.PackageId;
	const FGuid CreditsId = StageTestFixtures::PublishListing(TEXT("Credits Rig"), 100, false);
	if (!TestTrue(TEXT("Listings stored"), bStored && CreditsId.IsValid()) || !TestTrue(TEXT("Listings visible"), StageTestFixtures::QueryAll(Game))
		|| !TestNotNull(TEXT("Paid listing visible"), Marketplace->FindListing(PaidId)))
	{
		return false;
	}

	UStageCloudCommerceBackend* Store = NewObject<UStageCloudCommerceBackend>(&Game.Get());
	FStagePurchaseRequest Paid;
	Paid.TransactionId = FGuid::NewGuid();
	Paid.ProductId = StageMarketplaceRules::MakeListingProductId(PaidId);
	FStagePurchaseRequest Credits;
	Credits.TransactionId = FGuid::NewGuid();
	Credits.ProductId = StageMarketplaceRules::MakeListingProductId(CreditsId);

	int32 Answers = 0;
	bool bApproved = false;
	FText Reason;
	const auto Buy = [&](const FStagePurchaseRequest& Request)
	{
		Answers = 0;
		bApproved = false;
		Reason = FText::GetEmpty();
		Store->ProcessPurchase(Request, FOnStagePurchaseProcessed::CreateLambda([&](bool bOk, const FText& Why) { ++Answers; bApproved = bOk; Reason = Why; }));
	};

	// Signed out: credits stay local (approved at once, nothing sent), real money is refused.
	FFakeServer Fake;
	Cloud->SetTransportForTesting(Fake.Transport());
	Cloud->SetBaseUrlForTesting(Server);
	TestFalse(TEXT("No real money signed out"), Store->CanSettleRealMoney());
	TestEqual(TEXT("Credits keep the economy's deadline"), Store->GetSettlementTimeoutSeconds(Credits, 30.f), 30.f);
	TestEqual(TEXT("A checkout gets its own deadline"), Store->GetSettlementTimeoutSeconds(Paid, 30.f), 1920.f);
	Buy(Credits);
	TestTrue(TEXT("Credits approved locally"), Answers == 1 && bApproved && Fake.Requests.IsEmpty());
	Buy(Paid);
	TestTrue(TEXT("Real money needs sign-in"), Answers == 1 && !bApproved && Reason.ToString().Contains(TEXT("Sign in")));
	if (!TestTrue(TEXT("Signed in"), SignIn(Game, *Cloud, Fake)))
	{
		return false;
	}
	TestTrue(TEXT("Real money while signed in"), Store->CanSettleRealMoney());

	// The happy path: order (price from the server), hosted checkout in the browser, long polls until the webhook made it Paid.
	int32 Polls = 0;
	Fake.Handler = [&](const FStageCloudHttpRequest& Request) -> TOptional<FStageCloudResponse>
	{
		if (Request.Url.EndsWith(TEXT("/v1/orders")))
		{
			return Answer(200, OrderJson(PaidId, TEXT("CheckoutOpen"), TEXT("https://checkout.stripe.com/c/pay/cs_test_1")));
		}
		if (Request.Url.Contains(TEXT("/v1/orders/ord-1?waitSeconds=25")))
		{
			return Answer(200, OrderJson(PaidId, ++Polls < 3 ? TEXT("CheckoutOpen") : TEXT("Paid"), TEXT("https://checkout.stripe.com/c/pay/cs_test_1")));
		}
		return ErrorAnswer(404, TEXT("NotFound"));
	};
	const int32 RequestsBefore = Fake.Requests.Num();
	Buy(Paid);
	TestTrue(TEXT("Paid after the long polls"), Answers == 1 && bApproved);
	TestEqual(TEXT("Three long polls"), Polls, 3);
	TestTrue(TEXT("Checkout opened in the browser"), Cloud->GetOpenedUrlsForTesting().Contains(TEXT("https://checkout.stripe.com/c/pay/cs_test_1")));
	const FString OrderBody = Fake.BodyOf(Fake.Requests[RequestsBefore]);
	TestTrue(TEXT("The order names the listing and the idempotency key"), OrderBody.Contains(StageCloudJson::GuidToWire(PaidId)) && OrderBody.Contains(StageCloudJson::GuidToWire(Paid.TransactionId)));
	TestFalse(TEXT("The client never sends an amount"), OrderBody.Contains(TEXT("1500")) || OrderBody.Contains(TEXT("amount")));
	TestFalse(TEXT("Not waiting any more"), Store->IsWaitingForPayment(PaidId));

	// Bought on another computer: approved without a checkout.
	Fake.Handler = [](const FStageCloudHttpRequest&) { return TOptional<FStageCloudResponse>(ErrorAnswer(409, TEXT("AlreadyOwned"))); };
	Buy(Paid);
	TestTrue(TEXT("Already owned is approved"), Answers == 1 && bApproved);

	// Refusals, expiry, payment failure and a hold all decline, once, with a reason.
	for (const TPair<const TCHAR*, const TCHAR*>& Case : { TPair<const TCHAR*, const TCHAR*>(TEXT("Expired"), TEXT("expired")),
		TPair<const TCHAR*, const TCHAR*>(TEXT("Failed"), TEXT("did not go through")), TPair<const TCHAR*, const TCHAR*>(TEXT("OnHold"), TEXT("support")) })
	{
		Fake.Handler = [&, State = Case.Key](const FStageCloudHttpRequest& Request) -> TOptional<FStageCloudResponse>
		{
			return Request.Url.EndsWith(TEXT("/v1/orders")) ? Answer(200, OrderJson(PaidId, TEXT("CheckoutOpen"), TEXT("https://checkout.stripe.com/x")))
				: Answer(200, OrderJson(PaidId, State));
		};
		Buy(Paid);
		TestTrue(FString::Printf(TEXT("%s declines with a reason"), Case.Key), Answers == 1 && !bApproved && Reason.ToString().Contains(Case.Value));
	}
	Fake.Handler = [](const FStageCloudHttpRequest&) { return TOptional<FStageCloudResponse>(ErrorAnswer(409, TEXT("NotForSale"))); };
	Buy(Paid);
	TestTrue(TEXT("Not for sale"), Answers == 1 && !bApproved && Reason.ToString().Contains(TEXT("no longer for sale")));
	Fake.Handler = [&](const FStageCloudHttpRequest& Request) { return TOptional<FStageCloudResponse>(Answer(200, OrderJson(FGuid::NewGuid(), TEXT("Paid")))); };
	Buy(Paid);
	TestTrue(TEXT("An order for another listing is not trusted"), Answers == 1 && !bApproved);
	Fake.Handler = [&](const FStageCloudHttpRequest& Request) { return TOptional<FStageCloudResponse>(Answer(200, OrderJson(PaidId, TEXT("CheckoutOpen"), TEXT("http://evil.test/pay")))); };
	Buy(Paid);
	TestTrue(TEXT("An unsafe checkout page is never opened"), Answers == 1 && !bApproved && !Cloud->GetOpenedUrlsForTesting().Contains(TEXT("http://evil.test/pay")));

	// A dropped connection while waiting is retried; cancelling answers once and stops the polls.
	int32 Waits = 0;
	Fake.Handler = [&](const FStageCloudHttpRequest& Request) -> TOptional<FStageCloudResponse>
	{
		if (Request.Url.EndsWith(TEXT("/v1/orders")))
		{
			return Answer(200, OrderJson(PaidId, TEXT("CheckoutOpen"), TEXT("https://checkout.stripe.com/x")));
		}
		++Waits;
		if (Waits == 1)
		{
			return FStageCloudResponse();
		}
		if (Waits == 2)
		{
			return Answer(200, OrderJson(PaidId, TEXT("Paid")));
		}
		return {};
	};
	Buy(Paid);
	TestTrue(TEXT("Waiting after a dropped connection"), Answers == 0 && Store->IsWaitingForPayment(PaidId));
	TestTrue(TEXT("Retried and paid"), Game.Pump([&Answers]() { return Answers > 0; }, 8.0) && bApproved && Waits == 2);
	Waits = 2;
	Buy(Paid);
	TestTrue(TEXT("Held by the server"), Answers == 0 && Store->IsWaitingForPayment(PaidId));
	TestTrue(TEXT("Cancel"), Store->CancelPurchase(Paid.TransactionId));
	TestTrue(TEXT("Declined once with a reason"), Answers == 1 && !bApproved && Reason.ToString().Contains(TEXT("not be charged twice")));
	for (TPair<FStageCloudResponse, FOnStageCloudResponse>& Late : Fake.Held)
	{
		Late.Value.ExecuteIfBound(Answer(200, OrderJson(PaidId, TEXT("Paid"))));
	}
	TestEqual(TEXT("A late answer after cancel changes nothing"), Answers, 1);
	TestFalse(TEXT("Nothing to cancel"), Store->CancelPurchase(Paid.TransactionId));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageSupportSubmitTest, "StageCraft.Support.Submit", StageCloudTests::Flags)

bool FStageSupportSubmitTest::RunTest(const FString& Parameters)
{
	using namespace StageCloudTests;

	FStageTestGameInstance Game;
	UStageSupportSubsystem* Support = Game.Subsystem<UStageSupportSubsystem>();
	UStageCloudSubsystem* Cloud = Game.Subsystem<UStageCloudSubsystem>();
	if (!TestTrue(TEXT("Game instance"), Game.IsValid()) || !TestNotNull(TEXT("Support"), Support) || !TestNotNull(TEXT("Cloud"), Cloud))
	{
		return false;
	}
	TestTrue(TEXT("Saved under the data root"), Support->GetOfflineDirectory().StartsWith(Game.GetRoot()));

	FStageSupportReport Report = Support->MakeDraft(nullptr);
	TestTrue(TEXT("Draft has an id"), Report.ReportId.IsValid());
	TestFalse(TEXT("Draft has the app version"), Report.AppVersion.IsEmpty());
	Report.Title = TEXT("Crash on export");
	Report.Description = TEXT("My key sk-ant-secret123 was in the log; mail me at me@example.com");
	Report.bIncludeLog = true;
	Report.LogTail = TEXT("LogHttp: Authorization: Bearer abcdefghijklmnop");

	FStageSupportSubmitResult Result;
	bool bDone = false;
	FText Error;
	const auto Submit = [&](const FStageSupportReport& Each)
	{
		bDone = false;
		return Support->Submit(Each, FOnStageSupportSubmitted::CreateLambda([&](const FStageSupportSubmitResult& Got) { Result = Got; bDone = true; }), Error);
	};

	// Invalid reports are refused before anything happens.
	FStageSupportReport Empty = Report;
	Empty.Title.Reset();
	TestFalse(TEXT("Invalid refused"), Submit(Empty));
	TestFalse(TEXT("... with a reason"), Error.IsEmpty());

	// Offline: saved on disk, redacted.
	TestTrue(TEXT("Offline submit"), Submit(Report) && bDone && Result.bSaved && !Result.bSent);
	FString Saved;
	TestTrue(TEXT("report.txt"), FFileHelper::LoadFileToString(Saved, *FPaths::Combine(Result.Folder, TEXT("report.txt"))));
	TestFalse(TEXT("Key redacted on disk"), Saved.Contains(TEXT("sk-ant-secret123")));
	TestFalse(TEXT("E-mail redacted on disk"), Saved.Contains(TEXT("me@example.com")));
	FString Log;
	TestTrue(TEXT("log.txt redacted"), FFileHelper::LoadFileToString(Log, *FPaths::Combine(Result.Folder, TEXT("log.txt"))) && Log.Contains(TEXT("Bearer [REDACTED]")));
	TestFalse(TEXT("No screenshot file when not included"), IFileManager::Get().FileExists(*FPaths::Combine(Result.Folder, TEXT("screenshot.png"))));

	// Online: sent, with the session as a secret.
	FFakeServer Fake;
	Cloud->SetTransportForTesting(Fake.Transport());
	Cloud->SetBaseUrlForTesting(Server);
	if (!TestTrue(TEXT("Signed in"), SignIn(Game, *Cloud, Fake)))
	{
		return false;
	}
	Fake.Handler = [](const FStageCloudHttpRequest&) { return TOptional<FStageCloudResponse>(Answer(201, TEXT("{\"ticketId\":\"t-42\",\"status\":\"open\"}"))); };
	FStageSupportReport WithToken = Report;
	WithToken.Steps = FString::Printf(TEXT("Pasted %s by mistake"), Token);
	WithToken.bIncludeLog = false;
	TestTrue(TEXT("Sent"), Submit(WithToken) && bDone && Result.bSent && Result.TicketId == TEXT("t-42"));
	const FString Sent = Fake.BodyOf(Fake.Requests.Last());
	TestTrue(TEXT("To the tickets endpoint"), Fake.Requests.Last().Url.EndsWith(TEXT("/v1/support/tickets")));
	TestFalse(TEXT("The session never leaves in a report"), Sent.Contains(Token));
	TestFalse(TEXT("No log when not ticked"), Sent.Contains(TEXT("logTail")));
	TestTrue(TEXT("Signed in: sent with the session"), Fake.Requests.Last().Headers.Contains(TEXT("Authorization")));

	// A failed send is kept on disk with the reason; one report at a time.
	Fake.Handler = [](const FStageCloudHttpRequest&) { return TOptional<FStageCloudResponse>(ErrorAnswer(503, TEXT("BackendUnavailable"), TEXT("maintenance"))); };
	TestTrue(TEXT("Failed send saved"), Submit(Report) && bDone && !Result.bSent && Result.bSaved && Result.Message.ToString().Contains(TEXT("maintenance")));
	Fake.Handler = nullptr;
	TestTrue(TEXT("Held send"), Submit(Report) && !bDone && Support->IsSubmitting());
	TestFalse(TEXT("One at a time"), Submit(Report));
	for (TPair<FStageCloudResponse, FOnStageCloudResponse>& Held : Fake.Held)
	{
		Held.Value.ExecuteIfBound(Answer(201, TEXT("{\"ticketId\":\"t-43\"}")));
	}
	TestTrue(TEXT("Held send completes"), bDone && Result.bSent && !Support->IsSubmitting());
	return true;
}

#endif
