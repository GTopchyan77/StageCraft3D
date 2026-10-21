// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AI/StageAITypes.h"
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
 *  - Actions are never run here: SubmitPrompt takes the executor of the player who asked, and the controller runs the command
 *    through the same requests (GameMode rules, undo history) as its buttons. Every command is validated first
 *    (StageAICommandSchema::Validate), whichever interpreter produced it.
 *  - One prompt in flight at a time (IsBusy); a second is refused, not queued. The online request and any asset load are
 *    cancelled in Deinitialize.
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
	 * nothing added, for an empty prompt or while busy.
	 */
	bool SubmitPrompt(const FString& Prompt, FStageAICommandExecutor Executor);

	/** Ends a Pending action with its result line. Ignored unless an action is pending. */
	void FinishPendingAction(const FStageAIActionOutcome& Outcome);

	/**
	 * Streams Item's Game bundle for a pending action; OnReady runs once it is in memory, possibly before this returns. False
	 * unless an action is in progress (called from the executor) and no load is already running.
	 */
	bool RequestItemAssets(const class UBaseItemData& Item, FSimpleDelegate OnReady);

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
	void AddMessage(EStageAISpeaker Speaker, EStageAIOutcome Outcome, const FText& Text);
	void SetState(EStageAIAssistantState NewState);

	/** The online model's answer, or a fallback to the local parser when it failed. */
	void HandleBackendFinished(const struct FStageAIModelResponse& Response, FString Prompt);

	FStageAIInterpretation InterpretLocally(const FString& Prompt) const;
	void Respond(const FStageAIInterpretation& Interpretation);
	void HandleItemAssetsLoaded();

	/** Note appended to the next answer ("answered offline because ..."), then cleared. */
	static FText WithNote(const FText& Text, const FText& Note);

	TArray<FStageAIMessage> Messages;
	EStageAIAssistantState State = EStageAIAssistantState::Idle;

	/** The executor of the prompt being answered; unbound when idle. */
	FStageAICommandExecutor PendingExecutor;

	/** Null when the online model is off in the project settings. */
	UPROPERTY(Transient)
	TObjectPtr<class UStageAIAssistantBackend> Backend = nullptr;

	TSharedPtr<struct FStreamableHandle> LoadHandle;
	FSimpleDelegate PendingAssetsCallback;
};
