// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AI/StageAITypes.h"
#include "AI/StageKnowledge.h"

/**
 * Offline search of the assistant's knowledge (Docs/ADR/0030 §3): BM25 over normalised word stems, in English and Armenian.
 *  - English words are lower-cased and lightly stemmed (plural, -ing, -ed, a final e), so "storing", "stored" and "store" meet.
 *  - Armenian words lose the question and emphasis marks and their common endings (articles, plurals, cases), and an Armenian
 *    query is also read through the assistant's lexicon (StageAIArmenian), so it finds English entries as well as Armenian ones.
 *  - Question and filler words are dropped on both sides.
 * Pure and deterministic: ties are broken by entry order. Covered by the StageCraft.Assistant.Search tests.
 */
namespace StageKnowledgeSearch
{
	/** BM25 parameters. */
	constexpr float K1 = 1.2f;
	constexpr float B = 0.75f;

	/** The lowest top score that is answered; below it the prompt is "not understood". */
	constexpr float MinAnswerScore = 2.0f;

	struct FHit
	{
		int32 Entry = INDEX_NONE;
		float Score = 0.f;
	};

	/** Normalised search terms of Text, in order, repeats kept. */
	MODULARSCENEBUILDER_API void Tokenize(const FString& Text, TArray<FString>& OutTerms);

	/** The search terms of a prompt: its own terms, and for Armenian also those of its lexicon reading. Each term once. */
	MODULARSCENEBUILDER_API TArray<FString> MakeQueryTerms(const FString& Prompt);

	/**
	 * The best MaxHits entries for Prompt, best first, one per Id: the entry in Language when the Id has one. Empty when no term of
	 * the prompt is in the index.
	 */
	MODULARSCENEBUILDER_API TArray<FHit> Search(const FStageKnowledgeIndex& Index, const FString& Prompt, EStageAILanguage Language, int32 MaxHits = 3);

	/**
	 * Whether Prompt asks how to do something or where something is ("how do I store a cue", "where is blackout", "what does
	 * KILL do", "ինչպե՞ս պահել կյուն"), rather than asking for it to be done. A how-to question is answered from the knowledge
	 * and never runs the command it mentions.
	 */
	MODULARSCENEBUILDER_API bool IsHowToQuestion(const FString& Prompt);

	/** Where a prompt is answered from, given how the local grammar read it (StageAICommandParser::Parse). */
	enum class ERoute : uint8
	{
		/** Run or answer the parsed command as before. */
		Command,
		/** Answer from the knowledge; when it has nothing, fall back to the parsed command (a bare "help") or "not understood". */
		Knowledge,
		/** An action asked as a how-to question: answer from the knowledge, and never run the action, even when nothing is found. */
		KnowledgeOnly,
	};

	/**
	 * The route of Prompt: Knowledge when the grammar did not understand it or read it as "help" with a topic ("help with
	 * patching"); KnowledgeOnly for a how-to question the grammar read as an action ("how do I store a cue"); Command otherwise,
	 * so stage questions ("how many fixtures", "what is the total weight") keep their live answers.
	 */
	MODULARSCENEBUILDER_API ERoute Route(const FString& Prompt, const FStageAICommand* Parsed);
}
