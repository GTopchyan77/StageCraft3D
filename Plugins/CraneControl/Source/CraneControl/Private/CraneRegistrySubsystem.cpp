// Copyright DamenTools internal. Crane Control plugin.

#include "CraneRegistrySubsystem.h"
#include "CraneControl.h"
#include "CraneMeshMountComponent.h"

void UCraneRegistrySubsystem::RegisterCraneMount(UCraneMeshMountComponent* Mount)
{
	if (!Mount)
	{
		return;
	}

	if (CraneMount.IsValid() && CraneMount.Get() != Mount)
	{
		// The project has exactly one crane; a second one means a duplicated
		// actor, and silently switching targets would make the Crane Mesh List
		// act on whichever happened to begin play last.
		UE_LOG(LogCraneControl, Warning,
			TEXT("CraneRegistry: '%s' registered while '%s' is already the crane - keeping the first. Only one crane per level is supported."),
			*GetNameSafe(Mount->GetOwner()), *GetNameSafe(CraneMount->GetOwner()));
		return;
	}

	CraneMount = Mount;
	UE_LOG(LogCraneControl, Log, TEXT("CraneRegistry: crane '%s' registered."), *GetNameSafe(Mount->GetOwner()));
}

void UCraneRegistrySubsystem::UnregisterCraneMount(UCraneMeshMountComponent* Mount)
{
	if (CraneMount.Get() == Mount)
	{
		CraneMount.Reset();
	}
}

UCraneMeshMountComponent* UCraneRegistrySubsystem::GetCraneMount() const
{
	return CraneMount.Get();
}

AActor* UCraneRegistrySubsystem::GetCraneActor() const
{
	return CraneMount.IsValid() ? CraneMount->GetOwner() : nullptr;
}

bool UCraneRegistrySubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}
