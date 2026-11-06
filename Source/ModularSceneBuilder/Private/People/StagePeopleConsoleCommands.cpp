// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for the stand-in people (Docs/ADR/0031). Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Actors/StageAudienceActor.h"
#include "Actors/StagePropActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "ModularSceneBuilder.h"
#include "People/StageFigureAssetSubsystem.h"

namespace StagePeopleCommands
{
	/** Logs the compile errors of the material Component draws its people with (a figure material that failed shows the default). */
	void LogMaterialErrors(const UInstancedStaticMeshComponent& Component, TSet<UMaterial*>& Logged)
	{
		UMaterialInterface* Material = Component.GetMaterial(0);
		UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
		if (!Base || Logged.Contains(Base))
		{
			return;
		}
		Logged.Add(Base);
#if WITH_EDITOR
		const FMaterialResource* Resource = Base->GetMaterialResource(static_cast<EShaderPlatform>(GMaxRHIShaderPlatform));
		const TArray<FString> Errors = Resource ? Resource->GetCompileErrors() : TArray<FString>();
#else
		// Compile errors are kept by editor builds only; a cooked game has its materials compiled already.
		const TArray<FString> Errors;
#endif
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.People.Status: material %s: %s"), *Base->GetPathName(),
			Errors.IsEmpty() ? TEXT("no compile errors") : *FString::Printf(TEXT("%d error(s)"), Errors.Num()));
		for (const FString& Error : Errors)
		{
			UE_LOG(LogStageCraft, Display, TEXT("    %s"), *Error);
		}
	}

	void Status(const TArray<FString>& Args, UWorld* World)
	{
		if (!World)
		{
			return;
		}
		int32 Areas = 0;
		int32 People = 0;
		int32 Props = 0;
		int32 Figures = 0;
		TSet<UMaterial*> Logged;
		for (TActorIterator<AStageAudienceActor> It(World); It; ++It)
		{
			++Areas;
			People += It->GetPeopleCount();
			TInlineComponentArray<UInstancedStaticMeshComponent*> Components(*It);
			for (const UInstancedStaticMeshComponent* Component : Components)
			{
				LogMaterialErrors(*Component, Logged);
			}
		}
		for (TActorIterator<AStagePropActor> It(World); It; ++It)
		{
			++Props;
			Figures += It->GetFigureCount();
			TInlineComponentArray<UInstancedStaticMeshComponent*> Components(*It);
			for (const UInstancedStaticMeshComponent* Component : Components)
			{
				if (Component->NumCustomDataFloats == UStageFigureAssetSubsystem::CustomDataCount)
				{
					LogMaterialErrors(*Component, Logged);
				}
			}
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.People.Status: %d audience area(s) with %d people, %d prop(s) with %d figure(s)."), Areas, People, Props, Figures);
	}

	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.People.Status"),
		TEXT("Logs audience areas, props with people, and the compile state of their figure materials."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
}

#endif // !UE_BUILD_SHIPPING
