// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for the assistant's knowledge (Docs/ADR/0030 §3, §7; STATE.md #72): search it, rebuild and count it,
// point at a control (SHOW ME), and check that every panel anchor resolves to a live widget. Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "AI/StageAICommandParser.h"
#include "AI/StageKnowledgeSearch.h"
#include "AI/StageKnowledgeSubsystem.h"
#include "Blueprint/UserWidget.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "ModularSceneBuilder.h"
#include "UI/StageUIAnchors.h"
#include "Workspace/StageWorkspaceSubsystem.h"

namespace StageKnowledgeCommands
{
	template <typename T>
	T* GetSubsystem(const UWorld* World)
	{
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		return GameInstance ? GameInstance->GetSubsystem<T>() : nullptr;
	}

	const TCHAR* KindName(EStageKnowledgeKind Kind)
	{
		switch (Kind)
		{
		case EStageKnowledgeKind::Command:		return TEXT("command");
		case EStageKnowledgeKind::Control:		return TEXT("control");
		case EStageKnowledgeKind::Parameter:	return TEXT("parameter");
		case EStageKnowledgeKind::Panel:		return TEXT("panel");
		default:								return TEXT("article");
		}
	}

	void Search(const TArray<FString>& Args, UWorld* World)
	{
		UStageKnowledgeSubsystem* Knowledge = GetSubsystem<UStageKnowledgeSubsystem>(World);
		const FString Query = FString::Join(Args, TEXT(" "));
		if (!Knowledge || Query.IsEmpty())
		{
			UE_LOG(LogStageCraft, Warning, TEXT("StageCraft.Assistant.Search <question>: no knowledge subsystem or no question."));
			return;
		}
		const FStageKnowledgeIndex& Index = Knowledge->GetIndex();
		const TOptional<FStageAICommand> Parsed = StageAICommandParser::Parse(Query);
		const StageKnowledgeSearch::ERoute Route = StageKnowledgeSearch::Route(Query, Parsed.GetPtrOrNull());
		UE_LOG(LogStageCraft, Display, TEXT("Knowledge search \"%s\": how-to %s, route %s, terms [%s]"), *Query,
			StageKnowledgeSearch::IsHowToQuestion(Query) ? TEXT("yes") : TEXT("no"),
			Route == StageKnowledgeSearch::ERoute::Command ? TEXT("command") : Route == StageKnowledgeSearch::ERoute::Knowledge ? TEXT("knowledge") : TEXT("knowledge only"),
			*FString::Join(StageKnowledgeSearch::MakeQueryTerms(Query), TEXT(" ")));
		for (const StageKnowledgeSearch::FHit& Hit : StageKnowledgeSearch::Search(Index, Query, StageAILanguage::Detect(Query), 5))
		{
			const FStageKnowledgeEntry& Entry = Index.Entries[Hit.Entry];
			UE_LOG(LogStageCraft, Display, TEXT("  %.2f  %s  [%s, %s]  %s"), Hit.Score, *Entry.Id.ToString(), KindName(Entry.Kind),
				StageAILanguage::GetCode(Entry.Language), *Entry.Title);
		}
	}

	void Rebuild(const TArray<FString>& Args, UWorld* World)
	{
		UStageKnowledgeSubsystem* Knowledge = GetSubsystem<UStageKnowledgeSubsystem>(World);
		if (!Knowledge)
		{
			return;
		}
		const FStageKnowledgeIndex Built = StageKnowledge::Build(Knowledge->GatherSources());
		TMap<FString, int32> Counts;
		for (const FStageKnowledgeEntry& Entry : Built.Entries)
		{
			++Counts.FindOrAdd(FString::Printf(TEXT("%s/%s"), KindName(Entry.Kind), StageAILanguage::GetCode(Entry.Language)));
		}
		Counts.KeySort(TLess<FString>());
		for (const TPair<FString, int32>& Count : Counts)
		{
			UE_LOG(LogStageCraft, Display, TEXT("Knowledge: %s %d"), *Count.Key, Count.Value);
		}
		UE_LOG(LogStageCraft, Display, TEXT("Knowledge: %d entries, %d terms, digest %d characters, %d article problems."), Built.Num(),
			Built.DocumentFrequency.Num(), StageKnowledge::BuildDigest(Built).Len(), Knowledge->GetArticleErrors().Num());
		for (const FString& Error : Knowledge->GetArticleErrors())
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Knowledge: %s"), *Error);
		}
	}

	void ShowMe(const TArray<FString>& Args, UWorld* World)
	{
		UStageWorkspaceSubsystem* Workspace = GetSubsystem<UStageWorkspaceSubsystem>(World);
		const FGameplayTag Panel = Args.Num() > 0 ? FGameplayTag::RequestGameplayTag(FName(*Args[0]), false) : FGameplayTag();
		const FName Control = Args.Num() > 1 ? FName(*Args[1]) : NAME_None;
		const bool bShown = Workspace && Panel.IsValid() && Workspace->ShowControl(Panel, Control);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Assistant.ShowMe %s %s: %s"), *Panel.ToString(), *Control.ToString(), bShown ? TEXT("panel opened") : TEXT("could not open"));
	}

	void CheckAnchors(const TArray<FString>& Args, UWorld* World)
	{
		UStageWorkspaceSubsystem* Workspace = GetSubsystem<UStageWorkspaceSubsystem>(World);
		if (!Workspace)
		{
			return;
		}
		int32 Found = 0;
		int32 Missing = 0;
		for (const StageUIAnchors::FTable& Table : StageUIAnchors::GetTables())
		{
			Workspace->OpenPanel(Table.Panel);
			const IStageAnchoredPanel* Panel = Cast<IStageAnchoredPanel>(Workspace->FindPanelWidget(Table.Panel));
			for (const FStageUIAnchor& Anchor : Table.Anchors)
			{
				if (Panel && Panel->FindAnchorWidget(Anchor.ControlId))
				{
					++Found;
				}
				else
				{
					++Missing;
					UE_LOG(LogStageCraft, Warning, TEXT("Anchor check: %s %s has no live widget."), *Table.Panel.ToString(), *Anchor.ControlId.ToString());
				}
			}
		}
		UE_LOG(LogStageCraft, Display, TEXT("Anchor check: %d anchors resolve to live widgets, %d do not."), Found, Missing);
	}

	FAutoConsoleCommandWithWorldAndArgs SearchCommand(TEXT("StageCraft.Assistant.Search"), TEXT("<question> Lists the knowledge entries a question finds, with scores, and how the assistant would route it."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Search));
	FAutoConsoleCommandWithWorldAndArgs RebuildCommand(TEXT("StageCraft.Assistant.Knowledge"), TEXT("Rebuilds the knowledge index from the live tables and help articles and counts its entries."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Rebuild));
	FAutoConsoleCommandWithWorldAndArgs ShowMeCommand(TEXT("StageCraft.Assistant.ShowMe"), TEXT("<PanelTag> [ControlId] Opens the panel and pulses the control, as the SHOW ME button does."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ShowMe));
	FAutoConsoleCommandWithWorldAndArgs CheckAnchorsCommand(TEXT("StageCraft.Assistant.CheckAnchors"), TEXT("Opens every anchored panel and reports anchors with no live widget."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&CheckAnchors));
}

#endif // !UE_BUILD_SHIPPING
