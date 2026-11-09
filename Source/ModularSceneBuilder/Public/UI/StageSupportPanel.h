// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "StageSupportPanel.generated.h"

/**
 * The Support panel (StageCraft.Panel.Support, Docs/ADR/0032 §7): a bug report or support request with the user's own words, and the
 * log and a window screenshot only when the user ticks them. Before sending, the panel shows exactly what goes (redacted) and where:
 * StageCraft Cloud when it is online, else a folder on this computer.
 *
 * A view: the report is built and sent by UStageSupportSubsystem (validation, redaction, the cloud, the offline copy); the panel holds
 * only the form. Writes nothing to the stage. No tick.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageSupportPanel : public UUserWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Support")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	enum class EAction : int32 { IncludeLog, IncludeScreenshot, Send, OpenFolder, New, Count };

	void BuildTree();
	const class UStageCraftUITheme& GetTheme() const;
	class UStageSupportSubsystem* GetSupport() const;
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color);
	class UEditableTextBox* MakeField(const FText& Hint);
	class UMultiLineEditableTextBox* MakeMultiLine(const FText& Hint);
	class UStageChoiceButton* MakeButton(const FText& Label, const FText& Tip, int32 Index, bool bAccent);
	class UWidget* Labeled(const FText& Label, class UWidget* Field, float Height = 0.f);
	void StyleToggle(EAction Action, bool bOn);

	void Refresh();
	void HandleAction(int32 Index);
	void HandleSubmitted(const struct FStageSupportSubmitResult& Result);
	void HandleScreenshot(uint32 Serial, const struct FStageSupportScreenshot& Screenshot);
	void ResetForm();

	UPROPERTY(Transient)
	TObjectPtr<class UStageThemedComboBox> CategoryCombo = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UEditableTextBox> TitleField = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UMultiLineEditableTextBox> DescriptionField = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UMultiLineEditableTextBox> StepsField = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UEditableTextBox> ExpectedField = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UEditableTextBox> ActualField = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UEditableTextBox> EmailField = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> AttachText = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> StatusText = nullptr;
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> ActionButtons;

	bool bIncludeLog = false;
	bool bIncludeScreenshot = false;
	/** Read when the box is ticked, so the user sees how much goes; sent as it was then. */
	FString LogTail;
	TArray<uint8> ScreenshotPng;
	/** The capture being encoded; an answer for an older one (unticked or reset since) is dropped. 0 when none is pending. */
	uint32 PendingScreenshot = 0;
	uint32 ScreenshotSerial = 0;
	FString SavedFolder;
};
