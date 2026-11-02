// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Marketplace/StageMarketplaceTypes.h"
#include "StageDispatchTypes.generated.h"

/** Where an acquired stage goes (Docs/ADR/0022-dcc-bridge.md §11.2). */
UENUM(BlueprintType)
enum class EStageDispatchKind : uint8
{
	/** Into StageCraft's scenes; the view offers to open it. Always allowed. */
	OpenInStageCraft,
	/** A .glb file the user chose before buying. Needs export rights. */
	SaveFile,
	/** A connected 3D app's add-on (Blender, 3ds Max, Cinema 4D, Maya). Needs export rights. */
	BridgeApp,
	/** An external AI tool that takes models. Needs export rights and the tool's key. */
	AITool,
};

/** Whether a target can be used now, and if not, why. Appended only. */
UENUM(BlueprintType)
enum class EStageDispatchAvailability : uint8
{
	Available,
	/** The publisher did not allow the stage to leave StageCraft. */
	NeedsExportRights,
	/** The app is not connected (or the bridge is off). */
	NotConnected,
	/** The app imports only USD, which comes in a later phase (Maya). */
	NeedsUsd,
	/** The AI tool needs an API key. */
	NeedsKey,
	/** The AI tool refused the saved key. */
	KeyRejected,
	/** The app or tool is busy with another hand-off. */
	Busy,
	/** No such tool, or it does not take models. */
	Unavailable,
};

USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageDispatchTarget
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "Dispatch")
	EStageDispatchKind Kind = EStageDispatchKind::OpenInStageCraft;

	/** BridgeApp: the connected client. */
	UPROPERTY(BlueprintReadWrite, Category = "Dispatch")
	int32 BridgeClientId = INDEX_NONE;

	/** SaveFile: an absolute .glb path. */
	UPROPERTY(BlueprintReadWrite, Category = "Dispatch")
	FString FilePath;

	/** AITool: the tool's id. */
	UPROPERTY(BlueprintReadWrite, Category = "Dispatch")
	FName ProviderId;

	bool LeavesApp() const { return Kind != EStageDispatchKind::OpenInStageCraft; }
};

/** "Get this listing, then send it there", as the acquire dialog confirms it. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageAcquireOrder
{
	GENERATED_BODY()

	/** A listing of the marketplace's current results. */
	UPROPERTY(BlueprintReadWrite, Category = "Dispatch")
	FGuid ListingId;

	/**
	 * The whole price the user saw and confirmed (credits and any real-money part); the order is refused if any part of the
	 * listing's price is different now.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "Dispatch")
	FStageMarketplacePrice ExpectedPrice;

	UPROPERTY(BlueprintReadWrite, Category = "Dispatch")
	FStageDispatchTarget Target;
};

UENUM(BlueprintType)
enum class EStageOrderStage : uint8
{
	Idle,
	/** Buying (if paid), downloading and importing. */
	Acquiring,
	/** Exporting and handing the stage to its target. */
	Delivering,
};

/** Outcome codes of an order. Appended only. */
UENUM(BlueprintType)
enum class EStageOrderResult : uint8
{
	Success,
	InvalidRequest,
	/** Another order is running. */
	Busy,
	/** The listing is not in the marketplace's current results. */
	NotFound,
	/** The listing's price is not the one confirmed. */
	PriceChanged,
	/** The target cannot be used (see EStageDispatchAvailability). */
	TargetUnavailable,
	/** Buying, downloading or importing failed; nothing was acquired. */
	AcquireFailed,
	/** The stage was acquired, but could not be delivered to its target. */
	DeliveryFailed,
	/**
	 * The order did not end within UStageAcquireDispatchSubsystem::OrderTimeoutSeconds. SceneName says whether it was acquired;
	 * a purchase still being settled may complete later and appear in the user's scenes.
	 */
	TimedOut,
};

USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageOrderResultInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Dispatch")
	EStageOrderResult Code = EStageOrderResult::Success;

	/** User-facing; when the stage was acquired but not delivered, it says both. */
	UPROPERTY(BlueprintReadOnly, Category = "Dispatch")
	FText Message;

	UPROPERTY(BlueprintReadOnly, Category = "Dispatch")
	FGuid ListingId;

	UPROPERTY(BlueprintReadOnly, Category = "Dispatch")
	FStageDispatchTarget Target;

	/** The imported scene, once acquired (also on DeliveryFailed). */
	UPROPERTY(BlueprintReadOnly, Category = "Dispatch")
	FString SceneName;

	UPROPERTY(BlueprintReadOnly, Category = "Dispatch")
	EStageDispatchAvailability Availability = EStageDispatchAvailability::Available;

	bool IsSuccess() const { return Code == EStageOrderResult::Success; }
	bool WasAcquired() const { return !SceneName.IsEmpty(); }
};
