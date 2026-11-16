// Copyright Epic Games, Inc. All Rights Reserved.

#include "Licence/StageLicenceSubsystem.h"

#include "Cloud/StageCloudJson.h"
#include "Cloud/StageCloudSubsystem.h"
#include "Dom/JsonObject.h"
#include "Economy/StageEconomyTypes.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "IO/StageAtomicFile.h"
#include "IO/StageCraftPaths.h"
#include "Licence/StageLicenceToken.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TimerManager.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageLicenceSubsystem)

#define LOCTEXT_NAMESPACE "StageLicence"

#if !UE_BUILD_SHIPPING
static TAutoConsoleVariable<int32> CVarStageLicenceDevAssumePro(
	TEXT("StageCraft.Licence.DevAssumePro"),
	0,
	TEXT("Development builds only (absent from Shipping): non-zero makes every StageCraft Pro entitlement present without a licence (Docs/ADR/0034 §17.3)."),
	ECVF_Default);
#endif

namespace StageLicence
{
	/** The licence file is small; anything larger was not written by this subsystem. */
	constexpr int64 MaxFileBytes = 16 * 1024;

	/** The stored high-water time is rewritten only after this much progress, so a normal start does not write the file. */
	constexpr int64 HighWaterWriteStepSeconds = 60 * 60;

	/** A licence is re-evaluated at least this often, so a clock change while running is noticed. */
	constexpr float MaxExpiryCheckSeconds = 6.f * 60.f * 60.f;

	constexpr float CheckoutRetrySeconds = 5.f;

	int64 NowUnix()
	{
		return FDateTime::UtcNow().ToUnixTimestamp();
	}

	const TCHAR* PlanName(EStageProPlan Plan)
	{
		return Plan == EStageProPlan::Yearly ? TEXT("yearly") : TEXT("monthly");
	}

	TSharedPtr<FJsonObject> ParseObject(const FStageCloudResponse& Response)
	{
		TSharedPtr<FJsonObject> Object;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response.GetBodyAsString()), Object))
		{
			Object.Reset();
		}
		return Object;
	}

	/** A safe address the server answered under Field, or empty. */
	FString ParseUrl(const FStageCloudResponse& Response, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject> Object = ParseObject(Response);
		FString Url;
		return Object.IsValid() && Object->TryGetStringField(Field, Url) && StageCloudJson::IsSafeBrowserUrl(Url) ? Url : FString();
	}

	FString JsonBody(const TFunctionRef<void(TJsonWriter<>&)> Fill)
	{
		FString Body;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
		Writer->WriteObjectStart();
		Fill(*Writer);
		Writer->WriteObjectEnd();
		Writer->Close();
		return Body;
	}

	/** What a refusal from the licence endpoints means for the user. Unknown codes say the server refused (deny by default). */
	FText DescribeError(const FStageCloudError& Error)
	{
		if (Error.Status == 0)
		{
			return LOCTEXT("Unreachable", "StageCraft Cloud cannot be reached. The licence on this computer stays as it is.");
		}
		if (Error.Code == TEXT("NoActiveSubscription"))
		{
			return LOCTEXT("NoSubscription", "This account has no active StageCraft Pro subscription.");
		}
		if (Error.Code == TEXT("MachineLimitReached") || Error.Code == TEXT("AlreadySubscribed") || Error.Code == TEXT("PurchasePending")
			|| Error.Code == TEXT("PlanNotAvailable") || Error.Code == TEXT("RateLimited"))
		{
			return FText::FromString(Error.Message);
		}
		if (Error.Code == TEXT("Unauthenticated"))
		{
			return LOCTEXT("SignInAgain", "Your StageCraft Cloud session ended. Sign in again.");
		}
		return FText::Format(LOCTEXT("Refused", "StageCraft Cloud refused the request ({0})."), FText::FromString(Error.Code.IsEmpty() ? FString::FromInt(Error.Status) : Error.Code));
	}

	FText DescribeTokenError(EStageLicenceTokenError Error)
	{
		switch (Error)
		{
		case EStageLicenceTokenError::UnknownKey:
			return LOCTEXT("UnknownKey", "The stored licence was signed by a key this version of StageCraft does not trust. Refresh it online.");
		case EStageLicenceTokenError::BadSignature:
			return LOCTEXT("BadSignature", "The stored licence was changed after it was issued and cannot be used. Refresh it online.");
		case EStageLicenceTokenError::UnsupportedVersion:
			return LOCTEXT("NewerToken", "The stored licence needs a newer version of StageCraft.");
		default:
			return LOCTEXT("Malformed", "The stored licence is damaged. Refresh it online.");
		}
	}
}

