// Copyright Epic Games, Inc. All Rights Reserved.

#include "Fixtures/StageGdtfShareSubsystem.h"

#include "Async/Async.h"
#include "Engine/GameInstance.h"
#include "Fixtures/StageFixtureLibrarySubsystem.h"
#include "Fixtures/StageGdtfShareProtocol.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "ModularSceneBuilder.h"
#include "Tasks/Task.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageGdtfShareSubsystem)

#define LOCTEXT_NAMESPACE "StageGdtfShare"

namespace StageGdtfSharePrivate
{
	constexpr float DefaultRequestTimeoutSeconds = 60.f;
	constexpr float DefaultDownloadTimeoutSeconds = 120.f;
	constexpr int64 ProgressStepBytes = 256 * 1024;

	FText Cancelled()
	{
		return LOCTEXT("Cancelled", "Download cancelled.");
	}

	TArray<FString> ReadSetCookies(const IHttpResponse& Response)
	{
		TArray<FString> Values;
		for (const FString& Header : Response.GetAllHeaders())
		{
			FString Name;
			FString Value;
			if (Header.Split(TEXT(":"), &Name, &Value) && Name.TrimStartAndEnd().Equals(TEXT("Set-Cookie"), ESearchCase::IgnoreCase))
			{
				Values.Add(Value.TrimStartAndEnd());
			}
		}
		return Values;
	}
}

void UStageGdtfShareSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Collection.InitializeDependency<UStageFixtureLibrarySubsystem>();
	Super::Initialize(Collection);
}

void UStageGdtfShareSubsystem::Deinitialize()
{
	CancelRequests(false);
	++SessionSerial;
	++ListSerial;
	State = EStageGdtfShareState::SignedOut;
	SessionCookie.Reset();
	UserName.Reset();
	RunningDownloads.Reset();
	Downloads.Reset();
	Entries.Reset();
	bListLoading = false;
	OnChanged.Clear();
	OnRequestFinished.Clear();
#if WITH_DEV_AUTOMATION_TESTS
	TestTransport = nullptr;
#endif
	Super::Deinitialize();
}

FText UStageGdtfShareSubsystem::GetAttribution()
{
	return LOCTEXT("Attribution", "Powered by GDTF");
}

FString UStageGdtfShareSubsystem::GetBaseUrl() const
{
	return BaseUrl.IsEmpty() ? FString(StageGdtfShare::DefaultBaseUrl) : BaseUrl;
}

FStageGdtfShareResultInfo UStageGdtfShareSubsystem::SignIn(const FString& InUserName, const FString& Password)
{
	const FString Name = InUserName.TrimStartAndEnd();
	if (Name.IsEmpty() || Password.IsEmpty() || Name.Len() > MaxUserNameLength || Password.Len() > MaxPasswordLength)
	{
		return FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::InvalidRequest, LOCTEXT("Credentials", "Enter your GDTF Share user name and password."));
	}
	if (State == EStageGdtfShareState::SigningIn)
	{
		return FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::Busy, LOCTEXT("SigningIn", "Signing in to GDTF Share already."));
	}
	if (!StageGdtfShare::IsSafeBaseUrl(GetBaseUrl()))
	{
		UE_LOG(LogStageCraft, Warning, TEXT("GDTF Share: the configured API root is not an https address; sign-in refused."));
		return FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::NotAllowed, LOCTEXT("UnsafeUrl", "The GDTF Share address is not a secure (https) address, so your password is not sent."));
	}

	CancelRequests(false);
	for (const TPair<int32, uint32>& Running : RunningDownloads)
	{
		if (FStageGdtfShareDownload* Row = FindDownload(Running.Key))
		{
			Row->State = EStageGdtfShareDownloadState::Failed;
			Row->Message = StageGdtfSharePrivate::Cancelled();
		}
	}
	RunningDownloads.Reset();
	++ListSerial;
	bListLoading = false;

	const uint32 Serial = ++SessionSerial;
	State = EStageGdtfShareState::SigningIn;
	UserName = Name;
	SessionCookie.Reset();
	OnChanged.Broadcast();

	FStageGdtfShareHttpRequest Request;
	Request.Verb = TEXT("POST");
	Request.Url = GetBaseUrl() + StageGdtfShare::LoginEndpoint;
	Request.ContentType = TEXT("application/json");
	Request.Body = StageGdtfShare::MakeLoginBody(Name, Password);
	Request.MaxResponseBytes = StageGdtfShare::MaxLoginResponseBytes;
	Send(MoveTemp(Request), RequestTimeoutSeconds > 0.f ? RequestTimeoutSeconds : StageGdtfSharePrivate::DefaultRequestTimeoutSeconds,
		FOnStageGdtfShareResponse::CreateWeakLambda(this, [this, Serial](const FStageGdtfShareHttpResponse& Response) { HandleSignIn(Serial, Response); }),
		FOnStageGdtfShareProgress(), 0);
	return FStageGdtfShareResultInfo::Ok();
}

