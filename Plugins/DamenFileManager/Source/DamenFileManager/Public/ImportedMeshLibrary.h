// Copyright DamenTools internal. Damen File Manager plugin.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "RealTimeImport.h"
#include "ImportedMeshLibrary.generated.h"

class UStaticMesh;
class UStaticMeshComponent;

/**
 * Turns a RealTimeImport FBX load into a real (transient) UStaticMesh, so an
 * imported object can travel the exact path built-in placeables use: the
 * placer's GhostMesh, the placed actor's StaticMeshComponent, the crane mount.
 *
 * Why this exists: RealTimeImport returns vertices in each FBX node's own
 * space and puts the real size/orientation in the node's transform, often
 * through a parent chain (Blender exports: metres + x100 scale + -90 X on the
 * root). Building sections straight from raw vertices - what the old
 * procedural path did - drops all of that, so most files came out as
 * invisible slivers or lost all but one part (STATE.md #198).
 */
UCLASS()
class DAMENFILEMANAGER_API UImportedMeshLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Bakes every model of an FBX load (each model's full parent-chain
	 * transform applied to positions, normals and tangents; winding flipped
	 * under mirroring scales) into one StaticMesh, one section per mesh part,
	 * default material, with the pivot at the bottom-centre of the bounds so it
	 * stands on the grid like a built-in placeable. Has simple (box) collision
	 * so a placed object can be clicked like any other.
	 *
	 * Call once per import (it builds GPU resources); returns null and logs on
	 * empty/invalid input.
	 */
	UFUNCTION(BlueprintCallable, Category = "Damen|ImportedMesh")
	static UStaticMesh* BuildStaticMeshFromModels(const TArray<FRTIModelStruct>& ModelStructs, const FString& MeshName);

	/**
	 * Shows an imported mesh on a placed object's StaticMeshComponent and turns
	 * it by the ghost's free yaw. Imported placeables need to do their own yaw:
	 * UPreciseTransformLibrary::ApplyPlacementYaw only recognises a class's
	 * default mesh, which an import by definition doesn't have.
	 */
	UFUNCTION(BlueprintCallable, Category = "Damen|ImportedMesh")
	static bool ShowImportedMesh(UStaticMeshComponent* Target, UStaticMesh* Mesh, float Yaw);
};
