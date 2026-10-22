// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * The offline Armenian reading of assistant prompts (Docs/ADR/0017-multilingual-voice-and-photo-import.md §2.2). Rather than a
 * second grammar, it rewrites an Armenian prompt into the words the English grammar (StageAICommandParser) already understands,
 * so both languages share one set of rules, limits and tests:
 *  - Words are matched by stem, longest first, so the endings of Armenian (articles -ը/-ն, plurals -եր/-ներ, cases) need no
 *    rules of their own: "կյուների" (of the cues) and "կյուն" (the cue) both read as "cue". Short words are matched exactly.
 *  - Number words become digits; "5-րդ" reads as 5; ordinals before "կյու" move after it ("երկրորդ կյուն" = cue 2).
 *  - The question mark "՞" (written over a vowel inside a word) and question words make the prompt a question.
 *  - Text in «» or "" is kept as it was typed (a cue label), with plain quotes.
 *  - A number next to a part of the day becomes a clock time ("երեկոյան 8-ին" = 8 pm), unless the prompt asks for another change.
 *  - "մի" before a verb is a prohibition ("մի՛ ավելացրու" = don't add): the prompt is not understood rather than run.
 *  - Unknown Armenian words are kept, so "ավելացրու 2 վիշապ" asks for a fixture called "վիշապ" and is refused, never
 *    silently read as "add 2 of the first fixture".
 * Pure and deterministic; covered by the StageCraft.AI.Armenian tests.
 */
namespace StageAIArmenian
{
	/** Prompt rewritten for the English grammar. Text without Armenian letters is returned unchanged. */
	MODULARSCENEBUILDER_API FString TranslateToGrammar(const FString& Prompt);
}
