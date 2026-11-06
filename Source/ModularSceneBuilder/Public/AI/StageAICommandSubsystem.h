// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AI/StageAITypes.h"
#include "History/StageEditCommand.h"
#include "Subsystems/WorldSubsystem.h"
#include "StageAICommandSubsystem.generated.h"

/** A line was added to the conversation, it was cleared, or the assistant started or stopped working. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageAIConversationChanged);

/**
 * Runs one validated action command (Docs/ADR/0016-ai-assistant.md §2.2). Supplied by AModularPlayerController::RequestAssistantPrompt,
 * which runs it through its own request API; the subsystem itself never changes the stage. A Pending outcome promises a later
 * FinishPendingAction.
 */
DECLARE_DELEGATE_RetVal_OneParam(FStageAIActionOutcome, FStageAICommandExecutor, const FStageAICommand& /*Command*/);

/** Opens one undo step for a plan, named Description (AModularPlayerController::BeginAssistantPlan). False when it cannot. */
DECLARE_DELEGATE_RetVal_OneParam(bool, FStageAIPlanBegin, const FText& /*Description*/);

/**
 * Closes the plan's undo step: bKeep records it; otherwise everything it recorded is rolled back (AModularPlayerController::
 * EndAssistantPlan). Returns the rollback's result (Succeeded when kept or when there was nothing to roll back).
 */
DECLARE_DELEGATE_RetVal_OneParam(EStageCommandResult, FStageAIPlanEnd, bool /*bKeep*/);

/**
 * The asking player's side of an assistant prompt (Docs/ADR/0030 §6), supplied by AModularPlayerController: how a validated
 * command runs, how a plan becomes one undo step, and whether that player wants every change proposed first. Without
 * BeginPlan / EndPlan (tests, tools) a plan's steps still run in order, but are neither grouped nor rolled back.
 */
struct FStageAIActionHost
{
	FStageAICommandExecutor Execute;
	FStageAIPlanBegin BeginPlan;
	FStageAIPlanEnd EndPlan;
	/** The player's "ask before every change" preference: then even a local one-step plan waits for APPLY. */
	bool bAskBeforeChanges = false;
};

/** A marketplace answer an action waits for (Docs/ADR/0021 §13.4). */
enum class EStageAIMarketplaceEvent : uint8
{
	/** The next query answer, whoever asked. */
	Query,
	/** The next preview answer, whichever listing (only the newest preview is ever answered). */
	Preview,
	/** The acquisition of the awaited listing. */
	Acquire,
};

/** The awaited marketplace answer: the listing it is about (Preview, Acquire), the result, and the imported scene's name (Acquire). */
DECLARE_DELEGATE_ThreeParams(FOnStageAIMarketplaceAnswer, const FGuid& /*ListingId*/, const FStageMarketplaceResultInfo& /*Result*/, const FString& /*SceneName*/);

/** What the assistant is doing. One prompt at a time. */
UENUM(BlueprintType)
enum class EStageAIAssistantState : uint8
{
	Idle,
	/** Waiting for the online model. */
	Interpreting,
	/** An action started and has not finished yet (fixtures waiting for their assets). */
	Acting,
};

