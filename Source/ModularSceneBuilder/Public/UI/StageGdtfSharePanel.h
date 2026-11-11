// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Engine/TimerHandle.h"
#include "Fixtures/StageGdtfShareTypes.h"
#include "Types/SlateEnums.h"
#include "StageGdtfSharePanel.generated.h"

/**
 * The GDTF Share panel (StageCraft.Panel.GdtfShare, Docs/ADR/0033 §18): sign in with the user's own GDTF Share account, search the
 * fixture list, GET a file into the fixture import review, and follow the downloads.
 *
 * A view in the project's binding pattern:
 *  - Reads UStageGdtfShareSubsystem (state, list, downloads) and the fixture library (which listed types are already in the Library).
 *  - Writes only through the owning AModularPlayerController's GDTF Share requests; refusals arrive on OnRequestRejected like any
 *    request. The password field is cleared as soon as the sign-in is sent, so the panel never keeps it.
 *  - Push, never poll: follows OnChanged, OnRequestFinished and the library's OnLibraryChanged; every change rebuilds the result and
 *    download rows once, on the next tick. At most MaxShownResults rows are built; the search narrows the rest.
 *  - Shows "Powered by GDTF" and each file's uploader, as the GDTF Share API terms require.
 *
 * Built in code from the theme (a Blueprint subclass may restyle it). No tick.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageGdtfSharePanel : public UUserWidget
{
	GENERATED_BODY()

public:
	static constexpr int32 MaxShownResults = 100;

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|GDTF Share")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	enum class EAction : int32 { SignIn, SignOut, Refresh, FilterAll, FilterManufacturer, FilterUser, ClearFinished, Count };

	void BuildTree();
	const class UStageCraftUITheme& GetTheme() const;
	class UStageGdtfShareSubsystem* GetShare() const;
	class UStageFixtureLibrarySubsystem* GetLibrary() const;
	class AModularPlayerController* GetStageController() const;
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color, bool bWrap = false);
	class UEditableTextBox* MakeField(const FText& Hint);
	class UStageChoiceButton* MakeButton(const FText& Label, const FText& Tip, int32 Index, bool bPrimary);
	void StyleButton(class UStageChoiceButton& Button, bool bPrimary, bool bEnabled) const;

	/** Coalesces changes into one rebuild on the next tick. */
	void RequestRebuild();
	void Rebuild();
	void RebuildResults();
	void RebuildDownloads();

	void HandleAction(int32 Index);
	void HandleGet(int32 RowIndex);
	void HandleCancel(int32 RowIndex);

	UFUNCTION()
	void HandleShareChanged();

	UFUNCTION()
	void HandleRequestFinished(const FStageGdtfShareResultInfo& Result);

	UFUNCTION()
	void HandleLibraryChanged();

	UFUNCTION()
	void HandleSearchChanged(const FText& Text);

	UFUNCTION()
	void HandlePasswordCommitted(const FText& Text, ETextCommit::Type CommitMethod);

	/** The uploader filter; unset shows every file. */
	TOptional<EStageGdtfShareUploader> UploaderFilter;

	/** Revision ids of the result rows and the download rows, by the row index their buttons carry. */
	TArray<int32> ResultRevisions;
	TArray<int32> DownloadRevisions;

	FTimerHandle RebuildTimer;

	UPROPERTY(Transient)
	TObjectPtr<class UVerticalBox> SignInBox = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UVerticalBox> AccountBox = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UEditableTextBox> UserField = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UEditableTextBox> PasswordField = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UEditableTextBox> SearchField = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> AccountText = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> ResultInfoText = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UVerticalBox> ResultsBox = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UVerticalBox> DownloadsBox = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> StatusText = nullptr;
	/** Indexed by EAction. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> ActionButtons;
};
