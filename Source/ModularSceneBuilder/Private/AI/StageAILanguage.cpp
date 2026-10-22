// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/StageAILanguage.h"

namespace StageAILanguage
{
	bool IsArmenianLetter(TCHAR Char)
	{
		return (Char >= 0x0531 && Char <= 0x0556) || (Char >= 0x0560 && Char <= 0x0588) || (Char >= 0xFB13 && Char <= 0xFB17);
	}

	TCHAR ToLowerArmenian(TCHAR Char)
	{
		// The capitals U+0531..U+0556 sit exactly 0x30 below their small letters.
		return Char >= 0x0531 && Char <= 0x0556 ? TCHAR(Char + 0x30) : Char;
	}

	EStageAILanguage Detect(const FString& Text)
	{
		int32 Armenian = 0;
		int32 Latin = 0;
		for (const TCHAR Char : Text)
		{
			if (IsArmenianLetter(Char))
			{
				++Armenian;
			}
			else if ((Char >= TEXT('a') && Char <= TEXT('z')) || (Char >= TEXT('A') && Char <= TEXT('Z')))
			{
				++Latin;
			}
		}
		return Armenian > 0 && Armenian >= Latin ? EStageAILanguage::Armenian : EStageAILanguage::English;
	}

	const TCHAR* GetCode(EStageAILanguage Language)
	{
		return Language == EStageAILanguage::Armenian ? TEXT("hy") : TEXT("en");
	}

	TOptional<EStageAILanguage> FromCode(const FString& Code)
	{
		if (Code.Equals(TEXT("hy"), ESearchCase::IgnoreCase))
		{
			return EStageAILanguage::Armenian;
		}
		if (Code.Equals(TEXT("en"), ESearchCase::IgnoreCase))
		{
			return EStageAILanguage::English;
		}
		return {};
	}
}
