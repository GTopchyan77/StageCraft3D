// Copyright 2025-2026 NGG. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

UNREALNGGMCP_API DECLARE_LOG_CATEGORY_EXTERN(LogNGGChat, Log, All);

/**
 * FSidecarLauncher
 *
 * Static helpers for managing the external `ngg-sidecar` Node.js daemon
 * that hosts the long-lived Claude Code subprocess. The sidecar is
 * deliberately spawned detached so that it survives editor restarts.
 */
/** Result of FSidecarLauncher::EnsureCompatibleSidecar. */
enum class ESidecarEnsureResult : uint8
{
	/** Already running, version matches the plugin — connect directly. */
	AliveCompatible,
	/** Was running but version mismatched; shutdown succeeded; respawn issued —
	 *  caller should poll IsSidecarAlive before connecting. */
	RestartedAfterMismatch,
	/** Wasn't running; respawn issued — caller should poll IsSidecarAlive. */
	Spawned,
	/** Old sidecar refused to shut down within timeout — can't safely proceed. */
	ShutdownFailed,
	/** Spawn attempt failed (script not found, node missing, etc). */
	SpawnFailed,
};

class FSidecarLauncher
{
public:
	/**
	 * Project-specific sidecar port derived from the MD5 hash of the .uproject
	 * path. Identical to the first 4 hex digits of the session ID mapped into
	 * [20000, 29999]. Stable across editor restarts for the same project; unique
	 * per project so multiple projects can run their own sidecars simultaneously.
	 */
	static int32 GetSidecarPort();

	/** Permission-bridge port (always GetSidecarPort() + 1). */
	static int32 GetPermissionPort();

	/** WebSocket URL of this project's sidecar daemon. */
	static FString GetWebSocketURL();

	/** HTTP health-probe URL of this project's sidecar daemon. */
	static FString GetHealthURL();

	/** HTTP shutdown URL of this project's sidecar daemon (POST). */
	static FString GetShutdownURL();

	/**
	 * Sidecar version this build of the plugin is compatible with. Must match
	 * VERSION in Plugins/UnrealNGGMCP/Source/ThirdParty/ngg-sidecar/index.js and the
	 * "version" field in Plugins/UnrealNGGMCP/Source/ThirdParty/ngg-sidecar/package.json.
	 * Bump on all three places when the
	 * chat <-> sidecar protocol changes; the chat window will then shut down
	 * a stale running sidecar at connect time and respawn the matching one.
	 */
	static const TCHAR* GetExpectedSidecarVersion();

	/**
	 * Synchronously probe the sidecar `/health` endpoint.
	 * Returns true when it responds 200 within the timeout.
	 */
	static bool IsSidecarAlive(float TimeoutSeconds = 0.5f);

	/**
	 * Synchronously probe `/health` and return the sidecar's reported `version`
	 * string, or empty if unreachable / response was malformed.
	 */
	static FString ProbeSidecarVersion(float TimeoutSeconds = 0.5f);

	/**
	 * Synchronously probe `/health` and return the sidecar's reported OS process
	 * id, or 0 if unreachable / the response didn't carry a pid. Used as a last
	 * resort to forcibly reap an orphan when graceful /shutdown fails.
	 */
	static uint32 ProbeSidecarPid(float TimeoutSeconds = 0.5f);

	/**
	 * POST `/shutdown` to ask a running sidecar to exit, then poll `/health`
	 * until it stops responding (the process has actually died) or
	 * ShutdownTimeoutSeconds elapses. Returns true if it died within the
	 * timeout. Safe no-op when no sidecar is running.
	 */
	static bool RequestSidecarShutdown(float ShutdownTimeoutSeconds = 3.0f);

	/**
	 * Ensure the running sidecar (if any) matches GetExpectedSidecarVersion().
	 * If a stale older sidecar is running, shut it down and respawn the
	 * matching one. Returns the action taken; for any "*ed/spawned" outcome
	 * the caller should poll IsSidecarAlive() before opening the WebSocket.
	 */
	static ESidecarEnsureResult EnsureCompatibleSidecar();

	/**
	 * Spawn the sidecar in detached mode. Safe no-op if the sidecar is
	 * already alive. Returns true if spawn was attempted (not necessarily
	 * that it successfully came online — caller should poll IsSidecarAlive).
	 */
	static bool LaunchSidecarDetached();

	/** Resolves the absolute path of the sidecar entry script. */
	static FString GetSidecarScriptPath();

	/** Resolves the node executable command (honouring NGG_NODE_PATH). */
	static FString GetNodeExecutable();

	/** Per-platform sidecar log file path. */
	static FString GetSidecarLogPath();
};
