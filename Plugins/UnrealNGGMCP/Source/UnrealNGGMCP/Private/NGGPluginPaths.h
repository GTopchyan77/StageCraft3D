// Copyright 2025-2026 NGG. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * FNGGPluginPaths
 *
 * Paths inside this plugin, resolved from where the plugin actually sits on
 * disk instead of from a hard-coded "Plugins/UnrealNGGMCP".
 *
 * The folder name is not ours to assume. Fab installs the plugin into
 * <Engine>/Plugins/Marketplace/UnrealNGGMCP, and the developer who moves it
 * into their project (as README Step 1 requires) picks the destination folder
 * name themselves — "NGGMCP", "UnrealNGGMCP-1.0", "MCP" are all things people
 * end up with. Everything that has to reach the bundled Node packages —
 * the chat sidecar, the npm installer, the generated .mcp.json — goes through
 * here so a rename cannot break it.
 *
 * The plugin cannot move while the editor is running, so the lookup is done
 * once and cached.
 */
class FNGGPluginPaths
{
public:
	/**
	 * Absolute path to the folder holding the .uplugin, without a trailing
	 * slash. Falls back to <Project>/Plugins/UnrealNGGMCP if the plugin manager
	 * cannot name an owner for this module (should not happen while the module
	 * is loaded — the fallback exists so callers never see an empty path).
	 */
	static const FString& GetPluginDir();

	/** Absolute path to <PluginDir>/Source/ThirdParty/<PackageName>. */
	static FString GetThirdPartyPackageDir(const TCHAR* PackageName);

	/**
	 * The plugin folder relative to the project directory, forward-slashed and
	 * without a trailing slash — e.g. "Plugins/UnrealNGGMCP", or
	 * "Plugins/Tools/UnrealNGGMCP" for a nested layout.
	 *
	 * Empty when the plugin lives outside the project (an engine/Fab install),
	 * where no project-relative path exists.
	 */
	static FString GetPluginDirRelativeToProject();

	/** True when the plugin lives somewhere under the current project directory. */
	static bool IsInsideProject();
};
