//  Copyright WASD Studio 2025 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SequencerSectionBP.h"       
#include "Blueprint/UserWidget.h"
#include "UISSequencerSectionBP.generated.h"

UCLASS()
class UMGINSEQUENCE_API UUISSequencerSectionBP : public USequencerSectionBP
{
	GENERATED_BODY()

public:

	// 在 Sequencer 里配置的 UMG
	UPROPERTY(EditAnywhere,BlueprintReadWrite, Instanced, Category = "UMG")
	UUserWidget* WidgetInstance;
};
