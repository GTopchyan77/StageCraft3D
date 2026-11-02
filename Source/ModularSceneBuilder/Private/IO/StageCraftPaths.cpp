// Copyright Epic Games, Inc. All Rights Reserved.

#include "IO/StageCraftPaths.h"

#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Misc/ScopeRWLock.h"

namespace StageCraftPathsImpl
{
	FRWLock OverrideLock;
	/** Empty unless a test has pointed the roots elsewhere. */
	FString OverrideRoot;

	FString GetOverride()
	{
		FReadScopeLock ReadLock(OverrideLock);
		return OverrideRoot;
	}

	FString SetOverride(const FString& Root)
	{
		FWriteScopeLock WriteLock(OverrideLock);
		FString Previous = MoveTemp(OverrideRoot);
		OverrideRoot = Root;
		return Previous;
	}
}

FString StageCraftPaths::DataRoot()
{
	const FString Override = StageCraftPathsImpl::GetOverride();
	return Override.IsEmpty() ? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("StageCraft")) : Override;
}

FString StageCraftPaths::UserRoot()
{
	const FString Override = StageCraftPathsImpl::GetOverride();
	return Override.IsEmpty() ? FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("StageCraft")) : FPaths::Combine(Override, TEXT("User"));
}

#if WITH_DEV_AUTOMATION_TESTS
FStageScopedDataRoot::FStageScopedDataRoot(const FString& Directory)
	: Previous(StageCraftPathsImpl::SetOverride(FPaths::ConvertRelativePathToFull(Directory)))
{
}

FStageScopedDataRoot::~FStageScopedDataRoot()
{
	StageCraftPathsImpl::SetOverride(Previous);
}
#endif
