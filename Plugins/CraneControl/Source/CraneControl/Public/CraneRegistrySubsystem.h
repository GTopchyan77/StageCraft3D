// Copyright DamenTools internal. Crane Control plugin.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "CraneRegistrySubsystem.generated.h"

class UCraneMeshMountComponent;

/**
 * The single place anything asks "which crane is in this world?".
 *
 * The crane self-registers: UCraneMeshMountComponent (added to
 * WareHouseMeshBP2) calls RegisterCraneMount in its BeginPlay and unregisters
 * in EndPlay, so callers never scan the world or re-resolve the crane their own
 * way. World-scoped (not GameInstance) because the crane is a level actor - a
 * GameInstance-lifetime pointer would outlive it across PIE sessions/level loads.
 *
 * Pre-existing lookups (WB_Demo_UI.WareHouse2 via GetActorOfClass, the
 * PC_Demo_PlayerCntroller.WareHouseBP handshake) are intentionally untouched
 * for now; new crane consumers should come through here.
 */
UCLASS()
class CRANECONTROL_API UCraneRegistrySubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	void RegisterCraneMount(UCraneMeshMountComponent* Mount);
	void UnregisterCraneMount(UCraneMeshMountComponent* Mount);

	/** The registered crane's mesh mount, or null if no crane has begun play in this world. */
	UFUNCTION(BlueprintPure, Category = "Crane")
	UCraneMeshMountComponent* GetCraneMount() const;

	/** The registered crane actor (the mount's owner), or null. */
	UFUNCTION(BlueprintPure, Category = "Crane")
	AActor* GetCraneActor() const;

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	TWeakObjectPtr<UCraneMeshMountComponent> CraneMount;
};