/**
 * The AI assistant of this stage session (Docs/ADR/0016-ai-assistant.md): the source of truth for the conversation, and the
 * interpreter that turns a prompt into a command (the local StageAICommandParser, or the optional online model) and then into a
 * reply.
 *
 * Ownership and writes:
 *  - The conversation is tool state: lines are added only here, capped at MaxMessages, and dropped with the world (not saved).
 *  - Questions are answered read-only from the session, show control and environment subsystems (CaptureScene).
 *  - Actions are never run here: SubmitPrompt takes the host of the player who asked, and the controller runs the command
 *    through the same requests (GameMode rules, undo history) as its buttons. Every command is validated first
 *    (StageAICommandSchema::Validate), whichever interpreter produced it.
 *  - Plans (Docs/ADR/0030 §6): the actions of one answer form an FStageAIPlan, checked against StageAIToolCatalogue. A plan from
 *    the online model, a plan of several steps, or any plan when the player asks to be asked, is shown as a proposal card and
 *    waits for APPLY (ApplyProposal, or a typed yes) unless every step may apply without asking; CANCEL, a typed no, or any other
 *    prompt drops it unchanged. Applying runs the steps in order inside ONE history group ("Assistant: ..."), so Ctrl+Z reverts
 *    the whole plan; the first refused step stops it and the steps before it are rolled back.
 *  - One prompt in flight at a time (IsBusy); a second is refused, not queued. The online request and any asset load are
 *    cancelled in Deinitialize.
 *  - Marketplace (Docs/ADR/0021 §13): listing references are resolved here, against the marketplace's current results. A paid
 *    listing is bought only after the next prompt says yes, read locally (ParseConfirmation), never by the online model.
 *
 * Game and PIE worlds only. Never ticks and never iterates the world.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageAICommandSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	/** Oldest lines are dropped beyond this. */
	static constexpr int32 MaxMessages = 200;

	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	const TArray<FStageAIMessage>& GetMessages() const { return Messages; }

	EStageAIAssistantState GetState() const { return State; }
	bool IsBusy() const { return State != EStageAIAssistantState::Idle; }

	/**
	 * Adds Prompt to the conversation and answers it: interpreted, validated, then answered (questions), explained (help,
	 * unsupported) or handed to Executor (actions). Called by AModularPlayerController::RequestAssistantPrompt. False, and
	 * nothing added, for an empty prompt or while busy. SelectedItemNames are the asking player's
	 * selected items (catalog names), the context for "the selected ..." requests and the online model.
	 */
	bool SubmitPrompt(const FString& Prompt, FStageAIActionHost Host, TArray<FString> SelectedItemNames = TArray<FString>());

	/** SubmitPrompt with an executor only: plans run ungrouped (tests, tools). */
	bool SubmitPrompt(const FString& Prompt, FStageAICommandExecutor Executor, TArray<FString> SelectedItemNames = TArray<FString>());

	/**
	 * Ends a Pending action with its result line. Inside a plan it is that step's result: the plan goes on with the next step, or
	 * stops and rolls back on a refusal. Ignored unless an action is pending.
	 */
	void FinishPendingAction(const FStageAIActionOutcome& Outcome);

	/**
	 * APPLY on the open proposal card (Docs/ADR/0030 §6): runs its plan for Host's player as one undo step. False, and nothing
	 * runs, when there is no open proposal or a prompt is being answered. Called by AModularPlayerController::RequestAssistantProposal.
	 */
	bool ApplyProposal(FStageAIActionHost Host);

	/** CANCEL on the open proposal card: drops it; nothing changed. False when there is none (or while busy). */
	bool CancelProposal();

	/** The id of the proposal APPLY and CANCEL act on (FStageAIMessage::ProposalId), or 0 for none. */
	int32 GetOpenProposalId() const { return Proposal.IsSet() ? Proposal->Id : 0; }

	/** The open proposal's plan, or null. */
	const FStageAIPlan* GetOpenProposal() const { return Proposal.IsSet() ? &Proposal->Plan : nullptr; }

#if !UE_BUILD_SHIPPING
	/**
	 * Development and tests only: handles Plan for Host's player as an answer of the online model (validated step by step, then
	 * proposed or refused like one). The local grammar reads one change per prompt (several are Phase H4), so scripted runs and
	 * tests build multi-step plans here. False while busy.
	 */
	bool SubmitPlanForTesting(FStageAIPlan Plan, FStageAIActionHost Host);

	/** Development and tests only: prompts go to InBackend (a scripted mock) instead of the configured one; null for none. */
	void SetBackendForTesting(class UStageAIAssistantBackend* InBackend);