void UStageGdtfShareSubsystem::HandleSignIn(uint32 Serial, const FStageGdtfShareHttpResponse& Response)
{
	if (Serial != SessionSerial || State != EStageGdtfShareState::SigningIn)
	{
		return;
	}

	bool bResult = false;
	FString ServerText;
	const bool bParsed = Response.bConnected && StageGdtfShare::ParseResultFlag(Response.Body, bResult, ServerText);
	FStageGdtfShareResultInfo Result;
	if (!Response.bConnected)
	{
		Result = DescribeFailure(Response, LOCTEXT("WhatSignIn", "signing in"));
	}
	else if (Response.Status == 401 || (Response.Status == 200 && bParsed && !bResult))
	{
		Result = FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::BadCredentials, LOCTEXT("BadCredentials", "GDTF Share did not accept that user name and password."));
	}
	else if (Response.Status != 200)
	{
		Result = DescribeFailure(Response, LOCTEXT("WhatSignIn", "signing in"));
	}
	else if (!bParsed)
	{
		Result = FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::BadResponse, LOCTEXT("BadSignInAnswer", "GDTF Share answered the sign-in in a way StageCraft does not understand."));
	}
	else
	{
		SessionCookie = StageGdtfShare::JoinCookies(Response.SetCookies);
		if (SessionCookie.IsEmpty())
		{
			Result = FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::BadResponse, LOCTEXT("NoSession", "GDTF Share accepted the sign-in but did not open a session."));
		}
	}

	if (!Result.IsSuccess())
	{
		++SessionSerial;
		State = EStageGdtfShareState::SignedOut;
		UserName.Reset();
		SessionCookie.Reset();
		UE_LOG(LogStageCraft, Log, TEXT("GDTF Share: sign-in failed (%s, HTTP %d)."), *UEnum::GetValueAsString(Result.Code), Response.Status);
		OnChanged.Broadcast();
		OnRequestFinished.Broadcast(Result);
		return;
	}

	State = EStageGdtfShareState::SignedIn;
	UE_LOG(LogStageCraft, Log, TEXT("GDTF Share: signed in."));
	OnChanged.Broadcast();
	OnRequestFinished.Broadcast(FStageGdtfShareResultInfo::Ok(FText::Format(LOCTEXT("SignedIn", "Signed in to GDTF Share as {0}."), FText::FromString(UserName))));
	RefreshList(false);
}

void UStageGdtfShareSubsystem::SignOut()
{
	if (State == EStageGdtfShareState::SignedOut && RunningDownloads.IsEmpty() && !bListLoading)
	{
		return;
	}
	DropSession(FText::GetEmpty());
}

