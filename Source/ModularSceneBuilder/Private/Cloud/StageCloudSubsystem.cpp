// Copyright Epic Games, Inc. All Rights Reserved.

#include "Cloud/StageCloudSubsystem.h"

#include "Cloud/StageCloudJson.h"
#include "Engine/GameInstance.h"
#include "GenericPlatform/GenericPlatformProcess.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/App.h"
#include "ModularSceneBuilder.h"
#include "Services/StageCredentialVault.h"
#include "TimerManager.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageCloudSubsystem)

#define LOCTEXT_NAMESPACE "StageCloudSubsystem"

namespace StageCloud
{
	/** The vault entry of the session: "<server address>\n<token>", so it is only ever sent to the server that issued it. */
	const FName VaultId(TEXT("StageCraftCloud"));

#if !UE_BUILD_SHIPPING
	TAutoConsoleVariable<bool> CVarLogBrowserUrls(TEXT("StageCraft.Cloud.LogBrowserUrls"), false,
		TEXT("Development builds: log the addresses StageCraft would open in the browser (sign-in, checkout) instead of opening them."));
#endif

	TArray<uint8> ToUtf8(const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text, Text.Len());
		return TArray<uint8>(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	}
}

void UStageCloudSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	ApplyBaseUrl(BaseUrl);
}

void UStageCloudSubsystem::Deinitialize()
{
	++SignInSerial;
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().ClearTimer(PollTimer);
	}
	for (const TSharedPtr<IHttpRequest, ESPMode::ThreadSafe>& Request : InFlight)
	{
		Request->OnProcessRequestComplete().Unbind();
		Request->CancelRequest();
	}
	InFlight.Reset();
	FStageCredentialVault::WipeString(SessionToken);
	OnStateChanged.Clear();
	Super::Deinitialize();
}

void UStageCloudSubsystem::ApplyBaseUrl(const FString& Url)
{
	FStageCredentialVault::WipeString(SessionToken);
	Account = FStageCloudAccount();
	DeviceCode = FStageCloudDeviceCode();
	ResolvedBaseUrl.Reset();
	if (Url.TrimStartAndEnd().IsEmpty())
	{
		SetState(EStageCloudState::Offline, FText::GetEmpty());
		return;
	}
	if (!StageCloudJson::NormalizeBaseUrl(Url, ResolvedBaseUrl))
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Cloud: BaseUrl is not a usable https address; staying offline."));
		SetState(EStageCloudState::Offline, LOCTEXT("BadBaseUrl", "The StageCraft Cloud address in the settings is not valid."));
		return;
	}
	SetState(EStageCloudState::SignedOut, FText::GetEmpty());
	LoadSession();
}

void UStageCloudSubsystem::LoadSession()
{
	if (!FStageCredentialVault::IsSupported())
	{
		return;
	}
	const FStageCredentialVault Vault(FStageCredentialVault::GetDefaultDirectory());
	FString Stored;
	if (Vault.Load(StageCloud::VaultId, Stored) != EStageVaultResult::Success)
	{
		return;
	}
	FString Server;
	FString Token;
	if (Stored.Split(TEXT("\n"), &Server, &Token) && Server == ResolvedBaseUrl && !Token.IsEmpty())
	{
		SessionToken = MoveTemp(Token);
		// Signed in until the server says otherwise: the account is read now, and a 401 ends the session.
		SetState(EStageCloudState::SignedIn, FText::GetEmpty());
		RefreshAccount();
	}
	FStageCredentialVault::WipeString(Stored);
	FStageCredentialVault::WipeString(Token);
}

void UStageCloudSubsystem::StoreSession()
{
	if (!FStageCredentialVault::IsSupported())
	{
		return;
	}
	FString Stored = ResolvedBaseUrl + TEXT("\n") + SessionToken;
	const FStageCredentialVault Vault(FStageCredentialVault::GetDefaultDirectory());
	if (Vault.Store(StageCloud::VaultId, Stored) != EStageVaultResult::Success)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Cloud: the session could not be stored in the credential vault; sign in again next time."));
	}
	FStageCredentialVault::WipeString(Stored);
}