// --- Lifetime ---

void UStageLicenceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UStageCloudSubsystem* Cloud = Collection.InitializeDependency<UStageCloudSubsystem>();

	MachineId = StageLicenceToken::MakeMachineId(FPlatformMisc::GetLoginId());
	const FStoredLicence Stored = LoadStored();
	HighWaterUnix = Stored.HighWaterUnix;
	if (!Stored.Token.IsEmpty())
	{
		ApplyToken(Stored.Token, FText::GetEmpty());
		// Moves the high-water mark forward on ordinary starts, so a later clock rollback is measured from today.
		const int64 Now = StageLicence::NowUnix();
		if (Now > HighWaterUnix + StageLicence::HighWaterWriteStepSeconds)
		{
			HighWaterUnix = Now;
			SaveStored({ Stored.Token, HighWaterUnix });
		}
	}

	if (Cloud)
	{
		CloudStateHandle = Cloud->OnStateChanged.AddUObject(this, &ThisClass::HandleCloudStateChanged);
		HandleCloudStateChanged();
	}
#if !UE_BUILD_SHIPPING
	DevOverrideHandle = CVarStageLicenceDevAssumePro->OnChangedDelegate().AddWeakLambda(this, [this](IConsoleVariable*) { HandleDevOverrideChanged(); });
	UE_LOG(LogStageCraft, Log, TEXT("Licence: %s%s."), *UEnum::GetValueAsString(Status),
		IsDevAssumeProActive() ? TEXT(" (development override: Pro assumed)") : TEXT(""));
#else
	UE_LOG(LogStageCraft, Log, TEXT("Licence: %s."), *UEnum::GetValueAsString(Status));
#endif
}

void UStageLicenceSubsystem::Deinitialize()
{
#if !UE_BUILD_SHIPPING
	CVarStageLicenceDevAssumePro->OnChangedDelegate().Remove(DevOverrideHandle);
#endif
	DevOverrideHandle.Reset();
	if (UStageCloudSubsystem* Cloud = GetCloud())
	{
		Cloud->OnStateChanged.Remove(CloudStateHandle);
	}
	CloudStateHandle.Reset();
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		FTimerManager& Timers = GameInstance->GetTimerManager();
		Timers.ClearTimer(ExpiryTimer);
		Timers.ClearTimer(RefreshTimer);
		Timers.ClearTimer(CheckoutRetryTimer);
	}
	OnLicenceChanged.Clear();
	Super::Deinitialize();
}

UStageCloudSubsystem* UStageLicenceSubsystem::GetCloud() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UStageCloudSubsystem>() : nullptr;
}

// --- Queries ---

bool UStageLicenceSubsystem::IsDevAssumeProActive()
{
#if !UE_BUILD_SHIPPING
	return CVarStageLicenceDevAssumePro.GetValueOnGameThread() != 0;
#else
	return false;
#endif
}

bool UStageLicenceSubsystem::HasPro(const UGameInstance* GameInstance, const FGameplayTag& Tag)
{
	if (IsDevAssumeProActive())
	{
		return Tag.IsValid() && Tag.MatchesTag(StageCraftTags::Entitlement_Pro);
	}
	const UStageLicenceSubsystem* Licence = GameInstance ? GameInstance->GetSubsystem<UStageLicenceSubsystem>() : nullptr;
	return Licence && Licence->HasProEntitlement(Tag);
}

bool UStageLicenceSubsystem::HasProEntitlement(const FGameplayTag& Tag) const
{
	if (!Tag.IsValid())
	{
		return false;
	}
	if (IsDevAssumeProActive())
	{
		return Tag.MatchesTag(StageCraftTags::Entitlement_Pro);
	}
	return Status == EStageLicenceStatus::Pro && Entitlements.HasTagExact(Tag);
}

// --- Stored licence ---

FString UStageLicenceSubsystem::GetLicencePath() const
{
	return FPaths::Combine(StageCraftPaths::UserRoot(), TEXT("Licence.json"));
}

