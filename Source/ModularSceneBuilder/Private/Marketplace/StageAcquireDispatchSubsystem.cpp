// Copyright Epic Games, Inc. All Rights Reserved.

#include "Marketplace/StageAcquireDispatchSubsystem.h"

#include "Bridge/StageBridgeProtocol.h"
#include "Bridge/StageBridgeSubsystem.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "Marketplace/StageDispatchRules.h"
#include "Marketplace/StageMarketplaceSubsystem.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Services/StageAIToolSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageAcquireDispatchSubsystem)

#define LOCTEXT_NAMESPACE "StageCraftDispatch"

void UStageAcquireDispatchSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Collection.InitializeDependency<UStageMarketplaceSubsystem>();
	Collection.InitializeDependency<UStageBridgeSubsystem>();
	Collection.InitializeDependency<UStageAIToolSubsystem>();
	Super::Initialize(Collection);
}

void UStageAcquireDispatchSubsystem::Deinitialize()
{
	UnbindAll();
	OnOrderProgress.Clear();
	OnOrderCompleted.Clear();
	if (!ToolModelPath.IsEmpty())
	{
		IFileManager::Get().Delete(*ToolModelPath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
	}
	Stage = EStageOrderStage::Idle;
	Super::Deinitialize();
}

FString UStageAcquireDispatchSubsystem::GetToolOutboxDirectory()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("StageCraft"), TEXT("AITools"), TEXT("Outbox")));
}

FStageDispatchContext UStageAcquireDispatchSubsystem::MakeContext(const FStageMarketplaceListing& Listing) const
{
	const UGameInstance* GameInstance = GetGameInstance();
	FStageDispatchContext Context;
	Context.bListingAllowsExport = Listing.bAllowsExport;
	if (const UStageBridgeSubsystem* Bridge = GameInstance->GetSubsystem<UStageBridgeSubsystem>())
	{
		Context.bBridgeRunning = Bridge->IsRunning();
		Context.bBridgeSending = Bridge->IsSending();
		Context.Clients = Bridge->GetClients();
	}
	if (const UStageAIToolSubsystem* Tools = GameInstance->GetSubsystem<UStageAIToolSubsystem>())
	{
		Context.bToolBusy = Tools->IsBusy();
		Context.Tools = Tools->GetTools();
	}
	return Context;
}

EStageDispatchAvailability UStageAcquireDispatchSubsystem::EvaluateTarget(const FGuid& ListingId, const FStageDispatchTarget& Target) const
{
	const UStageMarketplaceSubsystem* Marketplace = GetGameInstance()->GetSubsystem<UStageMarketplaceSubsystem>();
	const FStageMarketplaceListing* Listing = Marketplace ? Marketplace->FindListing(ListingId) : nullptr;
	return Listing ? StageDispatchRules::EvaluateTarget(Target, MakeContext(*Listing)) : EStageDispatchAvailability::Unavailable;
}

