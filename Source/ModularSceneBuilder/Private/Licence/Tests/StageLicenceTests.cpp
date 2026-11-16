// Copyright Epic Games, Inc. All Rights Reserved.

// Tests of StageCraft Pro licensing on the client (Docs/ADR/0034 §17, STATE.md #93): token verification with the engine's OpenSSL against
// tokens the cloud server's own signer produced (src/licence/token.ts), evaluation (machine, expiry, clock rollback), the licence subsystem
// against a fake StageCraft Cloud (refresh, offline reload, refusals, checkout, release), the GameMode's Pro gates and the DMX limits.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Licence; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Cloud/StageCloudSubsystem.h"
#include "Economy/StageEconomyTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Game/StageCraftGameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Licence/StageLicenceSubsystem.h"
#include "Licence/StageLicenceToken.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Show/StageDMXNetworkTypes.h"
#include "Tests/StageTestGameInstance.h"

namespace StageLicenceTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	/** The machine the server-signed test tokens below were issued to. */
	const TCHAR* const Machine = TEXT("abababababababababababababababababababababababababababababababab");
	const TCHAR* const Account = TEXT("4f1c2d3e-0000-4000-8000-000000000001");

	// Signed with the development key (kid dev-1) by Services/StageCraftCloud's createSigner: all five Pro tags, monthly, issued
	// 2025-11-16, valid until 2100.
	const TCHAR* const ValidToken = TEXT("SC1.eyJ2IjoxLCJraWQiOiJkZXYtMSIsInN1YiI6IjRmMWMyZDNlLTAwMDAtNDAwMC04MDAwLTAwMDAwMDAwMDAwMSIsIm1pZCI6ImFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWIiLCJwbGFuIjoibW9udGhseSIsImVudCI6WyJTdGFnZUNyYWZ0LkVudGl0bGVtZW50LlByby5EbXhVbml2ZXJzZXMiLCJTdGFnZUNyYWZ0LkVudGl0bGVtZW50LlByby5EbXhJbiIsIlN0YWdlQ3JhZnQuRW50aXRsZW1lbnQuUHJvLk1pZGlPc2MiLCJTdGFnZUNyYWZ0LkVudGl0bGVtZW50LlByby5XZWJSZW1vdGUiLCJTdGFnZUNyYWZ0LkVudGl0bGVtZW50LlByby5NdnJFeHBvcnQiXSwiaWF0IjoxNzYzMjUxMjAwLCJleHAiOjQxMDI0NDQ4MDB9.QMw7bpbSxnY0iNXqKRnHmzMmGvI6jPQx4ZGcKRwOZrFuTFPailqgGUsaRHyQpSr4Vrlo2x_d55jfOmCRavb-Dg");
	// The same claims issued 2020-09-13 for 14 days: expired.
	const TCHAR* const ExpiredToken = TEXT("SC1.eyJ2IjoxLCJraWQiOiJkZXYtMSIsInN1YiI6IjRmMWMyZDNlLTAwMDAtNDAwMC04MDAwLTAwMDAwMDAwMDAwMSIsIm1pZCI6ImFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWIiLCJwbGFuIjoibW9udGhseSIsImVudCI6WyJTdGFnZUNyYWZ0LkVudGl0bGVtZW50LlByby5EbXhVbml2ZXJzZXMiLCJTdGFnZUNyYWZ0LkVudGl0bGVtZW50LlByby5EbXhJbiIsIlN0YWdlQ3JhZnQuRW50aXRsZW1lbnQuUHJvLk1pZGlPc2MiLCJTdGFnZUNyYWZ0LkVudGl0bGVtZW50LlByby5XZWJSZW1vdGUiLCJTdGFnZUNyYWZ0LkVudGl0bGVtZW50LlByby5NdnJFeHBvcnQiXSwiaWF0IjoxNjAwMDAwMDAwLCJleHAiOjE2MDEyMDk2MDB9._6oZiniutW_JBw_6DnFVbYdL0jMNl0dhBrewgOXTlpeibuTq8lcq_H1r0_734ATU6dIxLLznCy8EqZFRiPP4BA");
	// Yearly; MidiOsc, a Pro tag this build does not know, and a name that is not a tag.
	const TCHAR* const PartialToken = TEXT("SC1.eyJ2IjoxLCJraWQiOiJkZXYtMSIsInN1YiI6IjRmMWMyZDNlLTAwMDAtNDAwMC04MDAwLTAwMDAwMDAwMDAwMSIsIm1pZCI6ImFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWIiLCJwbGFuIjoieWVhcmx5IiwiZW50IjpbIlN0YWdlQ3JhZnQuRW50aXRsZW1lbnQuUHJvLk1pZGlPc2MiLCJTdGFnZUNyYWZ0LkVudGl0bGVtZW50LlByby5GdXR1cmUiLCJOb3QuQS5UYWciXSwiaWF0IjoxNzYzMjUxMjAwLCJleHAiOjQxMDI0NDQ4MDB9.V329LcnNoHY9WqZLAHUQ5JLhKI_YUtCfN5HR5SD3DumeIyTH0YNIsYiUYxP0Dd-QWWojpxR_aKkcYdd4NlIrDg");
	// ValidToken's claims under kid dev-1, signed by another key: a forgery.
	const TCHAR* const ForgedToken = TEXT("SC1.eyJ2IjoxLCJraWQiOiJkZXYtMSIsInN1YiI6IjRmMWMyZDNlLTAwMDAtNDAwMC04MDAwLTAwMDAwMDAwMDAwMSIsIm1pZCI6ImFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWIiLCJwbGFuIjoibW9udGhseSIsImVudCI6WyJTdGFnZUNyYWZ0LkVudGl0bGVtZW50LlByby5EbXhVbml2ZXJzZXMiLCJTdGFnZUNyYWZ0LkVudGl0bGVtZW50LlByby5EbXhJbiIsIlN0YWdlQ3JhZnQuRW50aXRsZW1lbnQuUHJvLk1pZGlPc2MiLCJTdGFnZUNyYWZ0LkVudGl0bGVtZW50LlByby5XZWJSZW1vdGUiLCJTdGFnZUNyYWZ0LkVudGl0bGVtZW50LlByby5NdnJFeHBvcnQiXSwiaWF0IjoxNzYzMjUxMjAwLCJleHAiOjQxMDI0NDQ4MDB9.rUTgsYZfgq6ozdsX_b5EqmP4D5ybbrTfDjh3pg6Skbwm-Q6fEGup-myO83JTfnLp1Cng-1VYr1ykrmGfFTOSCg");
	// Signed by the development key but naming a key id nobody trusts.
	const TCHAR* const UnknownKeyToken = TEXT("SC1.eyJ2IjoxLCJraWQiOiJwcm9kLXgiLCJzdWIiOiI0ZjFjMmQzZS0wMDAwLTQwMDAtODAwMC0wMDAwMDAwMDAwMDEiLCJtaWQiOiJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiYWJhYmFiIiwicGxhbiI6Im1vbnRobHkiLCJlbnQiOlsiU3RhZ2VDcmFmdC5FbnRpdGxlbWVudC5Qcm8uRG14VW5pdmVyc2VzIiwiU3RhZ2VDcmFmdC5FbnRpdGxlbWVudC5Qcm8uRG14SW4iLCJTdGFnZUNyYWZ0LkVudGl0bGVtZW50LlByby5NaWRpT3NjIiwiU3RhZ2VDcmFmdC5FbnRpdGxlbWVudC5Qcm8uV2ViUmVtb3RlIiwiU3RhZ2VDcmFmdC5FbnRpdGxlbWVudC5Qcm8uTXZyRXhwb3J0Il0sImlhdCI6MTc2MzI1MTIwMCwiZXhwIjo0MTAyNDQ0ODAwfQ.i3Jeg9CYW1aBQE3-s0mXFhX6QqI-ViKhNy4_BMc9FMToXkg1dRJ3aIar2P6ii62YRINlugzkMHLv3Fx-6oRZBA");

	constexpr int64 IssuedAt = 1763251200;
	constexpr int64 ValidUntil = 4102444800;

	TArray<uint8> Hex(const TCHAR* Text)
	{
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(FCString::Strlen(Text) / 2);
		HexToBytes(FString(Text), Bytes.GetData());
		return Bytes;
	}

	/** Sets StageCraft.Licence.DevAssumePro for the scope (DefaultEngine.ini sets 1 for development builds) and restores it after. */
	struct FScopedDevAssumePro
	{
		IConsoleVariable* Variable = nullptr;
		int32 Previous = 0;

		explicit FScopedDevAssumePro(int32 Value)
		{
			Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("StageCraft.Licence.DevAssumePro"));
			if (Variable)
			{
				Previous = Variable->GetInt();
				Variable->Set(Value, ECVF_SetByCode);
			}
		}

		~FScopedDevAssumePro()
		{
			if (Variable)
			{
				Variable->Set(Previous, ECVF_SetByCode);
			}
		}
	};

	FStageCloudResponse Answer(int32 Status, const FString& Json)
	{
		FStageCloudResponse Response;
		Response.bConnected = true;
		Response.Status = Status;
		const FTCHARToUTF8 Utf8(*Json, Json.Len());
		Response.Body = TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		return Response;
	}

	FStageCloudResponse ErrorAnswer(int32 Status, const TCHAR* Code)
	{
		return Answer(Status, FString::Printf(TEXT("{\"error\":{\"code\":\"%s\",\"message\":\"refused by the test server\"}}"), Code));
	}

	/**
	 * A fake StageCraft Cloud on UStageCloudSubsystem's test transport: signs in through the device flow, then answers the licence
	 * routes from the fields below. No network, no browser.
	 */
	struct FFakeCloud
	{
		TArray<FStageCloudHttpRequest> Requests;
		/** What POST /v1/licences answers. */
		FStageCloudResponse LicenceAnswer = Answer(200, FString::Printf(TEXT("{\"token\":\"%s\"}"), ValidToken));
		/** What GET /v1/me/subscription answers. */
		FStageCloudResponse SubscriptionAnswer = Answer(200, TEXT("{\"status\":\"active\",\"entitled\":true,\"plan\":\"monthly\",\"currentPeriodEnd\":\"2026-12-16T00:00:00.000Z\",\"cancelAtPeriodEnd\":false}"));
		int32 Polls = 0;

		UStageCloudSubsystem::FTestTransport Transport()
		{
			return [this](const FStageCloudHttpRequest& Request, FOnStageCloudResponse OnDone)
			{
				Requests.Add(Request);
				OnDone.ExecuteIfBound(Respond(Request));
			};
		}

		FStageCloudResponse Respond(const FStageCloudHttpRequest& Request)
		{
			const FString& Url = Request.Url;
			if (Url.EndsWith(TEXT("/v1/auth/device")))
			{
				return Answer(200, TEXT("{\"deviceCode\":\"scd_device\",\"userCode\":\"WXYZ-1234\",\"verificationUri\":\"https://cloud.test/auth/verify\",\"expiresIn\":600,\"interval\":1}"));
			}
			if (Url.EndsWith(TEXT("/v1/auth/device/token")))
			{
				return ++Polls < 2 ? ErrorAnswer(400, TEXT("authorization_pending"))
					: Answer(200, TEXT("{\"sessionToken\":\"scs_session_token_0123456789abcdef\",\"expiresAt\":\"2099-12-07T00:00:00Z\",\"accountId\":\"acc-1\"}"));
			}
			if (Url.EndsWith(TEXT("/v1/me")))
			{
				return Answer(200, TEXT("{\"accountId\":\"acc-1\",\"displayName\":\"Gevor\",\"email\":null,\"roles\":[],\"seller\":null}"));
			}
			if (Url.EndsWith(TEXT("/v1/licences")))
			{
				return LicenceAnswer;
			}
			if (Url.EndsWith(TEXT("/v1/licences/release")))
			{
				return Answer(200, TEXT("{\"released\":true}"));
			}
			if (Url.Contains(TEXT("/v1/me/subscription")))
			{
				return SubscriptionAnswer;
			}
			if (Url.EndsWith(TEXT("/v1/subscriptions/checkout")))
			{
				return Answer(200, TEXT("{\"checkoutId\":\"c1\",\"plan\":\"yearly\",\"state\":\"Open\",\"url\":\"https://checkout.test/pay/c1\",\"expiresAt\":null}"));
			}
			if (Url.EndsWith(TEXT("/v1/subscriptions/portal")))
			{
				return Answer(200, TEXT("{\"url\":\"https://billing.test/portal\"}"));
			}
			return ErrorAnswer(404, TEXT("NotFound"));
		}

		int32 Count(const TCHAR* PathEnd) const
		{
			return Requests.FilterByPredicate([PathEnd](const FStageCloudHttpRequest& Request) { return Request.Url.EndsWith(PathEnd); }).Num();
		}
	};

	/** Connects Game's cloud subsystem to Cloud, and signs in when bSignIn. */
	bool Connect(FStageTestGameInstance& Game, FFakeCloud& Cloud, bool bSignIn)
	{
		UStageCloudSubsystem* Subsystem = Game.Subsystem<UStageCloudSubsystem>();
		if (!Subsystem)
		{
			return false;
		}
		Subsystem->SetTransportForTesting(Cloud.Transport());
		Subsystem->SetBaseUrlForTesting(TEXT("https://cloud.test"));
		// A second game instance on the same folder restores the first one's session from the vault, as the next run of the app does.
		if (!bSignIn || Subsystem->IsSignedIn())
		{
			return true;
		}
		FText Error;
		return Subsystem->StartSignIn(Error) && Game.Pump([Subsystem]() { return Subsystem->IsSignedIn() && !Subsystem->GetAccount().DisplayName.IsEmpty(); }, 10.0);
	}

	bool WriteLicenceFile(const UStageLicenceSubsystem& Licence, const FString& Token, int64 Seen = 0)
	{
		const FString Path = Licence.GetLicencePathForTesting();
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
		return FFileHelper::SaveStringToFile(FString::Printf(TEXT("{\"token\":\"%s\",\"seen\":%lld}"), *Token, Seen), *Path);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageLicenceTokenTest, "StageCraft.Licence.Token", StageLicenceTests::Flags)