UStageLicenceSubsystem::FStoredLicence UStageLicenceSubsystem::LoadStored() const
{
	FStoredLicence Stored;
	const FString Path = GetLicencePath();
	const int64 Size = IFileManager::Get().FileSize(*Path);
	if (Size <= 0 || Size > StageLicence::MaxFileBytes)
	{
		return Stored;
	}
	FString Text;
	TSharedPtr<FJsonObject> Object;
	if (!FFileHelper::LoadFileToString(Text, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) || !Object.IsValid())
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Licence: %s cannot be read; it is ignored."), *Path);
		return Stored;
	}
	Object->TryGetStringField(TEXT("token"), Stored.Token);
	double Seen = 0.0;
	if (Object->TryGetNumberField(TEXT("seen"), Seen) && Seen > 0.0 && Seen < 253402300799.0)
	{
		Stored.HighWaterUnix = static_cast<int64>(Seen);
	}
	// "seen" is in the same unsigned file as the token; the file's own write time also bounds a rollback, so deleting the field after
	// setting the clock back still leaves the time of that edit. A deterrent only (Docs/ADR/0034 §17.4).
	const FDateTime Written = IFileManager::Get().GetTimeStamp(*Path);
	if (Written > FDateTime::MinValue())
	{
		Stored.HighWaterUnix = FMath::Max(Stored.HighWaterUnix, Written.ToUnixTimestamp());
	}
	return Stored;
}

bool UStageLicenceSubsystem::SaveStored(const FStoredLicence& Stored) const
{
	const FString Body = StageLicence::JsonBody([&Stored](TJsonWriter<>& Writer)
	{
		Writer.WriteValue(TEXT("token"), Stored.Token);
		Writer.WriteValue(TEXT("seen"), Stored.HighWaterUnix);
	});
	const FString Path = GetLicencePath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
	if (!StageAtomicFile::WriteText(Path, Body))
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Licence: could not write %s; the licence works until StageCraft closes."), *Path);
		return false;
	}
	return true;
}

void UStageLicenceSubsystem::DeleteStored()
{
	CurrentToken.Reset();
	IFileManager::Get().Delete(*GetLicencePath(), /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
}

// --- State ---

void UStageLicenceSubsystem::ApplyToken(const FString& Token, const FText& SuccessMessage)
{
	CurrentToken = Token;
	FStageLicenceClaims Claims;
	const EStageLicenceTokenError Error = StageLicenceToken::Verify(Token, StageLicenceToken::GetTrustedKeys(), Claims);
	if (Error != EStageLicenceTokenError::None)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Licence: refused a token (%d)."), static_cast<int32>(Error));
		ClearLicence(EStageLicenceStatus::Invalid, StageLicence::DescribeTokenError(Error));
		return;
	}

	const int64 Now = StageLicence::NowUnix();
	switch (StageLicenceToken::Evaluate(Claims, MachineId, Now, HighWaterUnix))
	{
	case EStageLicenceValidity::WrongMachine:
		ClearLicence(EStageLicenceStatus::Invalid, LOCTEXT("WrongMachine", "The stored licence belongs to another computer. Sign in and refresh it here."));
		return;
	case EStageLicenceValidity::ClockRollback:
		ClearLicence(EStageLicenceStatus::Invalid, LOCTEXT("ClockRollback", "This computer's clock is set before the last time StageCraft ran. Correct the date, or refresh the licence online."));
		return;
	case EStageLicenceValidity::Expired:
		ClearLicence(EStageLicenceStatus::Expired, LOCTEXT("Expired", "StageCraft Pro expired on this computer. Connect and sign in to renew it; your scenes are unaffected."),
			FDateTime::FromUnixTimestamp(Claims.ExpiresAt));
		return;
	case EStageLicenceValidity::Valid:
		break;
	}

	// Only exact, known Pro tags count: a name this build does not know grants nothing, and a parent tag never implies its children.
	FGameplayTagContainer Granted;
	for (const FString& Name : Claims.Entitlements)
	{
		const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*Name), /*ErrorIfNotFound*/ false);
		if (Tag.IsValid() && Tag.MatchesTag(StageCraftTags::Entitlement_Pro) && Tag != StageCraftTags::Entitlement_Pro)
		{
			Granted.AddTag(Tag);
		}
	}
	Status = EStageLicenceStatus::Pro;
	Entitlements = MoveTemp(Granted);
	ExpiresUtc = FDateTime::FromUnixTimestamp(Claims.ExpiresAt);
	Plan = Claims.Plan;
	StatusMessage = SuccessMessage.IsEmpty()
		? FText::Format(LOCTEXT("ProUntil", "StageCraft Pro is active on this computer until {0}."), FText::AsDate(ExpiresUtc, EDateTimeStyle::Medium))
		: SuccessMessage;
	HighWaterUnix = FMath::Max(HighWaterUnix, Now);
	ScheduleExpiryCheck();
	OnLicenceChanged.Broadcast();
}