void UStageCloudSubsystem::DropSession(const FText& Message)
{
	FStageCredentialVault::WipeString(SessionToken);
	Account = FStageCloudAccount();
	if (FStageCredentialVault::IsSupported())
	{
		FStageCredentialVault(FStageCredentialVault::GetDefaultDirectory()).Remove(StageCloud::VaultId);
	}
	SetState(IsOnline() ? EStageCloudState::SignedOut : EStageCloudState::Offline, Message);
}

void UStageCloudSubsystem::SetState(EStageCloudState NewState, const FText& Message)
{
	State = NewState;
	StatusMessage = Message;
	OnStateChanged.Broadcast();
}

bool UStageCloudSubsystem::StartSignIn(FText& OutError)
{
	if (State == EStageCloudState::Offline)
	{
		OutError = LOCTEXT("NoServer", "StageCraft Cloud is not set up in this version.");
		return false;
	}
	if (State != EStageCloudState::SignedOut)
	{
		OutError = State == EStageCloudState::SignedIn ? LOCTEXT("AlreadySignedIn", "You are already signed in.") : LOCTEXT("SigningIn", "Sign-in is already in progress.");
		return false;
	}
	const uint32 Serial = ++SignInSerial;
	DeviceCode = FStageCloudDeviceCode();
	SetState(EStageCloudState::SigningIn, LOCTEXT("Requesting", "Asking StageCraft Cloud for a sign-in code…"));
	TWeakObjectPtr<UStageCloudSubsystem> WeakThis = this;
	SendJson(TEXT("POST"), TEXT("/v1/auth/device"), FString(), /*bAuthenticated*/ false,
		FOnStageCloudResponse::CreateWeakLambda(this, [WeakThis, Serial](const FStageCloudResponse& Response)
		{
			if (UStageCloudSubsystem* Self = WeakThis.Get())
			{
				Self->HandleDeviceStarted(Response, Serial);
			}
		}));
	return true;
}

void UStageCloudSubsystem::HandleDeviceStarted(const FStageCloudResponse& Response, uint32 Serial)
{
	if (Serial != SignInSerial || State != EStageCloudState::SigningIn)
	{
		return;
	}
	FStageCloudDeviceCode Code;
	if (!Response.IsSuccess() || !StageCloudJson::ParseDeviceCode(Response.GetBodyAsString(), Code))
	{
		SetState(EStageCloudState::SignedOut, Response.bConnected ? LOCTEXT("BadDeviceAnswer", "StageCraft Cloud could not start a sign-in. Try again later.")
			: LOCTEXT("NoConnection", "StageCraft Cloud could not be reached. Check the internet connection."));
		return;
	}
	DeviceCode = MoveTemp(Code);
	DeviceExpiresUtc = FDateTime::UtcNow() + FTimespan::FromSeconds(DeviceCode.ExpiresInSeconds);
	OpenInBrowser(DeviceCode.VerificationUriComplete.IsEmpty() ? DeviceCode.VerificationUri : DeviceCode.VerificationUriComplete);
	SetState(EStageCloudState::SigningIn, FText::Format(LOCTEXT("ShowCode", "Sign in in your browser and check that it shows the code {0}."),
		FText::FromString(DeviceCode.UserCode)));
	SchedulePoll(DeviceCode.IntervalSeconds);
}

void UStageCloudSubsystem::SchedulePoll(int32 Seconds)
{
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().SetTimer(PollTimer, FTimerDelegate::CreateUObject(this, &ThisClass::PollDeviceToken), float(FMath::Max(1, Seconds)), /*bLoop*/ false);
	}
}

