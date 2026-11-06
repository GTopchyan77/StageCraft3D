// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AI/StageKnowledge.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "StageKnowledgeSubsystem.generated.h"

/**
 * The assistant's offline knowledge for the whole run (Docs/ADR/0030-assistant-knowledge-live-context-and-tools.md §3): the read-only
 * FStageKnowledgeIndex, built from the live tables (key commands, panel anchors, panels, the catalog's fixture attributes) and the
 * help articles in Content/StageCraft/Help/<en or hy>/<id>.md.
 *
 * Built lazily on first use and again after the item or panel catalog changes (their delegates mark it stale; nothing polls).
 * Reading the articles is a few small files, done on the game thread the first time an answer is asked for. Never ticks. Game
 * thread only.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageKnowledgeSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	/** The index, built now if it is stale. */
	const FStageKnowledgeIndex& GetIndex();

	/** The knowledge sources as they are now: what GetIndex is built from. GatherStaticSources plus panel names and catalog models. */
	FStageKnowledgeSources GatherSources() const;

	/**
	 * The sources that need no game instance: key commands, panel anchors (panels named by their tag), fixture attributes without
	 * catalog models, and the help articles read from disk. OutArticleErrors (optional) lists articles that could not be read.
	 */
	static FStageKnowledgeSources GatherStaticSources(TArray<FString>* OutArticleErrors = nullptr);

	/** The folder the help articles are read from (Content/StageCraft/Help). */
	static FString GetHelpDirectory();

	/** Problems found reading the articles last time (unreadable, malformed); empty when all were fine. For tests and logs. */
	const TArray<FString>& GetArticleErrors() const { return ArticleErrors; }

private:
	void MarkStale() { bStale = true; }

	UFUNCTION()
	void HandleCatalogChanged();

	/** Reads every article of both languages, sorted by file name; problems go to OutErrors. */
	static TArray<FStageHelpArticle> ReadArticles(TArray<FString>& OutErrors);

	FStageKnowledgeIndex Index;
	bool bStale = true;
	mutable TArray<FString> ArticleErrors;
	FDelegateHandle PanelCatalogHandle;
};
