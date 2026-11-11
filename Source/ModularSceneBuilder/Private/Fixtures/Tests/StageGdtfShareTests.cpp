// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Fixtures/StageFixtureLibraryRules.h"
#include "Fixtures/StageFixtureLibrarySubsystem.h"
#include "Fixtures/StageGdtfShareProtocol.h"
#include "Fixtures/StageGdtfShareSubsystem.h"
#include "Fixtures/Tests/StageGdtfTestFiles.h"
#include "Game/StageCraftGameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/StageTestEventRecorder.h"
#include "Tests/StageTestGameInstance.h"
#include "UObject/StrongObjectPtr.h"

namespace StageGdtfShareTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	TArray<uint8> Utf8(const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	}

	FString FromUtf8(const TArray<uint8>& Bytes)
	{
		const FUTF8ToTCHAR Converted(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
		return FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());
	}

	FString Str(EStageGdtfShareResult Result) { return UEnum::GetValueAsString(Result); }
	FString Str(EStageGdtfShareState State) { return UEnum::GetValueAsString(State); }

	FString ListJson(int64 SpotSize)
	{
		return FString::Printf(TEXT("{\"result\":true,\"list\":["
			"{\"rid\":101,\"fixture\":\"Test Spot\",\"manufacturer\":\"StageCraft\",\"revision\":\"Rev A\",\"creationDate\":1700000000,\"lastModified\":\"1710000000\","
			"\"uploader\":\"Manuf.\",\"rating\":4.5,\"version\":\"1.2\",\"creator\":\"maker\",\"uuid\":\"%s\",\"filesize\":%lld,"
			"\"modes\":[{\"name\":\"Standard\",\"dmxfootprint\":16},{\"name\":\"Basic\",\"dmxfootprint\":8}]},"
			"{\"rid\":102,\"fixture\":\"Gone Wash\",\"manufacturer\":\"Acme\",\"revision\":\"\",\"uploader\":\"User\",\"rating\":9,\"uuid\":\"not a guid\",\"modes\":[]},"
			"{\"rid\":103,\"fixture\":\"Json Beam\",\"manufacturer\":\"Acme\",\"uploader\":\"Robot\"},"
			"{\"rid\":104,\"fixture\":\"Text Par\",\"manufacturer\":\"Acme\",\"uploader\":\"User\"},"
			"{\"rid\":105,\"fixture\":\"Slow Bar\",\"manufacturer\":\"Acme\",\"uploader\":\"User\"},"
			"{\"rid\":106,\"fixture\":\"Huge Blinder\",\"manufacturer\":\"Acme\",\"uploader\":\"User\",\"filesize\":%lld},"
			"{\"fixture\":\"No Rid\",\"manufacturer\":\"Acme\"},"
			"{\"rid\":\"abc\",\"fixture\":\"Bad Rid\",\"manufacturer\":\"Acme\"},"
			"{\"rid\":101,\"fixture\":\"Repeated\",\"manufacturer\":\"Acme\"},"
			"{\"rid\":107,\"fixture\":\"\",\"manufacturer\":\"Acme\"},"
			"7]}"), StageGdtfTestFiles::SpotId, SpotSize, StageGdtfShare::MaxDownloadBytes + 1);
	}

	struct FFakeServer
	{
		TArray<FStageGdtfShareHttpRequest> Requests;
		TArray<uint8> SpotFile;
		bool bSessionValid = true;
		int32 ListRequests = 0;
		FOnStageGdtfShareResponse HeldAnswer;
		FOnStageGdtfShareProgress HeldProgress;

		static FStageGdtfShareHttpResponse Json(int32 Status, const FString& Text)
		{
			FStageGdtfShareHttpResponse Response;
			Response.bConnected = true;
			Response.Status = Status;
			Response.Body = Utf8(Text);
			return Response;
		}

		void Answer(const FStageGdtfShareHttpRequest& Request, FOnStageGdtfShareResponse Answer, FOnStageGdtfShareProgress Progress)
		{
			Requests.Add(Request);
			const FString Url = Request.Url;
			if (Url.EndsWith(TEXT("login.php")))
			{
				TSharedPtr<FJsonObject> Body;
				FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(FromUtf8(Request.Body)), Body);
				const bool bGood = Body.IsValid() && Body->GetStringField(TEXT("user")) == TEXT("alice") && Body->GetStringField(TEXT("password")) == TEXT("p\"ss word");
				if (!bGood)
				{
					Answer.ExecuteIfBound(Json(401, TEXT("{\"result\":false,\"error\":\"No valid user or password provided.\"}")));
					return;
				}
				FStageGdtfShareHttpResponse Response = Json(200, TEXT("{\"result\":true,\"notice\":\"Logged in.\"}"));
				Response.SetCookies = { TEXT("PHPSESSID=abc123; path=/; HttpOnly, theme=dark; Expires=Wed, 21 Oct 2026 07:28:00 GMT; path=/") };
				bSessionValid = true;
				Answer.ExecuteIfBound(Response);
				return;
			}
			if (!bSessionValid || Request.Cookie != TEXT("PHPSESSID=abc123; theme=dark"))
			{
				Answer.ExecuteIfBound(Json(401, TEXT("{\"result\":false,\"error\":\"Unauthorized.\"}")));
				return;
			}
			if (Url.EndsWith(TEXT("getList.php")))
			{
				++ListRequests;
				Answer.ExecuteIfBound(Json(200, ListJson(SpotFile.Num())));
				return;
			}
			if (Url.EndsWith(TEXT("rid=101")))
			{
				Progress.ExecuteIfBound(SpotFile.Num(), SpotFile.Num());
				FStageGdtfShareHttpResponse Response = Json(200, FString());
				Response.Body = SpotFile;
				Answer.ExecuteIfBound(Response);
				return;
			}
			if (Url.EndsWith(TEXT("rid=102")))
			{
				Answer.ExecuteIfBound(Json(404, TEXT("{\"result\":false,\"error\":\"File does not exist.\"}")));
				return;
			}
			if (Url.EndsWith(TEXT("rid=103")))
			{
				Answer.ExecuteIfBound(Json(200, TEXT("{\"result\":false,\"error\":\"File does not exist.\"}")));
				return;
			}
			if (Url.EndsWith(TEXT("rid=104")))
			{
				Answer.ExecuteIfBound(Json(200, TEXT("hello")));
				return;
			}
			if (Url.EndsWith(TEXT("rid=105")))
			{
				HeldAnswer = MoveTemp(Answer);
				HeldProgress = MoveTemp(Progress);
				return;
			}
			Answer.ExecuteIfBound(Json(500, TEXT("oops")));
		}
	};

	struct FScopedWorld
	{
		UWorld* World = nullptr;

		explicit FScopedWorld(UGameInstance& GameInstance)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("StageGdtfShareTestWorld"));
			World->SetGameInstance(&GameInstance);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			World->GetWorldSettings()->NotifyBeginPlay();
		}

		~FScopedWorld()
		{
			World->SetGameInstance(nullptr);
			World->BeginTearingDown();
			World->EndPlay(EEndPlayReason::Quit);
			World->DestroyWorld(false);
			GEngine->DestroyWorldContext(World);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageGdtfShareProtocolTest, "StageCraft.Fixtures.GdtfShare.Protocol", StageGdtfShareTests::Flags)