#endif

	/**
	 * Streams the Game bundles of Items (null entries skipped) for a pending action; OnReady runs once it is in memory, possibly before this returns. False
	 * unless an action is in progress (called from the executor) and no load is already running.
	 */
	bool RequestItemAssets(TConstArrayView<const class UBaseItemData*> Items, FSimpleDelegate OnReady);
	bool RequestItemAssets(const class UBaseItemData& Item, FSimpleDelegate OnReady);

	/**
	 * Waits for the marketplace's next Event answer for a pending action and hands it to OnAnswer, once (Docs/ADR/0021 §13.4). Call it
	 * before starting the request: a refusal can be broadcast inside the request call. False unless an action is in progress and no
	 * other wait is running. The marketplace delegates are bound only while waiting.
	 */
	bool AwaitMarketplace(EStageAIMarketplaceEvent Event, const FGuid& ListingId, FOnStageAIMarketplaceAnswer OnAnswer);

	/** Ends a wait without an answer (the request was refused before it started). */
	void CancelMarketplaceWait();

	/** The paid listing the last answer offered to buy, if the next prompt has not answered it yet. */
	const TOptional<FStageAIPurchaseOffer>& GetPurchaseOffer() const { return PurchaseOffer; }

	/** Empties the conversation. Ignored while busy. */
	void ClearConversation();

	/** Which interpreter answers: "Local commands" or the online model, with why it is unavailable when it is. */
	FText GetInterpreterDescription() const;

	/** The stage as the assistant sees it: totals, limits, item and fixture names, cues, environment. */
	FStageAISceneView CaptureScene() const;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Assistant")
	FOnStageAIConversationChanged OnConversationChanged;

protected:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

