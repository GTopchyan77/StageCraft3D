// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ComboBoxString.h"
#include "StageThemedComboBox.generated.h"

/**
 * A string dropdown drawn with the StageCraft theme.
 *
 * UComboBoxString only accepts its font and foreground colour before its Slate widget is built (InitFont / InitForegroundColor are
 * protected), and its defaults are a 16 pt bold font on a light button. This subclass exists only to apply the theme at that point.
 * Call ApplyTheme right after construction, before the widget is added to a live panel.
 */
UCLASS(MinimalAPI)
class UStageThemedComboBox : public UComboBoxString
{
	GENERATED_BODY()

public:
	void ApplyTheme(const class UStageCraftUITheme& Theme);
};
