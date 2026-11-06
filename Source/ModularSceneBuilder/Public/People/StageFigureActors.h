// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "People/StageAudienceMath.h"

/** How the people of an item answer cursor traces. */
enum class EStageFigureCollision : uint8
{
	/** Not hit by any trace: crowds, whose area has its own pick box. */
	None,
	/** Hit only by the stage item channel (selection), never by placement, camera or fixture picking (Visibility). */
	Selectable,
};

/**
 * The few steps every item showing stand-in people shares (AStagePropActor, AStageAudienceActor; Docs/ADR/0031 §3): set an
 * instanced component up for the figure mesh, and fill it with people. Game thread only.
 */
namespace StageFigureActors
{
	/** Movable, no navigation or overlaps, the collision asked for, and the custom data slots the figure material reads. */
	MODULARSCENEBUILDER_API void ConfigureFigureComponent(class UInstancedStaticMeshComponent& Component, EStageFigureCollision Collision);

	/**
	 * Replaces Component's instances with People: the shared figure mesh and the dynamic instance of FigureMaterial from
	 * UStageFigureAssetSubsystem, and each person's clip, phase, speed and colours as per-instance custom data. Clears the
	 * component and returns 0 without a material or a mesh. Returns the people shown.
	 */
	MODULARSCENEBUILDER_API int32 ShowPeople(class UInstancedStaticMeshComponent& Component, TConstArrayView<StageAudienceMath::FPerson> People,
		class UMaterialInterface* FigureMaterial);

	/** The custom data floats of one person, in UStageFigureAssetSubsystem::ECustomData order. */
	MODULARSCENEBUILDER_API void MakeCustomData(const StageAudienceMath::FPerson& Person, TArray<float>& OutData);
}
