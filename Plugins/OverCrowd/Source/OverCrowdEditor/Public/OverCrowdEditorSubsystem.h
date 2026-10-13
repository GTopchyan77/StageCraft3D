// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved

#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Engine/World.h"
#include "OverCrowdEditorSubsystem.generated.h"
//
///**
// * 
// */
UCLASS()
class OVERCROWDEDITOR_API UOverCrowdEditorSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()
public:

	UPROPERTY()
	FDirectoryPath SaveDirectory;

	

	
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;


	// void OnLevelAddedToWorld(ULevel* InLevel, UWorld* World);
	// void OnLevelChanged(ULevel* InNewLevel, ULevel* InOldLevel, UWorld* InWorld);
	// void OnObjectModified(UObject* Object);
	void OnPropertyChanged(UObject* ObjectBeingModified, FPropertyChangedEvent& PropertyChangedEvent);

	void OnLevelActorsRemoved(AActor* InActor);
};
