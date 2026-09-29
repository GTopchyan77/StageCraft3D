// Copyright 2025-2026 NGG. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include <atomic>

/** Aggregate install state of the plugin's bundled Node.js packages. */
enum class ENGGDepsState : uint8
{
	/** No install has been kicked this session (deps may or may not be present). */
	Idle,
	/** Background `npm install` in flight. */
	Installing,
	/** All bundled packages verified installed. */
	Ready,
	/** Last install attempt failed — see GetStatusDetail(). */
	Failed,
};

/**
 * FNodeDepsInstaller
 *
 * First-run bootstrap for the plugin's bundled Node.js packages
 * (Source/ThirdParty/ngg-sidecar and Source/ThirdParty/unrealngg-mcp). They ship without
 * node_modules, so on a fresh clone/install the chat sidecar can't start
 * and the MCP server can't be spawned until `npm install` has run in each.
 * This class detects that and runs the installs on a background thread so
 * the user never has to open a terminal.
 *
 * Freshness is tracked with a stamp file inside each package's node_modules
 * holding the MD5 of package-lock.json — when a plugin update changes the
 * lock file, the next editor session reinstalls automatically.
 *
 * Callers (module startup toast, chat window connect flow) poll GetState()
 * from game-thread tickers; all state here is thread-safe.
 */
class FNodeDepsInstaller
{
public:
	/** Disk check: true when every bundled package has an up-to-date
	 *  node_modules (stamp matches the current package-lock.json). Never
	 *  true while an install is in flight. */
	static bool AreDepsReady();

	/** Kick a background `npm install` for every package that needs one.
	 *  Idempotent — safe to call from both module startup and the chat
	 *  window; only one install runs at a time. Calling again after a
	 *  failure retries. */
	static void StartInstallIfNeeded();

	static ENGGDepsState GetState();

	/** Human-readable progress ("Installing ngg-sidecar (1/2)...") while
	 *  Installing, or the failure reason (npm log tail + Node.js install
	 *  hint) after Failed. */
	static FString GetStatusDetail();

private:
	struct FPackage
	{
		FString Name; // folder name under Source/ThirdParty/, e.g. "ngg-sidecar"
		FString Dir;  // absolute path
	};

	/** Bundled packages that have a package.json on disk. */
	static TArray<FPackage> GetPackages();
	static bool PackageNeedsInstall(const FPackage& Pkg);
	/** Blocking — must run on a background thread. */
	static bool RunNpmInstall(const FPackage& Pkg, FString& OutError);
	/** MD5 of package-lock.json (falls back to package.json). Empty on error. */
	static FString ComputeLockHash(const FString& Dir);
	static FString GetStampPath(const FString& Dir);
	static FString GetInstallLogPath(const FString& PkgName);
	/** npm executable: sibling of NGG_NODE_PATH when set, else PATH lookup. */
	static FString ResolveNpmCommand();

	static void SetDetail(const FString& Detail);

	static std::atomic<ENGGDepsState> State;
	static FCriticalSection DetailCS;
	static FString StatusDetail;
};
