// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for the AI assistant (Docs/ADR/0016-ai-assistant.md §2.7): send a prompt through the controller's
// request (the same path as the panel's SEND), print the conversation, clear it, and save the assistant window. Compiled out of
// Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "AI/StageAICommandParser.h"
#include "AI/StageAICommandSubsystem.h"
#include "AI/StageVoiceInputComponent.h"
#include "AudioCaptureCore.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Player/ModularPlayerController.h"
#include "UI/StageAIAssistantWidget.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/SWindow.h"

namespace StageAICommands
{
	const TCHAR* SpeakerName(const FStageAIMessage& Message)
	{
		if (Message.Speaker == EStageAISpeaker::User)
		{
			return TEXT("You");
		}
		switch (Message.Outcome)
		{
		case EStageAIOutcome::Succeeded:	return TEXT("Assistant (done)");
		case EStageAIOutcome::Refused:		return TEXT("Assistant (not done)");
		case EStageAIOutcome::Pending:		return TEXT("Assistant (working)");
		default:							return TEXT("Assistant");
		}
	}

	/** "StageCraft.Assistant.Ask add 4 moving heads": the words are joined with spaces. */
	void Ask(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
		const UStageAICommandSubsystem* Assistant = UWorld::GetSubsystem<UStageAICommandSubsystem>(World);
		if (!Controller || !Assistant || Args.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Assistant.Ask <request>   (needs a stage controller and the assistant; no commas inside -ExecCmds)"));
			return;
		}
		const int32 Before = Assistant->GetMessages().Num();
		const FString Prompt = FString::Join(Args, TEXT(" "));
		const bool bAccepted = Controller->RequestAssistantPrompt(Prompt);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Assistant.Ask \"%s\": %s."), *Prompt, bAccepted ? TEXT("accepted") : TEXT("refused"));
		for (int32 Index = FMath::Max(Before, 0); Index < Assistant->GetMessages().Num(); ++Index)
		{
			const FStageAIMessage& Message = Assistant->GetMessages()[Index];
			UE_LOG(LogStageCraft, Display, TEXT("  %s: %s"), SpeakerName(Message), *Message.Text.ToString().Replace(TEXT("\n"), TEXT(" / ")));
		}
	}

	void Transcript(const TArray<FString>& Args, UWorld* World)
	{
		const UStageAICommandSubsystem* Assistant = UWorld::GetSubsystem<UStageAICommandSubsystem>(World);
		if (!Assistant)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Assistant: no assistant in this world (Game and PIE worlds only)."));
			return;
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Assistant.Transcript: %d lines, state %s. %s"), Assistant->GetMessages().Num(),
			*UEnum::GetValueAsString(Assistant->GetState()), *Assistant->GetInterpreterDescription().ToString());
		for (const FStageAIMessage& Message : Assistant->GetMessages())
		{
			UE_LOG(LogStageCraft, Display, TEXT("  %s: %s"), SpeakerName(Message), *Message.Text.ToString().Replace(TEXT("\n"), TEXT(" / ")));
		}
	}

	void Clear(const TArray<FString>& Args, UWorld* World)
	{
		if (UStageAICommandSubsystem* Assistant = UWorld::GetSubsystem<UStageAICommandSubsystem>(World))
		{
			Assistant->ClearConversation();
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Assistant.Clear: %d lines left."), Assistant->GetMessages().Num());
		}
	}

	/** Saves the window that hosts the AI Assistant panel (a floating pop-up window by default) as Saved/Screenshots/Assistant_<time>.png. */
	void Screenshot(const TArray<FString>& Args, UWorld* World)
	{
		for (TObjectIterator<UStageAIAssistantWidget> It; It; ++It)
		{
			const TSharedPtr<SWidget> Widget = It->GetCachedWidget();
			const TSharedPtr<SWindow> Window = Widget.IsValid() ? FSlateApplication::Get().FindWidgetWindow(Widget.ToSharedRef()) : nullptr;
			if (!Window.IsValid())
			{
				continue;
			}
			TArray<FColor> Pixels;
			FIntVector Size;
			if (!FSlateApplication::Get().TakeScreenshot(Window.ToSharedRef(), Pixels, Size) || Pixels.IsEmpty())
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Assistant.Screenshot: capture failed."));
				return;
			}
			for (FColor& Pixel : Pixels)
			{
				Pixel.A = 255;
			}
			TArray64<uint8> Png;
			FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
			const FString File = FPaths::ProjectSavedDir() / TEXT("Screenshots") / FString::Printf(TEXT("Assistant_%s.png"), *FDateTime::Now().ToString(TEXT("%H%M%S")));
			const bool bSaved = FFileHelper::SaveArrayToFile(Png, *File);
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Assistant.Screenshot: window \"%s\" %dx%d at %s, %s %s."), *Window->GetTitle().ToString(), Size.X, Size.Y,
				*Window->GetPositionInScreen().ToString(), bSaved ? TEXT("saved") : TEXT("NOT saved"), *File);
			return;
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Assistant.Screenshot: the AI Assistant window is not open."));
	}

	/** "StageCraft.Assistant.Voice start|stop|cancel|status|devices": the MIC button's path, for scripted runs. */
	void Voice(const TArray<FString>& Args, UWorld* World)
	{
		const AModularPlayerController* Controller = World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
		UStageVoiceInputComponent* VoiceInput = Controller ? Controller->GetVoiceInput() : nullptr;
		if (!VoiceInput)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Assistant.Voice: no stage controller with voice input."));
			return;
		}
		const FString Action = Args.IsEmpty() ? FString(TEXT("status")) : Args[0].ToLower();
		if (Action == TEXT("start"))
		{
			VoiceInput->StartRecording();
		}
		else if (Action == TEXT("stop"))
		{
			VoiceInput->StopRecording();
		}
		else if (Action == TEXT("cancel"))
		{
			VoiceInput->Cancel();
		}
		else if (Action == TEXT("devices"))
		{
			// Lists the recording devices the capture backend sees; nothing is opened or recorded.
			Audio::FAudioCapture Capture;
			TArray<Audio::FCaptureDeviceInfo> Devices;
			Capture.GetCaptureDevicesAvailable(Devices);
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Assistant.Voice devices: %d recording %s."), Devices.Num(), Devices.Num() == 1 ? TEXT("device") : TEXT("devices"));
			for (const Audio::FCaptureDeviceInfo& Device : Devices)
			{
				UE_LOG(LogStageCraft, Display, TEXT("  %s: %d channels, %d Hz"), *Device.DeviceName, Device.InputChannels, Device.PreferredSampleRate);
			}
		}
		FText Reason;
		const bool bAvailable = VoiceInput->IsAvailable(Reason);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Assistant.Voice %s: state %s, %s. Status: %s"), *Action, *UEnum::GetValueAsString(VoiceInput->GetState()),
			bAvailable ? TEXT("available") : *FString::Printf(TEXT("unavailable (%s)"), *Reason.ToString()), *VoiceInput->GetStatus().ToString());
	}

	void PrintSince(const UStageAICommandSubsystem& Assistant, int32 Before)
	{
		for (int32 Index = FMath::Max(Before, 0); Index < Assistant.GetMessages().Num(); ++Index)
		{
			const FStageAIMessage& Message = Assistant.GetMessages()[Index];
			UE_LOG(LogStageCraft, Display, TEXT("  %s%s: %s"), SpeakerName(Message), Message.ProposalId != 0 ? TEXT(" (proposal)") : TEXT(""),
				*Message.Text.ToString().Replace(TEXT("\n"), TEXT(" / ")));
		}
	}

	/**
	 * "StageCraft.Assistant.Plan store cue 5 | patch fixtures 1 to 4 at 2.001": each part read by the local grammar, the parts
	 * proposed together as the online model would (Docs/ADR/0030 §6), so the card shows.
	 */
	void Plan(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
		const UStageAICommandSubsystem* Assistant = UWorld::GetSubsystem<UStageAICommandSubsystem>(World);
		TArray<FString> Parts;
		static const TCHAR* const Separators[] = { TEXT(";"), TEXT("|") };
		FString::Join(Args, TEXT(" ")).ParseIntoArray(Parts, Separators, UE_ARRAY_COUNT(Separators));
		if (!Controller || !Assistant || Parts.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Assistant.Plan <request> | <request> ...   (each part one change; a semicolon also separates them)"));
			return;
		}
		FStageAIPlan NewPlan;
		for (const FString& Part : Parts)
		{
			const TOptional<FStageAICommand> Command = StageAICommandParser::Parse(Part.TrimStartAndEnd());
			if (!Command.IsSet())
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Assistant.Plan: \"%s\" is not understood; nothing proposed."), *Part.TrimStartAndEnd());
				return;
			}
			NewPlan.Language = Command->Language;
			NewPlan.Steps.Add(*Command);
		}
		const int32 Before = Assistant->GetMessages().Num();
		const bool bAccepted = Controller->RequestAssistantPlanForTesting(NewPlan);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Assistant.Plan: %d steps %s; open proposal %d."), NewPlan.Steps.Num(), bAccepted ? TEXT("submitted") : TEXT("refused (busy)"),
			Assistant->GetOpenProposalId());
		PrintSince(*Assistant, Before);
	}

	/** APPLY (bApply) or CANCEL on the open proposal card, through the controller like the buttons. */
	void AnswerProposal(UWorld* World, bool bApply)
	{
		AModularPlayerController* Controller = World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
		const UStageAICommandSubsystem* Assistant = UWorld::GetSubsystem<UStageAICommandSubsystem>(World);
		if (!Controller || !Assistant)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Assistant: no stage controller or assistant."));
			return;
		}
		const int32 Before = Assistant->GetMessages().Num();
		const bool bTaken = Controller->RequestAssistantProposal(bApply);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Assistant.%s: %s."), bApply ? TEXT("Apply") : TEXT("Cancel"), bTaken ? TEXT("taken") : TEXT("refused"));
		PrintSince(*Assistant, Before);
	}

	FAutoConsoleCommandWithWorldAndArgs PlanCommand(TEXT("StageCraft.Assistant.Plan"),
		TEXT("<request> | <request> ...  Proposes the requests as one plan, as the online model would (a card with APPLY and CANCEL)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Plan));
	FAutoConsoleCommandWithWorld ApplyCommand(TEXT("StageCraft.Assistant.Apply"),
		TEXT("APPLY on the open proposal card: its plan runs as one undo step."), FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World) { AnswerProposal(World, true); }));
	FAutoConsoleCommandWithWorld CancelCommand(TEXT("StageCraft.Assistant.Cancel"),
		TEXT("CANCEL on the open proposal card."), FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World) { AnswerProposal(World, false); }));

	FAutoConsoleCommandWithWorldAndArgs AskCommand(TEXT("StageCraft.Assistant.Ask"),
		TEXT("<request>  Sends a request to the AI assistant through the controller (rules and undo as in the panel)."), FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Ask));
	FAutoConsoleCommandWithWorldAndArgs TranscriptCommand(TEXT("StageCraft.Assistant.Transcript"),
		TEXT("Prints the AI assistant's conversation and which interpreter is in use."), FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Transcript));
	FAutoConsoleCommandWithWorldAndArgs ClearCommand(TEXT("StageCraft.Assistant.Clear"),
		TEXT("Clears the AI assistant's conversation (the stage is unchanged)."), FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Clear));
	FAutoConsoleCommandWithWorldAndArgs VoiceCommand(TEXT("StageCraft.Assistant.Voice"),
		TEXT("start|stop|cancel|status|devices  Drives the assistant's voice prompt like the MIC button; devices lists microphones without recording."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Voice));
	FAutoConsoleCommandWithWorldAndArgs ScreenshotCommand(TEXT("StageCraft.Assistant.Screenshot"),
		TEXT("Saves the AI Assistant window (wherever it is docked or floating) to Saved/Screenshots."), FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Screenshot));
}

#endif // !UE_BUILD_SHIPPING
