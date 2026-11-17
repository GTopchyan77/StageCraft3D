// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageAudioMenus.h"

#include "UI/StageAudioPanel.h"
#include "UI/StageUIAnchors.h"

#define LOCTEXT_NAMESPACE "StageAudioMenus"

namespace StageAudioMenus
{
	namespace
	{
		/** The anchor of each source, in EStageAudioSource order (plain strings: no FName is made during static initialization). */
		const TCHAR* const SourceAnchors[] = { TEXT("SourceOff"), TEXT("SourceFile"), TEXT("SourceInput"), TEXT("SourceSystem"), TEXT("SourceTest") };
		static_assert(UE_ARRAY_COUNT(SourceAnchors) == static_cast<int32>(EStageAudioSource::TestSignal) + 1, "One anchor per source.");

		const FStageUIAnchor& Anchor(FName ControlId)
		{
			return StageUIAnchors::Get(UStageAudioPanel::GetAnchors(), ControlId);
		}
	}

	FText DescribeSource(EStageAudioSource Lit)
	{
		const int32 Index = static_cast<int32>(Lit);
		return Anchor(SourceAnchors[UE_ARRAY_COUNT(SourceAnchors) > Index && Index >= 0 ? Index : 0]).Label;
	}

	FStageMenu MakeSourceMenu(EStageAudioSource Lit, bool bBackground, bool bCanRequest, TFunction<void(EStageAudioSource)> PickSource,
		TFunction<void(bool)> SetBackground)
	{
		FStageMenu Menu;
		FStageMenuSection& Sources = Menu.AddSection(TEXT("Source"), LOCTEXT("SourceSection", "Music Source"));
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(SourceAnchors); ++Index)
		{
			const EStageAudioSource Source = static_cast<EStageAudioSource>(Index);
			const FStageUIAnchor& SourceAnchor = Anchor(SourceAnchors[Index]);
			// Picking the source already on is not a no-op for FILE... (it chooses another song), so every entry runs.
			Sources.Entries.Add(StageMenu::MakeRadio(SourceAnchor.Label, SourceAnchor.Tooltip, Source == Lit, bCanRequest, [PickSource, Source]()
			{
				if (PickSource)
				{
					PickSource(Source);
				}
			}));
		}

		const FStageUIAnchor& Background = Anchor(TEXT("Background"));
		Menu.AddSection(TEXT("Background"), LOCTEXT("BackgroundSection", "Other Windows")).Entries.Add(
			StageMenu::MakeToggle(Background.Label, Background.Tooltip, bBackground, bCanRequest, [SetBackground, bBackground]()
			{
				if (SetBackground)
				{
					SetBackground(!bBackground);
				}
			}));
		return Menu;
	}
}

#undef LOCTEXT_NAMESPACE