void UStageLicenceSubsystem::ClearLicence(EStageLicenceStatus NewStatus, const FText& Message, FDateTime KeptExpiresUtc)
{
	Status = NewStatus;
	Entitlements.Reset();
	ExpiresUtc = KeptExpiresUtc;
	Plan.Reset();
	StatusMessage = Message;
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().ClearTimer(ExpiryTimer);
	}
	OnLicenceChanged.Broadcast();
}

void UStageLicenceSubsystem::SetStatusMessage(const FText& Message)
{
	StatusMessage = Message;
	OnLicenceChanged.Broadcast();
}

void UStageLicenceSubsystem::ScheduleExpiryCheck()
{
	UGameInstance* GameInstance = GetGameInstance();
	if (!GameInstance || Status != EStageLicenceStatus::Pro)
	{
		return;
	}
	const double Remaining = (ExpiresUtc - FDateTime::UtcNow()).GetTotalSeconds() + 1.0;
	const float Delay = static_cast<float>(FMath::Clamp(Remaining, 1.0, static_cast<double>(StageLicence::MaxExpiryCheckSeconds)));
	GameInstance->GetTimerManager().SetTimer(ExpiryTimer, FTimerDelegate::CreateUObject(this, &ThisClass::HandleExpiryCheck), Delay, /*bLoop*/ false);
}

void UStageLicenceSubsystem::HandleExpiryCheck()
{
	if (CurrentToken.IsEmpty())
	{
		ClearLicence(EStageLicenceStatus::Free, FText::GetEmpty());
		return;
	}
	ApplyToken(CurrentToken, StatusMessage);
}

void UStageLicenceSubsystem::HandleDevOverrideChanged()
{
	OnLicenceChanged.Broadcast();
}

// --- Cloud ---

void UStageLicenceSubsystem::HandleCloudStateChanged()
{
	const UStageCloudSubsystem* Cloud = GetCloud();
	const bool bSignedIn = Cloud && Cloud->IsSignedIn();
	const bool bJustSignedIn = bSignedIn && !bWasSignedIn;
	bWasSignedIn = bSignedIn;
	UpdateRefreshTimer();
	if (!bSignedIn)
	{
		StopCheckoutWait();
		return;
	}
	if (bJustSignedIn)
	{
		FText Ignored;
		RequestRefresh(Ignored);
	}
}

void UStageLicenceSubsystem::UpdateRefreshTimer()
{
	UGameInstance* GameInstance = GetGameInstance();
	if (!GameInstance)
	{
		return;
	}
	FTimerManager& Timers = GameInstance->GetTimerManager();
	if (!bWasSignedIn)
	{
		Timers.ClearTimer(RefreshTimer);
		return;
	}
	if (!Timers.IsTimerActive(RefreshTimer))
	{
		const float Seconds = FMath::Max(RefreshIntervalHours, 0.1f) * 3600.f;
		Timers.SetTimer(RefreshTimer, FTimerDelegate::CreateWeakLambda(this, [this]()
		{
			FText Ignored;
			RequestRefresh(Ignored);
		}), Seconds, /*bLoop*/ true);
	}
}

