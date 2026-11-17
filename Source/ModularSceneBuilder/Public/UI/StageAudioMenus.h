// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Audio/StageAudioReactiveTypes.h"
#include "UI/StageMenuModel.h"

/**
 * The Audio panel's Source dropdown (UStageAudioPanel, STATE.md #100), as pure functions: tests call them without a widget or a
 * world. Labels and tooltips come from the panel's anchor table (UStageAudioPanel::GetAnchors), so the assistant's help, the
 * tooltip and the menu say the same thing.
 */
namespace StageAudioMenus
{
	/** The Source button's label: the source playing ("System Audio"), or Off. */
	FText DescribeSource(EStageAudioSource Lit);

	/**
	 * Off, File..., Input, System Audio, Test Beat (radio, Lit checked) and Keep Playing in the Background (toggle). With
	 * bCanRequest false (no controller to send the request) every entry is greyed out. A pick calls PickSource or SetBackground.
	 */
	FStageMenu MakeSourceMenu(EStageAudioSource Lit, bool bBackground, bool bCanRequest, TFunction<void(EStageAudioSource)> PickSource,
		TFunction<void(bool)> SetBackground);
}