void UStageCloudSubsystem::PollDeviceToken()
{
	if (State != EStageCloudState::SigningIn || DeviceCode.DeviceCode.IsEmpty())
	{
		return;
	}
	if (FDateTime::UtcNow() >= DeviceExpiresUtc)
	{
		++SignInSerial;
		DeviceCode = FStageCloudDeviceCode();
		SetState(EStageCloudState::SignedOut, LOCTEXT("CodeExpired", "The sign-in code expired. Start again."));
		return;
	}
	const uint32 Serial = SignInSerial;
	TWeakObjectPtr<UStageCloudSubsystem> WeakThis = this;
	SendJson(TEXT("POST"), TEXT("/v1/auth/device/token"), StageCloudJson::BuildDeviceTokenBody(DeviceCode.DeviceCode), /*bAuthenticated*/ false,
		FOnStageCloudResponse::CreateWeakLambda(this, [WeakThis, Serial](const FStageCloudResponse& Response)
		{
			if (UStageCloudSubsystem* Self = WeakThis.Get())
			{
				Self->HandleDevicePolled(Response, Serial);
			}
		}));
}

void UStageCloudSubsystem::HandleDevicePolled(const FStageCloudResponse& Response, uint32 Serial)
{
	if (Serial != SignInSerial || State != EStageCloudState::SigningIn)
	{
		return;
	}
	FStageCloudDevicePollResult Result = StageCloudJson::ParseDevicePoll(Response);
	switch (Result.Outcome)
	{
	case EStageCloudDevicePoll::SignedIn:
		SessionToken = MoveTemp(Result.SessionToken);
		FStageCredentialVault::WipeString(Result.SessionToken);
		DeviceCode = FStageCloudDeviceCode();
		StoreSession();
		SetState(EStageCloudState::SignedIn, LOCTEXT("SignedIn", "Signed in."));
		RefreshAccount();
		return;
	case EStageCloudDevicePoll::Pending:
		SchedulePoll(DeviceCode.IntervalSeconds);
		return;
	case EStageCloudDevicePoll::SlowDown:
		// RFC 8628 §3.5: add 5 seconds to the interval for this and every later poll.
		DeviceCode.IntervalSeconds = FMath::Min(DeviceCode.IntervalSeconds + 5, 60);
		SchedulePoll(DeviceCode.IntervalSeconds);
		return;
	case EStageCloudDevicePoll::Expired:
		DeviceCode = FStageCloudDeviceCode();
		SetState(EStageCloudState::SignedOut, LOCTEXT("CodeExpired", "The sign-in code expired. Start again."));
		return;
	case EStageCloudDevicePoll::Denied:
		DeviceCode = FStageCloudDeviceCode();
		SetState(EStageCloudState::SignedOut, LOCTEXT("Denied", "Sign-in was declined in the browser."));
		return;
	default:
		// A dropped connection is not the end of the sign-in: try again at the normal pace until the code expires.
		if (!Response.bConnected || Response.Status >= 500)
		{
			SchedulePoll(DeviceCode.IntervalSeconds);
			return;
		}
		DeviceCode = FStageCloudDeviceCode();
		SetState(EStageCloudState::SignedOut, LOCTEXT("SignInFailed", "Sign-in did not complete. Try again."));
		return;
	}
}

void UStageCloudSubsystem::CancelSignIn()
{
	if (State != EStageCloudState::SigningIn)
	{
		return;
	}
	++SignInSerial;
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().ClearTimer(PollTimer);
	}
	DeviceCode = FStageCloudDeviceCode();
	SetState(EStageCloudState::SignedOut, LOCTEXT("SignInCancelled", "Sign-in cancelled."));
}