FStageOrderResultInfo UStageAcquireDispatchSubsystem::RequestOrder(const FStageAcquireOrder& Order)
{
	FStageOrderResultInfo Refusal;
	Refusal.ListingId = Order.ListingId;
	Refusal.Target = Order.Target;
	const auto Refuse = [&Refusal](EStageOrderResult Code, const FText& Message, EStageDispatchAvailability Availability = EStageDispatchAvailability::Available)
	{
		Refusal.Code = Code;
		Refusal.Message = Message;
		Refusal.Availability = Availability;
		UE_LOG(LogStageCraft, Log, TEXT("Dispatch: order for %s refused (%s): %s"), *Refusal.ListingId.ToString(),
			*StaticEnum<EStageOrderResult>()->GetNameStringByValue(static_cast<int64>(Code)), *Message.ToString());
		return Refusal;
	};

	if (IsOrderPending())
	{
		return Refuse(EStageOrderResult::Busy, LOCTEXT("Busy", "Another order is still running; wait for it to finish."));
	}
	UStageMarketplaceSubsystem* Marketplace = GetGameInstance()->GetSubsystem<UStageMarketplaceSubsystem>();
	const FStageMarketplaceListing* Listing = Marketplace ? Marketplace->FindListing(Order.ListingId) : nullptr;
	if (!Listing)
	{
		return Refuse(EStageOrderResult::NotFound, LOCTEXT("NotFound", "That listing is no longer in the results; search again."));
	}
	// The user confirmed a price; a different one now (a newer query, a changed listing) needs a new confirmation.
	if (Listing->Price.Credits != Order.ExpectedCredits)
	{
		return Refuse(EStageOrderResult::PriceChanged, FText::Format(LOCTEXT("PriceChanged", "The price of \"{0}\" changed to {1} credits. Check it and confirm again."),
			FText::FromString(Listing->Title), FText::AsNumber(Listing->Price.Credits)));
	}
	if (!StageDispatchRules::HasValidPath(Order.Target))
	{
		return Refuse(EStageOrderResult::InvalidRequest, LOCTEXT("NoPath", "Choose where to save the .glb file."));
	}
	const EStageDispatchAvailability Availability = StageDispatchRules::EvaluateTarget(Order.Target, MakeContext(*Listing));
	if (Availability != EStageDispatchAvailability::Available)
	{
		return Refuse(EStageOrderResult::TargetUnavailable, StageDispatchRules::Describe(Availability), Availability);
	}

	Pending = Order;
	PendingListing = *Listing;
	AcquiredScene.Reset();
	// Bound before the request: an acquire can fail or finish inside RequestAcquire.
	Marketplace->OnAcquireCompleted.AddUniqueDynamic(this, &ThisClass::HandleAcquireCompleted);
	SetStage(EStageOrderStage::Acquiring);

	const FStageMarketplaceResultInfo Started = Marketplace->RequestAcquire(Order.ListingId);
	if (!Started.IsSuccess())
	{
		// Refused before anything was bought: back to idle, reported to the caller only.
		UnbindAll();
		SetStage(EStageOrderStage::Idle);
		Pending = FStageAcquireOrder();
		return Refuse(EStageOrderResult::AcquireFailed, Started.Message);
	}

	UE_LOG(LogStageCraft, Log, TEXT("Dispatch: order for \"%s\" (%lld credits) to %s started."), *PendingListing.Title, Order.ExpectedCredits, *DescribeTarget().ToString());
	FStageOrderResultInfo Accepted;
	Accepted.ListingId = Order.ListingId;
	Accepted.Target = Order.Target;
	return Accepted;
}

void UStageAcquireDispatchSubsystem::SetStage(EStageOrderStage NewStage)
{
	Stage = NewStage;
	OnOrderProgress.Broadcast(Pending.ListingId, Stage);
}

void UStageAcquireDispatchSubsystem::HandleAcquireCompleted(FGuid ListingId, FStageMarketplaceResultInfo Result, const FString& SceneName)
{
	if (Stage != EStageOrderStage::Acquiring || ListingId != Pending.ListingId)
	{
		return;
	}
	if (UStageMarketplaceSubsystem* Marketplace = GetGameInstance()->GetSubsystem<UStageMarketplaceSubsystem>())
	{
		Marketplace->OnAcquireCompleted.RemoveDynamic(this, &ThisClass::HandleAcquireCompleted);
	}
	if (!Result.IsSuccess() || SceneName.IsEmpty())
	{
		Finish(EStageOrderResult::AcquireFailed, Result.Message);
		return;
	}
	AcquiredScene = SceneName;
	Deliver();
}

