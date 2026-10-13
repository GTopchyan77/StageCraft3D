// Copyright(c) 2023 Dragon Li Software Limited. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "DlFmVersion.generated.h"

/**
 * 
 */

struct FDlFmLiteVersion_Observer;
USTRUCT()
struct DLFILEMANAGER_API FDlFmLiteVersion_Entity
{
	GENERATED_BODY()
public:
	
	UPROPERTY()
	int32 CurVersion;
	
	FDlFmLiteVersion_Entity()
	{
		CurVersion=0;
	}
	bool IsInitialized()const;
	void Update();
};

USTRUCT()
struct DLFILEMANAGER_API FDlFmLiteVersion_Observer
{
	GENERATED_BODY()
public:
	UPROPERTY()
	int32 CurVersion; 

	FDlFmLiteVersion_Observer();
	bool IsInitialized()const; 
	bool Observe_TriggerUpdate(const FDlFmLiteVersion_Entity& Entity); 
	bool Observe_Updated(const FDlFmLiteVersion_Entity& Entity) const;
	void Sync_AfterExternalUpdate(const FDlFmLiteVersion_Entity& Entity);
};