bool UStageLicenceSubsystem::RequestRefresh(FText& OutError)
{
	UStageCloudSubsystem* Cloud = GetCloud();
	if (!Cloud || !Cloud->IsSignedIn())
	{
		OutError = LOCTEXT("SignInFirst", "Sign in to StageCraft Cloud (Marketplace, SIGN IN) to get or renew StageCraft Pro.");
		return false;
	}
	if (MachineId.IsEmpty())
	{
		OutError = LOCTEXT("NoMachineId", "This computer cannot be identified for a licence.");
		return false;
	}
	const FString Body = StageLicence::JsonBody([this](TJsonWriter<>& Writer) { Writer.WriteValue(TEXT("machineId"), MachineId); });
	++RequestsInFlight;
	OnLicenceChanged.Broadcast();
	Cloud->SendJson(TEXT("POST"), TEXT("/v1/licences"), Body, /*bAuthenticated*/ true,
		FOnStageCloudResponse::CreateUObject(this, &ThisClass::HandleLicenceAnswer));
	return true;
}

void UStageLicenceSubsystem::HandleLicenceAnswer(const FStageCloudResponse& Response)
{
	RequestsInFlight = FMath::Max(RequestsInFlight - 1, 0);
	if (!Response.IsSuccess())
	{
		const FStageCloudError Error = StageCloudJson::ParseError(Response);
		if (Error.Code == TEXT("NoActiveSubscription"))
		{
			// The server is the authority and it has answered: a cancelled subscription ends Pro now, not when the token runs out.
			DeleteStored();
			ClearLicence(EStageLicenceStatus::Free, StageLicence::DescribeError(Error));
		}
		else
		{
			SetStatusMessage(StageLicence::DescribeError(Error));
		}
		FetchSubscription(0);
		return;
	}

	const TSharedPtr<FJsonObject> Object = StageLicence::ParseObject(Response);
	FString Token;
	if (!Object.IsValid() || !Object->TryGetStringField(TEXT("token"), Token) || Token.Len() > StageLicenceToken::MaxTokenLength)
	{
		SetStatusMessage(LOCTEXT("BadAnswer", "StageCraft Cloud sent an unreadable licence. The licence on this computer stays as it is."));
		return;
	}
	FStageLicenceClaims Claims;
	if (StageLicenceToken::Verify(Token, StageLicenceToken::GetTrustedKeys(), Claims) != EStageLicenceTokenError::None
		|| StageLicenceToken::Evaluate(Claims, MachineId, StageLicence::NowUnix(), HighWaterUnix) != EStageLicenceValidity::Valid)
	{
		// Never replace a working licence with one this computer cannot use (an untrusted key on a misconfigured server).
		UE_LOG(LogStageCraft, Warning, TEXT("Licence: the server's token does not verify on this computer; kept the current one."));
		SetStatusMessage(LOCTEXT("ServerTokenRefused", "StageCraft Cloud sent a licence this version of StageCraft cannot verify."));
		return;
	}
	HighWaterUnix = FMath::Max(HighWaterUnix, StageLicence::NowUnix());
	SaveStored({ Token, HighWaterUnix });
	ApplyToken(Token, FText::GetEmpty());
	UE_LOG(LogStageCraft, Log, TEXT("Licence: refreshed (%s, until %s)."), *Plan, *ExpiresUtc.ToIso8601());
	FetchSubscription(0);
}

void UStageLicenceSubsystem::FetchSubscription(int32 WaitSeconds, uint32 Serial)
{
	UStageCloudSubsystem* Cloud = GetCloud();
	if (!Cloud || !Cloud->IsSignedIn())
	{
		return;
	}
	const int32 Wait = FMath::Clamp(WaitSeconds, 0, 25);
	Cloud->SendJson(TEXT("GET"), FString::Printf(TEXT("/v1/me/subscription?waitSeconds=%d"), Wait), FString(), /*bAuthenticated*/ true,
		FOnStageCloudResponse::CreateUObject(this, &ThisClass::HandleSubscriptionAnswer, Serial), Wait > 0 ? static_cast<float>(Wait + 15) : 0.f);
}