private:
	void AddMessage(EStageAISpeaker Speaker, EStageAIOutcome Outcome, const FText& Text, const FStageAIReference& Reference = FStageAIReference(),
		int32 ProposalId = 0);

	/** The validated actions of an answer as a plan: proposed, run at once, or refused (StageAIToolCatalogue::CheckPlan). */
	void HandlePlan(FStageAIPlan Plan, const FText& Note);

	/** Whether Plan waits for APPLY for the asking player (Docs/ADR/0030 §6). */
	bool NeedsApproval(const FStageAIPlan& Plan) const;

	/** Shows Plan as a proposal card; the assistant is idle again until APPLY, CANCEL or the next prompt. */
	void ProposePlan(FStageAIPlan Plan, const FText& Note);

	/** Runs Plan for PendingHost's player: one history group, steps in order (AdvancePlan). */
	void RunPlan(FStageAIPlan Plan, const FText& Note);

	/** Runs the plan's next steps until it ends or a step is Pending. */
	void AdvancePlan();

	/** One step's result: the next step, or the end of the plan (FinishPlan) on a refusal. */
	void HandleStepOutcome(const FStageAIActionOutcome& Outcome);

	/** Ends the running plan: keeps its group, or rolls it back when Refusal is set (the refused step's outcome), and reports it. */
	void FinishPlan(TOptional<FStageAIActionOutcome> Refusal);

	/** A "yes" or "no" to the open proposal, read locally; false when Prompt is neither (the proposal is dropped). */
	bool AnswerProposal(const FString& Prompt, FStageAIActionHost& Host, TArray<FString>& SelectedItemNames);

	/**
	 * The reply to Prompt from the offline knowledge (UStageKnowledgeSubsystem, Docs/ADR/0030 §3), with what it points at; false
	 * when the knowledge has no confident answer. Commands are described with the asking player's current key bindings.
	 */
	bool AnswerFromKnowledge(const FString& Prompt, FStageAIInterpretation& OutReply) const;

	/**
	 * ExplainFixture and ListProblems, answered from the live stage by UStageShowDiagnosticsSubsystem (Docs/ADR/0030 §5).
	 * OutReference points SHOW ME at where the cause is fixed (the Problems panel for a list). Read-only.
	 */
	FText AnswerDiagnostics(const FStageAICommand& Command, FStageAIReference& OutReference) const;
	void SetState(EStageAIAssistantState NewState);

	/** The online model's answer, or a fallback to the local parser when it failed. */
	void HandleBackendFinished(const struct FStageAIModelResponse& Response, FString Prompt);

	FStageAIInterpretation InterpretLocally(const FString& Prompt) const;
	void Respond(const FStageAIInterpretation& Interpretation);
	/** Runs a validated action through the asking player's executor and reports it; keeps a purchase offer it returns. */
	void RunAction(const FStageAICommand& Command, const FText& Note);

	/** A "yes" or "no" to PurchaseOffer, read locally (Docs/ADR/0021 §13.3); false when Prompt is neither (the offer is dropped). */
	bool AnswerPurchaseOffer(const FString& Prompt, FStageAIActionHost& Host, TArray<FString>& SelectedItemNames);

	/** Fills Command.Listing.Id from its reference and the marketplace's current results; false with the reason when none matches. */
	bool ResolveListing(FStageAICommand& Command, FText& OutError);

	class UStageMarketplaceSubsystem* GetMarketplace() const;
	void AnswerMarketplace(EStageAIMarketplaceEvent Event, const FGuid& ListingId, const FStageMarketplaceResultInfo& Result, const FString& SceneName);
	void StopMarketplaceWait();

	UFUNCTION()
	void HandleMarketplaceListings(FStageMarketplaceResultInfo Result);

	UFUNCTION()
	void HandleMarketplaceAcquired(FGuid ListingId, FStageMarketplaceResultInfo Result, const FString& SceneName);
	void HandleItemAssetsLoaded();

	/** Note appended to the next answer ("answered offline because ..."), then cleared. */
	static FText WithNote(const FText& Text, const FText& Note);

	/** "answered offline: <reason>" in the prompt's language. */
	static FText DescribeOffline(const FText& Reason, EStageAILanguage Language);

	TArray<FStageAIMessage> Messages;
	EStageAIAssistantState State = EStageAIAssistantState::Idle;

	/** The host of the prompt being answered or the plan being applied; reset when idle. */
	FStageAIActionHost PendingHost;

	/** The plan a proposal card offers, and the id its line carries. One at a time; the next prompt drops it. */
	struct FProposal
	{
		int32 Id = 0;
		FStageAIPlan Plan;
	};
	TOptional<FProposal> Proposal;
	int32 LastProposalId = 0;

	/** The plan being applied: its steps, the next one, each finished step's line, and whether it owns a history group. */
	struct FRunningPlan
	{
		FStageAIPlan Plan;
		int32 NextStep = 0;
		TArray<FText> StepLines;
		bool bGrouped = false;
		FText Note;
		/** A step answered Pending inside its own Execute call (FinishPendingAction re-entered): its outcome, read after Execute returns. */
		TOptional<FStageAIActionOutcome> EarlyOutcome;
		bool bExecuting = false;
	};
	TOptional<FRunningPlan> RunningPlan;

	/** The asking player's selection while a prompt is answered (SubmitPrompt); emptied when idle. */
	TArray<FString> PromptSelection;

	/** Null when the online model is off in the project settings. */
	UPROPERTY(Transient)
	TObjectPtr<class UStageAIAssistantBackend> Backend = nullptr;

	TSharedPtr<struct FStreamableHandle> LoadHandle;
	FSimpleDelegate PendingAssetsCallback;

	/** The paid listing offered for a "yes" in the next prompt; dropped by any other prompt. */
	TOptional<FStageAIPurchaseOffer> PurchaseOffer;

	/** The listing the conversation last talked about ("buy it"). */
	FGuid LastListing;

	/** The marketplace answer a pending action waits for, and the marketplace bound for it. */
	struct FMarketplaceWait
	{
		EStageAIMarketplaceEvent Event = EStageAIMarketplaceEvent::Query;
		FGuid ListingId;
		FOnStageAIMarketplaceAnswer OnAnswer;
	};
	TOptional<FMarketplaceWait> MarketplaceWait;
	TWeakObjectPtr<class UStageMarketplaceSubsystem> WaitingMarketplace;
	FDelegateHandle PreviewReadyHandle;
};
