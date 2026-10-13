// Copyright DamenTools internal. Crane Control plugin.

#include "CraneMeshMountComponent.h"
#include "CraneControl.h"
#include "CraneRegistrySubsystem.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

UCraneMeshMountComponent::UCraneMeshMountComponent()
	: MountPointComponentName(TEXT("Importedmeshlocation"))
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UCraneMeshMountComponent::BeginPlay()
{
	Super::BeginPlay();

	AActor* Owner = GetOwner();
	TArray<USceneComponent*> SceneComponents;
	Owner->GetComponents(SceneComponents);
	for (USceneComponent* Candidate : SceneComponents)
	{
		// FName equality is case-insensitive, so "ImportedMeshLocation" also matches.
		if (Candidate && Candidate->GetFName() == MountPointComponentName)
		{
			MountPoint = Candidate;
			break;
		}
	}

	if (MountPoint.IsValid())
	{
		// Logged because the bridge can't read SCS attach parents: this line is
		// the evidence of what the hook actually follows at runtime.
		UE_LOG(LogCraneControl, Log, TEXT("CraneMeshMount: '%s' mount point '%s' (attached to '%s', mobility %d)."),
			*Owner->GetName(), *MountPoint->GetName(), *GetNameSafe(MountPoint->GetAttachParent()), static_cast<int32>(MountPoint->Mobility.GetValue()));
	}
	else
	{
		UE_LOG(LogCraneControl, Error, TEXT("CraneMeshMount: '%s' has no scene component named '%s' - meshes cannot be mounted. Fix MountPointComponentName on the component."),
			*Owner->GetName(), *MountPointComponentName.ToString());
	}

	// Registered even without a mount point, so a click reports the real
	// problem (missing mount) rather than "no crane".
	if (UCraneRegistrySubsystem* Registry = GetWorld()->GetSubsystem<UCraneRegistrySubsystem>())
	{
		Registry->RegisterCraneMount(this);
	}
}

void UCraneMeshMountComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		if (UCraneRegistrySubsystem* Registry = World->GetSubsystem<UCraneRegistrySubsystem>())
		{
			Registry->UnregisterCraneMount(this);
		}
	}
	Super::EndPlay(EndPlayReason);
}

UStaticMeshComponent* UCraneMeshMountComponent::GetOrCreateMountedMesh()
{
	if (MountedMesh)
	{
		return MountedMesh;
	}

	USceneComponent* Mount = MountPoint.Get();
	if (!Mount)
	{
		return nullptr;
	}

	AActor* Owner = GetOwner();
	MountedMesh = NewObject<UStaticMeshComponent>(Owner, TEXT("CraneMountedMesh"));
	// Movable: the hook moves every Tick, and a Static child can't attach to a Movable parent.
	MountedMesh->SetMobility(EComponentMobility::Movable);
	// Cargo on the hook must never become a click/trace/overlap target: selection
	// and placement traces in BP_BuildManager keep hitting what they hit today.
	MountedMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MountedMesh->SetGenerateOverlapEvents(false);
	MountedMesh->SetupAttachment(Mount);
	MountedMesh->RegisterComponent();
	Owner->AddInstanceComponent(MountedMesh);
	return MountedMesh;
}

bool UCraneMeshMountComponent::SetMountedMesh(UStaticMesh* Mesh)
{
	UE_LOG(LogCraneControl, Warning, TEXT("[CRANEDBG] SetMountedMesh received mesh '%s'"), *GetNameSafe(Mesh)); // TEMP [CRANEDBG] Bug B proof - remove after Gevor's test
	if (!Mesh)
	{
		ClearMountedMesh();
		return true;
	}

	UStaticMeshComponent* Component = GetOrCreateMountedMesh();
	if (!Component)
	{
		UE_LOG(LogCraneControl, Error, TEXT("CraneMeshMount: SetMountedMesh('%s') on '%s' failed - no mount point '%s' (see BeginPlay error)."),
			*Mesh->GetName(), *GetNameSafe(GetOwner()), *MountPointComponentName.ToString());
		return false;
	}

	Component->SetStaticMesh(Mesh);
	Component->SetVisibility(true);
	// Hang it: the mesh's top at the hook, centred under it. Works for any
	// pivot (imported meshes are bottom-centre, catalog meshes vary).
	const FBox Box = Mesh->GetBoundingBox();
	Component->SetRelativeLocationAndRotation(FVector(-Box.GetCenter().X, -Box.GetCenter().Y, -Box.Max.Z), FRotator::ZeroRotator);

	UE_LOG(LogCraneControl, Log, TEXT("CraneMeshMount: mounted '%s' (%.0f x %.0f x %.0f cm) on '%s'."),
		*Mesh->GetName(), Box.GetSize().X, Box.GetSize().Y, Box.GetSize().Z, *GetNameSafe(GetOwner()));
	return true;
}

void UCraneMeshMountComponent::ClearMountedMesh()
{
	if (MountedMesh)
	{
		MountedMesh->SetStaticMesh(nullptr);
		MountedMesh->SetVisibility(false);
		UE_LOG(LogCraneControl, Log, TEXT("CraneMeshMount: hook cleared on '%s'."), *GetNameSafe(GetOwner()));
	}
}

UStaticMesh* UCraneMeshMountComponent::GetMountedMesh() const
{
	return MountedMesh ? MountedMesh->GetStaticMesh() : nullptr;
}