void UStageCloudSubsystem::SignOut()
{
	if (State == EStageCloudState::SigningIn)
	{
		CancelSignIn();
		return;
	}
	if (State != EStageCloudState::SignedIn)
	{
		return;
	}
	// Revoke on the server too; the answer does not matter, the session ends here either way.
	SendJson(TEXT("POST"), TEXT("/v1/auth/logout"), FString(), /*bAuthenticated*/ true, FOnStageCloudResponse());
	DropSession(LOCTEXT("SignedOut", "Signed out."));
}

void UStageCloudSubsystem::RefreshAccount()
{
	if (State != EStageCloudState::SignedIn)
	{
		return;
	}
	SendJson(TEXT("GET"), TEXT("/v1/me"), FString(), /*bAuthenticated*/ true, FOnStageCloudResponse::CreateUObject(this, &ThisClass::HandleAccount));
}

void UStageCloudSubsystem::HandleAccount(const FStageCloudResponse& Response)
{
	FStageCloudAccount Read;
	if (State == EStageCloudState::SignedIn && Response.IsSuccess() && StageCloudJson::ParseAccount(Response.GetBodyAsString(), Read))
	{
		Account = MoveTemp(Read);
		SetState(EStageCloudState::SignedIn, FText::Format(LOCTEXT("SignedInAs", "Signed in as {0}."), FText::FromString(Account.DisplayName)));
	}
}

bool UStageCloudSubsystem::OpenInBrowser(const FString& Url)
{
	if (!StageCloudJson::IsSafeBrowserUrl(Url))
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Cloud: refused to open an address that is not safe."));
		return false;
	}
#if WITH_DEV_AUTOMATION_TESTS
	if (TestTransport)
	{
		OpenedUrls.Add(Url);
		return true;
	}
#endif
#if !UE_BUILD_SHIPPING
	if (StageCloud::CVarLogBrowserUrls.GetValueOnGameThread())
	{
		// Scripted end-to-end runs (Scripts/Tests/RunCloudEndToEnd.ps1) act as the browser from the log.
		UE_LOG(LogStageCraft, Display, TEXT("Cloud: browser %s"), *Url);
		return true;
	}
#endif
	FString Error;
	FPlatformProcess::LaunchURL(*Url, nullptr, &Error);
	return Error.IsEmpty();
}

void UStageCloudSubsystem::SendJson(const FString& Verb, const FString& Path, const FString& Json, bool bAuthenticated, FOnStageCloudResponse OnDone, float TimeoutSeconds)
{
	Send(Verb, Path, Json.IsEmpty() ? TArray<uint8>() : StageCloud::ToUtf8(Json), Json.IsEmpty() ? FString() : FString(TEXT("application/json")), bAuthenticated,
		TimeoutSeconds, MoveTemp(OnDone));
}

void UStageCloudSubsystem::Send(const FString& Verb, const FString& Path, TArray<uint8> Body, const FString& ContentType, bool bAuthenticated, float TimeoutSeconds,
	FOnStageCloudResponse OnDone)
{
	// Only paths of this server: an absolute address here would send the session elsewhere.
	if (!IsOnline() || (bAuthenticated && SessionToken.IsEmpty()) || !Path.StartsWith(TEXT("/v1/")) || Path.Contains(TEXT("..")) || Path.Contains(TEXT("//")))
	{
		OnDone.ExecuteIfBound(FStageCloudResponse());
		return;
	}
	FStageCloudHttpRequest Request;
	Request.Verb = Verb;
	Request.Url = ResolvedBaseUrl + Path;
	Request.Headers.Add(TEXT("Accept"), TEXT("application/json"));
	Request.Headers.Add(TEXT("User-Agent"), FString::Printf(TEXT("StageCraft/%s"), FApp::GetBuildVersion()));
	if (!ContentType.IsEmpty())
	{
		Request.Headers.Add(TEXT("Content-Type"), ContentType);
	}
	if (bAuthenticated)
	{
		Request.Headers.Add(TEXT("Authorization"), TEXT("Bearer ") + SessionToken);
	}
	Request.Body = MoveTemp(Body);
	Dispatch(MoveTemp(Request), TimeoutSeconds, bAuthenticated, MoveTemp(OnDone));
}

