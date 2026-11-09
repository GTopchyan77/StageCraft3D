// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Support/StageSupportReport.h"
#include "StageSupportSubsystem.generated.h"

/** How sending a support report ended. */
struct FStageSupportSubmitResult
{
	/** Received by StageCraft Cloud; TicketId is its reference. */
	bool bSent = false;
	/** Saved on this computer instead (offline, or sending failed); Folder holds it. */
	bool bSaved = false;
	FString TicketId;
	FString Folder;
	FText Message;
};

DECLARE_DELEGATE_OneParam(FOnStageSupportSubmitted, const FStageSupportSubmitResult& /*Result*/);

/**
 * Support requests and bug reports (Docs/ADR/0032 §7): builds a draft with the app's details, reads the log tail and captures the
 * window when the user asks, and sends the report to StageCraft Cloud, or saves it on disk when the cloud is offline or unreachable.
 *
 * Privacy: nothing is collected unless the user opens the form, the log and the screenshot are opt-in, and every text is redacted
 * (StageSupportReport::Redact: keys, tokens, the cloud session, e-mail addresses, the Windows user name) before it is shown for the last
 * time, sent or saved. One report is sent at a time. Game thread only; never ticks.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageSupportSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** A new report with its id, time, app version, platform, GPU and (when World is a stage) a short scene summary. */
	FStageSupportReport MakeDraft(const UWorld* World) const;

	/** The end of this session's log (StageSupportReport::MaxLogBytes), redacted. Empty when the log cannot be read. */
	FString ReadLogTail() const;

	/** The app window as a PNG of at most StageSupportReport::MaxScreenshotBytes (halved until it fits). False, with a reason, otherwise. */
	bool CaptureScreenshot(TArray<uint8>& OutPng, FText& OutError) const;

	/** Text the user sees and sends, redacted the same way as the report will be. */
	FString Redact(const FString& Text) const;

	bool IsSubmitting() const { return bSubmitting; }

	/**
	 * Validates, redacts and sends Report: to StageCraft Cloud when it is online (signed in or not), else saved on disk; a failed send is
	 * saved too. OnDone runs once on the game thread. False, with a reason, when the report is not valid or another is being sent.
	 */
	bool Submit(FStageSupportReport Report, FOnStageSupportSubmitted OnDone, FText& OutError);

	/** Writes report.txt, report.json, and log.txt / screenshot.png when included, to a new folder under GetOfflineDirectory. */
	bool SaveOffline(const FStageSupportReport& Report, FString& OutFolder, FText& OutError) const;

	/** Saved/StageCraft/Support (the data root's). */
	static FString GetOfflineDirectory();

private:
	TArray<FString> GetSecrets() const;
	void HandleSent(const struct FStageCloudResponse& Response, FStageSupportReport Report, FOnStageSupportSubmitted OnDone);

	bool bSubmitting = false;
};
