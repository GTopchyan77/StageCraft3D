// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageCraftUITheme.h"

#include "Data/StageParameterTypes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageCraftUITheme)

namespace
{
	FLinearColor Hex(const TCHAR* InHex)
	{
		return FLinearColor::FromSRGBColor(FColor::FromHex(InHex));
	}
}

UStageCraftUITheme::UStageCraftUITheme()
	: WindowBackground(Hex(TEXT("0B0C0F")))
	, PanelBackground(Hex(TEXT("15171C")))
	, PanelHeader(Hex(TEXT("1E2128")))
	, RowBackground(Hex(TEXT("1A1D23")))
	, RowHover(Hex(TEXT("252932")))
	, Divider(Hex(TEXT("2B2F38")))
	, TextPrimary(Hex(TEXT("E6E8EC")))
	, TextSecondary(Hex(TEXT("8A909C")))
	, TextDisabled(Hex(TEXT("50555F")))
	, AccentColor(Hex(TEXT("FFB300")))
	, SelectionColor(Hex(TEXT("3D8BFD")))
	, WarningColor(Hex(TEXT("FF5A4E")))
{
	FeatureGroupColors.Add(StageCraftTags::FeatureGroup_Info,		Hex(TEXT("6C7380")));
	FeatureGroupColors.Add(StageCraftTags::FeatureGroup_Transform,	Hex(TEXT("9AA3B2")));
	FeatureGroupColors.Add(StageCraftTags::FeatureGroup_Patch,		Hex(TEXT("FF8C42")));
	FeatureGroupColors.Add(StageCraftTags::FeatureGroup_Dimmer,		Hex(TEXT("F5C400")));
	FeatureGroupColors.Add(StageCraftTags::FeatureGroup_Position,	Hex(TEXT("3AA0FF")));
	FeatureGroupColors.Add(StageCraftTags::FeatureGroup_Color,		Hex(TEXT("E040FB")));
	FeatureGroupColors.Add(StageCraftTags::FeatureGroup_Beam,		Hex(TEXT("4CD964")));
	FeatureGroupColors.Add(StageCraftTags::FeatureGroup_Audio,		Hex(TEXT("00C2C7")));
	FeatureGroupColors.Add(StageCraftTags::FeatureGroup_Rigging,	Hex(TEXT("C08457")));
}

FLinearColor UStageCraftUITheme::GetFeatureGroupColor(FGameplayTag FeatureGroup) const
{
	for (FGameplayTag Tag = FeatureGroup; Tag.IsValid(); Tag = Tag.RequestDirectParent())
	{
		if (const FLinearColor* Color = FeatureGroupColors.Find(Tag))
		{
			return *Color;
		}
	}
	return AccentColor;
}