void UStageCloudSubsystem::Download(const FString& Url, float TimeoutSeconds, FOnStageCloudResponse OnDone)
{
	if (!IsOnline() || !StageCloudJson::IsSafeBrowserUrl(Url))
	{
		OnDone.ExecuteIfBound(FStageCloudResponse());
		return;
	}
	FStageCloudHttpRequest Request;
	Request.Verb = TEXT("GET");
	Request.Url = Url;
	Dispatch(MoveTemp(Request), TimeoutSeconds, /*bAuthenticated*/ false, MoveTemp(OnDone));
}

void UStageCloudSubsystem::Dispatch(FStageCloudHttpRequest Request, float TimeoutSeconds, bool bAuthenticated, FOnStageCloudResponse OnDone)
{
	TWeakObjectPtr<UStageCloudSubsystem> WeakThis = this;
	// Every answer passes here: an authenticated request answered 401 means the session is over.
	const auto Finish = [WeakThis, bAuthenticated, OnDone](const FStageCloudResponse& Response)
	{
		UStageCloudSubsystem* Self = WeakThis.Get();
		if (Self && bAuthenticated && Response.bConnected && Response.Status == 401 && Self->IsSignedIn())
		{
			Self->DropSession(LOCTEXT("SessionEnded", "Your StageCraft Cloud session ended. Sign in again."));
		}
		OnDone.ExecuteIfBound(Response);
	};

#if WITH_DEV_AUTOMATION_TESTS
	if (TestTransport)
	{
		TestTransport(Request, FOnStageCloudResponse::CreateLambda(Finish));
		return;
	}
#endif

	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Http = FHttpModule::Get().CreateRequest();
	Http->SetVerb(Request.Verb);
	Http->SetURL(Request.Url);
	for (const TPair<FString, FString>& Header : Request.Headers)
	{
		Http->SetHeader(Header.Key, Header.Value);
	}
	if (!Request.Body.IsEmpty())
	{
		Http->SetContent(MoveTemp(Request.Body));
	}
	Http->SetTimeout(TimeoutSeconds > 0.f ? TimeoutSeconds : FMath::Max(RequestTimeoutSeconds, 1.f));
	Http->OnProcessRequestComplete().BindWeakLambda(this, [this, Finish](FHttpRequestPtr Done, FHttpResponsePtr Response, bool bConnectedSuccessfully)
	{
		InFlight.RemoveAll([&Done](const TSharedPtr<IHttpRequest, ESPMode::ThreadSafe>& Each) { return Each == Done; });
		FStageCloudResponse Answer;
		Answer.bConnected = bConnectedSuccessfully && Response.IsValid();
		if (Answer.bConnected)
		{
			Answer.Status = Response->GetResponseCode();
			Answer.Body = Response->GetContent();
		}
		Finish(Answer);
	});
	if (!Http->ProcessRequest())
	{
		Http->OnProcessRequestComplete().Unbind();
		Finish(FStageCloudResponse());
		return;
	}
	InFlight.Add(Http);
}

TArray<FString> UStageCloudSubsystem::GetSecretsToRedact() const
{
	TArray<FString> Secrets;
	if (!SessionToken.IsEmpty())
	{
		Secrets.Add(SessionToken);
	}
	return Secrets;
}

#if WITH_DEV_AUTOMATION_TESTS
void UStageCloudSubsystem::SetTransportForTesting(FTestTransport InTransport)
{
	TestTransport = MoveTemp(InTransport);
	OpenedUrls.Reset();
}

void UStageCloudSubsystem::SetBaseUrlForTesting(const FString& Url)
{
	++SignInSerial;
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().ClearTimer(PollTimer);
	}
	ApplyBaseUrl(Url);
}
#endif

#undef LOCTEXT_NAMESPACE
