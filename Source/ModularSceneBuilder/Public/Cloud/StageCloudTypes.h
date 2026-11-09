// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Where this install is with StageCraft Cloud (Docs/ADR/0032 §5–§6). */
enum class EStageCloudState : uint8
{
	/** No server configured ([/Script/ModularSceneBuilder.StageCloudSubsystem] BaseUrl is empty): everything stays local. */
	Offline,
	SignedOut,
	/** A device code is shown and the browser is open; the app waits for the user to sign in there. */
	SigningIn,
	SignedIn,
};

/** An order's state as the server reports it (orders.ts ORDER_STATES); Unknown for anything this build does not know. */
enum class EStageCloudOrderState : uint8
{
	Created,
	CheckoutOpen,
	Paid,
	PartiallyRefunded,
	Refunded,
	Expired,
	Failed,
	Disputed,
	OnHold,
	Unknown,
};

/** The signed-in account (GET /v1/me). Display data only: the server decides every permission. */
struct FStageCloudAccount
{
	FString AccountId;
	FString DisplayName;
	TArray<FString> Roles;
	/** none, pending or enabled. */
	FString SellerStatus = TEXT("none");
	bool bPayoutsEnabled = false;

	bool IsAdmin() const { return Roles.Contains(TEXT("admin")); }
};

/** A device sign-in in progress (POST /v1/auth/device, RFC 8628 shape). */
struct FStageCloudDeviceCode
{
	FString DeviceCode;
	FString UserCode;
	FString VerificationUri;
	FString VerificationUriComplete;
	int32 ExpiresInSeconds = 0;
	int32 IntervalSeconds = 5;
};

enum class EStageCloudDevicePoll : uint8
{
	Pending,
	SlowDown,
	Expired,
	Denied,
	SignedIn,
	Failed,
};

struct FStageCloudDevicePollResult
{
	EStageCloudDevicePoll Outcome = EStageCloudDevicePoll::Failed;
	FString SessionToken;
};

/** One order (POST /v1/orders, GET /v1/orders/:id). */
struct FStageCloudOrder
{
	FString OrderId;
	FGuid ListingId;
	EStageCloudOrderState State = EStageCloudOrderState::Unknown;
	/** Set only while CheckoutOpen; checked by StageCloudJson::IsSafeBrowserUrl before it is ever opened. */
	FString CheckoutUrl;
	int64 AmountMinor = 0;
	FString Currency;

	/** Paid in any form the buyer keeps (refunds and disputes after payment still count as having bought). */
	bool IsPaid() const { return State == EStageCloudOrderState::Paid || State == EStageCloudOrderState::PartiallyRefunded || State == EStageCloudOrderState::Disputed; }

	/** Nothing more will happen to this order from the buyer's side. */
	bool IsSettled() const { return IsPaid() || State == EStageCloudOrderState::Refunded || State == EStageCloudOrderState::Expired || State == EStageCloudOrderState::Failed || State == EStageCloudOrderState::OnHold; }
};

/** A server error ({error:{code,message}}) or a transport failure (Status 0). */
struct FStageCloudError
{
	int32 Status = 0;
	FString Code;
	FString Message;
};

/** One HTTP request the cloud subsystem sends (also what a test transport receives). */
struct FStageCloudHttpRequest
{
	FString Verb;
	FString Url;
	TMap<FString, FString> Headers;
	TArray<uint8> Body;
};

/** One HTTP answer. bConnected is false when nothing came back (no network, timeout, cancelled). */
struct FStageCloudResponse
{
	bool bConnected = false;
	int32 Status = 0;
	TArray<uint8> Body;

	bool IsSuccess() const { return bConnected && Status >= 200 && Status < 300; }
	FString GetBodyAsString() const;
};

DECLARE_DELEGATE_OneParam(FOnStageCloudResponse, const FStageCloudResponse& /*Response*/);
