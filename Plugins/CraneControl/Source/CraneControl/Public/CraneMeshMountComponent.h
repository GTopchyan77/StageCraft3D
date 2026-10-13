// Copyright DamenTools internal. Crane Control plugin.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "CraneMeshMountComponent.generated.h"

class UStaticMesh;
class UStaticMeshComponent;

/**
 * Lets the crane carry one mesh at its hook.
 *
 * Added to WareHouseMeshBP2 as a component rather than by reparenting the
 * Blueprint or editing its graphs (the #192 decision: that Blueprint's graphs
 * have repeatedly lost wires on unrelated structural edits). On BeginPlay it
 * finds the mount point component by name, registers the crane with
 * UCraneRegistrySubsystem, and from then on owns a single StaticMeshComponent
 * attached to that mount point.
 *
 * The mesh is the same one the Mesh List's ghost/placed object uses (imported
 * meshes are real runtime StaticMeshes, pivot at their bottom-centre), hung so
 * its top sits at the hook.
 */
UCLASS(ClassGroup = (Crane), meta = (BlueprintSpawnableComponent))
class CRANECONTROL_API UCraneMeshMountComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCraneMeshMountComponent();

	/** Name of the scene component the mesh attaches to (WareHouseMeshBP2's hook-tip Arrow). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crane|Mount")
	FName MountPointComponentName;

	/**
	 * Replaces whatever is on the hook with Mesh (only one mesh is ever
	 * mounted). Null clears the hook.
	 * @return false if the mount point was never found (logged).
	 */
	UFUNCTION(BlueprintCallable, Category = "Crane|Mount")
	bool SetMountedMesh(UStaticMesh* Mesh);

	/** Removes the mounted mesh, if any. */
	UFUNCTION(BlueprintCallable, Category = "Crane|Mount")
	void ClearMountedMesh();

	/** The mesh currently on the hook, or null. */
	UFUNCTION(BlueprintPure, Category = "Crane|Mount")
	UStaticMesh* GetMountedMesh() const;

	UFUNCTION(BlueprintPure, Category = "Crane|Mount")
	USceneComponent* GetMountPoint() const { return MountPoint.Get(); }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UStaticMeshComponent* GetOrCreateMountedMesh();

	TWeakObjectPtr<USceneComponent> MountPoint;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> MountedMesh;
};