void UStageLicenceSubsystem::HandleSubscriptionAnswer(const FStageCloudResponse& Response, uint32 Serial)
{
	const TSharedPtr<FJsonObject> Object = Response.IsSuccess() ? StageLicence::ParseObject(Response) : nullptr;
	if (Object.IsValid())
	{
		FStageSubscriptionInfo Info;
		Info.bKnown = true;
		Object->TryGetStringField(TEXT("status"), Info.Status);
		Info.Status = Info.Status.Left(32);
		Object->TryGetBoolField(TEXT("entitled"), Info.bEntitled);
		Object->TryGetStringField(TEXT("plan"), Info.Plan);
		Info.Plan = Info.Plan.Left(32);
		FString PeriodEnd;
		if (Object->TryGetStringField(TEXT("currentPeriodEnd"), PeriodEnd))
		{
			FDateTime::ParseIso8601(*PeriodEnd, Info.PeriodEndUtc);
		}
		Object->TryGetBoolField(TEXT("cancelAtPeriodEnd"), Info.bCancelAtPeriodEnd);
		Subscription = MoveTemp(Info);
		OnLicenceChanged.Broadcast();
	}

	if (!bWaitingForCheckout || Serial == 0 || Serial != CheckoutSerial)
	{
		return;
	}
	if (Subscription.bEntitled)
	{
		StopCheckoutWait();
		FText Ignored;
		RequestRefresh(Ignored);
		return;
	}
	if (FDateTime::UtcNow() >= CheckoutDeadlineUtc)
	{
		StopCheckoutWait();
		SetStatusMessage(LOCTEXT("CheckoutTimedOut", "The checkout was not completed. If you paid, click REFRESH."));
		return;
	}
	// The next poll always starts from a timer, never inside this answer: a long poll that came back unchanged goes again on the next
	// tick, a failed one after a few seconds. Calling it here would recurse when a transport answers at once.
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		const FTimerDelegate Next = FTimerDelegate::CreateWeakLambda(this, [this, Serial]() { FetchSubscription(SubscriptionWaitSeconds, Serial); });
		if (Response.IsSuccess())
		{
			CheckoutRetryTimer = GameInstance->GetTimerManager().SetTimerForNextTick(Next);
		}
		else
		{
			GameInstance->GetTimerManager().SetTimer(CheckoutRetryTimer, Next, StageLicence::CheckoutRetrySeconds, /*bLoop*/ false);
		}
	}
}