void UStageGdtfShareSubsystem::DropSession(const FText& Reason)
{
	CancelRequests(false);
	for (const TPair<int32, uint32>& Running : RunningDownloads)
	{
		if (FStageGdtfShareDownload* Row = FindDownload(Running.Key))
		{
			Row->State = EStageGdtfShareDownloadState::Failed;
			Row->Message = StageGdtfSharePrivate::Cancelled();
		}
	}
	RunningDownloads.Reset();
	++SessionSerial;
	++ListSerial;
	bListLoading = false;
	State = EStageGdtfShareState::SignedOut;
	UserName.Reset();
	SessionCookie.Reset();
	UE_LOG(LogStageCraft, Log, TEXT("GDTF Share: signed out%s"), Reason.IsEmpty() ? TEXT(".") : TEXT(" (session ended)."));
	OnChanged.Broadcast();
	if (!Reason.IsEmpty())
	{
		OnRequestFinished.Broadcast(FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::SessionExpired, Reason));
	}
}

FStageGdtfShareResultInfo UStageGdtfShareSubsystem::RefreshList(bool bForce)
{
	if (State != EStageGdtfShareState::SignedIn)
	{
		return FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::SignInRequired, LOCTEXT("SignInFirst", "Sign in to GDTF Share first."));
	}
	if (bListLoading)
	{
		return FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::Busy, LOCTEXT("ListLoading", "The GDTF Share list is already loading."));
	}
	const bool bFresh = ListFetchedUtc != FDateTime() && (FDateTime::UtcNow() - ListFetchedUtc).GetTotalSeconds() < StageGdtfShare::ListMaxAgeSeconds;
	if (bFresh && !bForce)
	{
		return FStageGdtfShareResultInfo::Ok();
	}

	const uint32 Serial = ++ListSerial;
	bListLoading = true;
	OnChanged.Broadcast();

	FStageGdtfShareHttpRequest Request;
	Request.Verb = TEXT("GET");
	Request.Url = GetBaseUrl() + StageGdtfShare::ListEndpoint;
	Request.Cookie = SessionCookie;
	Request.MaxResponseBytes = StageGdtfShare::MaxListResponseBytes;
	Send(MoveTemp(Request), RequestTimeoutSeconds > 0.f ? RequestTimeoutSeconds : StageGdtfSharePrivate::DefaultRequestTimeoutSeconds,
		FOnStageGdtfShareResponse::CreateWeakLambda(this, [this, Serial](const FStageGdtfShareHttpResponse& Response) { HandleList(Serial, Response); }),
		FOnStageGdtfShareProgress(), 0);
	return FStageGdtfShareResultInfo::Ok();
}

void UStageGdtfShareSubsystem::HandleList(uint32 Serial, const FStageGdtfShareHttpResponse& Response)
{
	if (Serial != ListSerial || !bListLoading)
	{
		return;
	}
	if (!Response.bConnected || Response.Status != 200)
	{
		bListLoading = false;
		const FStageGdtfShareResultInfo Result = DescribeFailure(Response, LOCTEXT("WhatList", "loading the fixture list"));
		if (Result.Code != EStageGdtfShareResult::SessionExpired)
		{
			OnChanged.Broadcast();
			OnRequestFinished.Broadcast(Result);
		}
		return;
	}

	UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis = TWeakObjectPtr<UStageGdtfShareSubsystem>(this), Serial, Body = Response.Body]()
	{
		TArray<FStageGdtfShareEntry> Parsed;
		int32 Skipped = 0;
		const bool bParsed = StageGdtfShare::ParseList(Body, Parsed, Skipped);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Serial, Parsed = MoveTemp(Parsed), Skipped, bParsed]() mutable
		{
			if (UStageGdtfShareSubsystem* Self = WeakThis.Get())
			{
				Self->FinishList(Serial, MoveTemp(Parsed), Skipped, bParsed);
			}
		});
	});
}

