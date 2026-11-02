// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Bridge/StageBridgeTypes.h"
#include "Engine/TimerHandle.h"
#include "Marketplace/StageDispatchTypes.h"
#include "Marketplace/StageMarketplaceTypes.h"
#include "Services/StageAIToolTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "StageAcquireDispatchSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnStageOrderProgress, FGuid, ListingId, EStageOrderStage, Stage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageOrderCompleted, const FStageOrderResultInfo&, Result);

/**
 * "Get this listing and send it there" (Docs/ADR/0022-dcc-bridge.md §11.2): runs one acquire order from the acquire dialog to
 * its end, so it survives the dialog or the Marketplace panel being closed.
 *
 * An order is checked before anything happens (one order at a time; the listing is in the current results; its price is the one
 * the user confirmed; the target is usable by StageDispatchRules), then acquired through UStageMarketplaceSubsystem::RequestAcquire
 * (the economy's validate -> settle -> re-validate -> commit path, unchanged), then delivered:
 *  - OpenInStageCraft: done when imported; the view opens the scene (it owns the unsaved-changes question).
 *  - SaveFile: UStageBridgeSubsystem::BeginExportSceneToFile to the path chosen before buying (over several frames).
 *  - BridgeApp: UStageBridgeSubsystem::SendSceneToApp, finished by the app's answer.
 *  - AITool: exported to the tool outbox the same way, then UStageAIToolSubsystem::SubmitModel, finished by the tool's answer.
 * Delivery starts on the tick after the acquire completed, never inside the marketplace's broadcast.
 * The target is evaluated again after acquiring, and the exporter checks the publisher's rights from the library package.
 * A purchase is never rolled back because a delivery failed: the stage is in the user's scenes, and the result says both.
 *
 * Push, never poll: delegates of the other subsystems are bound only while an order waits for them.
 * Callers in play go through AModularPlayerController::RequestAcquireAndDispatch.
 */
UCLASS(Config = Game)
class MODULARSCENEBUILDER_API UStageAcquireDispatchSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	/** Whether Target can take ListingId now (for display; RequestOrder checks again). Unavailable for an unknown listing. */
	EStageDispatchAvailability EvaluateTarget(const FGuid& ListingId, const FStageDispatchTarget& Target) const;

	/**
	 * The state every target is judged on for ListingId (bridge clients, tool key states, busy flags), for a view that evaluates
	 * several targets at once with StageDispatchRules::EvaluateTarget: one snapshot per refresh instead of one per row.
	 * False for an unknown listing.
	 */
	bool MakeContext(const FGuid& ListingId, struct FStageDispatchContext& OutContext) const;

	/** Starts an order. Success means started; the outcome arrives on OnOrderCompleted. A refusal is returned and not broadcast. */
	FStageOrderResultInfo RequestOrder(const FStageAcquireOrder& Order);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Dispatch")
	bool IsOrderPending() const { return Stage != EStageOrderStage::Idle; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Dispatch")
	EStageOrderStage GetStage() const { return Stage; }

	/** The order in flight; valid only while IsOrderPending. */
	const FStageAcquireOrder& GetPendingOrder() const { return Pending; }

	/** Where models for AI tools are exported before the hand-off (Saved/StageCraft/AITools/Outbox, absolute). */
	static FString GetToolOutboxDirectory();

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Dispatch")
	FOnStageOrderProgress OnOrderProgress;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Dispatch")
	FOnStageOrderCompleted OnOrderCompleted;

	/** The time an order has to end before it is given up with TimedOut (OrderTimeoutSeconds, validated). */
	float GetOrderTimeoutSeconds() const;

protected:
	/**
	 * A safety net over the dependencies' own timeouts (economy, marketplace fetch, bridge send, AI tool), so no answer that never
	 * comes can keep the dispatcher busy for the rest of the run. Values <= 0 fall back to the default.
	 */
	UPROPERTY(Config)
	float OrderTimeoutSeconds = 300.f;

private:
	struct FStageDispatchContext MakeContext(const FStageMarketplaceListing& Listing) const;

	void SetStage(EStageOrderStage NewStage);
	/** Runs Deliver on the next tick: never inside the marketplace's OnAcquireCompleted broadcast (an export builds a world). */
	void ScheduleDelivery();
	void Deliver();
	void HandleOrderTimeout();
	void ClearTimers();
	void Finish(EStageOrderResult Code, const FText& Message, EStageDispatchAvailability Availability = EStageDispatchAvailability::Available);
	void UnbindAll();
	FText DescribeDeliveryFailure(const FText& Reason) const;
	FText DescribeTarget() const;

	UFUNCTION()
	void HandleAcquireCompleted(FGuid ListingId, FStageMarketplaceResultInfo Result, const FString& SceneName);

	UFUNCTION()
	void HandleSendCompleted(const FStageBridgeResultInfo& Result);

	/** SaveFile / AITool: the export started by Deliver has ended (Serial: the delivery it belongs to). */
	void HandleExported(const FStageBridgeResultInfo& Exported, uint32 Serial);

	UFUNCTION()
	void HandleToolJobCompleted(const FStageAIToolResultInfo& Result);

	FStageAcquireOrder Pending;
	/** The listing as it was when the order started, for messages after the results have moved on. */
	FStageMarketplaceListing PendingListing;
	FString AcquiredScene;
	/** AITool: the exported model, deleted when the tool has answered. */
	FString ToolModelPath;
	EStageOrderStage Stage = EStageOrderStage::Idle;

	FTimerHandle DeliveryTimer;
	FTimerHandle OrderTimer;
	/** Increments per delivery and when an order ends, so a late export answer for an ended order is ignored. */
	uint32 DeliverySerial = 0;
};
