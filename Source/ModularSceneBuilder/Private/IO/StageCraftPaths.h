// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * The two folders every StageCraft file lives under, in one place:
 *  - DataRoot: Saved/StageCraft (scenes, marketplace community and library, outboxes, workspace, reports, imports, benchmarks).
 *  - UserRoot: the user's local application data folder + StageCraft (credential vault, bridge session file), which other
 *    Windows users cannot read.
 *
 * Integration tests run real subsystems against a temporary folder with FStageScopedDataRoot, so a test never reads or writes
 * the user's scenes, keys or bridge session. Thread-safe: package imports read these paths on worker threads.
 */
namespace StageCraftPaths
{
	/** Saved/StageCraft, or the test override. */
	FString DataRoot();

	/** <local app data>/StageCraft, or <test override>/User. */
	FString UserRoot();
}

#if WITH_DEV_AUTOMATION_TESTS
/**
 * Tests only: points DataRoot and UserRoot at Directory for its lifetime and restores the previous root after. Create it before
 * the subsystems under test start, and destroy it after they have stopped (worker pipes drained), since their paths are read
 * when used, not cached.
 */
class FStageScopedDataRoot
{
public:
	explicit FStageScopedDataRoot(const FString& Directory);
	~FStageScopedDataRoot();

	FStageScopedDataRoot(const FStageScopedDataRoot&) = delete;
	FStageScopedDataRoot& operator=(const FStageScopedDataRoot&) = delete;

private:
	FString Previous;
};
#endif