bool FStageLicenceTokenTest::RunTest(const FString& Parameters)
{
	using namespace StageLicenceTests;

	// RFC 8032 §7.1, test 1: the engine's OpenSSL verifies Ed25519 as the standard says.
	const TArray<uint8> RfcKey = Hex(TEXT("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"));
	TArray<uint8> RfcSignature = Hex(TEXT("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"));
	TestTrue(TEXT("RFC 8032 test 1 verifies"), StageLicenceToken::VerifyEd25519({}, RfcSignature, RfcKey));
	TestFalse(TEXT("...not for another message"), StageLicenceToken::VerifyEd25519(Hex(TEXT("72")), RfcSignature, RfcKey));
	RfcSignature[10] ^= 0x01;
	TestFalse(TEXT("...not with one bit changed"), StageLicenceToken::VerifyEd25519({}, RfcSignature, RfcKey));
	TestFalse(TEXT("A short signature is refused"), StageLicenceToken::VerifyEd25519({}, TArray<uint8>{ 1, 2, 3 }, RfcKey));
	TestFalse(TEXT("A short key is refused"), StageLicenceToken::VerifyEd25519({}, RfcSignature, TArray<uint8>{ 1, 2, 3 }));

	// Strict base64url.
	TArray<uint8> Bytes;
	TestTrue(TEXT("base64url decodes"), StageLicenceToken::DecodeBase64Url(TEXT("_-8"), Bytes) && Bytes == TArray<uint8>{ 0xFF, 0xEF });
	TestFalse(TEXT("padding is refused"), StageLicenceToken::DecodeBase64Url(TEXT("QQ=="), Bytes));
	TestFalse(TEXT("standard alphabet is refused"), StageLicenceToken::DecodeBase64Url(TEXT("a+b/"), Bytes));
	TestFalse(TEXT("impossible length is refused"), StageLicenceToken::DecodeBase64Url(TEXT("QUJDR"), Bytes));
	TestFalse(TEXT("non-zero spare bits are refused"), StageLicenceToken::DecodeBase64Url(TEXT("QR"), Bytes));

	// Tokens from the server's own signer verify here: the wire contract holds across Node and OpenSSL.
	const TArray<FStageLicencePublicKey> Keys = StageLicenceToken::GetTrustedKeys();
	FStageLicenceClaims Claims;
	if (TestEqual(TEXT("A server token verifies"), StageLicenceToken::Verify(ValidToken, Keys, Claims), EStageLicenceTokenError::None))
	{
		TestEqual(TEXT("...key id"), Claims.KeyId, FString(TEXT("dev-1")));
		TestEqual(TEXT("...account"), Claims.AccountId, FString(Account));
		TestEqual(TEXT("...machine"), Claims.MachineId, FString(Machine));
		TestEqual(TEXT("...plan"), Claims.Plan, FString(TEXT("monthly")));
		TestEqual(TEXT("...five Pro tags"), Claims.Entitlements.Num(), 5);
		TestEqual(TEXT("...issued"), Claims.IssuedAt, IssuedAt);
		TestEqual(TEXT("...expires"), Claims.ExpiresAt, ValidUntil);

		const int64 Now = IssuedAt + 3600;
		TestEqual(TEXT("Valid on its machine"), StageLicenceToken::Evaluate(Claims, Machine, Now, 0), EStageLicenceValidity::Valid);
		TestEqual(TEXT("Another computer"), StageLicenceToken::Evaluate(Claims, FString::ChrN(64, TEXT('c')), Now, 0), EStageLicenceValidity::WrongMachine);
		TestEqual(TEXT("At expiry"), StageLicenceToken::Evaluate(Claims, Machine, ValidUntil, 0), EStageLicenceValidity::Expired);
		TestEqual(TEXT("Clock set back two days from the newest time seen"),
			StageLicenceToken::Evaluate(Claims, Machine, Now, Now + 2 * 86400), EStageLicenceValidity::ClockRollback);
		TestEqual(TEXT("Less than a day back is drift"), StageLicenceToken::Evaluate(Claims, Machine, Now, Now + 3600), EStageLicenceValidity::Valid);
		TestEqual(TEXT("Clock well before the issue time"), StageLicenceToken::Evaluate(Claims, Machine, IssuedAt - 2 * 86400, 0), EStageLicenceValidity::ClockRollback);
	}
	if (TestEqual(TEXT("An expired token still verifies"), StageLicenceToken::Verify(ExpiredToken, Keys, Claims), EStageLicenceTokenError::None))
	{
		TestEqual(TEXT("...and is expired"), StageLicenceToken::Evaluate(Claims, Machine, IssuedAt, 0), EStageLicenceValidity::Expired);
	}
	TestEqual(TEXT("Another key under our key id: forged"), StageLicenceToken::Verify(ForgedToken, Keys, Claims), EStageLicenceTokenError::BadSignature);
	TestEqual(TEXT("An untrusted key id"), StageLicenceToken::Verify(UnknownKeyToken, Keys, Claims), EStageLicenceTokenError::UnknownKey);
	TestEqual(TEXT("No trusted keys at all"), StageLicenceToken::Verify(ValidToken, {}, Claims), EStageLicenceTokenError::UnknownKey);

	// Any edit to the payload breaks the signature (or the payload itself).
	FString Edited = ValidToken;
	Edited[40] = Edited[40] == TEXT('A') ? TEXT('B') : TEXT('A');
	const EStageLicenceTokenError EditedResult = StageLicenceToken::Verify(Edited, Keys, Claims);
	TestTrue(TEXT("An edited payload is refused"), EditedResult == EStageLicenceTokenError::BadSignature || EditedResult == EStageLicenceTokenError::Malformed);
	TestEqual(TEXT("Wrong prefix"), StageLicenceToken::Verify(FString(ValidToken).Replace(TEXT("SC1."), TEXT("SC2.")), Keys, Claims), EStageLicenceTokenError::Malformed);
	TestEqual(TEXT("No signature"), StageLicenceToken::Verify(TEXT("SC1.eyJ2IjoxfQ"), Keys, Claims), EStageLicenceTokenError::Malformed);
	TestEqual(TEXT("Not JSON"), StageLicenceToken::Verify(TEXT("SC1.bm90IGpzb24.AAAA"), Keys, Claims), EStageLicenceTokenError::Malformed);
	TestEqual(TEXT("Too long"), StageLicenceToken::Verify(FString(TEXT("SC1.")) + FString::ChrN(5000, TEXT('A')) + TEXT(".AA"), Keys, Claims), EStageLicenceTokenError::Malformed);
	TestEqual(TEXT("Empty"), StageLicenceToken::Verify(FString(), Keys, Claims), EStageLicenceTokenError::Malformed);

	// The machine id: SHA-256 of the fixed salt and the install id, lowercase hex (also computed with Node's crypto).
	TestEqual(TEXT("Machine id"), StageLicenceToken::MakeMachineId(TEXT("abc")), FString(TEXT("7b1e90c85cb76d8c8ae62a558e6bafd69698a897f36a9f30eda8f7217cea156a")));
	TestNotEqual(TEXT("...differs per install"), StageLicenceToken::MakeMachineId(TEXT("abd")), StageLicenceToken::MakeMachineId(TEXT("abc")));

#if UE_BUILD_SHIPPING
	TestFalse(TEXT("Shipping never trusts the development key"), Keys.ContainsByPredicate([](const FStageLicencePublicKey& Key) { return Key.KeyId == TEXT("dev-1"); }));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageLicenceSubsystemTest, "StageCraft.Licence.Subsystem", StageLicenceTests::Flags)

