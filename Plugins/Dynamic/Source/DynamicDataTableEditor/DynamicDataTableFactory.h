// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "Factories/Factory.h"
#include "DynamicDataTableFactory.generated.h"

class UDynamicDataTable;

UCLASS(hidecategories=Object)
class DYNAMICDATATABLEEDITOR_API UDynamicDataTableFactory : public UFactory
{
	GENERATED_UCLASS_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "Data Table Factory")
	TObjectPtr<const class UScriptStruct> Struct;

	//~ Begin UFactory Interface
	virtual bool ConfigureProperties() override;
	virtual UObject* FactoryCreateNew(UClass* Class, UObject* InParent, FName Name, EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn) override;
	//~ Begin UFactory Interface

protected:
	virtual UDynamicDataTable* MakeNewDynamicDataTable(UObject* InParent, FName Name, EObjectFlags Flags);
};
