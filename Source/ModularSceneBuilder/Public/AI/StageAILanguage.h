// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * The language the assistant replies in (Docs/ADR/0017-multilingual-voice-and-photo-import.md §2.1). It follows each prompt, not
 * the game's culture: the menus stay in the UI language while one user asks in Armenian and the next in English.
 */
enum class EStageAILanguage : uint8
{
	English,
	Armenian,
};

namespace StageAILanguage
{
	/** A letter of the Armenian script (capital or small, the ligatures U+0587 and U+FB13..FB17 included). */
	MODULARSCENEBUILDER_API bool IsArmenianLetter(TCHAR Char);

	/** Armenian capitals become small letters; every other character is returned as it is. */
	MODULARSCENEBUILDER_API TCHAR ToLowerArmenian(TCHAR Char);

	/** Armenian when the text has Armenian letters and at least as many of them as Latin ones; English otherwise. */
	MODULARSCENEBUILDER_API EStageAILanguage Detect(const FString& Text);

	/** "en" or "hy" (ISO 639-1), as the online model's reply_language and the speech service's language hint. */
	MODULARSCENEBUILDER_API const TCHAR* GetCode(EStageAILanguage Language);

	/** The language of an ISO 639-1 code ("hy", "en"); unset for any other value. */
	MODULARSCENEBUILDER_API TOptional<EStageAILanguage> FromCode(const FString& Code);

	/**
	 * The reply in Language. English is the localizable source text (LOCTEXT); the Armenian rendition is culture-invariant text
	 * written next to it, because the reply language is chosen per prompt and must not be translated again by the game's culture.
	 */
	inline FText Pick(EStageAILanguage Language, const FText& English, const FText& Armenian)
	{
		return Language == EStageAILanguage::Armenian ? Armenian : English;
	}
}
