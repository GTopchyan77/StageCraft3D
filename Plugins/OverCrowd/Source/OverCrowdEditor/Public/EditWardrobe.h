// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved

#pragma once

#include "CoreMinimal.h"

#include "Layout/Visibility.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SComboBox.h"
#include "GameplayTagContainer.h"
#include "VatAnimationWidget.h"

/**
 *
 */
class UOverCrowdUtilityWidget;

class SEditWardrobe : public SCompoundWidget
{
public:
	DECLARE_DELEGATE_OneParam(FOnWardrobeEdited, bool);
	
	SLATE_BEGIN_ARGS(SEditWardrobe)
	{}
		//SLATE_ARGUMENT( FGameplayTagContainer, WardrobeContainer )
		SLATE_ARGUMENT( UOverCrowdUtilityWidget*, InOverCrowdUtilityWidget )
		SLATE_EVENT( FOnWardrobeEdited, OnWardrobeEdited )
	SLATE_END_ARGS()

	UPROPERTY()
	UOverCrowdUtilityWidget* OverCrowdUtilityWidget; 
	//TArray<FGameplayTagContainer> ContainersForPicker;
	//FGameplayTagContainer CurrentWardrobeContainer;

	UPROPERTY()
	UCustomDetailsView* WardrobeIDsView;
	
	FOnWardrobeEdited OnWardrobeEdited;
	
	void Construct(const FArguments& InArgs);
	
protected:
	
};
