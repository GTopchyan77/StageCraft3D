// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "CrowdInterface.generated.h"




USTRUCT(Blueprintable)
struct FDebugTextData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DebugText")
		FVector Location = FVector(0,0,0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DebugText")
		FString Text = "";

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DebugText")
		FColor Color = FColor();

};



// This class does not need to be modified.
UINTERFACE(MinimalAPI)
class UCrowdInterface : public UInterface
{
	GENERATED_BODY()
};

/**
 * 
 */
class OVERCROWD_API ICrowdInterface
{
	GENERATED_BODY()

	// Add interface functions to this class. This is the class that will be inherited to implement this interface.
public:

	UFUNCTION(BlueprintCallable, BlueprintImplementableEvent, Category = "OverCrowd")
		void GetDebugText(TMap<FVector, FString> &DebugText);

	UFUNCTION(BlueprintCallable, BlueprintImplementableEvent, Category = "OverCrowd")
		void GetDebugData(TArray<FDebugTextData> &Texts);
};
