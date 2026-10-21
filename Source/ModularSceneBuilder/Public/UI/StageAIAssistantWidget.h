// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "StageAIAssistantWidget.generated.h"

/**
 * The AI Assistant window (StageCraft.Panel.Assistant, Docs/ADR/0016-ai-assistant.md §2.6): the conversation as user and assistant
 * lines, quick-prompt chips, a text field with SEND (Enter sends), CLEAR, and a footer naming the interpreter in use. Opens as a
 * floating pop-up window from Edit › Assistant › AI Assistant..., the Window menu and the viewport toolbar's ASSIST button.
 *
 * A view in the project's binding pattern:
 *  - The conversation is read from UStageAICommandSubsystem. A prompt goes only through AModularPlayerController::
 *    RequestAssistantPrompt, so every action the assistant takes obeys the stage's rules and undo history.
 *  - It reads back on OnConversationChanged; the list rebuilds on the next tick, never inside the event that changed it.
 *  - Bound in NativeConstruct, unbound in NativeDestruct. No tick (DisableNativeTick).
 *
 * The widget tree is built in code from the theme; there is no Widget Blueprint.
 */
UCLASS(meta = (DisableNativeTick))
class MODULARSCENEBUILDER_API UStageAIAssistantWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Sends Prompt as if typed (quick-prompt chips, development commands). False when the controller refused it. */
	bool SendPrompt(const FString& Prompt);

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Assistant")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	void BuildTree();
	class UWidget* BuildHeader();
	class UWidget* BuildChips();
	class UWidget* BuildInputRow();
	class UWidget* BuildMessage(const struct FStageAIMessage& Message) const;
	class UStageChoiceButton* MakeButton(const FText& Label, const FText& Tip, int32 Index, void (UStageAIAssistantWidget::*Handler)(int32), bool bPrimary);
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color) const;
	const class UStageCraftUITheme& GetTheme() const;
	void StyleButton(class UStageChoiceButton& Button, bool bPrimary, bool bEnabled) const;

	/** Coalesces conversation changes into one rebuild on the next tick. */
	void RequestRebuild();
	void Rebuild();

	void HandleChipPicked(int32 Index);
	void HandleSendPicked(int32 Unused);
	void HandleClearPicked(int32 Unused);

	UFUNCTION()
	void HandleTextCommitted(const FText& Text, ETextCommit::Type CommitMethod);

	UFUNCTION()
	void HandleConversationChanged();

	class UStageAICommandSubsystem* GetAssistant() const { return BoundAssistant.Get(); }

	UPROPERTY(Transient)
	TObjectPtr<class UScrollBox> MessageScroll = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UVerticalBox> MessageList = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UEditableTextBox> PromptField = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageChoiceButton> SendButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageChoiceButton> ClearButton = nullptr;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> ChipButtons;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> FooterText = nullptr;

	TWeakObjectPtr<class UStageAICommandSubsystem> BoundAssistant;
	bool bRebuildQueued = false;
};