void UStageAcquireDispatchSubsystem::Deliver()
{
	if (!Pending.Target.LeavesApp())
	{
		Finish(EStageOrderResult::Success, FText::Format(LOCTEXT("Added", "\"{0}\" is in your scenes."), FText::FromString(AcquiredScene)));
		return;
	}

	// Things change while buying and downloading (an app disconnects, a tool starts another job): check again.
	const EStageDispatchAvailability Availability = StageDispatchRules::EvaluateTarget(Pending.Target, MakeContext(PendingListing));
	if (Availability != EStageDispatchAvailability::Available)
	{
		Finish(EStageOrderResult::DeliveryFailed, DescribeDeliveryFailure(StageDispatchRules::Describe(Availability)), Availability);
		return;
	}

	UGameInstance* GameInstance = GetGameInstance();
	UStageBridgeSubsystem* Bridge = GameInstance->GetSubsystem<UStageBridgeSubsystem>();
	UStageAIToolSubsystem* Tools = GameInstance->GetSubsystem<UStageAIToolSubsystem>();
	if (!Bridge || !Tools)
	{
		Finish(EStageOrderResult::DeliveryFailed, DescribeDeliveryFailure(StageDispatchRules::Describe(EStageDispatchAvailability::Unavailable)));
		return;
	}
	SetStage(EStageOrderStage::Delivering);

	switch (Pending.Target.Kind)
	{
	case EStageDispatchKind::SaveFile:
	{
		const FStageBridgeResultInfo Exported = Bridge->ExportSceneToFile(AcquiredScene, Pending.Target.FilePath);
		if (Exported.IsSuccess())
		{
			Finish(EStageOrderResult::Success, FText::Format(LOCTEXT("Saved", "\"{0}\" is in your scenes and saved to {1}."),
				FText::FromString(AcquiredScene), FText::FromString(FPaths::GetCleanFilename(Pending.Target.FilePath))));
		}
		else
		{
			Finish(EStageOrderResult::DeliveryFailed, DescribeDeliveryFailure(Exported.Message));
		}
		return;
	}

	case EStageDispatchKind::BridgeApp:
	{
		Bridge->OnSendCompleted.AddUniqueDynamic(this, &ThisClass::HandleSendCompleted);
		const FStageBridgeResultInfo Sent = Bridge->SendSceneToApp(AcquiredScene, Pending.Target.BridgeClientId);
		if (!Sent.IsSuccess())
		{
			Finish(EStageOrderResult::DeliveryFailed, DescribeDeliveryFailure(Sent.Message));
		}
		return;
	}

	case EStageDispatchKind::AITool:
	{
		IFileManager::Get().MakeDirectory(*GetToolOutboxDirectory(), /*Tree*/ true);
		ToolModelPath = FPaths::Combine(GetToolOutboxDirectory(), StageBridgeProtocol::MakeAssetId() + TEXT(".glb"));
		const FStageBridgeResultInfo Exported = Bridge->ExportSceneToFile(AcquiredScene, ToolModelPath);
		if (!Exported.IsSuccess())
		{
			Finish(EStageOrderResult::DeliveryFailed, DescribeDeliveryFailure(Exported.Message));
			return;
		}
		FStageAIToolJob Job;
		Job.ModelPath = ToolModelPath;
		Job.Name = AcquiredScene;
		// Bound before the call: a tool may answer synchronously.
		Tools->OnJobCompleted.AddUniqueDynamic(this, &ThisClass::HandleToolJobCompleted);
		const FStageAIToolResultInfo Submitted = Tools->SubmitModel(Pending.Target.ProviderId, Job);
		if (!Submitted.IsSuccess() && Stage == EStageOrderStage::Delivering)
		{
			Finish(EStageOrderResult::DeliveryFailed, DescribeDeliveryFailure(Submitted.Message));
		}
		return;
	}

	default:
		Finish(EStageOrderResult::DeliveryFailed, DescribeDeliveryFailure(StageDispatchRules::Describe(EStageDispatchAvailability::Unavailable)));
		return;
	}
}

void UStageAcquireDispatchSubsystem::HandleSendCompleted(const FStageBridgeResultInfo& Result)
{
	// One send at a time in the bridge; this is ours if it is the scene and app the order sent to.
	if (Stage != EStageOrderStage::Delivering || Pending.Target.Kind != EStageDispatchKind::BridgeApp
		|| Result.SceneName != AcquiredScene || Result.ClientId != Pending.Target.BridgeClientId)
	{
		return;
	}
	if (Result.IsSuccess())
	{
		Finish(EStageOrderResult::Success, FText::Format(LOCTEXT("Sent", "\"{0}\" is in your scenes and was sent to {1}."), FText::FromString(AcquiredScene), DescribeTarget()));
	}
	else
	{
		Finish(EStageOrderResult::DeliveryFailed, DescribeDeliveryFailure(Result.Message));
	}
}