bool UStageLicenceSubsystem::StartCheckout(EStageProPlan InPlan, FText& OutError)
{
	UStageCloudSubsystem* Cloud = GetCloud();
	if (!Cloud || !Cloud->IsSignedIn())
	{
		OutError = LOCTEXT("SignInToBuy", "Sign in to StageCraft Cloud (Marketplace, SIGN IN) to subscribe to StageCraft Pro.");
		return false;
	}
	if (bWaitingForCheckout)
	{
		OutError = LOCTEXT("CheckoutOpen", "A checkout is already open in your browser.");
		return false;
	}
	const FString Body = StageLicence::JsonBody([InPlan](TJsonWriter<>& Writer)
	{
		Writer.WriteValue(TEXT("plan"), StageLicence::PlanName(InPlan));
		Writer.WriteValue(TEXT("idempotencyKey"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	});
	++RequestsInFlight;
	SetStatusMessage(LOCTEXT("OpeningCheckout", "Opening the checkout in your browser..."));
	Cloud->SendJson(TEXT("POST"), TEXT("/v1/subscriptions/checkout"), Body, /*bAuthenticated*/ true,
		FOnStageCloudResponse::CreateUObject(this, &ThisClass::HandleCheckoutAnswer));
	return true;
}

void UStageLicenceSubsystem::HandleCheckoutAnswer(const FStageCloudResponse& Response)
{
	RequestsInFlight = FMath::Max(RequestsInFlight - 1, 0);
	if (!Response.IsSuccess())
	{
		const FStageCloudError Error = StageCloudJson::ParseError(Response);
		SetStatusMessage(StageLicence::DescribeError(Error));
		if (Error.Code == TEXT("AlreadySubscribed"))
		{
			FText Ignored;
			RequestRefresh(Ignored);
		}
		return;
	}
	const FString Url = StageLicence::ParseUrl(Response, TEXT("url"));
	UStageCloudSubsystem* Cloud = GetCloud();
	if (Url.IsEmpty() || !Cloud || !Cloud->OpenInBrowser(Url))
	{
		SetStatusMessage(LOCTEXT("CheckoutNoBrowser", "The checkout page could not be opened. Try again."));
		return;
	}
	bWaitingForCheckout = true;
	++CheckoutSerial;
	CheckoutDeadlineUtc = FDateTime::UtcNow() + FTimespan::FromMinutes(FMath::Max(CheckoutWaitMinutes, 1.f));
	SetStatusMessage(LOCTEXT("WaitingForPayment", "Complete the checkout in your browser. StageCraft Pro switches on here when the payment is confirmed."));
	FetchSubscription(SubscriptionWaitSeconds, CheckoutSerial);
}

void UStageLicenceSubsystem::CancelCheckoutWait()
{
	if (bWaitingForCheckout)
	{
		StopCheckoutWait();
		SetStatusMessage(LOCTEXT("CheckoutStopped", "Stopped waiting for the checkout. If you pay later, click REFRESH."));
	}
}

void UStageLicenceSubsystem::StopCheckoutWait()
{
	bWaitingForCheckout = false;
	++CheckoutSerial;
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().ClearTimer(CheckoutRetryTimer);
	}
}

bool UStageLicenceSubsystem::OpenManageSubscription(FText& OutError)
{
	UStageCloudSubsystem* Cloud = GetCloud();
	if (!Cloud || !Cloud->IsSignedIn())
	{
		OutError = LOCTEXT("SignInToManage", "Sign in to StageCraft Cloud to manage your subscription.");
		return false;
	}
	++RequestsInFlight;
	OnLicenceChanged.Broadcast();
	Cloud->SendJson(TEXT("POST"), TEXT("/v1/subscriptions/portal"), FString(), /*bAuthenticated*/ true,
		FOnStageCloudResponse::CreateUObject(this, &ThisClass::HandlePortalAnswer));
	return true;
}

void UStageLicenceSubsystem::HandlePortalAnswer(const FStageCloudResponse& Response)
{
	RequestsInFlight = FMath::Max(RequestsInFlight - 1, 0);
	if (!Response.IsSuccess())
	{
		SetStatusMessage(StageLicence::DescribeError(StageCloudJson::ParseError(Response)));
		return;
	}
	const FString Url = StageLicence::ParseUrl(Response, TEXT("url"));
	UStageCloudSubsystem* Cloud = GetCloud();
	SetStatusMessage(!Url.IsEmpty() && Cloud && Cloud->OpenInBrowser(Url)
		? LOCTEXT("PortalOpen", "Manage your subscription in the browser. Changes reach StageCraft at the next REFRESH.")
		: LOCTEXT("PortalNoBrowser", "The subscription page could not be opened. Try again."));
}

bool UStageLicenceSubsystem::ReleaseThisMachine(FText& OutError)
{
	UStageCloudSubsystem* Cloud = GetCloud();
	if (!Cloud || !Cloud->IsSignedIn())
	{
		OutError = LOCTEXT("SignInToRelease", "Sign in to StageCraft Cloud to release this computer.");
		return false;
	}
	const FString Body = StageLicence::JsonBody([this](TJsonWriter<>& Writer) { Writer.WriteValue(TEXT("machineId"), MachineId); });
	++RequestsInFlight;
	OnLicenceChanged.Broadcast();
	Cloud->SendJson(TEXT("POST"), TEXT("/v1/licences/release"), Body, /*bAuthenticated*/ true,
		FOnStageCloudResponse::CreateUObject(this, &ThisClass::HandleReleaseAnswer));
	return true;
}

void UStageLicenceSubsystem::HandleReleaseAnswer(const FStageCloudResponse& Response)
{
	RequestsInFlight = FMath::Max(RequestsInFlight - 1, 0);
	if (!Response.IsSuccess())
	{
		SetStatusMessage(StageLicence::DescribeError(StageCloudJson::ParseError(Response)));
		return;
	}
	DeleteStored();
	ClearLicence(EStageLicenceStatus::Free, LOCTEXT("Released", "StageCraft Pro was released on this computer. Its place on your account is free for another one."));
}

#if WITH_DEV_AUTOMATION_TESTS
void UStageLicenceSubsystem::SetMachineIdForTesting(const FString& InMachineId)
{
	MachineId = InMachineId;
	const FStoredLicence Stored = LoadStored();
	HighWaterUnix = Stored.HighWaterUnix;
	if (Stored.Token.IsEmpty())
	{
		ClearLicence(EStageLicenceStatus::Free, FText::GetEmpty());
	}
	else
	{
		ApplyToken(Stored.Token, FText::GetEmpty());
	}
}
#endif

#undef LOCTEXT_NAMESPACE