bool FStageLicenceSubsystemTest::RunTest(const FString& Parameters)
{
	using namespace StageLicenceTests;
	const FScopedDevAssumePro RealLicence(0);
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("StageLicence"), FGuid::NewGuid().ToString()));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, /*RequireExists*/ false, /*Tree*/ true); };
	FStageTestGameInstanceOptions Options;
	Options.Root = Root;

	{
		FStageTestGameInstance Game(Options);
		UStageLicenceSubsystem* Licence = Game.Subsystem<UStageLicenceSubsystem>();
		if (!TestTrue(TEXT("Game instance"), Game.IsValid()) || !TestNotNull(TEXT("Licence subsystem"), Licence))
		{
			return false;
		}
		TestEqual(TEXT("This computer has a machine id"), Licence->GetMachineIdForTesting().Len(), 64);
		Licence->SetMachineIdForTesting(Machine);
		TestEqual(TEXT("Free without a licence"), Licence->GetStatus(), EStageLicenceStatus::Free);
		TestFalse(TEXT("No Pro entitlement"), Licence->HasProEntitlement(StageCraftTags::Entitlement_Pro_MidiOsc));
		TestFalse(TEXT("Deny by default without a game instance"), UStageLicenceSubsystem::HasPro(nullptr, StageCraftTags::Entitlement_Pro_MidiOsc));

		FText Error;
		TestFalse(TEXT("Refresh needs a signed-in account"), Licence->RequestRefresh(Error));
		TestFalse(TEXT("...and says so"), Error.IsEmpty());
		TestFalse(TEXT("So does a checkout"), Licence->StartCheckout(EStageProPlan::Monthly, Error));

		FFakeCloud Cloud;
		if (!TestTrue(TEXT("Signed in"), Connect(Game, Cloud, /*bSignIn*/ true)))
		{
			return false;
		}
		// Signing in refreshes the licence by itself.
		TestTrue(TEXT("Pro after sign-in"), Game.Pump([Licence]() { return Licence->GetStatus() == EStageLicenceStatus::Pro; }));
		TestTrue(TEXT("The licence request carried this machine"), Cloud.Requests.ContainsByPredicate([](const FStageCloudHttpRequest& Request)
		{
			const FUTF8ToTCHAR Body(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
			return Request.Url.EndsWith(TEXT("/v1/licences")) && FString::ConstructFromPtrSize(Body.Get(), Body.Length()).Contains(Machine);
		}));
		TestTrue(TEXT("Exact Pro tags"), Licence->HasProEntitlement(StageCraftTags::Entitlement_Pro_MidiOsc) && Licence->HasProEntitlement(StageCraftTags::Entitlement_Pro_DmxUniverses));
		TestFalse(TEXT("The parent tag is not an entitlement"), Licence->HasProEntitlement(StageCraftTags::Entitlement_Pro));
		TestTrue(TEXT("HasPro through the game instance"), UStageLicenceSubsystem::HasPro(&Game.Get(), StageCraftTags::Entitlement_Pro_WebRemote));
		TestEqual(TEXT("Plan"), Licence->GetPlan(), FString(TEXT("monthly")));
		TestTrue(TEXT("Expiry"), Licence->GetExpiresUtc() == FDateTime::FromUnixTimestamp(ValidUntil));
		TestTrue(TEXT("Subscription read for display"), Game.Pump([Licence]() { return Licence->GetSubscription().bKnown; }) && Licence->GetSubscription().Status == TEXT("active"));
		TestTrue(TEXT("Stored"), IFileManager::Get().FileExists(*Licence->GetLicencePathForTesting()));

		// An unreachable server keeps the licence: the offline case.
		Cloud.LicenceAnswer = FStageCloudResponse();
		TestTrue(TEXT("Refresh sent"), Licence->RequestRefresh(Error));
		Game.Pump([Licence]() { return !Licence->IsRequestInFlight(); });
		TestEqual(TEXT("Offline keeps Pro"), Licence->GetStatus(), EStageLicenceStatus::Pro);

		// A server token this build cannot verify never replaces a working licence.
		Cloud.LicenceAnswer = Answer(200, FString::Printf(TEXT("{\"token\":\"%s\"}"), ForgedToken));
		Licence->RequestRefresh(Error);
		Game.Pump([Licence]() { return !Licence->IsRequestInFlight(); });
		TestEqual(TEXT("A forged answer keeps Pro"), Licence->GetStatus(), EStageLicenceStatus::Pro);
		TestFalse(TEXT("...and says why"), Licence->GetStatusMessage().IsEmpty());

		// Fewer entitlements: only the known, exact tags count.
		Cloud.LicenceAnswer = Answer(200, FString::Printf(TEXT("{\"token\":\"%s\"}"), PartialToken));
		Licence->RequestRefresh(Error);
		Game.Pump([Licence]() { return !Licence->IsRequestInFlight(); });
		TestTrue(TEXT("Partial: MIDI and OSC"), Licence->HasProEntitlement(StageCraftTags::Entitlement_Pro_MidiOsc));
		TestFalse(TEXT("Partial: no Web Remote"), Licence->HasProEntitlement(StageCraftTags::Entitlement_Pro_WebRemote));
		TestEqual(TEXT("Partial: yearly"), Licence->GetPlan(), FString(TEXT("yearly")));

		// The server says the subscription is gone: Pro ends now and the file goes.
		Cloud.LicenceAnswer = ErrorAnswer(403, TEXT("NoActiveSubscription"));
		Licence->RequestRefresh(Error);
		Game.Pump([Licence]() { return !Licence->IsRequestInFlight(); });
		TestEqual(TEXT("No subscription: Free"), Licence->GetStatus(), EStageLicenceStatus::Free);
		TestFalse(TEXT("...file removed"), IFileManager::Get().FileExists(*Licence->GetLicencePathForTesting()));
		TestFalse(TEXT("...entitlement gone"), Licence->HasProEntitlement(StageCraftTags::Entitlement_Pro_MidiOsc));

		// A checkout not paid yet keeps watching, one poll per tick (never recursing on an instant answer), until it is cancelled.
		Cloud.SubscriptionAnswer = Answer(200, TEXT("{\"status\":\"none\",\"entitled\":false,\"plan\":null,\"currentPeriodEnd\":null,\"cancelAtPeriodEnd\":false}"));
		TestTrue(TEXT("Unpaid checkout starts"), Licence->StartCheckout(EStageProPlan::Monthly, Error));
		Game.PumpFor(0.3);
		const int32 Polls = Cloud.Requests.FilterByPredicate([](const FStageCloudHttpRequest& Request) { return Request.Url.Contains(TEXT("/v1/me/subscription")); }).Num();
		TestTrue(TEXT("...still waiting"), Licence->IsWaitingForCheckout());
		TestTrue(*FString::Printf(TEXT("...polled again on later ticks (%d)"), Polls), Polls > 2);
		Licence->CancelCheckoutWait();
		Game.PumpFor(0.2);
		const int32 PollsAfter = Cloud.Requests.FilterByPredicate([](const FStageCloudHttpRequest& Request) { return Request.Url.Contains(TEXT("/v1/me/subscription")); }).Num();
		TestTrue(TEXT("...and stops when cancelled"), PollsAfter <= Polls + 1);
		Cloud.SubscriptionAnswer = Answer(200, TEXT("{\"status\":\"active\",\"entitled\":true,\"plan\":\"yearly\",\"currentPeriodEnd\":null,\"cancelAtPeriodEnd\":false}"));

		// Checkout: the page opens in the browser, the subscription is watched, then the licence refreshes.
		Cloud.LicenceAnswer = Answer(200, FString::Printf(TEXT("{\"token\":\"%s\"}"), ValidToken));
		TestTrue(TEXT("Checkout starts"), Licence->StartCheckout(EStageProPlan::Yearly, Error));
		TestTrue(TEXT("Pro after checkout"), Game.Pump([Licence]() { return Licence->GetStatus() == EStageLicenceStatus::Pro && !Licence->IsWaitingForCheckout(); }));
		UStageCloudSubsystem* CloudSubsystem = Game.Subsystem<UStageCloudSubsystem>();
		TestTrue(TEXT("The checkout page was opened"), CloudSubsystem->GetOpenedUrlsForTesting().Contains(TEXT("https://checkout.test/pay/c1")));
		TestEqual(TEXT("Two checkouts"), Cloud.Count(TEXT("/v1/subscriptions/checkout")), 2);

		TestTrue(TEXT("Manage opens the portal"), Licence->OpenManageSubscription(Error)
			&& Game.Pump([CloudSubsystem]() { return CloudSubsystem->GetOpenedUrlsForTesting().Contains(TEXT("https://billing.test/portal")); }));
	}

	// The next run of the app, offline: the stored licence verifies with no server at all.
	{
		FStageTestGameInstance Game(Options);
		UStageLicenceSubsystem* Licence = Game.Subsystem<UStageLicenceSubsystem>();
		if (!TestNotNull(TEXT("Second run"), Licence))
		{
			return false;
		}
		Licence->SetMachineIdForTesting(Machine);
		TestEqual(TEXT("Offline restart keeps Pro"), Licence->GetStatus(), EStageLicenceStatus::Pro);

		Licence->SetMachineIdForTesting(FString::ChrN(64, TEXT('c')));
		TestEqual(TEXT("A copied licence on another computer"), Licence->GetStatus(), EStageLicenceStatus::Invalid);
		Licence->SetMachineIdForTesting(Machine);

		WriteLicenceFile(*Licence, ExpiredToken);
		Licence->SetMachineIdForTesting(Machine);
		TestEqual(TEXT("Expired"), Licence->GetStatus(), EStageLicenceStatus::Expired);
		TestFalse(TEXT("...locks Pro"), Licence->HasProEntitlement(StageCraftTags::Entitlement_Pro_DmxIn));

		FString Edited = ValidToken;
		Edited[60] = Edited[60] == TEXT('A') ? TEXT('B') : TEXT('A');
		WriteLicenceFile(*Licence, Edited);
		Licence->SetMachineIdForTesting(Machine);
		TestEqual(TEXT("An edited file"), Licence->GetStatus(), EStageLicenceStatus::Invalid);

		WriteLicenceFile(*Licence, ValidToken, FDateTime::UtcNow().ToUnixTimestamp() + 3 * 86400);
		Licence->SetMachineIdForTesting(Machine);
		TestEqual(TEXT("The clock set back three days"), Licence->GetStatus(), EStageLicenceStatus::Invalid);

		// Release frees the place on the account and removes the licence here.
		WriteLicenceFile(*Licence, ValidToken);
		Licence->SetMachineIdForTesting(Machine);
		FFakeCloud Cloud;
		Cloud.LicenceAnswer = Answer(200, FString::Printf(TEXT("{\"token\":\"%s\"}"), ValidToken));
		if (TestTrue(TEXT("Signed in again"), Connect(Game, Cloud, /*bSignIn*/ true)))
		{
			FText Error;
			Game.Pump([Licence]() { return !Licence->IsRequestInFlight(); });
			TestTrue(TEXT("Release sent"), Licence->ReleaseThisMachine(Error));
			TestTrue(TEXT("Released"), Game.Pump([Licence]() { return Licence->GetStatus() == EStageLicenceStatus::Free; }));
			TestFalse(TEXT("...file removed"), IFileManager::Get().FileExists(*Licence->GetLicencePathForTesting()));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageLicenceGatesTest, "StageCraft.Licence.Gates", StageLicenceTests::Flags)

bool FStageLicenceGatesTest::RunTest(const FString& Parameters)
{
	using namespace StageLicenceTests;
	FStageTestGameInstance Game;
	UStageLicenceSubsystem* Licence = Game.Subsystem<UStageLicenceSubsystem>();
	UWorld* World = Game.IsValid() ? Game.Get().GetWorld() : nullptr;
	AStageCraftGameModeBase* GameMode = World ? World->SpawnActor<AStageCraftGameModeBase>() : nullptr;
	const APlayerController* Player = World ? World->SpawnActor<APlayerController>() : nullptr;
	if (!TestNotNull(TEXT("Licence"), Licence) || !TestNotNull(TEXT("Game mode"), GameMode) || !TestNotNull(TEXT("Player"), Player))
	{
		return false;
	}
	Licence->SetMachineIdForTesting(Machine);

	const auto Locked = [](const FStageEconomyResultInfo& Verdict)
	{
		return Verdict.Code == EStageEconomyResult::Locked && Verdict.Message.ToString().Contains(TEXT("StageCraft Pro"));
	};
	{
		const FScopedDevAssumePro RealLicence(0);
		TestTrue(TEXT("Free: MVR export locked"), Locked(GameMode->EvaluateMvrExport(Player)));
		TestTrue(TEXT("Free: MIDI and OSC locked"), Locked(GameMode->EvaluateRemoteControl(Player)));
		TestTrue(TEXT("Free: Web Remote locked"), Locked(GameMode->EvaluateWebRemote(Player)));
		TestTrue(TEXT("Free: DMX IN locked"), Locked(GameMode->EvaluateDmxInput(Player)));
		TestEqual(TEXT("Free: one DMX OUT universe"), GameMode->GetDmxOutputUniverseLimit(Player), AStageCraftGameModeBase::FreeDmxOutputUniverses);
		TestFalse(TEXT("No player: refused"), GameMode->EvaluateDmxInput(nullptr).IsSuccess());

		WriteLicenceFile(*Licence, ValidToken);
		Licence->SetMachineIdForTesting(Machine);
		TestTrue(TEXT("Pro: MVR export"), GameMode->EvaluateMvrExport(Player).IsSuccess());
		TestTrue(TEXT("Pro: MIDI and OSC"), GameMode->EvaluateRemoteControl(Player).IsSuccess());
		TestTrue(TEXT("Pro: Web Remote"), GameMode->EvaluateWebRemote(Player).IsSuccess());
		TestTrue(TEXT("Pro: DMX IN"), GameMode->EvaluateDmxInput(Player).IsSuccess());
		TestEqual(TEXT("Pro: no universe limit"), GameMode->GetDmxOutputUniverseLimit(Player), 0);

		// The session rule is judged before the licence: a curated session refuses even with Pro.
		const FStructProperty* RulesProperty = FindFProperty<FStructProperty>(AStageCraftGameModeBase::StaticClass(), TEXT("SessionRules"));
		if (TestNotNull(TEXT("Session rules"), RulesProperty))
		{
			RulesProperty->ContainerPtrToValuePtr<FStageSessionRules>(GameMode)->bAllowWebRemote = false;
			TestEqual(TEXT("Session rule first"), GameMode->EvaluateWebRemote(Player).Code, EStageEconomyResult::SessionRuleViolation);
		}

		WriteLicenceFile(*Licence, PartialToken);
		Licence->SetMachineIdForTesting(Machine);
		TestTrue(TEXT("Partial licence: MIDI and OSC"), GameMode->EvaluateRemoteControl(Player).IsSuccess());
		TestTrue(TEXT("Partial licence: no MVR export"), Locked(GameMode->EvaluateMvrExport(Player)));
		TestEqual(TEXT("Partial licence: one universe"), GameMode->GetDmxOutputUniverseLimit(Player), 1);

		WriteLicenceFile(*Licence, ExpiredToken);
		Licence->SetMachineIdForTesting(Machine);
		TestTrue(TEXT("Expired: DMX IN locked again"), Locked(GameMode->EvaluateDmxInput(Player)));
	}
	{
		const FScopedDevAssumePro DevelopmentOverride(1);
#if UE_BUILD_SHIPPING
		TestTrue(TEXT("Shipping ignores the development override"), Locked(GameMode->EvaluateDmxInput(Player)));
#else
		TestTrue(TEXT("Development override: DMX IN"), GameMode->EvaluateDmxInput(Player).IsSuccess());
		TestEqual(TEXT("Development override: no universe limit"), GameMode->GetDmxOutputUniverseLimit(Player), 0);
#endif
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageLicenceDmxLimitsTest, "StageCraft.Licence.DmxLimits", StageLicenceTests::Flags)

bool FStageLicenceDmxLimitsTest::RunTest(const FString& Parameters)
{
	TArray<int32> Sent = { 1, 2, 5 };
	TArray<int32> Held = { 9 };
	StageDMXOutput::ApplyUniverseLimit(Sent, 1, Held);
	TestEqual(TEXT("Free sends the lowest universe"), Sent, TArray<int32>{ 1 });
	TestEqual(TEXT("...and holds the others"), Held, TArray<int32>({ 2, 5 }));

	Sent = { 3, 4 };
	StageDMXOutput::ApplyUniverseLimit(Sent, 0, Held);
	TestEqual(TEXT("No limit sends all"), Sent, TArray<int32>({ 3, 4 }));
	TestTrue(TEXT("...and holds none"), Held.IsEmpty());

	Sent = { 7 };
	StageDMXOutput::ApplyUniverseLimit(Sent, 1, Held);
	TestEqual(TEXT("Within the limit"), Sent, TArray<int32>{ 7 });
	TestTrue(TEXT("...nothing held"), Held.IsEmpty());

	Sent = {};
	StageDMXOutput::ApplyUniverseLimit(Sent, 1, Held);
	TestTrue(TEXT("Nothing patched"), Sent.IsEmpty() && Held.IsEmpty());

	TestTrue(TEXT("Default limits limit nothing"), FStageDMXCapabilityLimits().bInputAllowed && FStageDMXCapabilityLimits().MaxOutputUniverses == 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