void UStageAcquireDispatchSubsystem::HandleToolJobCompleted(const FStageAIToolResultInfo& Result)
{
	if (Stage != EStageOrderStage::Delivering || Pending.Target.Kind != EStageDispatchKind::AITool || Result.ProviderId != Pending.Target.ProviderId)
	{
		return;
	}
	if (Result.IsSuccess())
	{
		Finish(EStageOrderResult::Success, FText::Format(LOCTEXT("Tool", "\"{0}\" is in your scenes and was handed to {1}."), FText::FromString(AcquiredScene), DescribeTarget()));
	}
	else
	{
		Finish(EStageOrderResult::DeliveryFailed, DescribeDeliveryFailure(Result.Message));
	}
}

FText UStageAcquireDispatchSubsystem::DescribeTarget() const
{
	switch (Pending.Target.Kind)
	{
	case EStageDispatchKind::SaveFile:
		return FText::FromString(FPaths::GetCleanFilename(Pending.Target.FilePath));
	case EStageDispatchKind::BridgeApp:
		if (const UStageBridgeSubsystem* Bridge = GetGameInstance()->GetSubsystem<UStageBridgeSubsystem>())
		{
			for (const FStageBridgeClientInfo& Client : Bridge->GetClients())
			{
				if (Client.ClientId == Pending.Target.BridgeClientId)
				{
					return StageBridgeProtocol::GetAppDisplayName(Client.App);
				}
			}
		}
		return LOCTEXT("TheApp", "the 3D app");
	case EStageDispatchKind::AITool:
		if (const UStageAIToolSubsystem* Tools = GetGameInstance()->GetSubsystem<UStageAIToolSubsystem>())
		{
			FStageAIToolInfo Info;
			if (Tools->FindTool(Pending.Target.ProviderId, Info))
			{
				return Info.DisplayName;
			}
		}
		return LOCTEXT("TheTool", "the AI tool");
	case EStageDispatchKind::OpenInStageCraft:
	default:
		return LOCTEXT("StageCraft", "StageCraft");
	}
}

FText UStageAcquireDispatchSubsystem::DescribeDeliveryFailure(const FText& Reason) const
{
	return FText::Format(LOCTEXT("DeliveryFailed", "\"{0}\" is in your scenes, but it could not go to {1}: {2} You can send it later with File > Send to 3D App."),
		FText::FromString(AcquiredScene), DescribeTarget(), Reason);
}

void UStageAcquireDispatchSubsystem::Finish(EStageOrderResult Code, const FText& Message, EStageDispatchAvailability Availability)
{
	UnbindAll();
	if (!ToolModelPath.IsEmpty())
	{
		IFileManager::Get().Delete(*ToolModelPath, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
		ToolModelPath.Reset();
	}

	FStageOrderResultInfo Result;
	Result.Code = Code;
	Result.Message = Message;
	Result.ListingId = Pending.ListingId;
	Result.Target = Pending.Target;
	Result.SceneName = AcquiredScene;
	Result.Availability = Availability;
	Stage = EStageOrderStage::Idle;
	Pending = FStageAcquireOrder();
	AcquiredScene.Reset();

	UE_LOG(LogStageCraft, Log, TEXT("Dispatch: order for %s ended (%s): %s"), *Result.ListingId.ToString(),
		*StaticEnum<EStageOrderResult>()->GetNameStringByValue(static_cast<int64>(Code)), *Message.ToString());
	OnOrderProgress.Broadcast(Result.ListingId, Stage);
	OnOrderCompleted.Broadcast(Result);
}

void UStageAcquireDispatchSubsystem::UnbindAll()
{
	const UGameInstance* GameInstance = GetGameInstance();
	if (!GameInstance)
	{
		return;
	}
	if (UStageMarketplaceSubsystem* Marketplace = GameInstance->GetSubsystem<UStageMarketplaceSubsystem>())
	{
		Marketplace->OnAcquireCompleted.RemoveDynamic(this, &ThisClass::HandleAcquireCompleted);
	}
	if (UStageBridgeSubsystem* Bridge = GameInstance->GetSubsystem<UStageBridgeSubsystem>())
	{
		Bridge->OnSendCompleted.RemoveDynamic(this, &ThisClass::HandleSendCompleted);
	}
	if (UStageAIToolSubsystem* Tools = GameInstance->GetSubsystem<UStageAIToolSubsystem>())
	{
		Tools->OnJobCompleted.RemoveDynamic(this, &ThisClass::HandleToolJobCompleted);
	}
}

#undef LOCTEXT_NAMESPACE