bool FStageGdtfShareProtocolTest::RunTest(const FString& Parameters)
{
	using namespace StageGdtfShareTests;

	TestTrue(TEXT("The public API root is safe"), StageGdtfShare::IsSafeBaseUrl(StageGdtfShare::DefaultBaseUrl));
	TestFalse(TEXT("Plain http is not"), StageGdtfShare::IsSafeBaseUrl(TEXT("http://gdtf-share.com/apis/public/")));
	TestFalse(TEXT("Without the closing slash"), StageGdtfShare::IsSafeBaseUrl(TEXT("https://gdtf-share.com/apis/public")));
	TestFalse(TEXT("With user info"), StageGdtfShare::IsSafeBaseUrl(TEXT("https://me@evil.example/")));
	TestFalse(TEXT("With a query"), StageGdtfShare::IsSafeBaseUrl(TEXT("https://example.com/?a=")));
	TestFalse(TEXT("Empty host"), StageGdtfShare::IsSafeBaseUrl(TEXT("https:///")));

	const TArray<uint8> Login = StageGdtfShare::MakeLoginBody(TEXT("al\"ice"), TEXT("p\\ss\nword"));
	TSharedPtr<FJsonObject> LoginJson;
	TestTrue(TEXT("The login body is JSON with both fields escaped"), FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(FromUtf8(Login)), LoginJson)
		&& LoginJson.IsValid() && LoginJson->GetStringField(TEXT("user")) == TEXT("al\"ice") && LoginJson->GetStringField(TEXT("password")) == TEXT("p\\ss\nword"));

	bool bResult = true;
	FString Error;
	TestTrue(TEXT("A refusal reads"), StageGdtfShare::ParseResultFlag(Utf8(TEXT("{\"result\":false,\"error\":\"No valid user\\u0007 or password provided.\"}")), bResult, Error)
		&& !bResult && Error == TEXT("No valid user or password provided."));
	TestTrue(TEXT("A success with a notice reads"), StageGdtfShare::ParseResultFlag(Utf8(TEXT("{\"result\":true,\"notice\":\"ok\"}")), bResult, Error) && bResult && Error == TEXT("ok"));
	TestFalse(TEXT("A result that is not a boolean"), StageGdtfShare::ParseResultFlag(Utf8(TEXT("{\"result\":\"true\"}")), bResult, Error));
	TestFalse(TEXT("Not JSON"), StageGdtfShare::ParseResultFlag(Utf8(TEXT("<html>")), bResult, Error));
	TestFalse(TEXT("Nothing"), StageGdtfShare::ParseResultFlag({}, bResult, Error));

	TArray<FStageGdtfShareEntry> Entries;
	int32 Skipped = 0;
	TestTrue(TEXT("The list reads"), StageGdtfShare::ParseList(Utf8(ListJson(1234)), Entries, Skipped));
	TestEqual(TEXT("Six entries kept"), Entries.Num(), 6);
	TestEqual(TEXT("Five skipped: no rid, a text rid, a repeated rid, no name, not an object"), Skipped, 5);
	if (Entries.Num() == 6)
	{
		const FStageGdtfShareEntry& Spot = Entries[0];
		TestTrue(TEXT("Spot fields"), Spot.RevisionId == 101 && Spot.Manufacturer == TEXT("StageCraft") && Spot.Fixture == TEXT("Test Spot") && Spot.Revision == TEXT("Rev A")
			&& Spot.Uploader == EStageGdtfShareUploader::Manufacturer && FMath::IsNearlyEqual(Spot.Rating, 4.5f) && Spot.GdtfVersion == TEXT("1.2") && Spot.Creator == TEXT("maker")
			&& Spot.FileSizeBytes == 1234);
		FGuid SpotId;
		FGuid::Parse(StageGdtfTestFiles::SpotId, SpotId);
		TestEqual(TEXT("The FixtureTypeID"), Spot.FixtureTypeId, SpotId);
		TestEqual(TEXT("A date given as a numeric string"), Spot.LastModifiedUtc, FDateTime::FromUnixTimestamp(1710000000));
		TestTrue(TEXT("Modes"), Spot.Modes.Num() == 2 && Spot.Modes[0].Name == TEXT("Standard") && Spot.Modes[0].Footprint == 16 && Spot.Modes[1].Footprint == 8);
		TestTrue(TEXT("User upload, rating capped at 5, no id from a bad uuid"), Entries[1].Uploader == EStageGdtfShareUploader::User && Entries[1].Rating == 5.f && !Entries[1].FixtureTypeId.IsValid());
		TestEqual(TEXT("Another uploader kind is not guessed"), Entries[2].Uploader, EStageGdtfShareUploader::Unknown);
	}
	TestFalse(TEXT("result false is not a list"), StageGdtfShare::ParseList(Utf8(TEXT("{\"result\":false,\"list\":[]}")), Entries, Skipped));
	TestFalse(TEXT("No list"), StageGdtfShare::ParseList(Utf8(TEXT("{\"result\":true}")), Entries, Skipped));
	TestFalse(TEXT("Not JSON"), StageGdtfShare::ParseList(Utf8(TEXT("[1,2]")), Entries, Skipped));
	FString Long = FString::ChrN(StageGdtfShare::MaxTextLength + 50, TEXT('x'));
	FString ManyModes;
	for (int32 Index = 0; Index < StageGdtfShare::MaxModes + 5; ++Index)
	{
		ManyModes += FString::Printf(TEXT("%s{\"name\":\"M%d\",\"dmxfootprint\":-3}"), Index > 0 ? TEXT(",") : TEXT(""), Index);
	}
	TestTrue(TEXT("A hostile entry reads"), StageGdtfShare::ParseList(Utf8(FString::Printf(TEXT("{\"result\":true,\"list\":[{\"rid\":5.5,\"fixture\":\"a\",\"manufacturer\":\"b\"},"
		"{\"rid\":9,\"fixture\":\"%s\",\"manufacturer\":\"  Ac\\u0001me  \",\"filesize\":-1,\"lastModified\":\"-5\",\"modes\":[%s]}]}"), *Long, *ManyModes)), Entries, Skipped));
	TestTrue(TEXT("A fractional rid is skipped; texts are cleaned and capped; modes capped; bad numbers dropped"), Entries.Num() == 1 && Skipped == 1
		&& Entries[0].Fixture.Len() == StageGdtfShare::MaxTextLength && Entries[0].Manufacturer == TEXT("Acme") && Entries[0].FileSizeBytes == 0
		&& Entries[0].LastModifiedUtc == FDateTime() && Entries[0].Modes.Num() == StageGdtfShare::MaxModes && Entries[0].Modes[0].Footprint == 0);

	TestEqual(TEXT("One cookie with attributes"), StageGdtfShare::JoinCookies({ TEXT("PHPSESSID=abc; path=/; HttpOnly") }), FString(TEXT("PHPSESSID=abc")));
	TestEqual(TEXT("Merged headers with an Expires date"), StageGdtfShare::JoinCookies({ TEXT("a=1; Expires=Wed, 21 Oct 2026 07:28:00 GMT; path=/, b=2") }), FString(TEXT("a=1; b=2")));
	TestEqual(TEXT("Deleted, empty, unnamed and unsafe cookies are dropped"),
		StageGdtfShare::JoinCookies({ TEXT("a=deleted"), TEXT("b="), TEXT("=c"), TEXT("d=x y"), TEXT("e\"=1"), TEXT("f=\"ok\"") }), FString(TEXT("f=\"ok\"")));
	TestEqual(TEXT("A repeated name keeps the last value"), StageGdtfShare::JoinCookies({ TEXT("s=1"), TEXT("t=2"), TEXT("s=3") }), FString(TEXT("t=2; s=3")));
	TestTrue(TEXT("Nothing usable"), StageGdtfShare::JoinCookies({}).IsEmpty());

	TArray<FStageGdtfShareEntry> Catalog;
	const auto Add = [&Catalog](int32 Rid, const TCHAR* Manufacturer, const TCHAR* Fixture, EStageGdtfShareUploader Uploader, int64 Modified)
	{
		FStageGdtfShareEntry& Entry = Catalog.AddDefaulted_GetRef();
		Entry.RevisionId = Rid;
		Entry.Manufacturer = Manufacturer;
		Entry.Fixture = Fixture;
		Entry.Uploader = Uploader;
		Entry.LastModifiedUtc = FDateTime::FromUnixTimestamp(Modified);
	};
	Add(1, TEXT("Robe"), TEXT("MegaPointe"), EStageGdtfShareUploader::Manufacturer, 100);
	Add(2, TEXT("Robe"), TEXT("MegaPointe"), EStageGdtfShareUploader::User, 200);
	Add(3, TEXT("Clay Paky"), TEXT("Sharpy"), EStageGdtfShareUploader::Manufacturer, 100);
	Add(4, TEXT("robe"), TEXT("Spiider"), EStageGdtfShareUploader::Manufacturer, 100);
	TestTrue(TEXT("Every word must match, ignoring case; newest revision first"), StageGdtfShare::Search(Catalog, TEXT("ROBE mega"), {}, 10) == TArray<int32>({ 1, 0 }));
	TestTrue(TEXT("Sorted by manufacturer, then fixture"), StageGdtfShare::Search(Catalog, TEXT(""), {}, 10) == TArray<int32>({ 2, 1, 0, 3 }));
	TestTrue(TEXT("Uploader filter"), StageGdtfShare::Search(Catalog, TEXT("robe"), EStageGdtfShareUploader::Manufacturer, 10) == TArray<int32>({ 0, 3 }));
	TestTrue(TEXT("Result cap"), StageGdtfShare::Search(Catalog, TEXT(""), {}, 1).Num() == 1);
	TestTrue(TEXT("No match"), StageGdtfShare::Search(Catalog, TEXT("robe sharpy"), {}, 10).IsEmpty());

	FStageGdtfShareEntry Named;
	Named.RevisionId = 42;
	Named.Manufacturer = TEXT("Robe");
	Named.Fixture = TEXT("MegaPointe/Spot:2");
	Named.Revision = TEXT("FW 1.0");
	TestEqual(TEXT("GDTF Share naming, unsafe characters replaced"), StageGdtfShare::MakeFileName(Named), FString(TEXT("Robe@MegaPointe_Spot_2@FW 1.0.gdtf")));
	TestEqual(TEXT("Label"), StageGdtfShare::MakeLabel(Named), FString(TEXT("Robe MegaPointe/Spot:2 (FW 1.0)")));
	Named.Revision.Reset();
	TestEqual(TEXT("No revision part without a revision"), StageGdtfShare::MakeFileName(Named), FString(TEXT("Robe@MegaPointe_Spot_2.gdtf")));
	Named.Manufacturer = TEXT("...");
	TestEqual(TEXT("Nothing usable left: the rid names it"), StageGdtfShare::MakeFileName(Named), FString(TEXT("GDTF_Share_42.gdtf")));
	Named.Manufacturer = FString::ChrN(100, TEXT('m'));
	TestTrue(TEXT("Parts are capped"), StageGdtfShare::MakeFileName(Named).StartsWith(FString::ChrN(60, TEXT('m')) + TEXT("@")));
	TestTrue(TEXT("A file name the library accepts"), UStageFixtureLibrarySubsystem::IsFixtureFile(StageGdtfShare::MakeFileName(Named)));

	TestTrue(TEXT("A GDTF file is a ZIP"), StageGdtfShare::IsZipFile(StageGdtfTestFiles::MakeGdtf(StageGdtfTestFiles::SpotXml())));
	TestFalse(TEXT("JSON is not"), StageGdtfShare::IsZipFile(Utf8(TEXT("{\"result\":false,\"error\":\"File does not exist.\"}"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageGdtfShareSessionTest, "StageCraft.Fixtures.GdtfShare.Session", StageGdtfShareTests::Flags)

bool FStageGdtfShareSessionTest::RunTest(const FString& Parameters)
{
	using namespace StageGdtfShareTests;

	FStageTestGameInstance Game;
	UStageGdtfShareSubsystem* Share = Game.Subsystem<UStageGdtfShareSubsystem>();
	UStageFixtureLibrarySubsystem* Library = Game.Subsystem<UStageFixtureLibrarySubsystem>();
	if (!TestTrue(TEXT("The test game instance starts"), Game.IsValid()) || !TestNotNull(TEXT("GDTF Share"), Share) || !TestNotNull(TEXT("Library"), Library))
	{
		return false;
	}
	TestTrue(TEXT("The empty library is read"), Game.Pump([Library]() { return !Library->HasLoadsInProgress(); }));
	TSharedRef<FFakeServer> Server = MakeShared<FFakeServer>();
	Server->SpotFile = StageGdtfTestFiles::MakeGdtf(StageGdtfTestFiles::SpotXml());
	Share->SetTransportForTesting([Server](const FStageGdtfShareHttpRequest& Request, FOnStageGdtfShareResponse Answer, FOnStageGdtfShareProgress Progress)
	{
		Server->Answer(Request, MoveTemp(Answer), MoveTemp(Progress));
	});
	TStrongObjectPtr<UStageTestEventRecorder> Recorder(NewObject<UStageTestEventRecorder>());
	Recorder->Bind(Game.Get());
	ON_SCOPE_EXIT { Recorder->Unbind(); };

	AddInfo(TEXT("Signed out, nothing is sent."));
	TestEqual(TEXT("List refused"), Str(Share->RefreshList(true).Code), Str(EStageGdtfShareResult::SignInRequired));
	TestEqual(TEXT("Download refused"), Str(Share->Download(101).Code), Str(EStageGdtfShareResult::SignInRequired));
	TestEqual(TEXT("Empty password refused"), Str(Share->SignIn(TEXT("alice"), TEXT("")).Code), Str(EStageGdtfShareResult::InvalidRequest));
	TestEqual(TEXT("Over-long name refused"), Str(Share->SignIn(FString::ChrN(UStageGdtfShareSubsystem::MaxUserNameLength + 1, TEXT('a')), TEXT("x")).Code), Str(EStageGdtfShareResult::InvalidRequest));
	TestEqual(TEXT("No request went out"), Server->Requests.Num(), 0);
	TestEqual(TEXT("Attribution"), UStageGdtfShareSubsystem::GetAttribution().ToString(), FString(TEXT("Powered by GDTF")));

	AddInfo(TEXT("A wrong password."));
	TestTrue(TEXT("Sign-in sent"), Share->SignIn(TEXT("alice"), TEXT("wrong")).IsSuccess());
	TestEqual(TEXT("Signed out again"), Str(Share->GetState()), Str(EStageGdtfShareState::SignedOut));
	TestTrue(TEXT("Reported as bad credentials"), !Recorder->GdtfShareResults.IsEmpty() && Recorder->GdtfShareResults.Last().Code == EStageGdtfShareResult::BadCredentials);
	TestTrue(TEXT("The login went to the API over https as JSON"), Server->Requests.Num() == 1 && Server->Requests[0].Verb == TEXT("POST")
		&& Server->Requests[0].Url == FString(StageGdtfShare::DefaultBaseUrl) + TEXT("login.php") && Server->Requests[0].ContentType == TEXT("application/json"));

	AddInfo(TEXT("The right password: a session, then the list."));
	TestTrue(TEXT("Sign-in sent"), Share->SignIn(TEXT(" alice "), TEXT("p\"ss word")).IsSuccess());
	TestEqual(TEXT("Signed in"), Str(Share->GetState()), Str(EStageGdtfShareState::SignedIn));
	TestEqual(TEXT("The trimmed name"), Share->GetUserName(), FString(TEXT("alice")));
	TestEqual(TEXT("The session cookie from merged Set-Cookie headers"), Share->GetSessionCookieForTesting(), FString(TEXT("PHPSESSID=abc123; theme=dark")));
	TestTrue(TEXT("The list loads"), Game.Pump([Share]() { return !Share->IsListLoading(); }, 10.0));
	TestEqual(TEXT("Six files listed"), Share->GetEntries().Num(), 6);
	TestEqual(TEXT("One list request"), Server->ListRequests, 1);
	for (const FStageGdtfShareHttpRequest& Request : Server->Requests)
	{
		TestFalse(TEXT("The password is only in the login body"), !Request.Url.EndsWith(TEXT("login.php")) && (FromUtf8(Request.Body).Contains(TEXT("p\"ss")) || Request.Url.Contains(TEXT("ss word"))));
	}

	AddInfo(TEXT("The list is fetched again only when old, or forced."));
	TestTrue(TEXT("A fresh list is kept"), Share->RefreshList(false).IsSuccess() && !Share->IsListLoading() && Server->ListRequests == 1);
	Share->AgeListForTesting();
	TestTrue(TEXT("An old list is fetched again"), Share->RefreshList(false).IsSuccess());
	TestEqual(TEXT("A second request meanwhile is busy"), Str(Share->RefreshList(true).Code), Str(EStageGdtfShareResult::Busy));
	TestTrue(TEXT("It loads"), Game.Pump([Share]() { return !Share->IsListLoading(); }, 10.0) && Server->ListRequests == 2);

	AddInfo(TEXT("Downloads: refused ones never reach the network."));
	TestEqual(TEXT("Not in the list"), Str(Share->Download(999).Code), Str(EStageGdtfShareResult::InvalidRequest));
	TestEqual(TEXT("Listed over the size cap"), Str(Share->Download(106).Code), Str(EStageGdtfShareResult::TooLarge));

	AddInfo(TEXT("A good file becomes an import review, under GDTF Share's file name."));
	TestTrue(TEXT("Download starts"), Share->Download(101).IsSuccess());
	TestTrue(TEXT("The review appears"), Game.Pump([Library]() { return Library->GetReviews().Num() == 1 && !Library->HasLoadsInProgress(); }, 10.0));
	const FStageGdtfShareDownload* Spot = Share->GetDownloads().FindByPredicate([](const FStageGdtfShareDownload& Row) { return Row.RevisionId == 101; });
	TestTrue(TEXT("Delivered"), Spot && Spot->State == EStageGdtfShareDownloadState::Delivered && Spot->ReceivedBytes == Server->SpotFile.Num()
		&& Spot->FileName == TEXT("StageCraft@Test Spot@Rev A.gdtf"));
	TestTrue(TEXT("The download carried the session"), Server->Requests.Last().Cookie == TEXT("PHPSESSID=abc123; theme=dark") && Server->Requests.Last().MaxResponseBytes == StageGdtfShare::MaxDownloadBytes);
	TestEqual(TEXT("Nothing is in the library before the review"), Library->GetTypes().Num(), 0);
	if (Library->GetReviews().Num() == 1)
	{
		const FStageFixtureImportReview Review = Library->GetReviews()[0];
		TestTrue(TEXT("The review can be added"), Review.bCanImport && Review.SourceFile == TEXT("StageCraft@Test Spot@Rev A.gdtf"));
		TestTrue(TEXT("The user adds it"), Library->ConfirmImport(Review.ReviewId, {}).IsSuccess());
		FGuid SpotId;
		FGuid::Parse(StageGdtfTestFiles::SpotId, SpotId);
		const FStageFixtureLibraryType* Type = Library->FindType(SpotId);
		TestTrue(TEXT("In the library under its GDTF Share name"), Type && Type->SourceFileName == TEXT("StageCraft@Test Spot@Rev A.gdtf"));
	}

	AddInfo(TEXT("Failures say why."));
	TestTrue(TEXT("Gone file starts"), Share->Download(102).IsSuccess());
	TestTrue(TEXT("Gone file: 404"), Recorder->GdtfShareResults.Last().Code == EStageGdtfShareResult::NotFound);
	TestTrue(TEXT("JSON refusal starts"), Share->Download(103).IsSuccess());
	TestTrue(TEXT("JSON refusal in a 200 answer"), Recorder->GdtfShareResults.Last().Code == EStageGdtfShareResult::NotFound);
	TestTrue(TEXT("Not a ZIP starts"), Share->Download(104).IsSuccess());
	TestTrue(TEXT("Not a GDTF file"), Recorder->GdtfShareResults.Last().Code == EStageGdtfShareResult::BadResponse);
	const FStageGdtfShareDownload* Failed = Share->GetDownloads().FindByPredicate([](const FStageGdtfShareDownload& Row) { return Row.RevisionId == 104; });
	TestTrue(TEXT("The row failed with a message"), Failed && Failed->State == EStageGdtfShareDownloadState::Failed && !Failed->Message.IsEmpty());

	AddInfo(TEXT("A slow download: busy, cancelled, its late answer dropped."));
	TestTrue(TEXT("Slow download starts"), Share->Download(105).IsSuccess());
	TestEqual(TEXT("The same file again is busy"), Str(Share->Download(105).Code), Str(EStageGdtfShareResult::Busy));
	TestTrue(TEXT("Cancel"), Share->CancelDownload(105));
	TestFalse(TEXT("Cancel twice"), Share->CancelDownload(105));
	TestTrue(TEXT("Reported cancelled"), Recorder->GdtfShareResults.Last().Code == EStageGdtfShareResult::Cancelled);
	const int32 ResultsBefore = Recorder->GdtfShareResults.Num();
	FStageGdtfShareHttpResponse Late;
	Late.bConnected = true;
	Late.Status = 200;
	Late.Body = Server->SpotFile;
	Server->HeldAnswer.ExecuteIfBound(Late);
	TestEqual(TEXT("The late answer changes nothing"), Recorder->GdtfShareResults.Num(), ResultsBefore);
	TestEqual(TEXT("No second review"), Library->GetReviews().Num(), 0);

	AddInfo(TEXT("A session GDTF Share ended."));
	Server->bSessionValid = false;
	Share->AgeListForTesting();
	TestTrue(TEXT("List request sent"), Share->RefreshList(false).IsSuccess());
	TestEqual(TEXT("Signed out by the 401"), Str(Share->GetState()), Str(EStageGdtfShareState::SignedOut));
	TestTrue(TEXT("Told to sign in again"), Recorder->GdtfShareResults.Last().Code == EStageGdtfShareResult::SessionExpired);
	TestTrue(TEXT("The cookie is gone"), Share->GetSessionCookieForTesting().IsEmpty());
	TestEqual(TEXT("The last list stays visible"), Share->GetEntries().Num(), 6);

	AddInfo(TEXT("Signing out cancels running downloads."));
	TestTrue(TEXT("Signed in again"), Share->SignIn(TEXT("alice"), TEXT("p\"ss word")).IsSuccess() && Share->GetState() == EStageGdtfShareState::SignedIn);
	TestTrue(TEXT("Slow download again"), Share->Download(105).IsSuccess());
	Share->SignOut();
	const FStageGdtfShareDownload* Slow = Share->GetDownloads().FindByPredicate([](const FStageGdtfShareDownload& Row) { return Row.RevisionId == 105; });
	TestTrue(TEXT("Cancelled by the sign-out"), Share->GetState() == EStageGdtfShareState::SignedOut && Slow && Slow->State == EStageGdtfShareDownloadState::Failed);
	Share->ClearFinishedDownloads();
	TestEqual(TEXT("Finished rows cleared"), Share->GetDownloads().Num(), 0);
	Share->SignOut();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageGdtfShareSecurityTest, "StageCraft.Fixtures.GdtfShare.Rules", StageGdtfShareTests::Flags)

bool FStageGdtfShareSecurityTest::RunTest(const FString& Parameters)
{
	using namespace StageGdtfShareTests;

	{
		TUniquePtr<FStageScopedDefault> Insecure = FStageScopedDefault::Make(UStageGdtfShareSubsystem::StaticClass(), TEXT("BaseUrl"), FString(TEXT("http://gdtf-share.example/")));
		if (!TestTrue(TEXT("The API root can be set for the test"), Insecure.IsValid() && Insecure->IsValid()))
		{
			return false;
		}
		AddExpectedMessage(TEXT("not an https address"), EAutomationExpectedMessageFlags::Contains, 1);
		FStageTestGameInstance Game;
		UStageGdtfShareSubsystem* Share = Game.Subsystem<UStageGdtfShareSubsystem>();
		int32 Sent = 0;
		if (TestNotNull(TEXT("GDTF Share"), Share))
		{
			Share->SetTransportForTesting([&Sent](const FStageGdtfShareHttpRequest&, FOnStageGdtfShareResponse, FOnStageGdtfShareProgress) { ++Sent; });
			TestEqual(TEXT("A password is never sent to an http address"), Str(Share->SignIn(TEXT("alice"), TEXT("secret")).Code), Str(EStageGdtfShareResult::NotAllowed));
			TestEqual(TEXT("Nothing sent"), Sent, 0);
		}
	}

	FStageTestGameInstance RulesGame;
	if (!TestTrue(TEXT("A game instance for the world"), RulesGame.IsValid()))
	{
		return false;
	}
	FScopedWorld Scope(RulesGame.Get());
	AStageCraftGameModeBase* GameMode = Scope.World->SpawnActor<AStageCraftGameModeBase>();
	const APlayerController* Player = Scope.World->SpawnActor<APlayerController>();
	if (!TestTrue(TEXT("Game mode and player"), GameMode && Player))
	{
		return false;
	}
	TestFalse(TEXT("No player: refused"), GameMode->EvaluateOnlineFixtureSource(nullptr).IsSuccess());
	TestTrue(TEXT("Allowed by default"), GameMode->EvaluateOnlineFixtureSource(Player).IsSuccess());
	const FStructProperty* RulesProperty = FindFProperty<FStructProperty>(AStageCraftGameModeBase::StaticClass(), TEXT("SessionRules"));
	if (TestNotNull(TEXT("Session rules"), RulesProperty))
	{
		FStageSessionRules* Rules = RulesProperty->ContainerPtrToValuePtr<FStageSessionRules>(GameMode);
		Rules->bAllowOnlineFixtureSources = false;
		FStageEconomyResultInfo Verdict = GameMode->EvaluateOnlineFixtureSource(Player);
		TestTrue(TEXT("Switched off: refused with a reason"), !Verdict.IsSuccess() && Verdict.Code == EStageEconomyResult::SessionRuleViolation && !Verdict.Message.IsEmpty());
		Rules->bAllowOnlineFixtureSources = true;
		Rules->bAllowFixtureImport = false;
		Verdict = GameMode->EvaluateOnlineFixtureSource(Player);
		TestTrue(TEXT("Without fixture import there are no downloads either"), !Verdict.IsSuccess() && Verdict.Code == EStageEconomyResult::SessionRuleViolation);
	}
	return true;
}

#endif
