// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for show diagnostics (Docs/ADR/0030 §5; STATE.md #73): list the findings, do what clicking a row of
// the Problems panel does, and explain one fixture. They drive scripted -game runs; compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "ModularSceneBuilder.h"
#include "Subsystems/StageShowDiagnosticsSubsystem.h"
#include "UI/StageProblemsPanel.h"
#include "Workspace/StageWorkspaceSubsystem.h"
#include "Workspace/StageWorkspaceTypes.h"

namespace StageDiagnosticsCommands
{
	EStageAILanguage LanguageOf(const TArray<FString>& Args, int32 Index)
	{
		return Args.IsValidIndex(Index) && Args[Index].Equals(TEXT("hy"), ESearchCase::IgnoreCase) ? EStageAILanguage::Armenian : EStageAILanguage::English;
	}

	void List(const TArray<FString>& Args, UWorld* World)
	{
		UStageShowDiagnosticsSubsystem* Diagnostics = UWorld::GetSubsystem<UStageShowDiagnosticsSubsystem>(World);
		if (!Diagnostics)
		{
			UE_LOG(LogStageCraft, Warning, TEXT("StageCraft.Problems.List: no stage."));
			return;
		}
		const TArray<FStageDiagnostic>& Problems = Diagnostics->RefreshProblems();
		UE_LOG(LogStageCraft, Display, TEXT("Problems: %d"), Problems.Num());
		for (int32 Index = 0; Index < Problems.Num(); ++Index)
		{
			UE_LOG(LogStageCraft, Display, TEXT("  %d. [%d] %s"), Index + 1, static_cast<int32>(Problems[Index].Severity),
				*StageShowDiagnostics::Describe(Problems[Index], LanguageOf(Args, 0)).ToString());
		}
	}

	void GoTo(const TArray<FString>& Args, UWorld* World)
	{
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		UStageWorkspaceSubsystem* Workspace = GameInstance ? GameInstance->GetSubsystem<UStageWorkspaceSubsystem>() : nullptr;
		const int32 Row = Args.Num() > 0 ? FCString::Atoi(*Args[0]) - 1 : 0;
		if (!Workspace || !Workspace->OpenPanel(StageCraftTags::Panel_Problems))
		{
			UE_LOG(LogStageCraft, Warning, TEXT("StageCraft.Problems.GoTo: the Problems panel cannot open."));
			return;
		}
		UStageProblemsPanel* Panel = Cast<UStageProblemsPanel>(Workspace->FindPanelWidget(StageCraftTags::Panel_Problems));
		const bool bWent = Panel && Panel->GoToProblem(Row);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Problems.GoTo %d: %s"), Row + 1, bWent ? TEXT("went") : TEXT("no such row"));
	}

	void Explain(const TArray<FString>& Args, UWorld* World)
	{
		const UStageShowDiagnosticsSubsystem* Diagnostics = UWorld::GetSubsystem<UStageShowDiagnosticsSubsystem>(World);
		if (!Diagnostics || Args.IsEmpty())
		{
			UE_LOG(LogStageCraft, Warning, TEXT("StageCraft.Show.Explain <fixture id> [en|hy]: no stage or no fixture."));
			return;
		}
		const FStageFixtureVerdict Verdict = Diagnostics->ExplainFixture(FCString::Atoi(*Args[0]), LanguageOf(Args, 1));
		TArray<FString> Lines;
		Verdict.Text.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			UE_LOG(LogStageCraft, Display, TEXT("Explain: %s"), *Line);
		}
	}

	FAutoConsoleCommandWithWorldAndArgs ListCommand(TEXT("StageCraft.Problems.List"), TEXT("[en|hy] Runs the show diagnostics now and logs every finding, worst first."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&List));
	FAutoConsoleCommandWithWorldAndArgs GoToCommand(TEXT("StageCraft.Problems.GoTo"), TEXT("<row> Opens the Problems panel and does what clicking that row does (1 is the first)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&GoTo));
	FAutoConsoleCommandWithWorldAndArgs ExplainCommand(TEXT("StageCraft.Show.Explain"), TEXT("<fixture id> [en|hy] Logs why the fixture is dark or lit, along its signal chain."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Explain));
}

#endif // !UE_BUILD_SHIPPING
