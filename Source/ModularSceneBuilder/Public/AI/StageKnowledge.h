// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AI/StageAILanguage.h"
#include "GameplayTagContainer.h"
#include "Input/StageKeyBindings.h"

/** Where a knowledge entry came from (Docs/ADR/0030-assistant-knowledge-live-context-and-tools.md §3). */
enum class EStageKnowledgeKind : uint8
{
	/** A key-bindable command (StageKeyBindings). */
	Command,
	/** A panel control with a UI anchor (StageUIAnchors). */
	Control,
	/** A fixture attribute, with the catalog models that offer it and their ranges. */
	Parameter,
	/** A help article (Content/StageCraft/Help/<lang>/<id>.md). */
	Article,
	/** A workspace panel and where it opens from. */
	Panel,
};

/**
 * One searchable piece of knowledge. Entries in two languages share an Id (an article and its translation); search reports each
 * Id once, in the asker's language when it has one. Plain values, safe on any thread.
 */
struct FStageKnowledgeEntry
{
	/** "article.tracking", "command.Blackout", "control.Playback.Blackout", "panel.Playback", "parameter.Zoom". */
	FName Id;
	EStageKnowledgeKind Kind = EStageKnowledgeKind::Article;
	EStageAILanguage Language = EStageAILanguage::English;
	FString Title;
	/** The answer text. A Command entry's "{keys}" is replaced with the user's current chords when it is shown. */
	FString Body;
	/** Extra search words, weighted like the title. */
	FString Keywords;
	/** The panel OPEN PANEL or SHOW ME opens; unset when the entry has none. */
	FGameplayTag Panel;
	/** The anchored control SHOW ME points at; None for a panel or no control. */
	FName Control;
	/** A Command entry's command, for its current chords. */
	TOptional<EStageInputCommand> Command;
};

/** A parsed help article. */
struct FStageHelpArticle
{
	/** The file name without extension; the same in every language. */
	FName Id;
	EStageAILanguage Language = EStageAILanguage::English;
	FString Title;
	FString Keywords;
	FString Body;
	FGameplayTag Panel;
	FName Control;
};

/**
 * What the index is built from. StageKnowledge::Build is pure over this, so tests can build from a known corpus; the runtime fills
 * it from the live tables (UStageKnowledgeSubsystem::GatherSources).
 */
struct FStageKnowledgeSources
{
	struct FCommand
	{
		EStageInputCommand Command = EStageInputCommand::Count;
		FString Label;
		FString Description;
		/** The default chords, searchable ("ctrl+shift+b"); the answer shows the user's current ones. */
		FString DefaultKeys;
	};

	struct FPanel
	{
		FGameplayTag Panel;
		FString Name;
	};

	struct FControl
	{
		FGameplayTag Panel;
		FString PanelName;
		FName Control;
		FString Label;
		FString Tooltip;
	};

	struct FParameter
	{
		FGameplayTag Attribute;
		FString Name;
		FString Meaning;
		/** "Beam 280: 4-60" style lines, one per model that offers the attribute. */
		TArray<FString> Models;
	};

	TArray<FCommand> Commands;
	TArray<FPanel> Panels;
	TArray<FControl> Controls;
	TArray<FParameter> Parameters;
	TArray<FStageHelpArticle> Articles;
};

/** The read-only, searchable index (ADR 0030 §3): the entries and their BM25 term statistics. Built once; never changed after. */
struct FStageKnowledgeIndex
{
	struct FDocument
	{
		/** Weighted term frequency: title and keywords count three times, the body once. */
		TMap<FString, float> Terms;
		float Length = 0.f;
	};

	TArray<FStageKnowledgeEntry> Entries;
	/** Parallel to Entries. */
	TArray<FDocument> Documents;
	/** How many documents hold each term. */
	TMap<FString, int32> DocumentFrequency;
	float AverageLength = 0.f;

	int32 Num() const { return Entries.Num(); }
	/** The entry with Id in Language, else in English, else any; null when there is none. */
	const FStageKnowledgeEntry* Find(FName Id, EStageAILanguage Language = EStageAILanguage::English) const;
};

/** A reply built from the index for one prompt. */
struct FStageKnowledgeAnswer
{
	bool bFound = false;
	/** In the prompt's language, ready to show. */
	FText Text;
	/** The top entry's title, cited under the reply. */
	FString SourceTitle;
	FGameplayTag Panel;
	FName Control;
};

/**
 * Builds the assistant's offline knowledge (ADR 0030 §3) and composes replies from it. Pure: no world, no files (the subsystem reads
 * the articles), so the StageCraft.Assistant.Knowledge tests build it from known sources.
 */
namespace StageKnowledge
{
	/** Longest article accepted (characters); a longer file is refused. */
	constexpr int32 MaxArticleLength = 16 * 1024;

	/**
	 * Reads one article: a "# Title" line, then "key: value" header lines (keywords, panel, control) up to a blank line, then the
	 * body. Unknown header keys are ignored; an unknown panel tag leaves Panel unset. False, with OutError, without a title or
	 * body, or when it is too long.
	 */
	MODULARSCENEBUILDER_API bool ParseArticle(const FString& Text, FName Id, EStageAILanguage Language, FStageHelpArticle& OutArticle, FString& OutError);

	/** The index of every source, in a fixed order, so the same sources always give the same index. */
	MODULARSCENEBUILDER_API FStageKnowledgeIndex Build(const FStageKnowledgeSources& Sources);

	/**
	 * A compact, byte-stable list of panel, command and control names for the online model's cached system prompt (ADR 0030 §8):
	 * names only, never article text or anything about the current show.
	 */
	MODULARSCENEBUILDER_API FString BuildDigest(const FStageKnowledgeIndex& Index);

	/**
	 * The reply to Prompt from the index, in Language: the best entry's text (a command with Bindings' chords, a control with
	 * where it is), and up to two more titles to try. Not found when nothing scores StageKnowledgeSearch::MinAnswerScore.
	 */
	MODULARSCENEBUILDER_API FStageKnowledgeAnswer Answer(const FStageKnowledgeIndex& Index, const FString& Prompt, EStageAILanguage Language,
		const FStageKeyBindings& Bindings);
}