void UStageGdtfShareSubsystem::FinishList(uint32 Serial, TArray<FStageGdtfShareEntry> Parsed, int32 Skipped, bool bParsed)
{
	if (Serial != ListSerial || !bListLoading)
	{
		return;
	}
	bListLoading = false;
	if (!bParsed)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("GDTF Share: the fixture list could not be read."));
		OnChanged.Broadcast();
		OnRequestFinished.Broadcast(FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::BadResponse,
			LOCTEXT("BadList", "GDTF Share sent a fixture list StageCraft does not understand.")));
		return;
	}
	Entries = MoveTemp(Parsed);
	ListFetchedUtc = FDateTime::UtcNow();
	UE_LOG(LogStageCraft, Log, TEXT("GDTF Share: %d fixture files listed, %d entries skipped."), Entries.Num(), Skipped);
	OnChanged.Broadcast();
	OnRequestFinished.Broadcast(FStageGdtfShareResultInfo::Ok(FText::Format(LOCTEXT("Listed", "GDTF Share lists {0} fixture files."), FText::AsNumber(Entries.Num()))));
}

const FStageGdtfShareEntry* UStageGdtfShareSubsystem::FindEntry(int32 RevisionId) const
{
	return Entries.FindByPredicate([RevisionId](const FStageGdtfShareEntry& Entry) { return Entry.RevisionId == RevisionId; });
}

FStageGdtfShareDownload* UStageGdtfShareSubsystem::FindDownload(int32 RevisionId)
{
	return Downloads.FindByPredicate([RevisionId](const FStageGdtfShareDownload& Row) { return Row.RevisionId == RevisionId; });
}

FStageGdtfShareResultInfo UStageGdtfShareSubsystem::Download(int32 RevisionId)
{
	if (State != EStageGdtfShareState::SignedIn)
	{
		return FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::SignInRequired, LOCTEXT("SignInFirst", "Sign in to GDTF Share first."));
	}
	const FStageGdtfShareEntry* Entry = FindEntry(RevisionId);
	if (!Entry)
	{
		return FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::InvalidRequest, LOCTEXT("NotListed", "That file is not in the GDTF Share list. Refresh the list."));
	}
	if (RunningDownloads.Contains(RevisionId))
	{
		return FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::Busy, FText::Format(LOCTEXT("AlreadyDownloading", "{0} is already downloading."), FText::FromString(StageGdtfShare::MakeLabel(*Entry))));
	}
	if (RunningDownloads.Num() >= MaxConcurrentDownloads)
	{
		return FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::Busy, FText::Format(LOCTEXT("TooManyDownloads", "{0} downloads are running; wait for one to finish."), FText::AsNumber(MaxConcurrentDownloads)));
	}
	if (Entry->FileSizeBytes > StageGdtfShare::MaxDownloadBytes)
	{
		return FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::TooLarge, FText::Format(LOCTEXT("ListedTooLarge", "{0} is larger than {1} MB, the most a GDTF file may be."),
			FText::FromString(StageGdtfShare::MakeLabel(*Entry)), FText::AsNumber(StageGdtfShare::MaxDownloadBytes / (1024 * 1024))));
	}

	Downloads.RemoveAll([RevisionId](const FStageGdtfShareDownload& Row) { return Row.RevisionId == RevisionId; });
	FStageGdtfShareDownload Row;
	Row.RevisionId = RevisionId;
	Row.Label = StageGdtfShare::MakeLabel(*Entry);
	Row.FileName = StageGdtfShare::MakeFileName(*Entry);
	Row.TotalBytes = Entry->FileSizeBytes;
	Row.Message = LOCTEXT("Downloading", "Downloading...");
	Downloads.Insert(MoveTemp(Row), 0);
	for (int32 Index = Downloads.Num() - 1; Index >= 0 && Downloads.Num() > MaxListedDownloads; --Index)
	{
		if (Downloads[Index].State != EStageGdtfShareDownloadState::Downloading)
		{
			Downloads.RemoveAt(Index);
		}
	}

	const uint32 Serial = ++DownloadSerial;
	RunningDownloads.Add(RevisionId, Serial);
	OnChanged.Broadcast();

	FStageGdtfShareHttpRequest Request;
	Request.Verb = TEXT("GET");
	Request.Url = FString::Printf(TEXT("%s%s?rid=%d"), *GetBaseUrl(), StageGdtfShare::DownloadEndpoint, RevisionId);
	Request.Cookie = SessionCookie;
	Request.MaxResponseBytes = StageGdtfShare::MaxDownloadBytes;
	Send(MoveTemp(Request), DownloadTimeoutSeconds > 0.f ? DownloadTimeoutSeconds : StageGdtfSharePrivate::DefaultDownloadTimeoutSeconds,
		FOnStageGdtfShareResponse::CreateWeakLambda(this, [this, RevisionId, Serial](const FStageGdtfShareHttpResponse& Response) { HandleDownload(RevisionId, Serial, Response); }),
		FOnStageGdtfShareProgress::CreateWeakLambda(this, [this, RevisionId, Serial](int64 Received, int64 Total) { HandleDownloadProgress(RevisionId, Serial, Received, Total); }),
		RevisionId);
	return FStageGdtfShareResultInfo::Ok();
}

void UStageGdtfShareSubsystem::HandleDownloadProgress(int32 RevisionId, uint32 Serial, int64 Received, int64 Total)
{
	const uint32* Running = RunningDownloads.Find(RevisionId);
	FStageGdtfShareDownload* Row = FindDownload(RevisionId);
	if (!Running || *Running != Serial || !Row || Received - Row->ReceivedBytes < StageGdtfSharePrivate::ProgressStepBytes)
	{
		return;
	}
	Row->ReceivedBytes = Received;
	if (Total > 0)
	{
		Row->TotalBytes = Total;
	}
	OnChanged.Broadcast();
}

void UStageGdtfShareSubsystem::HandleDownload(int32 RevisionId, uint32 Serial, const FStageGdtfShareHttpResponse& Response)
{
	const uint32* Running = RunningDownloads.Find(RevisionId);
	if (!Running || *Running != Serial)
	{
		return;
	}
	RunningDownloads.Remove(RevisionId);

	if (!Response.bConnected || Response.Status != 200)
	{
		const FStageGdtfShareResultInfo Result = DescribeFailure(Response, LOCTEXT("WhatDownload", "downloading the file"));
		if (Result.Code != EStageGdtfShareResult::SessionExpired)
		{
			FailDownload(RevisionId, Result);
		}
		else if (FStageGdtfShareDownload* Row = FindDownload(RevisionId))
		{
			Row->State = EStageGdtfShareDownloadState::Failed;
			Row->Message = Result.Message;
			OnChanged.Broadcast();
		}
		return;
	}
	if (!StageGdtfShare::IsZipFile(Response.Body))
	{
		bool bResult = false;
		FString ServerText;
		const bool bNotFound = StageGdtfShare::ParseResultFlag(Response.Body, bResult, ServerText) && !bResult;
		FailDownload(RevisionId, bNotFound
			? FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::NotFound, LOCTEXT("FileGone", "GDTF Share no longer has this file. Refresh the list."))
			: FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::BadResponse, LOCTEXT("NotGdtf", "GDTF Share did not send a GDTF file.")));
		return;
	}

	FStageGdtfShareDownload* Row = FindDownload(RevisionId);
	UStageFixtureLibrarySubsystem* Library = GetGameInstance() ? GetGameInstance()->GetSubsystem<UStageFixtureLibrarySubsystem>() : nullptr;
	if (!Row || !Library)
	{
		FailDownload(RevisionId, FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::ImportRefused, LOCTEXT("NoLibrary", "The fixture Library is not available.")));
		return;
	}
	const int64 Size = Response.Body.Num();
	const FStageFixtureImportOutcome Outcome = Library->RequestImportBytes(Response.Body, Row->FileName);
	if (!Outcome.IsSuccess())
	{
		FailDownload(RevisionId, FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::ImportRefused, Outcome.Message));
		return;
	}
	Row->State = EStageGdtfShareDownloadState::Delivered;
	Row->ReceivedBytes = Size;
	Row->TotalBytes = Size;
	Row->Message = LOCTEXT("Delivered", "Downloaded. Review it to add it to the Library.");
	UE_LOG(LogStageCraft, Log, TEXT("GDTF Share: downloaded revision %d (%lld bytes) for review as %s."), RevisionId, Size, *Row->FileName);
	OnChanged.Broadcast();
	OnRequestFinished.Broadcast(FStageGdtfShareResultInfo::Ok(FText::Format(LOCTEXT("DeliveredMessage", "Downloaded {0}. Review it to add it to the Library."), FText::FromString(Row->Label))));
}

void UStageGdtfShareSubsystem::FailDownload(int32 RevisionId, const FStageGdtfShareResultInfo& Result)
{
	FStageGdtfShareDownload* Row = FindDownload(RevisionId);
	if (Row)
	{
		Row->State = EStageGdtfShareDownloadState::Failed;
		Row->Message = Result.Message;
	}
	UE_LOG(LogStageCraft, Log, TEXT("GDTF Share: download of revision %d failed (%s)."), RevisionId, *UEnum::GetValueAsString(Result.Code));
	OnChanged.Broadcast();
	OnRequestFinished.Broadcast(Row ? FStageGdtfShareResultInfo::Fail(Result.Code, FText::Format(LOCTEXT("DownloadFailed", "{0}: {1}"), FText::FromString(Row->Label), Result.Message)) : Result);
}

bool UStageGdtfShareSubsystem::CancelDownload(int32 RevisionId)
{
	if (!RunningDownloads.Contains(RevisionId))
	{
		return false;
	}
	RunningDownloads.Remove(RevisionId);
	for (int32 Index = InFlight.Num() - 1; Index >= 0; --Index)
	{
		if (InFlight[Index].RevisionId == RevisionId)
		{
			const TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Request = InFlight[Index].Request;
			InFlight.RemoveAt(Index);
			if (Request.IsValid())
			{
				Request->OnProcessRequestComplete().Unbind();
				Request->OnRequestProgress64().Unbind();
				Request->CancelRequest();
			}
		}
	}
	FailDownload(RevisionId, FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::Cancelled, StageGdtfSharePrivate::Cancelled()));
	return true;
}

void UStageGdtfShareSubsystem::ClearFinishedDownloads()
{
	const int32 Removed = Downloads.RemoveAll([](const FStageGdtfShareDownload& Row) { return Row.State != EStageGdtfShareDownloadState::Downloading; });
	if (Removed > 0)
	{
		OnChanged.Broadcast();
	}
}

FStageGdtfShareResultInfo UStageGdtfShareSubsystem::DescribeFailure(const FStageGdtfShareHttpResponse& Response, const FText& What)
{
	if (Response.bTooLarge)
	{
		return FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::TooLarge, FText::Format(LOCTEXT("TooLarge", "GDTF Share sent more than StageCraft accepts ({0})."), What));
	}
	if (!Response.bConnected)
	{
		return FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::NetworkError, FText::Format(LOCTEXT("Network", "GDTF Share could not be reached ({0}). Check the internet connection."), What));
	}
	if (Response.Status == 401 && State == EStageGdtfShareState::SignedIn)
	{
		const FText Reason = LOCTEXT("SessionEnded", "Your GDTF Share session ended. Sign in again.");
		DropSession(Reason);
		return FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::SessionExpired, Reason);
	}
	if (Response.Status == 404)
	{
		return FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::NotFound, LOCTEXT("NotFound", "GDTF Share no longer has this file. Refresh the list."));
	}
	return FStageGdtfShareResultInfo::Fail(EStageGdtfShareResult::ServerError, FText::Format(LOCTEXT("ServerError", "GDTF Share answered with error {0} ({1})."), FText::AsNumber(Response.Status), What));
}

void UStageGdtfShareSubsystem::CancelRequests(bool bDownloadsOnly)
{
	TArray<FInFlight> Cancelled;
	for (int32 Index = InFlight.Num() - 1; Index >= 0; --Index)
	{
		if (!bDownloadsOnly || InFlight[Index].RevisionId != 0)
		{
			Cancelled.Add(InFlight[Index]);
			InFlight.RemoveAt(Index);
		}
	}
	for (const FInFlight& Each : Cancelled)
	{
		if (Each.Request.IsValid())
		{
			Each.Request->OnProcessRequestComplete().Unbind();
			Each.Request->OnRequestProgress64().Unbind();
			Each.Request->CancelRequest();
		}
	}
}

void UStageGdtfShareSubsystem::Send(FStageGdtfShareHttpRequest Request, float TimeoutSeconds, FOnStageGdtfShareResponse Answer, FOnStageGdtfShareProgress Progress, int32 DownloadRevisionId)
{
#if WITH_DEV_AUTOMATION_TESTS
	if (TestTransport)
	{
		TestTransport(Request, MoveTemp(Answer), MoveTemp(Progress));
		return;
	}
#endif

	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Http = FHttpModule::Get().CreateRequest();
	Http->SetVerb(Request.Verb);
	Http->SetURL(Request.Url);
	if (!Request.ContentType.IsEmpty())
	{
		Http->SetHeader(TEXT("Content-Type"), Request.ContentType);
	}
	if (!Request.Cookie.IsEmpty())
	{
		Http->SetHeader(TEXT("Cookie"), Request.Cookie);
	}
	if (!Request.Body.IsEmpty())
	{
		Http->SetContent(MoveTemp(Request.Body));
	}
	Http->SetTimeout(TimeoutSeconds);

	const int64 MaxBytes = Request.MaxResponseBytes;
	const TSharedRef<bool, ESPMode::ThreadSafe> bTooLarge = MakeShared<bool, ESPMode::ThreadSafe>(false);
	Http->OnRequestProgress64().BindWeakLambda(this, [Progress, MaxBytes, bTooLarge](FHttpRequestPtr Running, uint64 Sent, uint64 Received)
	{
		if (MaxBytes > 0 && Received > static_cast<uint64>(MaxBytes))
		{
			if (!*bTooLarge && Running.IsValid())
			{
				*bTooLarge = true;
				Running->CancelRequest();
			}
			return;
		}
		const FHttpResponsePtr Partial = Running.IsValid() ? Running->GetResponse() : nullptr;
		Progress.ExecuteIfBound(static_cast<int64>(Received), Partial.IsValid() ? static_cast<int64>(Partial->GetContentLength()) : 0);
	});
	Http->OnProcessRequestComplete().BindWeakLambda(this, [this, Answer, MaxBytes, bTooLarge](FHttpRequestPtr Done, FHttpResponsePtr Response, bool bConnectedSuccessfully)
	{
		InFlight.RemoveAll([&Done](const FInFlight& Each) { return Each.Request == Done; });
		FStageGdtfShareHttpResponse Out;
		Out.bTooLarge = *bTooLarge || (Response.IsValid() && MaxBytes > 0 && Response->GetContent().Num() > MaxBytes);
		Out.bConnected = !Out.bTooLarge && bConnectedSuccessfully && Response.IsValid();
		if (Out.bConnected)
		{
			Out.Status = Response->GetResponseCode();
			Out.Body = Response->GetContent();
			Out.SetCookies = StageGdtfSharePrivate::ReadSetCookies(*Response);
		}
		Answer.ExecuteIfBound(Out);
	});
	if (!Http->ProcessRequest())
	{
		Http->OnProcessRequestComplete().Unbind();
		Http->OnRequestProgress64().Unbind();
		Answer.ExecuteIfBound(FStageGdtfShareHttpResponse());
		return;
	}
	InFlight.Add({ Http, DownloadRevisionId });
}

#if WITH_DEV_AUTOMATION_TESTS
void UStageGdtfShareSubsystem::SetTransportForTesting(FTestTransport InTransport)
{
	TestTransport = MoveTemp(InTransport);
}

void UStageGdtfShareSubsystem::AgeListForTesting()
{
	if (ListFetchedUtc != FDateTime())
	{
		ListFetchedUtc -= FTimespan::FromSeconds(StageGdtfShare::ListMaxAgeSeconds + 1.0);
	}
}
#endif

#undef LOCTEXT_NAMESPACE
